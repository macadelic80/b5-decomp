// FLAG PC divergence: the mod host. See ModHost.h for the contract, pc/mods/api/brn_mod_api.h
// for what a WebAssembly mod sees and pc/mods/README.md for the security model. No console
// counterpart.
//
// Lifetime: the first RenderFrame or OnGameTick reads config.ini [Mods] (Enabled, default 1),
// scans mods\*\mod.ini next to the exe, and loads each mod whose mods\mods.ini entry allows it:
//   Wasm=<file>.wasm  a sandboxed WebAssembly mod (ModWasm.cpp); mods.ini [Enabled] <folder>,
//                     default on. A trap switches it off; switching it back on reloads it.
//   Dll=<file>.dll    a native mod (ModNative.cpp), only with config.ini [Mods] AllowNativeMods=1
//                     AND mods.ini [Native] <folder>=1. Not sandboxed; a fault is caught by SEH,
//                     the DLL stays loaded until exit.
// Both tiers share brn.core.v1's state (capture, the mods list) and the game-state services.

#include "pc/mods/ModHost.h"

#if BRN_PC_MODS

#include "pc/mods/ModInternal.h"
#include "pc/mods/ModInput.h"
#include "pc/mods/ModUi.h"
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
#include <utility>
#include <vector>

namespace CgsPC { namespace Mods
{
    namespace
    {
        std::once_flag                       sInitOnce;
        std::atomic<bool>                    sbActive{false};   // at least one mod is (or can be) loaded
        std::vector<std::unique_ptr<BrnMod>> saMods;            // fixed once sInitOnce has run
        std::string                          sModsDirectory;
        std::atomic<uint32_t>                suCapture{0};
        HostConfig                           sConfig;
        LARGE_INTEGER                        sStart = {};
        LARGE_INTEGER                        sFrequency = {};

        std::mutex                           sMessageMutex;
        std::vector<BrnWindowMessage>        saMessages;

        // brn.core.v1 set_mod_enabled: applied at the start of the next frame, outside every mod
        // call -- a mod may switch itself off, which must not drop the instance it is running in.
        std::mutex                           sToggleMutex;
        std::vector<std::pair<int, bool>>    saToggles;

        // ---- capture ----------------------------------------------------------------------

        void RecomputeCapture()
        {
            uint32_t luCapture = 0;
            for (const auto& lpMod : saMods)
                if (lpMod->mbEnabled.load()) luCapture |= lpMod->muCapture.load();
            suCapture.store(luCapture);
        }

        void Fault(BrnMod& lrMod, const std::string& lrWhat)
        {
            lrMod.mbEnabled.store(false);
            lrMod.muCapture.store(0);
            lrMod.SetError(lrWhat + "; disabled for this run");
            RecomputeCapture();
            const std::string lLine = "[mods] " + lrMod.mName + ": " + lrWhat + " -- disabled\n";
            LogLine(lLine.c_str());
        }

        // ---- mods.ini ------------------------------------------------------------------------
        // [Enabled] holds the WebAssembly mods (default on), [Native] the DLL mods (default off).

        std::string ModsIniPath() { return sModsDirectory + "mods.ini"; }
        const char* ModsIniSection(const BrnMod& lrMod) { return lrMod.mbNative ? "Native" : "Enabled"; }

        bool ReadEnabled(const BrnMod& lrMod)
        {
            return GetPrivateProfileIntA(ModsIniSection(lrMod), lrMod.mFolder.c_str(), lrMod.mbNative ? 0 : 1,
                                         ModsIniPath().c_str()) != 0;
        }

        void WriteEnabled(const BrnMod& lrMod, bool lbEnabled)
        {
            WritePrivateProfileStringA(ModsIniSection(lrMod), lrMod.mFolder.c_str(), lbEnabled ? "1" : "0",
                                       ModsIniPath().c_str());
        }

        // ---- loading ---------------------------------------------------------------------------

        std::string IniString(const std::string& lrPath, const char* lpcSection, const char* lpcKey, const char* lpcDefault)
        {
            char lacValue[260];
            GetPrivateProfileStringA(lpcSection, lpcKey, lpcDefault, lacValue, sizeof(lacValue), lrPath.c_str());
            return lacValue;
        }

        uint32_t IniUInt(const std::string& lrPath, const char* lpcKey, uint32_t luDefault, uint32_t luMin, uint32_t luMax)
        {
            const UINT luValue = GetPrivateProfileIntA("Mods", lpcKey, static_cast<INT>(luDefault), lrPath.c_str());
            return luValue < luMin ? luMin : (luValue > luMax ? luMax : luValue);
        }

