// FLAG PC divergence: "brn.debugvars.v1" -- the game's own CgsDev debug menu (Ctrl+Space) as a
// flat listing a mod can show and edit. No console counterpart.
//
// Everything happens on the update thread, inside the debug critical section, at the mods'
// game tick: the listing is rebuilt there (only while a mod keeps asking for it), and set / call
// go through Variable::SetValueFromString / Function::Select -- the same paths as the game's
// console and menu, so ranges, read-only flags and change callbacks all apply.

#include "pc/mods/ModHost.h"

#if BRN_PC_MODS

#include "pc/mods/ModInternal.h"
#include "GameShared/GameClasses/Development/DebugSystem/Core/CgsDebugManager.h"
#include "GameShared/GameClasses/Development/DebugSystem/Core/CgsDebugComponent.h"
#include "GameShared/GameClasses/Development/DebugSystem/Core/UI/CgsDebugUI.h"
#include "GameShared/GameClasses/Development/DebugSystem/Core/UI/Menu/CgsMenu.h"
#include "GameShared/GameClasses/Development/DebugSystem/Core/UI/Menu/CgsMenuManager.h"
#include "GameShared/GameClasses/Development/DebugSystem/Core/UI/Variables/CgsMenuItemVariable.h"
#include "GameShared/GameClasses/Development/DebugSystem/Core/UI/Variables/CgsVariable.h"
#include "GameShared/GameClasses/Development/DebugSystem/Core/UI/Functions/CgsMenuItemFunction.h"
#include "GameShared/GameClasses/Development/DebugSystem/Core/UI/Functions/CgsFunction.h"

#include <Windows.h>   // GetTickCount64
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <string.h>
#include <utility>
#include <vector>

namespace CgsPC { namespace Mods
{
    namespace
    {
        namespace DUI = CgsDev::DebugUI;

        struct Entry
        {
            BrnDebugVar                              mVar;
            std::vector<std::pair<std::string, int>> maOptions;
        };

        struct Command
        {
            std::string mPath;
            std::string mValue;
            bool        mbCall;
        };

        const size_t KU_MAX_ENTRIES = 20000;
        const ULONGLONG KU_KEEP_FRESH_MS = 1000;   // after the last refresh() call
        const ULONGLONG KU_REBUILD_MS = 150;

        std::mutex             sMutex;             // guards the three below
        std::vector<Entry>     saEntries;
        std::vector<Command>   saCommands;
        uint32_t               suGeneration = 0;
        std::atomic<ULONGLONG> suWantedAt{0};
    }

    // The one door into Menu's item list (friend of DUI::Menu, FLAG PC divergence there).
    struct DebugVarsAccess
    {
        template <class Visit>
        static void Walk(DUI::Menu* lpMenu, const std::string& lrPrefix, int liDepth, Visit& lrVisit)
        {
            for (DUI::MenuItem* lpItem = lpMenu->mMenuItems.GetFirst(); lpItem != nullptr;
                 lpItem = lpMenu->mMenuItems.GetNext(lpItem))
            {
                if (!lpItem->IsVisible()) continue;
                if (!lrVisit(lpItem, lrPrefix, liDepth)) return;
                if (DUI::Menu* lpSubMenu = dynamic_cast<DUI::Menu*>(lpItem))
                    Walk(lpSubMenu, lrPrefix + lpSubMenu->GetCaption() + "/", liDepth + 1, lrVisit);
            }
        }
    };

    namespace
    {
        const char* ItemName(DUI::MenuItem* lpItem)
        {
            if (DUI::Menu* lpMenu = dynamic_cast<DUI::Menu*>(lpItem)) return lpMenu->GetCaption();
            if (DUI::MenuItemVariable* lpVariable = dynamic_cast<DUI::MenuItemVariable*>(lpItem))
                return lpVariable->GetVariable() ? lpVariable->GetVariable()->GetName() : nullptr;
            if (DUI::MenuItemFunction* lpFunction = dynamic_cast<DUI::MenuItemFunction*>(lpItem))
                return lpFunction->GetFunction() ? lpFunction->GetFunction()->GetName() : nullptr;
            return nullptr;
        }

