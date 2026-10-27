// FLAG PC divergence: the WebAssembly tier of the mod host -- one wasmtime store + instance per
// mod in mods/<name>/<file>.wasm. See pc/mods/api/brn_mod_api.h for the mod's side and
// pc/mods/README.md for the security model.
//
// What keeps a mod in its box:
//   * the instance sees its own linear memory and the host imports of ModWasmImports.cpp only:
//     no WASI file system / network / environment (a minimal WASI stub set lets C/C++ libc start);
//   * every pointer it passes is bounds-checked against that memory (GuestMemory);
//   * epoch interruption: each call has a wall-clock budget ([Mods] WasmTimeLimitMs, init
//     WasmInitTimeLimitMs); a mod stuck in a loop traps instead of freezing the frame;
//   * a memory limit ([Mods] WasmMemoryLimitMB) and one instance / memory per store;
//   * signals-based traps are OFF: out-of-bounds accesses are caught by explicit bounds checks,
//     so a wasm fault never raises a Windows exception for the game's vectored crash probe
//     (CgsCrashHandlerPC.cpp) to see. A trap unwinds back into Call() and switches the mod off.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "pc/mods/ModHost.h"

#if BRN_PC_MODS

#include "pc/mods/ModWasm.h"

#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>

namespace CgsPC { namespace Mods
{
    namespace
    {
        wasm_engine_t*  spEngine = nullptr;
        std::once_flag  sEngineOnce;
        std::string     sEngineError;

        const uint64_t KU_MAX_MODULE_BYTES = 256ull * 1024ull * 1024ull;

        // The epoch advances once per millisecond of wall time, whatever Sleep's granularity.
        void EpochThread()
        {
            LARGE_INTEGER lFrequency, lStart, lNow;
            QueryPerformanceFrequency(&lFrequency);
            QueryPerformanceCounter(&lStart);
            uint64_t luTicks = 0;
            for (;;)
            {
                Sleep(1);
                QueryPerformanceCounter(&lNow);
                const uint64_t luElapsedMs = static_cast<uint64_t>((lNow.QuadPart - lStart.QuadPart) * 1000 / lFrequency.QuadPart);
                for (; luTicks < luElapsedMs; ++luTicks) wasmtime_engine_increment_epoch(spEngine);
            }
        }

        void CreateEngine()
        {
            wasm_config_t* lpConfig = wasm_config_new();
            if (lpConfig == nullptr) { sEngineError = "wasm_config_new failed"; return; }
            wasmtime_config_epoch_interruption_set(lpConfig, true);
            wasmtime_config_signals_based_traps_set(lpConfig, false);
            wasmtime_config_wasm_threads_set(lpConfig, false);
            wasmtime_config_shared_memory_set(lpConfig, false);
            wasmtime_config_cranelift_opt_level_set(lpConfig, WASMTIME_OPT_LEVEL_SPEED);
            spEngine = wasm_engine_new_with_config(lpConfig);   // takes the config
            if (spEngine == nullptr) { sEngineError = "wasm_engine_new failed"; return; }
            std::thread(EpochThread).detach();
        }

        std::string TrapText(wasm_trap_t* lpTrap, const char* lpcFunction, uint32_t luTimeLimitMs)
        {
            std::string lText = std::string("trap in ") + lpcFunction + ": ";
            wasmtime_trap_code_t luCode = 0;
            if (wasmtime_trap_code(lpTrap, &luCode) && luCode == WASMTIME_TRAP_CODE_INTERRUPT)
                lText += "ran longer than " + std::to_string(luTimeLimitMs) + " ms ([Mods] WasmTimeLimitMs) -- ";
            wasm_message_t lMessage;
            wasm_trap_message(lpTrap, &lMessage);
            std::string lBody(lMessage.data, lMessage.size);
            wasm_byte_vec_delete(&lMessage);
            wasm_trap_delete(lpTrap);
            while (!lBody.empty() && (lBody.back() == '\0' || lBody.back() == '\n')) lBody.pop_back();
            for (char& lrChar : lBody) if (lrChar == '\n') lrChar = '|';   // the backtrace, on one log line
            return lText + lBody;
        }

