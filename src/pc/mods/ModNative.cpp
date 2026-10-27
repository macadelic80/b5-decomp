// FLAG PC divergence: the native (DLL) tier of the mod host -- the loader 463af61 shipped, kept
// behind config.ini [Mods] AllowNativeMods=1 (off by default) because a DLL is not sandboxed:
// it runs with the game's rights, and can only be stopped after a fault by SEH. ModHost.cpp
// decides whether to call this at all; see pc/mods/api/brn_native_mod_api.h for the contract.

#include "pc/mods/ModHost.h"

#if BRN_PC_MODS

#include "pc/mods/ModInternal.h"

#include <Windows.h>
#include <cstring>

namespace CgsPC { namespace Mods
{
    namespace
    {
        typedef uint32_t (*ApiVersionFn)();
        typedef int (*InitFn)(const BrnHostApi*, BrnModCallbacks*);

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

        // ---- brn.core.v1, native shape ------------------------------------------------------

        void NativeLog(BrnMod* lpSelf, const char* lpcText)
        {
            if (lpcText != nullptr) CoreLog(lpSelf, lpcText, std::strlen(lpcText));
        }
        const char* NativeModDirectory(BrnMod* lpSelf) { return lpSelf ? lpSelf->mDirectory.c_str() : ""; }
        void NativeSetModEnabled(int liIndex, int liEnabled) { CoreSetModEnabled(liIndex, liEnabled); }

        const BrnCoreV1 KCORE_V1 = {
            sizeof(BrnCoreV1), NativeLog, NativeModDirectory, CoreSetInputCapture,
            CoreModCount, CoreGetModInfo, NativeSetModEnabled,
        };

        const void* GetService(const char* lpcName)
        {
            if (lpcName == nullptr) return nullptr;
            if (std::strcmp(lpcName, "brn.core.v1") == 0) return &KCORE_V1;
            return GetNativeService(lpcName);
        }

        class NativeRuntime final : public Runtime
        {
        public:
            NativeRuntime(HMODULE lhModule, const BrnModCallbacks& lrCallbacks)
                : mhModule(lhModule), mCallbacks(lrCallbacks) {}

            bool Render(BrnMod&, const RenderContext& lrContext, std::string& lrError) override
            {
                if (mCallbacks.on_message != nullptr)
                    for (const BrnWindowMessage& lrMessage : lrContext.marMessages)
                        if (!CallMessage(mCallbacks, &lrMessage)) { lrError = "crashed in on_message"; return false; }
                const BrnFrameContext lFrame = { lrContext.mpDevice, lrContext.mhWindow, lrContext.muWidth,
                                                 lrContext.muHeight, lrContext.mfDeltaSeconds, lrContext.muFrameIndex };
                if (mCallbacks.on_frame != nullptr && !CallFrame(mCallbacks, &lFrame))
                {
                    lrError = "crashed in on_frame";
                    return false;
                }
                return true;
            }

            bool Tick(BrnMod&, std::string& lrError) override
            {
                if (mCallbacks.on_game_tick != nullptr && !CallTick(mCallbacks))
                {
                    lrError = "crashed in on_game_tick";
                    return false;
                }
                return true;
            }

            void Shutdown(BrnMod&) override {}   // on_shutdown stays reserved, as before
            bool CanUnload() const override { return false; }   // the DLL stays mapped until exit

        private:
            HMODULE         mhModule;
            BrnModCallbacks mCallbacks;
        };
    }

    std::shared_ptr<Runtime> LoadNativeMod(BrnMod& lrMod, std::string& lrError)
    {
        const std::string lDllPath = lrMod.mDirectory + lrMod.mFile;
        // The mod's folder joins the search path so the DLL finds its own dependencies.
        const HMODULE lhModule = LoadLibraryExA(lDllPath.c_str(), nullptr,
                                                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (lhModule == nullptr)
        {
            lrError = "LoadLibrary failed (error " + std::to_string(GetLastError()) + ")";
            return nullptr;
        }
        const auto lpfnVersion = reinterpret_cast<ApiVersionFn>(GetProcAddress(lhModule, "brn_mod_api_version"));
        const auto lpfnInit = reinterpret_cast<InitFn>(GetProcAddress(lhModule, "brn_mod_init"));
        if (lpfnVersion == nullptr || lpfnInit == nullptr)
        {
            lrError = "missing brn_mod_api_version / brn_mod_init";
            FreeLibrary(lhModule);
            return nullptr;
        }
        const uint32_t luVersion = lpfnVersion();
        if (luVersion != BRN_NATIVE_MOD_API_VERSION)
        {
            lrError = "built for native API " + std::to_string(luVersion) + ", game has " +
                      std::to_string(BRN_NATIVE_MOD_API_VERSION);
            FreeLibrary(lhModule);
            return nullptr;
        }
        const BrnHostApi lHost = { sizeof(BrnHostApi), BRN_NATIVE_MOD_API_VERSION, &lrMod, GetService };
        BrnModCallbacks lCallbacks = {};
        lCallbacks.size = sizeof(BrnModCallbacks);
        int liResult = 0;
        if (!CallInit(lpfnInit, &lHost, &lCallbacks, &liResult))
        {
            lrError = "crashed in brn_mod_init";
            return nullptr;   // the DLL stays mapped: unloading code that just faulted is not safer
        }
        if (liResult == 0)
        {
            lrError = "brn_mod_init refused to load";
            return nullptr;
        }
        return std::make_shared<NativeRuntime>(lhModule, lCallbacks);
    }
} }

#endif // BRN_PC_MODS