        // A mod.ini file name is a plain name inside the mod's folder.
        bool IsPlainFileName(const std::string& lrName)
        {
            return !lrName.empty() && lrName.find_first_of("\\/:") == std::string::npos && lrName != "." && lrName != "..";
        }

        void LoadMod(BrnMod& lrMod)
        {
            std::string lError;
            std::shared_ptr<Runtime> lpRuntime = lrMod.mbNative ? LoadNativeMod(lrMod, lError)
                                                                : LoadWasmMod(lrMod, lError);
            if (lpRuntime == nullptr)
            {
                lrMod.SetError(lError);
                return;
            }
            lrMod.SetError(std::string());
            lrMod.SetRuntime(std::move(lpRuntime));
            lrMod.mbEnabled.store(true);
        }

        void Initialise()
        {
            QueryPerformanceFrequency(&sFrequency);
            QueryPerformanceCounter(&sStart);
            const std::string lExeDirectory = ExeDirectory();
            if (lExeDirectory.empty()) return;
            const std::string lConfig = lExeDirectory + "config.ini";
            if (GetPrivateProfileIntA("Mods", "Enabled", 1, lConfig.c_str()) == 0)
            {
                LogLine("[mods] config.ini [Mods] Enabled=0 -- no mod loaded\n");
                return;
            }
            sConfig.mbAllowNative = GetPrivateProfileIntA("Mods", "AllowNativeMods", 0, lConfig.c_str()) != 0;
            sConfig.muTimeLimitMs = IniUInt(lConfig, "WasmTimeLimitMs", sConfig.muTimeLimitMs, 10, 60000);
            sConfig.muInitTimeLimitMs = IniUInt(lConfig, "WasmInitTimeLimitMs", sConfig.muInitTimeLimitMs, 100, 600000);
            sConfig.muMemoryLimitMB = IniUInt(lConfig, "WasmMemoryLimitMB", sConfig.muMemoryLimitMB, 16, 4095);
            if (sConfig.mbAllowNative)
                LogLine("[mods] WARNING: config.ini [Mods] AllowNativeMods=1 -- native (DLL) mods listed in "
                        "mods\\mods.ini [Native] run UNSANDBOXED, with the game's full access to this PC\n");
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
                lpMod->mName = IniString(lManifest, "Mod", "Name", lFind.cFileName);
                lpMod->mVersion = IniString(lManifest, "Mod", "Version", "");
                const std::string lWasm = IniString(lManifest, "Mod", "Wasm", "");
                const std::string lDll = IniString(lManifest, "Mod", "Dll", "");
                lpMod->mbNative = lWasm.empty() && !lDll.empty();
                lpMod->mFile = lpMod->mbNative ? lDll : lWasm;
                if (lpMod->mFile.empty())
                    lpMod->SetError("mod.ini has no Wasm=");
                else if (!IsPlainFileName(lpMod->mFile))
                    lpMod->SetError("mod.ini names a file outside the mod's folder");
                else if (lpMod->mbNative && !sConfig.mbAllowNative)
                    lpMod->SetError("native (DLL) mod: needs config.ini [Mods] AllowNativeMods=1");
                else if (!ReadEnabled(*lpMod))
                    lpMod->SetError(lpMod->mbNative ? "native mod not enabled in mods.ini [Native]" : "disabled in mods.ini");
                else
                {
                    if (lpMod->mbNative)
                    {
                        const std::string lWarning = "[mods] WARNING: loading NATIVE mod " + lpMod->mName +
                                                     " (" + lpMod->mFile + ") -- not sandboxed\n";
                        LogLine(lWarning.c_str());
                    }
                    LoadMod(*lpMod);
                }

                const std::string lLine = "[mods] " + lpMod->mName + " (" + lpMod->mFolder + ", " +
                                          (lpMod->mbNative ? "native" : "wasm") + "): " +
                                          (lpMod->mbEnabled.load() ? std::string("loaded") : lpMod->GetError()) + "\n";
                LogLine(lLine.c_str());
                // A wasm mod can be switched on later from another mod's list: the host stays up.
                if (lpMod->mbEnabled.load() || !lpMod->mbNative) sbActive.store(true);
                saMods.push_back(std::move(lpMod));
            } while (FindNextFileA(lhFind, &lFind));
            FindClose(lhFind);
        }