        bool ReadFile(const std::string& lrPath, std::vector<uint8_t>& lrOut)
        {
            std::ifstream lFile(lrPath, std::ios::binary | std::ios::ate);
            if (!lFile) return false;
            const std::streamoff liSize = lFile.tellg();
            if (liSize <= 0 || static_cast<uint64_t>(liSize) > KU_MAX_MODULE_BYTES) return false;
            lrOut.resize(static_cast<size_t>(liSize));
            lFile.seekg(0);
            return static_cast<bool>(lFile.read(reinterpret_cast<char*>(lrOut.data()), liSize));
        }

        // "ii>i": params then results, i = i32, I = i64, f = f32, F = f64.
        bool HasSignature(wasmtime_context_t* lpContext, const wasmtime_func_t& lrFunc, const char* lpcSignature)
        {
            wasm_functype_t* lpType = wasmtime_func_type(lpContext, &lrFunc);
            const wasm_valtype_vec_t* lpParams = wasm_functype_params(lpType);
            const wasm_valtype_vec_t* lpResults = wasm_functype_results(lpType);
            auto lKind = [](char lcCode) -> wasm_valkind_t {
                switch (lcCode) { case 'i': return WASM_I32; case 'I': return WASM_I64; case 'f': return WASM_F32; default: return WASM_F64; }
            };
            const char* lpcArrow = std::strchr(lpcSignature, '>');
            const size_t luParams = static_cast<size_t>(lpcArrow - lpcSignature);
            const size_t luResults = std::strlen(lpcArrow + 1);
            bool lbMatch = lpParams->size == luParams && lpResults->size == luResults;
            for (size_t lu = 0; lbMatch && lu < luParams; ++lu)
                lbMatch = wasm_valtype_kind(lpParams->data[lu]) == lKind(lpcSignature[lu]);
            for (size_t lu = 0; lbMatch && lu < luResults; ++lu)
                lbMatch = wasm_valtype_kind(lpResults->data[lu]) == lKind(lpcArrow[1 + lu]);
            wasm_functype_delete(lpType);
            return lbMatch;
        }

        wasmtime_val_t ValI32(int32_t liValue) { wasmtime_val_t lValue = {}; lValue.kind = WASMTIME_I32; lValue.of.i32 = liValue; return lValue; }
        wasmtime_val_t ValI64(int64_t liValue) { wasmtime_val_t lValue = {}; lValue.kind = WASMTIME_I64; lValue.of.i64 = liValue; return lValue; }
        wasmtime_val_t ValF32(float lfValue)   { wasmtime_val_t lValue = {}; lValue.kind = WASMTIME_F32; lValue.of.f32 = lfValue; return lValue; }

        // Imports this build does not define: the mod still loads (they trap if called).
        std::string MissingImports(const wasmtime_module_t* lpModule)
        {
            wasm_importtype_vec_t laImports;
            wasmtime_module_imports(lpModule, &laImports);
            std::string lMissing;
            for (size_t lu = 0; lu < laImports.size; ++lu)
            {
                const wasm_name_t* lpModuleName = wasm_importtype_module(laImports.data[lu]);
                const wasm_name_t* lpName = wasm_importtype_name(laImports.data[lu]);
                const bool lbWasi = lpModuleName->size == 22 && std::memcmp(lpModuleName->data, "wasi_snapshot_preview1", 22) == 0;
                if (lbWasi || IsHostImportModule(lpModuleName->data, lpModuleName->size)) continue;
                if (!lMissing.empty()) lMissing += ", ";
                lMissing.append(lpModuleName->data, lpModuleName->size);
                lMissing += '.';
                lMissing.append(lpName->data, lpName->size);
            }
            wasm_importtype_vec_delete(&laImports);
            return lMissing;
        }
    }

    std::string ErrorText(wasmtime_error_t* lpError)
    {
        wasm_name_t lMessage;
        wasmtime_error_message(lpError, &lMessage);
        std::string lText(lMessage.data, lMessage.size);
        wasm_byte_vec_delete(&lMessage);
        wasmtime_error_delete(lpError);
        for (char& lrChar : lText) if (lrChar == '\n') lrChar = '|';
        return lText;
    }