        // One option of a choice list, read under SEH: some tables are not terminated by a null
        // entry (the game itself only scans them up to the current value), so the scan below is
        // bounded and a bad read ends it instead of the run.
        bool ReadOption(const DUI::StringList* lpOption, const char** lppcName, int* lpiValue)
        {
            __try
            {
                *lppcName = lpOption->mpcName;
                *lpiValue = lpOption->miValue;
                if (*lppcName != nullptr) (void)**lppcName;
                return *lppcName != nullptr;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        void Describe(DUI::Variable& lrVariable, Entry& lrEntry)
        {
            BrnDebugVar& lrOut = lrEntry.mVar;
            DUI::Variant& lrValue = lrVariable.GetValue();
            const DUI::StringList* lpOptions = lrVariable.GetStringList();
            switch (lrValue.meType)
            {
            case DUI::Variant::E_TYPE_BOOL:      lrOut.kind = BRN_DEBUGVAR_BOOL;  lrOut.value = lrValue.mValue.mbBool; break;
            case DUI::Variant::E_TYPE_PTR_BOOL:  lrOut.kind = BRN_DEBUGVAR_BOOL;  lrOut.value = *lrValue.mValue.mpbBool; break;
            case DUI::Variant::E_TYPE_INT32:     lrOut.kind = BRN_DEBUGVAR_INT;   lrOut.value = lrValue.mValue.miInt32; break;
            case DUI::Variant::E_TYPE_PTR_INT32: lrOut.kind = BRN_DEBUGVAR_INT;   lrOut.value = *lrValue.mValue.mpiInt32; break;
            case DUI::Variant::E_TYPE_UINT32:    lrOut.kind = BRN_DEBUGVAR_UINT;  lrOut.value = lrValue.mValue.muUInt32; break;
            case DUI::Variant::E_TYPE_PTR_UINT32:lrOut.kind = BRN_DEBUGVAR_UINT;  lrOut.value = *lrValue.mValue.mpuUInt32; break;
            case DUI::Variant::E_TYPE_FLOAT:     lrOut.kind = BRN_DEBUGVAR_FLOAT; lrOut.value = lrValue.mValue.mfFloat; break;
            case DUI::Variant::E_TYPE_PTR_FLOAT: lrOut.kind = BRN_DEBUGVAR_FLOAT; lrOut.value = *lrValue.mValue.mpfFloat; break;
            default:                             lrOut.kind = BRN_DEBUGVAR_FUNCTION; lrOut.read_only = 1; return;
            }
            lrOut.read_only = lrVariable.IsReadOnly();
            // The getters return the default when no range is set: two impossible defaults say so.
            const f32 lfMin = lrVariable.GetMinAsFloat(3.0e38f);
            const f32 lfMax = lrVariable.GetMaxAsFloat(-3.0e38f);
            if (lfMin != 3.0e38f && lfMax != -3.0e38f)
            {
                lrOut.has_range = 1;
                lrOut.min = lfMin;
                lrOut.max = lfMax;
            }
            if (lpOptions != nullptr && lrOut.kind == BRN_DEBUGVAR_INT)
            {
                lrOut.kind = BRN_DEBUGVAR_CHOICE;
                // A range bounds the table (Debug/Framerate's 0..2 has three unterminated rows).
                const f32 lfCount = lrOut.has_range ? lfMax - lfMin + 1.0f : 64.0f;
                const int liLimit = lfCount < 1.0f ? 1 : (lfCount > 256.0f ? 256 : static_cast<int>(lfCount));
                const char* lpcName = nullptr;
                int liValue = 0;
                for (int li = 0; li < liLimit && ReadOption(lpOptions + li, &lpcName, &liValue); ++li)
                {
                    lrEntry.maOptions.emplace_back(lpcName, liValue);
                    if (liValue == static_cast<int>(lrOut.value))
                        std::snprintf(lrOut.text, sizeof(lrOut.text), "%s", lpcName);
                }
            }
            if (lrOut.text[0] == '\0')
            {
                if (lrOut.kind == BRN_DEBUGVAR_BOOL) std::snprintf(lrOut.text, sizeof(lrOut.text), "%s", lrOut.value != 0.0 ? "on" : "off");
                else if (lrOut.kind == BRN_DEBUGVAR_FLOAT) std::snprintf(lrOut.text, sizeof(lrOut.text), "%.4g", lrOut.value);
                else std::snprintf(lrOut.text, sizeof(lrOut.text), "%.0f", lrOut.value);
            }
        }

        // A component's section row is a Function whose parameter is that component
        // (DebugManager::RegisterComponent); IsComponentRegistered only compares pointers.
        CgsDev::DebugComponent* SectionComponent(CgsDev::DebugManager& lrManager, DUI::MenuItemFunction& lrItem)
        {
            DUI::Function* lpFunction = lrItem.GetFunction();
            if (lpFunction == nullptr) return nullptr;
            auto* lpComponent = static_cast<CgsDev::DebugComponent*>(lpFunction->GetParameter());
            return lrManager.IsComponentRegistered(lpComponent) ? lpComponent : nullptr;
        }

        std::vector<Entry> BuildListing(CgsDev::DebugManager& lrManager, DUI::Menu* lpRoot)
        {
            std::vector<Entry> laEntries;
            auto lVisit = [&](DUI::MenuItem* lpItem, const std::string& lrPrefix, int liDepth) -> bool
            {
                if (laEntries.size() >= KU_MAX_ENTRIES) return false;
                const char* lpcName = ItemName(lpItem);
                if (lpcName == nullptr || lpcName[0] == '\0') return true;
                Entry lEntry = {};
                std::snprintf(lEntry.mVar.path, sizeof(lEntry.mVar.path), "%s%s", lrPrefix.c_str(), lpcName);
                std::snprintf(lEntry.mVar.name, sizeof(lEntry.mVar.name), "%s", lpcName);
                lEntry.mVar.depth = liDepth;
                if (dynamic_cast<DUI::Menu*>(lpItem) != nullptr)
                    lEntry.mVar.kind = BRN_DEBUGVAR_MENU;
                else if (DUI::MenuItemFunction* lpFunction = dynamic_cast<DUI::MenuItemFunction*>(lpItem))
                {
                    CgsDev::DebugComponent* lpComponent = SectionComponent(lrManager, *lpFunction);
                    lEntry.mVar.kind = lpComponent != nullptr ? BRN_DEBUGVAR_SECTION : BRN_DEBUGVAR_FUNCTION;
                    lEntry.mVar.value = (lpComponent != nullptr && lpComponent->IsActive()) ? 1.0 : 0.0;
                }
                else if (DUI::MenuItemVariable* lpVariable = dynamic_cast<DUI::MenuItemVariable*>(lpItem))
                    Describe(*lpVariable->GetVariable(), lEntry);
                laEntries.push_back(std::move(lEntry));
                return true;
            };
            DebugVarsAccess::Walk(lpRoot, std::string(), 0, lVisit);
            return laEntries;
        }

        DUI::MenuItem* FindItem(DUI::Menu* lpRoot, const std::string& lrPath)
        {
            DUI::MenuItem* lpFound = nullptr;
            auto lVisit = [&](DUI::MenuItem* lpItem, const std::string& lrPrefix, int) -> bool
            {
                const char* lpcName = ItemName(lpItem);
                if (lpcName != nullptr && _stricmp((lrPrefix + lpcName).c_str(), lrPath.c_str()) == 0)
                {
                    lpFound = lpItem;
                    return false;
                }
                return true;
            };
            DebugVarsAccess::Walk(lpRoot, std::string(), 0, lVisit);
            return lpFound;
        }

        void Apply(CgsDev::DebugManager& lrManager, DUI::Menu* lpRoot, const Command& lrCommand)
        {
            DUI::MenuItem* lpItem = FindItem(lpRoot, lrCommand.mPath);
            if (lpItem == nullptr)
            {
                LogLine(("[mods] debugvars: no item \"" + lrCommand.mPath + "\"\n").c_str());
                return;
            }
            if (lrCommand.mbCall)
            {
                DUI::MenuItemFunction* lpFunction = dynamic_cast<DUI::MenuItemFunction*>(lpItem);
                if (lpFunction == nullptr || lpFunction->GetFunction() == nullptr) return;
                // A section: activate the component only. Its menu callback would also open the
                // page in the game's own debug menu window, which the mod's window replaces.
                if (CgsDev::DebugComponent* lpComponent = SectionComponent(lrManager, *lpFunction))
                    lrManager.ActivateComponent(lpComponent);
                else
                    lpFunction->GetFunction()->Select();
                return;
            }
            if (DUI::MenuItemVariable* lpVariable = dynamic_cast<DUI::MenuItemVariable*>(lpItem))
                if (lpVariable->GetVariable() != nullptr)
                    lpVariable->GetVariable()->SetValueFromString(lrCommand.mValue.c_str());
        }

        // ---- the service -----------------------------------------------------------------

        uint32_t VarsRefresh()
        {
            suWantedAt.store(GetTickCount64());
            std::lock_guard<std::mutex> lLock(sMutex);
            return suGeneration;
        }

        int VarsCount()
        {
            std::lock_guard<std::mutex> lLock(sMutex);
            return static_cast<int>(saEntries.size());
        }

        int VarsGet(int liIndex, BrnDebugVar* lpOut)
        {
            std::lock_guard<std::mutex> lLock(sMutex);
            if (lpOut == nullptr || liIndex < 0 || static_cast<size_t>(liIndex) >= saEntries.size()) return 0;
            *lpOut = saEntries[static_cast<size_t>(liIndex)].mVar;
            return 1;
        }

        int VarsOptionCount(int liIndex)
        {
            std::lock_guard<std::mutex> lLock(sMutex);
            if (liIndex < 0 || static_cast<size_t>(liIndex) >= saEntries.size()) return 0;
            return static_cast<int>(saEntries[static_cast<size_t>(liIndex)].maOptions.size());
        }

        int VarsGetOption(int liIndex, int liOption, char* lpcName, int liNameSize, int* lpiValue)
        {
            std::lock_guard<std::mutex> lLock(sMutex);
            if (liIndex < 0 || static_cast<size_t>(liIndex) >= saEntries.size()) return 0;
            const auto& lrOptions = saEntries[static_cast<size_t>(liIndex)].maOptions;
            if (liOption < 0 || static_cast<size_t>(liOption) >= lrOptions.size()) return 0;
            if (lpcName != nullptr && liNameSize > 0)
                std::snprintf(lpcName, static_cast<size_t>(liNameSize), "%s", lrOptions[static_cast<size_t>(liOption)].first.c_str());
            if (lpiValue != nullptr) *lpiValue = lrOptions[static_cast<size_t>(liOption)].second;
            return 1;
        }

        void Queue(const char* lpcPath, const char* lpcValue, bool lbCall)
        {
            if (lpcPath == nullptr) return;
            std::lock_guard<std::mutex> lLock(sMutex);
            if (saCommands.size() < 256)
                saCommands.push_back(Command{ lpcPath, lpcValue != nullptr ? lpcValue : "", lbCall });
        }

        void VarsSet(const char* lpcPath, const char* lpcValue) { Queue(lpcPath, lpcValue, false); }
        void VarsCall(const char* lpcPath) { Queue(lpcPath, nullptr, true); }

        const BrnDebugVarsV1 KDEBUGVARS_V1 = {
            sizeof(BrnDebugVarsV1), VarsRefresh, VarsCount, VarsGet, VarsOptionCount, VarsGetOption, VarsSet, VarsCall,
        };
    }

    const void* FindDebugVarsService(const char* lpcName)
    {
        return std::strcmp(lpcName, "brn.debugvars.v1") == 0 ? &KDEBUGVARS_V1 : nullptr;
    }

    void TickDebugVars()
    {
        static ULONGLONG suBuiltAt = 0;
        std::vector<Command> laCommands;
        {
            std::lock_guard<std::mutex> lLock(sMutex);
            laCommands.swap(saCommands);
        }
        const ULONGLONG luNow = GetTickCount64();
        const bool lbWanted = luNow - suWantedAt.load() < KU_KEEP_FRESH_MS;
        if (laCommands.empty() && (!lbWanted || luNow - suBuiltAt < KU_REBUILD_MS)) return;
        if (CgsDev::DebugManager::GetInstance() == nullptr) return;

        CgsDev::DebugManager* lpManager = CgsDev::DebugManager::ThreadSafeAquire();
        DUI::Menu* lpRoot = lpManager->GetUI().GetMenuManager().GetMenuFromPath(nullptr, nullptr);
        std::vector<Entry> laEntries;
        if (lpRoot != nullptr)
        {
            for (const Command& lrCommand : laCommands) Apply(*lpManager, lpRoot, lrCommand);
            if (lbWanted) laEntries = BuildListing(*lpManager, lpRoot);
        }
        CgsDev::DebugManager::ThreadSafeRelease(lpManager);
        if (!lbWanted) return;
        suBuiltAt = luNow;

        std::lock_guard<std::mutex> lLock(sMutex);
        saEntries.swap(laEntries);
        ++suGeneration;
    }
} }

#endif // BRN_PC_MODS
