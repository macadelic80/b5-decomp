// FLAG PC divergence: the DLL mod host. See ModHost.h for the contract and
// pc/mods/api/brn_mod_api.h for what a mod sees. No console counterpart.
//
// Lifetime: the first RenderFrame or OnGameTick reads config.ini [Mods] Enabled (default 1),
// scans mods\*\mod.ini next to the exe, and loads each DLL whose mods\mods.ini [Enabled]
// entry is not 0. A mod that throws (SEH) is disabled and named in BrnGame.log; the game
// keeps running. Disabling a mod stops its callbacks; its DLL stays loaded until exit.

#include "pc/mods/ModHost.h"

#if BRN_PC_MODS

#include "pc/mods/ModInternal.h"
#include "pc/gcm/renderengine/device.h"                       // renderengine::PCGetBackBuffer
#include "GameShared/GameClasses/Development/Log/CgsLog.h"   // CgsDev::Log::WriteToLog
#include "GameShared/GameClasses/Development/AssertSystem/CgsAssertManager.h"   // gAssertManager.HasAssert

#include <Windows.h>
#include <d3d9.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct BrnMod
{
    std::string             mName;
    std::string             mFolder;       // the folder name under mods\ (the mods.ini key)
    std::string             mVersion;
    std::string             mDirectory;    // "<exe dir>\mods\<folder>\"
    std::string             mError;
    HMODULE                 mhModule = nullptr;
    BrnModCallbacks         mCallbacks = {};
    std::atomic<bool>       mbEnabled{false};
    std::atomic<uint32_t>   muCapture{0};
};

namespace CgsPC { namespace Mods
{
    namespace
    {
        typedef uint32_t (*ApiVersionFn)();
        typedef int (*InitFn)(const BrnHostApi*, BrnModCallbacks*);

        std::once_flag                       sInitOnce;
        std::atomic<bool>                    sbActive{false};   // at least one mod is loaded
        std::vector<std::unique_ptr<BrnMod>> saMods;            // fixed once sInitOnce has run
        std::string                          sModsDirectory;
        std::atomic<uint32_t>                suCapture{0};

        std::mutex                           sMessageMutex;
        std::vector<BrnWindowMessage>        saMessages;

        // ---- SEH shells: no C++ objects in these frames ------------------------------------