    WasmRuntime& CallerRuntime(wasmtime_caller_t* lpCaller)
    {
        return *static_cast<WasmRuntime*>(wasmtime_context_get_data(wasmtime_caller_context(lpCaller)));
    }

    GuestMemory::GuestMemory(wasmtime_caller_t* lpCaller)
    {
        wasmtime_extern_t lMemory;
        if (!wasmtime_caller_export_get(lpCaller, "memory", 6, &lMemory) || lMemory.kind != WASMTIME_EXTERN_MEMORY)
            return;
        wasmtime_context_t* lpContext = wasmtime_caller_context(lpCaller);
        mpBase = wasmtime_memory_data(lpContext, &lMemory.of.memory);
        muSize = wasmtime_memory_data_size(lpContext, &lMemory.of.memory);
    }

    WasmRuntime::~WasmRuntime()
    {
        if (mpStore) wasmtime_store_delete(mpStore);   // the instance and its memory go with it
        if (mpModule) wasmtime_module_delete(mpModule);
    }

    bool WasmRuntime::Call(const wasmtime_func_t& lrFunc, const char* lpcName, const wasmtime_val_t* lpArgs, size_t luArgs,
                           wasmtime_val_t* lpResults, size_t luResults, uint32_t luTimeLimitMs, std::string& lrError)
    {
        wasmtime_context_set_epoch_deadline(mpContext, static_cast<uint64_t>(luTimeLimitMs) + 1);
        wasm_trap_t* lpTrap = nullptr;
        wasmtime_error_t* lpError = wasmtime_func_call(mpContext, &lrFunc, lpArgs, luArgs, lpResults, luResults, &lpTrap);
        if (lpError != nullptr) { lrError = std::string("error calling ") + lpcName + ": " + ErrorText(lpError); return false; }
        if (lpTrap != nullptr) { lrError = TrapText(lpTrap, lpcName, luTimeLimitMs); return false; }
        return true;
    }

