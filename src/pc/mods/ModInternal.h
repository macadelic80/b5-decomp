#pragma once

// FLAG PC divergence: shared between the mod host (ModHost.cpp), its two runtimes (ModNative.cpp,
// ModWasm.cpp), the UI renderer (ModUi*.cpp) and the game-state services (ModServices.cpp,
// ModDebugVars.cpp). Not seen by the game or by the mods.

#include "pc/mods/api/brn_native_mod_api.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct IDirect3DDevice9;
struct HWND__;
namespace BrnGameState { class GameStateModule; namespace GameStateModuleIO { struct OutputBuffer; } }
namespace BrnWorld { class WorldModule; }

namespace CgsPC { namespace Mods
{
    namespace Ui { class Backend; }

    // What a runtime gets for one frame, on the render thread, inside the scene with the back
    // buffer bound and the device state saved (ModHost.cpp puts it back afterwards).
    struct RenderContext
    {
        IDirect3DDevice9*                    mpDevice;
        HWND__*                              mhWindow;
        uint32_t                             muWidth, muHeight;   // back buffer pixels
        float                                mfDpiScale;          // window DPI / 96
        float                                mfDeltaSeconds;
        uint64_t                             muFrameIndex;
        const std::vector<BrnWindowMessage>& marMessages;         // raw, for native mods
        const std::vector<brn_input_event>&  marEvents;           // portable, for wasm mods
        Ui::Backend*                         mpUi;                // null: no UI this frame
    };

    // One loaded mod's code: a DLL (ModNative.cpp) or a WebAssembly instance (ModWasm.cpp).
    // Render/Tick return false when the mod faulted; the host then switches it off.
    class Runtime
    {
    public:
        virtual ~Runtime() = default;
        virtual bool Render(BrnMod& lrMod, const RenderContext& lrContext, std::string& lrError) = 0;
        virtual bool Tick(BrnMod& lrMod, std::string& lrError) = 0;
        // Switched off from the mods list (not after a fault): let the mod save its state.
        virtual void Shutdown(BrnMod& lrMod) = 0;
        // A runtime that can be dropped while the game runs (wasm); a DLL stays mapped.
        virtual bool CanUnload() const = 0;
    };

    // config.ini [Mods], read once by ModHost.cpp.
    struct HostConfig
    {
        bool     mbAllowNative = false;      // AllowNativeMods
        uint32_t muTimeLimitMs = 200;        // WasmTimeLimitMs: one render / input / tick call
        uint32_t muInitTimeLimitMs = 5000;   // WasmInitTimeLimitMs: brn_mod_init
        uint32_t muMemoryLimitMB = 256;      // WasmMemoryLimitMB: a mod's linear memory
    };
    const HostConfig& GetHostConfig();

    // ModNative.cpp / ModWasm.cpp: load lrMod's code (lrMod.mFile in lrMod.mDirectory).
    // Null with lrError set when it does not load.
    std::shared_ptr<Runtime> LoadNativeMod(BrnMod& lrMod, std::string& lrError);
    std::shared_ptr<Runtime> LoadWasmMod(BrnMod& lrMod, std::string& lrError);

    // "brn.core.v1" behind both tiers (ModHost.cpp).
    void        CoreLog(BrnMod* lpSelf, const char* lpcText, size_t luLength);
    void        CoreSetInputCapture(BrnMod* lpSelf, uint32_t luFlags);
    int         CoreModCount();
    int         CoreGetModInfo(int liIndex, BrnModInfo* lpOut);
    void        CoreSetModEnabled(int liIndex, int liEnabled);
    const void* GetNativeService(const char* lpcName);   // BrnHostApi::get_service
    double      HostTimeSeconds();
    std::string ExeDirectory();                           // "<exe dir>\"

    // The service tables of ModServices.cpp ("brn.player.v1", ...); null for an unknown name.
    const void* FindGameService(const char* lpcName);

    // Update thread: apply the queued commands, then refresh every snapshot.
    void TickGameServices(BrnGameState::GameStateModule& lrGameState, BrnWorld::WorldModule& lrWorld,
                          BrnGameState::GameStateModuleIO::OutputBuffer* lpGameStateOutput);

    // ModDebugVars.cpp: "brn.debugvars.v1" and its tick (the game's CgsDev debug menu tree).
    const void* FindDebugVarsService(const char* lpcName);
    void TickDebugVars();

    void LogLine(const char* lpcText);
} }

// The host's view of one mod (opaque to the mods: BrnMod* in brn_native_mod_api.h).
struct BrnMod
{
    std::string             mName;
    std::string             mFolder;       // the folder name under mods\ (the mods.ini key)
    std::string             mVersion;
    std::string             mDirectory;    // "<exe dir>\mods\<folder>\"
    std::string             mFile;         // mod.ini Wasm= or Dll=
    bool                    mbNative = false;
    std::atomic<bool>       mbEnabled{false};
    std::atomic<uint32_t>   muCapture{0};

    // Swapped on the render thread when a wasm mod is switched on/off; read from both threads.
    std::shared_ptr<CgsPC::Mods::Runtime> GetRuntime() const
    {
        std::lock_guard<std::mutex> lLock(mRuntimeMutex);
        return mpRuntime;
    }
    void SetRuntime(std::shared_ptr<CgsPC::Mods::Runtime> lpRuntime)
    {
        std::lock_guard<std::mutex> lLock(mRuntimeMutex);
        mpRuntime.swap(lpRuntime);
    }
    std::string GetError() const
    {
        std::lock_guard<std::mutex> lLock(mRuntimeMutex);
        return mError;
    }
    void SetError(std::string lError)
    {
        std::lock_guard<std::mutex> lLock(mRuntimeMutex);
        mError.swap(lError);
    }

private:
    mutable std::mutex                    mRuntimeMutex;
    std::shared_ptr<CgsPC::Mods::Runtime> mpRuntime;
    std::string                           mError;
};