        void EnsureInitialised() { std::call_once(sInitOnce, Initialise); }

        void ApplyToggles()
        {
            std::vector<std::pair<int, bool>> laToggles;
            {
                std::lock_guard<std::mutex> lLock(sToggleMutex);
                laToggles.swap(saToggles);
            }
            for (const auto& lrToggle : laToggles)
            {
                BrnMod& lrMod = *saMods[static_cast<size_t>(lrToggle.first)];
                const std::shared_ptr<Runtime> lpRuntime = lrMod.GetRuntime();
                if (lrToggle.second)
                {
                    if (lrMod.mbEnabled.load()) continue;
                    if (lpRuntime != nullptr && !lpRuntime->CanUnload()) { lrMod.mbEnabled.store(true); continue; }
                    if (lrMod.mbNative) continue;   // a native mod that was never loaded waits for the next start
                    lrMod.SetRuntime(nullptr);
                    LoadMod(lrMod);
                    const std::string lLine = "[mods] " + lrMod.mName + ": " +
                                              (lrMod.mbEnabled.load() ? std::string("reloaded") : lrMod.GetError()) + "\n";
                    LogLine(lLine.c_str());
                }
                else
                {
                    if (!lrMod.mbEnabled.load()) continue;
                    lrMod.mbEnabled.store(false);
                    lrMod.muCapture.store(0);
                    if (lpRuntime != nullptr)
                    {
                        lpRuntime->Shutdown(lrMod);
                        if (lpRuntime->CanUnload()) lrMod.SetRuntime(nullptr);   // freed once the update thread lets go
                    }
                    lrMod.SetError("switched off");
                }
            }
            if (!laToggles.empty()) RecomputeCapture();
        }

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

        float WindowDpiScale(HWND lhWindow)
        {
            // GetDpiForWindow is Windows 10 1607+; the game still starts on older systems.
            typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
            static const auto lpfnGetDpi = reinterpret_cast<GetDpiForWindowFn>(
                GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow"));
            const UINT luDpi = lpfnGetDpi != nullptr ? lpfnGetDpi(lhWindow) : 0;
            return luDpi == 0 ? 1.0f : static_cast<float>(luDpi) / 96.0f;
        }
    }

    // ---- brn.core.v1 (both tiers) ------------------------------------------------------------

    const HostConfig& GetHostConfig() { return sConfig; }

    void LogLine(const char* lpcText) { CgsDev::Log::WriteToLog(lpcText); }

    void CoreLog(BrnMod* lpSelf, const char* lpcText, size_t luLength)
    {
        if (lpcText == nullptr) return;
        std::string lLine = "[mod:";
        lLine += lpSelf ? lpSelf->mFolder : std::string("?");
        lLine += "] ";
        lLine.append(lpcText, luLength < 4000 ? luLength : 4000);
        if (lLine.empty() || lLine.back() != '\n') lLine += '\n';
        LogLine(lLine.c_str());
    }

    void CoreSetInputCapture(BrnMod* lpSelf, uint32_t luFlags)
    {
        if (lpSelf == nullptr) return;
        lpSelf->muCapture.store(lpSelf->mbEnabled.load() ? (luFlags & (BRN_CAPTURE_KEYBOARD | BRN_CAPTURE_MOUSE | BRN_CAPTURE_GAMEPAD)) : 0u);
        RecomputeCapture();
    }

    int CoreModCount() { return static_cast<int>(saMods.size()); }

    int CoreGetModInfo(int liIndex, BrnModInfo* lpOut)
    {
        if (lpOut == nullptr || liIndex < 0 || liIndex >= CoreModCount()) return 0;
        const BrnMod& lrMod = *saMods[static_cast<size_t>(liIndex)];
        *lpOut = BrnModInfo{};
        std::snprintf(lpOut->name, sizeof(lpOut->name), "%s", lrMod.mName.c_str());
        std::snprintf(lpOut->folder, sizeof(lpOut->folder), "%s", lrMod.mFolder.c_str());
        std::snprintf(lpOut->version, sizeof(lpOut->version), "%s", lrMod.mVersion.c_str());
        std::snprintf(lpOut->error, sizeof(lpOut->error), "%s", lrMod.GetError().c_str());
        lpOut->loaded = lrMod.GetRuntime() != nullptr;
        lpOut->enabled = lrMod.mbEnabled.load();
        return 1;
    }