    bool WasmRuntime::Load(std::string& lrError)
    {
        std::call_once(sEngineOnce, CreateEngine);
        if (spEngine == nullptr) { lrError = "no wasm engine: " + sEngineError; return false; }
        const HostConfig& lrConfig = GetHostConfig();

        std::vector<uint8_t> laBytes;
        if (!ReadFile(mrMod.mDirectory + mrMod.mFile, laBytes)) { lrError = "cannot read " + mrMod.mFile; return false; }
        if (wasmtime_error_t* lpError = wasmtime_module_new(spEngine, laBytes.data(), laBytes.size(), &mpModule))
        {
            lrError = "not a valid WebAssembly module: " + ErrorText(lpError);
            return false;
        }
        const std::string lMissing = MissingImports(mpModule);
        if (!lMissing.empty())
            CoreLog(&mrMod, ("imports this build lacks (they trap if called): " + lMissing).c_str(), 4000);

        mpStore = wasmtime_store_new(spEngine, this, nullptr);
        mpContext = wasmtime_store_context(mpStore);
        wasmtime_store_limiter(mpStore, static_cast<int64_t>(lrConfig.muMemoryLimitMB) * 1024 * 1024,
                               1000000 /* table elements */, 1 /* instances */, 4 /* tables */, 1 /* memories */);

        wasmtime_linker_t* lpLinker = wasmtime_linker_new(spEngine);
        wasmtime_error_t* lpError = DefineHostImports(lpLinker);
        if (lpError == nullptr) lpError = wasmtime_linker_define_unknown_imports_as_traps(lpLinker, mpModule);
        wasm_trap_t* lpTrap = nullptr;
        if (lpError == nullptr)
        {
            wasmtime_context_set_epoch_deadline(mpContext, static_cast<uint64_t>(lrConfig.muInitTimeLimitMs) + 1);
            lpError = wasmtime_linker_instantiate(lpLinker, mpContext, mpModule, &mInstance, &lpTrap);
        }
        wasmtime_linker_delete(lpLinker);
        if (lpError != nullptr) { lrError = "cannot instantiate: " + ErrorText(lpError); return false; }
        if (lpTrap != nullptr) { lrError = TrapText(lpTrap, "the start function", lrConfig.muInitTimeLimitMs); return false; }

        wasmtime_extern_t lExport;
        if (!wasmtime_instance_export_get(mpContext, &mInstance, "memory", 6, &lExport) || lExport.kind != WASMTIME_EXTERN_MEMORY)
        {
            lrError = "does not export its memory";
            return false;
        }
        const wasmtime_memory_t lMemory = lExport.of.memory;

        // Fetches an exported function; false only when it exists with the wrong signature.
        auto lGet = [&](const char* lpcName, const char* lpcSignature, wasmtime_func_t& lrFunc, bool& lrbFound) -> bool {
            wasmtime_extern_t lItem;
            lrbFound = wasmtime_instance_export_get(mpContext, &mInstance, lpcName, std::strlen(lpcName), &lItem) &&
                       lItem.kind == WASMTIME_EXTERN_FUNC;
            if (!lrbFound) return true;
            lrFunc = lItem.of.func;
            if (HasSignature(mpContext, lrFunc, lpcSignature)) return true;
            lrError = std::string("export ") + lpcName + " has the wrong signature (see brn_mod_api.h)";
            return false;
        };
        wasmtime_func_t lVersionFunc, lInitFunc, lInitializeFunc, lInputBufferFunc;
        bool lbHasVersion, lbHasInit, lbHasInitialize, lbHasInputBuffer;
        if (!lGet("brn_mod_api_version", ">i", lVersionFunc, lbHasVersion) ||
            !lGet("brn_mod_init", ">i", lInitFunc, lbHasInit) ||
            !lGet("_initialize", ">", lInitializeFunc, lbHasInitialize) ||
            !lGet("brn_mod_tick", ">", mTickFunc, mbHasTick) ||
            !lGet("brn_mod_render", "iiffI>", mRenderFunc, mbHasRender) ||
            !lGet("brn_mod_input_buffer", ">i", lInputBufferFunc, lbHasInputBuffer) ||
            !lGet("brn_mod_on_input", "i>", mInputFunc, mbHasInput) ||
            !lGet("brn_mod_shutdown", ">", mShutdownFunc, mbHasShutdown))
            return false;
        if (!lbHasVersion || !lbHasInit) { lrError = "missing export brn_mod_api_version / brn_mod_init"; return false; }
        if (mbHasInput != lbHasInputBuffer) { lrError = "brn_mod_on_input and brn_mod_input_buffer go together"; return false; }

        // A WASI "reactor" (C/C++ with wasi-sdk) runs its libc and static constructors here.
        if (lbHasInitialize && !Call(lInitializeFunc, "_initialize", nullptr, 0, nullptr, 0, lrConfig.muInitTimeLimitMs, lrError))
            return false;
        wasmtime_val_t lResult;
        if (!Call(lVersionFunc, "brn_mod_api_version", nullptr, 0, &lResult, 1, lrConfig.muInitTimeLimitMs, lrError)) return false;
        if (static_cast<uint32_t>(lResult.of.i32) != BRN_MOD_API_VERSION)
        {
            lrError = "built for API " + std::to_string(static_cast<uint32_t>(lResult.of.i32)) + ", game has " +
                      std::to_string(BRN_MOD_API_VERSION);
            return false;
        }
        if (mbHasInput)
        {
            if (!Call(lInputBufferFunc, "brn_mod_input_buffer", nullptr, 0, &lResult, 1, lrConfig.muInitTimeLimitMs, lrError)) return false;
            muInputBuffer = static_cast<uint32_t>(lResult.of.i32);
            const size_t luSize = wasmtime_memory_data_size(mpContext, &lMemory);
            if (muInputBuffer > luSize || luSize - muInputBuffer < sizeof(brn_input_event) * BRN_INPUT_BUFFER_CAPACITY)
            {
                lrError = "brn_mod_input_buffer points outside the mod's memory";
                return false;
            }
        }
        if (!Call(lInitFunc, "brn_mod_init", nullptr, 0, &lResult, 1, lrConfig.muInitTimeLimitMs, lrError)) return false;
        if (lResult.of.i32 == 0) { lrError = "brn_mod_init refused to load"; return false; }
        return true;
    }

