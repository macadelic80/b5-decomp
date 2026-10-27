#pragma once

// FLAG PC divergence: the WebAssembly tier of the mod host, shared by its runtime (ModWasm.cpp)
// and its host imports (ModWasmImports.cpp). Not seen by the game or by the mods.
//
// Runtime: wasmtime's C API (tools/build/fetch_wasmtime.py pins the release and says why). One
// engine for the process, one store + instance per mod; nothing is shared between two mods.

#include "pc/mods/ModInternal.h"
#include "pc/mods/ModUi.h"

#include <wasmtime.h>

#include <mutex>
#include <string>

namespace CgsPC { namespace Mods
{
    class WasmRuntime final : public Runtime
    {
    public:
        explicit WasmRuntime(BrnMod& lrMod) : mrMod(lrMod) {}
        ~WasmRuntime() override;

        bool Load(std::string& lrError);

        bool Render(BrnMod& lrMod, const RenderContext& lrContext, std::string& lrError) override;
        bool Tick(BrnMod& lrMod, std::string& lrError) override;
        void Shutdown(BrnMod& lrMod) override;
        bool CanUnload() const override { return true; }

        // ---- for the host imports (they run inside one of this runtime's calls) -----------
        BrnMod&         mrMod;
        Ui::TextureSet  mTextures;
        Ui::DrawList    mDrawList;
        bool            mbInRender = false;   // brn.ui.v1 submit is only valid inside brn_mod_render

    private:
        // Calls lrFunc under the per-call time limit; false + lrError on a trap / error.
        bool Call(const wasmtime_func_t& lrFunc, const char* lpcName, const wasmtime_val_t* lpArgs, size_t luArgs,
                  wasmtime_val_t* lpResults, size_t luResults, uint32_t luTimeLimitMs, std::string& lrError);
        bool DeliverInput(const std::vector<brn_input_event>& lrEvents, std::string& lrError);

        std::mutex           mMutex;              // one call at a time: render thread vs update thread
        bool                 mbStopped = false;   // shut down or trapped: no further call
        wasmtime_module_t*   mpModule = nullptr;
        wasmtime_store_t*    mpStore = nullptr;
        wasmtime_context_t*  mpContext = nullptr;
        wasmtime_instance_t  mInstance = {};
        wasmtime_func_t      mTickFunc = {}, mRenderFunc = {}, mInputFunc = {}, mShutdownFunc = {};
        bool                 mbHasTick = false, mbHasRender = false, mbHasInput = false, mbHasShutdown = false;
        uint32_t             muInputBuffer = 0;   // brn_mod_input_buffer(), an address in the mod's memory
    };

    // The caller's linear memory, for bounds-checked copies in and out (ModWasmImports.cpp).
    class GuestMemory
    {
    public:
        explicit GuestMemory(wasmtime_caller_t* lpCaller);
        bool     Valid() const { return mpBase != nullptr; }
        // Null when [luAddress, luAddress + luLength) is not inside the memory.
        uint8_t* At(uint32_t luAddress, uint64_t luLength) const
        {
            return (mpBase != nullptr && luAddress <= muSize && luLength <= muSize - luAddress) ? mpBase + luAddress : nullptr;
        }
    private:
        uint8_t* mpBase = nullptr;
        size_t   muSize = 0;
    };

    // ModWasmImports.cpp: every host function, under its versioned import module.
    wasmtime_error_t* DefineHostImports(wasmtime_linker_t* lpLinker);
    bool              IsHostImportModule(const char* lpcName, size_t luLength);   // brn.core.v1 has_module

    // The mod a host import runs for.
    WasmRuntime& CallerRuntime(wasmtime_caller_t* lpCaller);

    std::string ErrorText(wasmtime_error_t* lpError);   // takes ownership
} }