    void CoreSetModEnabled(int liIndex, int liEnabled)
    {
        if (liIndex < 0 || liIndex >= CoreModCount()) return;
        BrnMod& lrMod = *saMods[static_cast<size_t>(liIndex)];
        WriteEnabled(lrMod, liEnabled != 0);
        if (lrMod.mbNative && !sConfig.mbAllowNative) return;   // saved; the tier itself is off
        std::lock_guard<std::mutex> lLock(sToggleMutex);
        saToggles.emplace_back(liIndex, liEnabled != 0);
    }

    const void* GetNativeService(const char* lpcName) { return FindGameService(lpcName); }

    double HostTimeSeconds()
    {
        LARGE_INTEGER lNow;
        QueryPerformanceCounter(&lNow);
        return sFrequency.QuadPart == 0 ? 0.0 :
            static_cast<double>(lNow.QuadPart - sStart.QuadPart) / static_cast<double>(sFrequency.QuadPart);
    }

    std::string ExeDirectory()
    {
        char lacPath[MAX_PATH];
        const DWORD luLength = GetModuleFileNameA(nullptr, lacPath, MAX_PATH);
        if (luLength == 0 || luLength >= MAX_PATH) return std::string();
        std::string lPath(lacPath, luLength);
        const size_t luSlash = lPath.find_last_of("\\/");
        return luSlash == std::string::npos ? std::string() : lPath.substr(0, luSlash + 1);
    }

    // ---- the game's hooks ----------------------------------------------------------------------

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
        ApplyToggles();

        static std::vector<BrnWindowMessage> saPending;
        {
            std::lock_guard<std::mutex> lLock(sMessageMutex);
            saPending.swap(saMessages);
        }

        static LARGE_INTEGER sLast = {};
        static uint64_t suFrame = 0;
        LARGE_INTEGER lNow;
        QueryPerformanceCounter(&lNow);
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

        // The wasm mods' input: window client pixels -> back buffer pixels, and a resize event
        // whenever the back buffer changes size.
        static InputTranslator sTranslator;
        static std::vector<brn_input_event> saEvents;
        static uint32_t suLastWidth = 0, suLastHeight = 0;
        saEvents.clear();
        float lfScaleX = 1.0f, lfScaleY = 1.0f;
        RECT lClient = {};
        if (GetClientRect(lhWindow, &lClient) && lClient.right > lClient.left && lClient.bottom > lClient.top)
        {
            lfScaleX = static_cast<float>(lDesc.Width) / static_cast<float>(lClient.right - lClient.left);
            lfScaleY = static_cast<float>(lDesc.Height) / static_cast<float>(lClient.bottom - lClient.top);
        }
        if (lDesc.Width != suLastWidth || lDesc.Height != suLastHeight)
        {
            brn_input_event lResize = {};
            lResize.type = BRN_INPUT_RESIZE;
            lResize.x = static_cast<float>(lDesc.Width);
            lResize.y = static_cast<float>(lDesc.Height);
            saEvents.push_back(lResize);
            suLastWidth = lDesc.Width;
            suLastHeight = lDesc.Height;
        }
        for (const BrnWindowMessage& lrMessage : saPending) sTranslator.Translate(lrMessage, lfScaleX, lfScaleY, saEvents);

        static std::unique_ptr<Ui::Backend> spUi;
        static IDirect3DDevice9* spUiDevice = nullptr;
        if (spUiDevice != lpDevice) { spUi = Ui::CreateD3D9Backend(lpDevice); spUiDevice = lpDevice; }
        spUi->BeginFrame(lDesc.Width, lDesc.Height);

        const RenderContext lContext = { lpDevice, lhWindow, lDesc.Width, lDesc.Height, WindowDpiScale(lhWindow),
                                         lfDelta, suFrame++, saPending, saEvents, spUi.get() };
        for (const auto& lpMod : saMods)
        {
            BrnMod& lrMod = *lpMod;
            if (!lrMod.mbEnabled.load()) continue;
            const std::shared_ptr<Runtime> lpRuntime = lrMod.GetRuntime();
            std::string lError;
            if (lpRuntime != nullptr && !lpRuntime->Render(lrMod, lContext, lError)) Fault(lrMod, lError);
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
            if (!lrMod.mbEnabled.load()) continue;
            const std::shared_ptr<Runtime> lpRuntime = lrMod.GetRuntime();
            std::string lError;
            if (lpRuntime != nullptr && !lpRuntime->Tick(lrMod, lError)) Fault(lrMod, lError);
        }
    }
} }

#endif // BRN_PC_MODS