    bool WasmRuntime::DeliverInput(const std::vector<brn_input_event>& lrEvents, std::string& lrError)
    {
        wasmtime_extern_t lExport;
        if (!wasmtime_instance_export_get(mpContext, &mInstance, "memory", 6, &lExport)) { lrError = "lost its memory"; return false; }
        for (size_t luFirst = 0; luFirst < lrEvents.size(); luFirst += BRN_INPUT_BUFFER_CAPACITY)
        {
            const size_t luCount = std::min<size_t>(BRN_INPUT_BUFFER_CAPACITY, lrEvents.size() - luFirst);
            // Re-read every batch: the mod's previous call may have grown (moved) its memory.
            uint8_t* lpBase = wasmtime_memory_data(mpContext, &lExport.of.memory);
            const size_t luSize = wasmtime_memory_data_size(mpContext, &lExport.of.memory);
            if (muInputBuffer > luSize || luSize - muInputBuffer < luCount * sizeof(brn_input_event))
            {
                lrError = "brn_mod_input_buffer points outside the mod's memory";
                return false;
            }
            std::memcpy(lpBase + muInputBuffer, &lrEvents[luFirst], luCount * sizeof(brn_input_event));
            const wasmtime_val_t lCount = ValI32(static_cast<int32_t>(luCount));
            if (!Call(mInputFunc, "brn_mod_on_input", &lCount, 1, nullptr, 0, GetHostConfig().muTimeLimitMs, lrError))
                return false;
        }
        return true;
    }

    bool WasmRuntime::Render(BrnMod&, const RenderContext& lrContext, std::string& lrError)
    {
        std::lock_guard<std::mutex> lLock(mMutex);
        if (mbStopped) return true;
        bool lbOk = true;
        if (mbHasInput && !lrContext.marEvents.empty()) lbOk = DeliverInput(lrContext.marEvents, lrError);
        if (lbOk && mbHasRender)
        {
            mDrawList.Clear();
            const wasmtime_val_t laArgs[5] = {
                ValI32(static_cast<int32_t>(lrContext.muWidth)), ValI32(static_cast<int32_t>(lrContext.muHeight)),
                ValF32(lrContext.mfDpiScale), ValF32(lrContext.mfDeltaSeconds), ValI64(static_cast<int64_t>(lrContext.muFrameIndex)),
            };
            mbInRender = true;
            lbOk = Call(mRenderFunc, "brn_mod_render", laArgs, 5, nullptr, 0, GetHostConfig().muTimeLimitMs, lrError);
            mbInRender = false;
            if (lbOk && lrContext.mpUi != nullptr && !mDrawList.maCommands.empty())
                lrContext.mpUi->Draw(mDrawList, mTextures);
        }
        if (!lbOk) mbStopped = true;
        return lbOk;
    }

    bool WasmRuntime::Tick(BrnMod&, std::string& lrError)
    {
        std::lock_guard<std::mutex> lLock(mMutex);
        if (mbStopped || !mbHasTick) return true;
        if (Call(mTickFunc, "brn_mod_tick", nullptr, 0, nullptr, 0, GetHostConfig().muTimeLimitMs, lrError)) return true;
        mbStopped = true;
        return false;
    }

    void WasmRuntime::Shutdown(BrnMod& lrMod)
    {
        std::lock_guard<std::mutex> lLock(mMutex);
        if (mbStopped) return;
        mbStopped = true;
        std::string lError;
        if (mbHasShutdown && !Call(mShutdownFunc, "brn_mod_shutdown", nullptr, 0, nullptr, 0, GetHostConfig().muTimeLimitMs, lError))
            CoreLog(&lrMod, lError.c_str(), lError.size());
    }

    std::shared_ptr<Runtime> LoadWasmMod(BrnMod& lrMod, std::string& lrError)
    {
        auto lpRuntime = std::make_shared<WasmRuntime>(lrMod);
        if (!lpRuntime->Load(lrError)) return nullptr;
        return lpRuntime;
    }
} }

#endif // BRN_PC_MODS