        bool CallInit(InitFn lpfnInit, const BrnHostApi* lpHost, BrnModCallbacks* lpCallbacks, int* lpiResult)
        {
            __try { *lpiResult = lpfnInit(lpHost, lpCallbacks); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        bool CallMessage(const BrnModCallbacks& lrCallbacks, const BrnWindowMessage* lpMessage)
        {
            __try { lrCallbacks.on_message(lrCallbacks.user, lpMessage); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        bool CallFrame(const BrnModCallbacks& lrCallbacks, const BrnFrameContext* lpFrame)
        {
            __try { lrCallbacks.on_frame(lrCallbacks.user, lpFrame); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        bool CallTick(const BrnModCallbacks& lrCallbacks)
        {
            __try { lrCallbacks.on_game_tick(lrCallbacks.user); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        // ---- capture ----------------------------------------------------------------------

        void RecomputeCapture()
        {
            uint32_t luCapture = 0;
            for (const auto& lpMod : saMods)
                if (lpMod->mbEnabled.load()) luCapture |= lpMod->muCapture.load();
            suCapture.store(luCapture);
        }

        void Fault(BrnMod& lrMod, const char* lpcWhere)
        {
            lrMod.mbEnabled.store(false);
            lrMod.muCapture.store(0);
            lrMod.mError = std::string("crashed in ") + lpcWhere + "; disabled for this run";
            RecomputeCapture();
            char lacLine[256];
            std::snprintf(lacLine, sizeof(lacLine), "[mods] %s crashed in %s -- disabled\n",
                          lrMod.mName.c_str(), lpcWhere);
            LogLine(lacLine);
        }

        // ---- mods.ini ------------------------------------------------------------------------

        std::string ModsIniPath() { return sModsDirectory + "mods.ini"; }

        bool ReadEnabled(const std::string& lrFolder)
        {
            return GetPrivateProfileIntA("Enabled", lrFolder.c_str(), 1, ModsIniPath().c_str()) != 0;
        }

        void WriteEnabled(const std::string& lrFolder, bool lbEnabled)
        {
            WritePrivateProfileStringA("Enabled", lrFolder.c_str(), lbEnabled ? "1" : "0",
                                       ModsIniPath().c_str());
        }

        // ---- brn.core.v1 ---------------------------------------------------------------------

        void CoreLog(BrnMod* lpSelf, const char* lpcText)
        {
            if (lpcText == nullptr) return;
            std::string lLine = "[mod:";
            lLine += lpSelf ? lpSelf->mFolder : std::string("?");
            lLine += "] ";
            lLine += lpcText;
            if (lLine.empty() || lLine.back() != '\n') lLine += '\n';
            LogLine(lLine.c_str());
        }

        const char* CoreModDirectory(BrnMod* lpSelf) { return lpSelf ? lpSelf->mDirectory.c_str() : ""; }

        void CoreSetInputCapture(BrnMod* lpSelf, uint32_t luFlags)
        {
            if (lpSelf == nullptr) return;
            lpSelf->muCapture.store(lpSelf->mbEnabled.load() ? luFlags : 0u);
            RecomputeCapture();
        }

        int CoreModCount() { return static_cast<int>(saMods.size()); }

        void CopyText(char* lpcOut, size_t luSize, const std::string& lrText)
        {
            std::snprintf(lpcOut, luSize, "%s", lrText.c_str());
        }

        int CoreGetModInfo(int liIndex, BrnModInfo* lpOut)
        {
            if (lpOut == nullptr || liIndex < 0 || liIndex >= CoreModCount()) return 0;
            const BrnMod& lrMod = *saMods[static_cast<size_t>(liIndex)];
            *lpOut = BrnModInfo{};
            CopyText(lpOut->name, sizeof(lpOut->name), lrMod.mName);
            CopyText(lpOut->folder, sizeof(lpOut->folder), lrMod.mFolder);
            CopyText(lpOut->version, sizeof(lpOut->version), lrMod.mVersion);
            CopyText(lpOut->error, sizeof(lpOut->error), lrMod.mError);
            lpOut->loaded = lrMod.mhModule != nullptr;
            lpOut->enabled = lrMod.mbEnabled.load();
            return 1;
        }

        void CoreSetModEnabled(int liIndex, int liEnabled)
        {
            if (liIndex < 0 || liIndex >= CoreModCount()) return;
            BrnMod& lrMod = *saMods[static_cast<size_t>(liIndex)];
            WriteEnabled(lrMod.mFolder, liEnabled != 0);
            if (lrMod.mhModule == nullptr) return;   // takes effect at the next start
            lrMod.mbEnabled.store(liEnabled != 0);
            if (!liEnabled) lrMod.muCapture.store(0);
            RecomputeCapture();
        }

        const BrnCoreV1 KCORE_V1 = {
            sizeof(BrnCoreV1), CoreLog, CoreModDirectory, CoreSetInputCapture,
            CoreModCount, CoreGetModInfo, CoreSetModEnabled,
        };

        const void* GetService(const char* lpcName)
        {
            if (lpcName == nullptr) return nullptr;
            if (std::strcmp(lpcName, "brn.core.v1") == 0) return &KCORE_V1;
            return FindGameService(lpcName);
        }

        // ---- loading ---------------------------------------------------------------------------

        std::string ExeDirectory()
        {
            char lacPath[MAX_PATH];
            const DWORD luLength = GetModuleFileNameA(nullptr, lacPath, MAX_PATH);
            if (luLength == 0 || luLength >= MAX_PATH) return std::string();
            std::string lPath(lacPath, luLength);
            const size_t luSlash = lPath.find_last_of("\\/");
            return luSlash == std::string::npos ? std::string() : lPath.substr(0, luSlash + 1);
        }

        std::string IniString(const std::string& lrPath, const char* lpcKey, const char* lpcDefault)
        {
            char lacValue[260];
            GetPrivateProfileStringA("Mod", lpcKey, lpcDefault, lacValue, sizeof(lacValue), lrPath.c_str());
            return lacValue;
        }

        void LoadMod(BrnMod& lrMod, const std::string& lrDll)
        {
            if (!ReadEnabled(lrMod.mFolder)) { lrMod.mError = "disabled in mods.ini"; return; }
            const std::string lDllPath = lrMod.mDirectory + lrDll;
            // The mod's folder joins the search path so the DLL finds its own dependencies.
            lrMod.mhModule = LoadLibraryExA(lDllPath.c_str(), nullptr,
                                            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (lrMod.mhModule == nullptr)
            {
                lrMod.mError = "LoadLibrary failed (error " + std::to_string(GetLastError()) + ")";
                return;
            }
            const auto lpfnVersion = reinterpret_cast<ApiVersionFn>(GetProcAddress(lrMod.mhModule, "brn_mod_api_version"));
            const auto lpfnInit = reinterpret_cast<InitFn>(GetProcAddress(lrMod.mhModule, "brn_mod_init"));
            if (lpfnVersion == nullptr || lpfnInit == nullptr)
            {
                lrMod.mError = "missing brn_mod_api_version / brn_mod_init";
                FreeLibrary(lrMod.mhModule); lrMod.mhModule = nullptr;
                return;
            }
            const uint32_t luVersion = lpfnVersion();
            if (luVersion != BRN_MOD_API_VERSION)
            {
                lrMod.mError = "built for API " + std::to_string(luVersion) + ", game has " +
                               std::to_string(BRN_MOD_API_VERSION);
                FreeLibrary(lrMod.mhModule); lrMod.mhModule = nullptr;
                return;
            }
            const BrnHostApi lHost = { sizeof(BrnHostApi), BRN_MOD_API_VERSION, &lrMod, GetService };
            lrMod.mCallbacks.size = sizeof(BrnModCallbacks);
            int liResult = 0;
            if (!CallInit(lpfnInit, &lHost, &lrMod.mCallbacks, &liResult))
            {
                lrMod.mError = "crashed in brn_mod_init";
                lrMod.mCallbacks = BrnModCallbacks{};
                return;   // the DLL stays mapped: unloading code that just faulted is not safer
            }
            if (liResult == 0)
            {
                lrMod.mError = "brn_mod_init refused to load";
                lrMod.mCallbacks = BrnModCallbacks{};
                return;
            }
            lrMod.mbEnabled.store(true);
            sbActive.store(true);
        }

        void Initialise()
        {
            const std::string lExeDirectory = ExeDirectory();
            if (lExeDirectory.empty()) return;
            const std::string lConfig = lExeDirectory + "config.ini";
            if (GetPrivateProfileIntA("Mods", "Enabled", 1, lConfig.c_str()) == 0)
            {
                LogLine("[mods] config.ini [Mods] Enabled=0 -- no mod loaded\n");
                return;
            }
            sModsDirectory = lExeDirectory + "mods\\";

            WIN32_FIND_DATAA lFind;
            const HANDLE lhFind = FindFirstFileA((sModsDirectory + "*").c_str(), &lFind);
            if (lhFind == INVALID_HANDLE_VALUE) return;
            do
            {
                if ((lFind.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || lFind.cFileName[0] == '.')
                    continue;
                const std::string lDirectory = sModsDirectory + lFind.cFileName + "\\";
                const std::string lManifest = lDirectory + "mod.ini";
                if (GetFileAttributesA(lManifest.c_str()) == INVALID_FILE_ATTRIBUTES)
                    continue;
                auto lpMod = std::make_unique<BrnMod>();
                lpMod->mFolder = lFind.cFileName;
                lpMod->mDirectory = lDirectory;
                lpMod->mName = IniString(lManifest, "Name", lFind.cFileName);
                lpMod->mVersion = IniString(lManifest, "Version", "");
                const std::string lDll = IniString(lManifest, "Dll", "");
                if (lDll.empty()) lpMod->mError = "mod.ini has no Dll=";
                else LoadMod(*lpMod, lDll);

                char lacLine[512];
                std::snprintf(lacLine, sizeof(lacLine), "[mods] %s (%s): %s\n", lpMod->mName.c_str(),
                              lpMod->mFolder.c_str(),
                              lpMod->mbEnabled.load() ? "loaded" : lpMod->mError.c_str());
                LogLine(lacLine);
                saMods.push_back(std::move(lpMod));
            } while (FindNextFileA(lhFind, &lFind));
            FindClose(lhFind);
        }

        void EnsureInitialised() { std::call_once(sInitOnce, Initialise); }

        uint32_t CurrentModifiers()
        {
            uint32_t luModifiers = 0;
            if (GetKeyState(VK_CONTROL) & 0x8000) luModifiers |= BRN_MODIFIER_CTRL;
            if (GetKeyState(VK_SHIFT) & 0x8000)   luModifiers |= BRN_MODIFIER_SHIFT;
            if (GetKeyState(VK_MENU) & 0x8000)    luModifiers |= BRN_MODIFIER_ALT;
            if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) luModifiers |= BRN_MODIFIER_SUPER;
            return luModifiers;
        }

        bool IsForwarded(UINT luMessage)
        {
            return (luMessage >= WM_KEYFIRST && luMessage <= WM_KEYLAST) ||
                   (luMessage >= WM_MOUSEFIRST && luMessage <= WM_MOUSELAST) ||
                   luMessage == WM_SETFOCUS || luMessage == WM_KILLFOCUS ||
                   luMessage == WM_ACTIVATEAPP || luMessage == WM_SIZE || luMessage == WM_MOUSELEAVE;
        }
    }

    void LogLine(const char* lpcText) { CgsDev::Log::WriteToLog(lpcText); }

    bool IsCapturingKeyboard() { return (suCapture.load() & BRN_CAPTURE_KEYBOARD) != 0; }
    bool IsCapturingGamepad()  { return (suCapture.load() & BRN_CAPTURE_GAMEPAD) != 0; }

    bool OnWindowMessage(HWND__* lhWindow, unsigned int luMessage, unsigned long long luWParam,
                         long long liLParam)
    {
        if (!sbActive.load() || !IsForwarded(luMessage)) return false;
        {
            const BrnWindowMessage lMessage = { lhWindow, luMessage, luWParam, liLParam, CurrentModifiers() };
            std::lock_guard<std::mutex> lLock(sMessageMutex);
            if (saMessages.size() < 4096) saMessages.push_back(lMessage);
        }
        const uint32_t luCapture = suCapture.load();
        const bool lbKeyboard = luMessage >= WM_KEYFIRST && luMessage <= WM_KEYLAST;
        const bool lbMouse = luMessage >= WM_MOUSEFIRST && luMessage <= WM_MOUSELAST;
        if (lbKeyboard && luMessage == WM_SYSKEYDOWN && luWParam == VK_F4) return false;   // Alt+F4 still closes
        return (lbKeyboard && (luCapture & BRN_CAPTURE_KEYBOARD) != 0) ||
               (lbMouse && (luCapture & BRN_CAPTURE_MOUSE) != 0);
    }

    void RenderFrame(IDirect3DDevice9* lpDevice, HWND__* lhWindow)
    {
        EnsureInitialised();
        if (!sbActive.load() || lpDevice == nullptr) return;
        // The assert screen redraws over an uncleared frame: an overlay would smear across it.
        if (CgsDev::Assert::gAssertManager.HasAssert()) return;

        static std::vector<BrnWindowMessage> saPending;
        {
            std::lock_guard<std::mutex> lLock(sMessageMutex);
            saPending.swap(saMessages);
        }

        static LARGE_INTEGER sFrequency = {};
        static LARGE_INTEGER sLast = {};
        static uint64_t suFrame = 0;
        LARGE_INTEGER lNow;
        QueryPerformanceCounter(&lNow);
        if (sFrequency.QuadPart == 0) QueryPerformanceFrequency(&sFrequency);
        const float lfDelta = sLast.QuadPart == 0 ? 0.0f :
            static_cast<float>(static_cast<double>(lNow.QuadPart - sLast.QuadPart) / static_cast<double>(sFrequency.QuadPart));
        sLast = lNow;

        // The renderer caches device state (ShaderConstantCache, SamplerStateCache): whatever a
        // mod changes must be put back exactly, so every frame is bracketed by a full state block
        // plus the render target / depth binding a state block does not cover.
        IDirect3DSurface9* lpBackBuffer = nullptr;
        if (FAILED(renderengine::PCGetBackBuffer(&lpBackBuffer)) || lpBackBuffer == nullptr) return;
        D3DSURFACE_DESC lDesc = {};
        lpBackBuffer->GetDesc(&lDesc);
        IDirect3DStateBlock9* lpState = nullptr;
        if (FAILED(lpDevice->CreateStateBlock(D3DSBT_ALL, &lpState)) || lpState == nullptr)
        {
            lpBackBuffer->Release();
            return;
        }
        IDirect3DSurface9* lpOldTarget = nullptr;
        IDirect3DSurface9* lpOldDepth = nullptr;
        lpDevice->GetRenderTarget(0, &lpOldTarget);
        lpDevice->GetDepthStencilSurface(&lpOldDepth);
        lpDevice->SetRenderTarget(0, lpBackBuffer);
        lpDevice->SetDepthStencilSurface(nullptr);
        const D3DVIEWPORT9 lViewport = { 0, 0, lDesc.Width, lDesc.Height, 0.0f, 1.0f };
        lpDevice->SetViewport(&lViewport);

        const BrnFrameContext lFrame = { lpDevice, lhWindow, lDesc.Width, lDesc.Height, lfDelta, suFrame++ };
        for (const auto& lpMod : saMods)
        {
            BrnMod& lrMod = *lpMod;
            if (!lrMod.mbEnabled.load()) continue;
            bool lbOk = true;
            if (lrMod.mCallbacks.on_message != nullptr)
                for (const BrnWindowMessage& lrMessage : saPending)
                    if (!(lbOk = CallMessage(lrMod.mCallbacks, &lrMessage))) break;
            if (!lbOk) { Fault(lrMod, "on_message"); continue; }
            if (lrMod.mCallbacks.on_frame != nullptr && !CallFrame(lrMod.mCallbacks, &lFrame))
                Fault(lrMod, "on_frame");
        }
        saPending.clear();

        lpDevice->SetRenderTarget(0, lpOldTarget);
        lpDevice->SetDepthStencilSurface(lpOldDepth);
        lpState->Apply();
        lpState->Release();
        if (lpOldTarget) lpOldTarget->Release();
        if (lpOldDepth) lpOldDepth->Release();
        lpBackBuffer->Release();
    }

    void OnGameTick(BrnGameState::GameStateModule& lrGameState, BrnWorld::WorldModule& lrWorld,
                    BrnGameState::GameStateModuleIO::OutputBuffer* lpGameStateOutput)
    {
        EnsureInitialised();
        if (!sbActive.load()) return;
        TickGameServices(lrGameState, lrWorld, lpGameStateOutput);
        for (const auto& lpMod : saMods)
        {
            BrnMod& lrMod = *lpMod;
            if (lrMod.mbEnabled.load() && lrMod.mCallbacks.on_game_tick != nullptr && !CallTick(lrMod.mCallbacks))
                Fault(lrMod, "on_game_tick");
        }
    }
} }

#endif // BRN_PC_MODS
