// FLAG PC divergence: the host functions a WebAssembly mod imports, one versioned import module
// per service (pc/mods/api/brn_mod_api.h). Each is a thin, bounds-checked shim over the same
// service tables the native tier uses (ModServices.cpp, ModDebugVars.cpp): data is COPIED into the
// mod's memory, never pointed at; commands still go through the game's own channels there.
//
// Rules every function here follows:
//   * a pointer + length from the mod is resolved with GuestMemory::At, which refuses anything
//     outside the mod's linear memory -- the call then traps (OutOfBounds), naming the import;
//   * a length has a hard cap, so one call can neither allocate nor copy without bound;
//   * nothing returns or stores a host address.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "pc/mods/ModHost.h"

#if BRN_PC_MODS

#include "pc/mods/ModWasm.h"

#include <Windows.h>
#include <bcrypt.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")   // BCryptGenRandom, the WASI random_get stub

namespace CgsPC { namespace Mods
{
    namespace
    {
        // The structs cross as raw bytes: their sizes must be the ones a wasm32 mod compiles
        // (checked against wasi-sdk clang --target=wasm32-wasip1).
        static_assert(sizeof(brn_vertex) == 20 && sizeof(brn_input_event) == 28, "brn_mod_api.h layout");
        static_assert(sizeof(BrnPlayerState) == 200 && sizeof(BrnDebugVar) == 368 && sizeof(BrnModInfo) == 296 &&
                      sizeof(BrnVehicleInfo) == 176 && sizeof(BrnLocation) == 156 && sizeof(BrnRaceCarInfo) == 80 &&
                      sizeof(BrnTrafficSettings) == 36, "brn_mod_api.h layout");

        const uint32_t KU_MAX_TEXT = 4096;                       // a log line, a path, a value
        const uint32_t KU_MAX_FILE = 16u * 1024u * 1024u;        // brn.storage.v1, one file

        struct HostImport;

        // One call of a host import: typed access to its arguments and results.
        struct HostCall
        {
            const HostImport*     mpImport;
            wasmtime_caller_t*    mpCaller;
            const wasmtime_val_t* mpArgs;
            wasmtime_val_t*       mpResults;

            int32_t  I32(size_t luIndex) const { return mpArgs[luIndex].of.i32; }
            uint32_t U32(size_t luIndex) const { return static_cast<uint32_t>(mpArgs[luIndex].of.i32); }
            int64_t  I64(size_t luIndex) const { return mpArgs[luIndex].of.i64; }
            float    F32(size_t luIndex) const { return mpArgs[luIndex].of.f32; }
            void     ReturnI32(int32_t liValue) const { mpResults[0].of.i32 = liValue; }
            void     ReturnF64(double lfValue) const { mpResults[0].of.f64 = lfValue; }
            WasmRuntime& Runtime() const { return CallerRuntime(mpCaller); }
        };

        typedef wasm_trap_t* (*HostFn)(HostCall& lrCall);

        struct HostImport
        {
            const char* mpcModule;
            const char* mpcName;
            const char* mpcSignature;   // "ii>i": params then results; i i32, I i64, f f32, F f64
            HostFn      mpfnBody;
        };

        wasm_trap_t* Trap(const HostCall& lrCall, const char* lpcWhat)
        {
            const std::string lText = std::string(lrCall.mpImport->mpcModule) + "." + lrCall.mpImport->mpcName + ": " + lpcWhat;
            return wasmtime_trap_new(lText.c_str(), lText.size());
        }
        wasm_trap_t* OutOfBounds(const HostCall& lrCall) { return Trap(lrCall, "pointer/length outside the mod's memory"); }

        // Copy a host struct out to the mod's memory at luAddress.
        template <typename T>
        wasm_trap_t* CopyOut(const HostCall& lrCall, uint32_t luAddress, const T& lrValue)
        {
            uint8_t* lpTarget = GuestMemory(lrCall.mpCaller).At(luAddress, sizeof(T));
            if (lpTarget == nullptr) return OutOfBounds(lrCall);
            std::memcpy(lpTarget, &lrValue, sizeof(T));
            return nullptr;
        }

        // The mod's (pointer, length) text; false = out of bounds or longer than KU_MAX_TEXT.
        bool ReadText(const HostCall& lrCall, uint32_t luAddress, uint32_t luLength, std::string& lrOut)
        {
            if (luLength > KU_MAX_TEXT) return false;
            const uint8_t* lpSource = GuestMemory(lrCall.mpCaller).At(luAddress, luLength);
            if (lpSource == nullptr) return false;
            lrOut.assign(reinterpret_cast<const char*>(lpSource), luLength);
            return true;
        }

        template <typename T> const T* Service(const char* lpcName)
        {
            return static_cast<const T*>(FindGameService(lpcName));
        }
        const BrnPlayerV1*    Player()    { static const auto* lp = Service<BrnPlayerV1>("brn.player.v1"); return lp; }
        const BrnVehiclesV1*  Vehicles()  { static const auto* lp = Service<BrnVehiclesV1>("brn.vehicles.v1"); return lp; }
        const BrnLocationsV1* Locations() { static const auto* lp = Service<BrnLocationsV1>("brn.locations.v1"); return lp; }
        const BrnRaceCarsV1*  RaceCars()  { static const auto* lp = Service<BrnRaceCarsV1>("brn.racecars.v1"); return lp; }
        const BrnTrafficV1*   Traffic()   { static const auto* lp = Service<BrnTrafficV1>("brn.traffic.v1"); return lp; }
        const BrnDebugVarsV1* DebugVars() { static const auto* lp = Service<BrnDebugVarsV1>("brn.debugvars.v1"); return lp; }

        // ---- brn.core.v1 ----------------------------------------------------------------------

        wasm_trap_t* CoreLogFn(HostCall& lrCall)
        {
            std::string lText;
            if (!ReadText(lrCall, lrCall.U32(0), lrCall.U32(1), lText)) return OutOfBounds(lrCall);
            CoreLog(&lrCall.Runtime().mrMod, lText.data(), lText.size());
            return nullptr;
        }
        wasm_trap_t* CoreHasModuleFn(HostCall& lrCall)
        {
            std::string lName;
            if (!ReadText(lrCall, lrCall.U32(0), lrCall.U32(1), lName)) return OutOfBounds(lrCall);
            lrCall.ReturnI32(IsHostImportModule(lName.data(), lName.size()) ? 1 : 0);
            return nullptr;
        }
        wasm_trap_t* CoreTimeFn(HostCall& lrCall) { lrCall.ReturnF64(HostTimeSeconds()); return nullptr; }
        wasm_trap_t* CoreSetInputCaptureFn(HostCall& lrCall)
        {
            CoreSetInputCapture(&lrCall.Runtime().mrMod, lrCall.U32(0));
            return nullptr;
        }
        wasm_trap_t* CoreModCountFn(HostCall& lrCall) { lrCall.ReturnI32(CoreModCount()); return nullptr; }
        wasm_trap_t* CoreGetModInfoFn(HostCall& lrCall)
        {
            BrnModInfo lInfo = {};
            const int liFound = CoreGetModInfo(lrCall.I32(0), &lInfo);
            lrCall.ReturnI32(liFound);
            return liFound ? CopyOut(lrCall, lrCall.U32(1), lInfo) : nullptr;
        }
        wasm_trap_t* CoreSetModEnabledFn(HostCall& lrCall) { CoreSetModEnabled(lrCall.I32(0), lrCall.I32(1)); return nullptr; }

        // ---- brn.player.v1 --------------------------------------------------------------------

        BrnVec3 Vec3(const HostCall& lrCall, size_t luFirst)
        {
            return BrnVec3{ lrCall.F32(luFirst), lrCall.F32(luFirst + 1), lrCall.F32(luFirst + 2) };
        }
        wasm_trap_t* PlayerGetStateFn(HostCall& lrCall)
        {
            BrnPlayerState lState = {};
            lState.size = sizeof(BrnPlayerState);
            const int liValid = Player()->get_state(&lState);
            lrCall.ReturnI32(liValid);
            return CopyOut(lrCall, lrCall.U32(0), lState);
        }
        wasm_trap_t* PlayerTeleportFn(HostCall& lrCall) { Player()->teleport(Vec3(lrCall, 0), Vec3(lrCall, 3)); return nullptr; }
        wasm_trap_t* PlayerResetOnTrackFn(HostCall& lrCall) { Player()->reset_on_track(lrCall.F32(0)); return nullptr; }
        wasm_trap_t* PlayerPlaceOnTrackFn(HostCall& lrCall)
        {
            Player()->place_on_track(Vec3(lrCall, 0), Vec3(lrCall, 3), lrCall.F32(6));
            return nullptr;
        }
        wasm_trap_t* PlayerSetInfiniteBoostFn(HostCall& lrCall) { Player()->set_infinite_boost(lrCall.I32(0)); return nullptr; }
        wasm_trap_t* PlayerSetInvulnerableFn(HostCall& lrCall) { Player()->set_invulnerable(lrCall.I32(0)); return nullptr; }
        wasm_trap_t* PlayerChangeCarFn(HostCall& lrCall)
        {
            lrCall.ReturnI32(Player()->change_car(static_cast<uint64_t>(lrCall.I64(0)), static_cast<uint64_t>(lrCall.I64(1))));
            return nullptr;
        }

        // ---- brn.vehicles.v1 / brn.locations.v1 / brn.racecars.v1 / brn.traffic.v1 -------------

        wasm_trap_t* VehiclesCountFn(HostCall& lrCall) { lrCall.ReturnI32(Vehicles()->count()); return nullptr; }
        wasm_trap_t* VehiclesGetFn(HostCall& lrCall)
        {
            BrnVehicleInfo lInfo = {};
            const int liFound = Vehicles()->get(lrCall.I32(0), &lInfo);
            lrCall.ReturnI32(liFound);
            return liFound ? CopyOut(lrCall, lrCall.U32(1), lInfo) : nullptr;
        }
        wasm_trap_t* LocationsCountFn(HostCall& lrCall) { lrCall.ReturnI32(Locations()->count()); return nullptr; }
        wasm_trap_t* LocationsGetFn(HostCall& lrCall)
        {
            BrnLocation lInfo = {};
            const int liFound = Locations()->get(lrCall.I32(0), &lInfo);
            lrCall.ReturnI32(liFound);
            return liFound ? CopyOut(lrCall, lrCall.U32(1), lInfo) : nullptr;
        }
        wasm_trap_t* RaceCarsCountFn(HostCall& lrCall) { lrCall.ReturnI32(RaceCars()->count()); return nullptr; }
        wasm_trap_t* RaceCarsGetFn(HostCall& lrCall)
        {
            BrnRaceCarInfo lInfo = {};
            const int liActive = RaceCars()->get(lrCall.I32(0), &lInfo);
            lrCall.ReturnI32(liActive);
            return liActive ? CopyOut(lrCall, lrCall.U32(1), lInfo) : nullptr;
        }
        wasm_trap_t* RaceCarsRemoveAllRivalsFn(HostCall&) { RaceCars()->remove_all_rivals(); return nullptr; }
        wasm_trap_t* TrafficGetFn(HostCall& lrCall)
        {
            BrnTrafficSettings lSettings = {};
            lSettings.size = sizeof(BrnTrafficSettings);
            lrCall.ReturnI32(Traffic()->get(&lSettings));
            return CopyOut(lrCall, lrCall.U32(0), lSettings);
        }
        wasm_trap_t* TrafficSetFn(HostCall& lrCall)
        {
            const uint8_t* lpSource = GuestMemory(lrCall.mpCaller).At(lrCall.U32(0), sizeof(BrnTrafficSettings));
            if (lpSource == nullptr) return OutOfBounds(lrCall);
            BrnTrafficSettings lSettings;
            std::memcpy(&lSettings, lpSource, sizeof(lSettings));
            lSettings.size = sizeof(BrnTrafficSettings);
            Traffic()->set(&lSettings);
            return nullptr;
        }
        wasm_trap_t* TrafficKillNearPlayerFn(HostCall&) { Traffic()->kill_near_player(); return nullptr; }

        // ---- brn.debugvars.v1 -----------------------------------------------------------------

        wasm_trap_t* DebugVarsRefreshFn(HostCall& lrCall) { lrCall.ReturnI32(static_cast<int32_t>(DebugVars()->refresh())); return nullptr; }
        wasm_trap_t* DebugVarsCountFn(HostCall& lrCall) { lrCall.ReturnI32(DebugVars()->count()); return nullptr; }
        wasm_trap_t* DebugVarsGetFn(HostCall& lrCall)
        {
            BrnDebugVar lVar = {};
            const int liFound = DebugVars()->get(lrCall.I32(0), &lVar);
            lrCall.ReturnI32(liFound);
            return liFound ? CopyOut(lrCall, lrCall.U32(1), lVar) : nullptr;
        }
        wasm_trap_t* DebugVarsOptionCountFn(HostCall& lrCall) { lrCall.ReturnI32(DebugVars()->option_count(lrCall.I32(0))); return nullptr; }
        wasm_trap_t* DebugVarsGetOptionFn(HostCall& lrCall)
        {
            const uint32_t luNameSize = lrCall.U32(3);
            if (luNameSize == 0 || luNameSize > KU_MAX_TEXT) return Trap(lrCall, "name_size must be 1..4096");
            const GuestMemory lMemory(lrCall.mpCaller);
            uint8_t* lpName = lMemory.At(lrCall.U32(2), luNameSize);
            uint8_t* lpValue = lMemory.At(lrCall.U32(4), sizeof(int32_t));
            if (lpName == nullptr || lpValue == nullptr) return OutOfBounds(lrCall);
            std::vector<char> laName(luNameSize, '\0');
            int liValue = 0;
            const int liFound = DebugVars()->get_option(lrCall.I32(0), lrCall.I32(1), laName.data(),
                                                        static_cast<int>(luNameSize), &liValue);
            laName.back() = '\0';
            lrCall.ReturnI32(liFound);
            if (liFound)
            {
                std::memcpy(lpName, laName.data(), luNameSize);
                std::memcpy(lpValue, &liValue, sizeof(liValue));
            }
            return nullptr;
        }
        wasm_trap_t* DebugVarsSetFn(HostCall& lrCall)
        {
            std::string lPath, lValue;
            if (!ReadText(lrCall, lrCall.U32(0), lrCall.U32(1), lPath) || !ReadText(lrCall, lrCall.U32(2), lrCall.U32(3), lValue))
                return OutOfBounds(lrCall);
            DebugVars()->set(lPath.c_str(), lValue.c_str());
            return nullptr;
        }
        wasm_trap_t* DebugVarsCallFn(HostCall& lrCall)
        {
            std::string lPath;
            if (!ReadText(lrCall, lrCall.U32(0), lrCall.U32(1), lPath)) return OutOfBounds(lrCall);
            DebugVars()->call(lPath.c_str());
            return nullptr;
        }

        // ---- brn.debugini.v1: config.ini [Debug] only (pc/debug/DebugIni.h reads it at start) ----

        bool IsIniSafe(const std::string& lrText, bool lbKey)
        {
            if (lrText.find_first_of(std::string("\r\n\0", 3)) != std::string::npos) return false;
            if (!lbKey) return true;
            return !lrText.empty() && lrText.find('=') == std::string::npos && lrText[0] != '[' && lrText[0] != ';';
        }
        wasm_trap_t* DebugIniSetFn(HostCall& lrCall)
        {
            std::string lPath, lValue;
            if (!ReadText(lrCall, lrCall.U32(0), lrCall.U32(1), lPath) || !ReadText(lrCall, lrCall.U32(2), lrCall.U32(3), lValue))
                return OutOfBounds(lrCall);
            if (!IsIniSafe(lPath, true) || !IsIniSafe(lValue, false)) { lrCall.ReturnI32(0); return nullptr; }
            const std::string lConfig = ExeDirectory() + "config.ini";
            const BOOL lbWritten = WritePrivateProfileStringA("Debug", lPath.c_str(), lValue.empty() ? nullptr : lValue.c_str(),
                                                              lConfig.c_str());
            lrCall.ReturnI32(lbWritten ? 1 : 0);
            return nullptr;
        }

        // ---- brn.storage.v1: mods\<folder>\data\<file>, nothing else ----------------------------

        bool StoragePath(const HostCall& lrCall, uint32_t luAddress, uint32_t luLength, std::string& lrPath, bool& lrbBounds)
        {
            std::string lName;
            lrbBounds = ReadText(lrCall, luAddress, luLength, lName);
            if (!lrbBounds || lName.empty() || lName.size() > 64 || lName[0] == '.') return false;
            for (const char lcChar : lName)
            {
                const bool lbAllowed = (lcChar >= 'a' && lcChar <= 'z') || (lcChar >= 'A' && lcChar <= 'Z') ||
                                       (lcChar >= '0' && lcChar <= '9') || lcChar == '.' || lcChar == '_' || lcChar == '-';
                if (!lbAllowed) return false;
            }
            lrPath = lrCall.Runtime().mrMod.mDirectory + "data\\" + lName;
            return true;
        }
        wasm_trap_t* StorageReadFn(HostCall& lrCall)
        {
            std::string lPath;
            bool lbBounds;
            lrCall.ReturnI32(-1);
            if (!StoragePath(lrCall, lrCall.U32(0), lrCall.U32(1), lPath, lbBounds)) return lbBounds ? nullptr : OutOfBounds(lrCall);
            uint8_t* lpOut = GuestMemory(lrCall.mpCaller).At(lrCall.U32(2), lrCall.U32(3));
            if (lpOut == nullptr) return OutOfBounds(lrCall);
            const HANDLE lhFile = CreateFileA(lPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                              FILE_ATTRIBUTE_NORMAL, nullptr);
            if (lhFile == INVALID_HANDLE_VALUE) return nullptr;
            LARGE_INTEGER lSize = {};
            if (GetFileSizeEx(lhFile, &lSize) && lSize.QuadPart <= KU_MAX_FILE)
            {
                const DWORD luWanted = static_cast<DWORD>(std::min<LONGLONG>(lSize.QuadPart, lrCall.U32(3)));
                DWORD luRead = 0;
                if (luWanted == 0 || ReadFile(lhFile, lpOut, luWanted, &luRead, nullptr))
                    lrCall.ReturnI32(static_cast<int32_t>(lSize.QuadPart));
            }
            CloseHandle(lhFile);
            return nullptr;
        }
        wasm_trap_t* StorageWriteFn(HostCall& lrCall)
        {
            std::string lPath;
            bool lbBounds;
            lrCall.ReturnI32(0);
            if (!StoragePath(lrCall, lrCall.U32(0), lrCall.U32(1), lPath, lbBounds)) return lbBounds ? nullptr : OutOfBounds(lrCall);
            if (lrCall.U32(3) > KU_MAX_FILE) return nullptr;
            const uint8_t* lpData = GuestMemory(lrCall.mpCaller).At(lrCall.U32(2), lrCall.U32(3));
            if (lpData == nullptr) return OutOfBounds(lrCall);
            CreateDirectoryA((lrCall.Runtime().mrMod.mDirectory + "data").c_str(), nullptr);
            // Written beside, then swapped in: a crash mid-write never leaves half a file.
            const std::string lTemp = lPath + "~";
            const HANDLE lhFile = CreateFileA(lTemp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (lhFile == INVALID_HANDLE_VALUE) return nullptr;
            DWORD luWritten = 0;
            const bool lbOk = (lrCall.U32(3) == 0 || WriteFile(lhFile, lpData, lrCall.U32(3), &luWritten, nullptr)) &&
                              luWritten == lrCall.U32(3);
            CloseHandle(lhFile);
            if (lbOk && MoveFileExA(lTemp.c_str(), lPath.c_str(), MOVEFILE_REPLACE_EXISTING)) lrCall.ReturnI32(1);
            else DeleteFileA(lTemp.c_str());
            return nullptr;
        }
        wasm_trap_t* StorageRemoveFn(HostCall& lrCall)
        {
            std::string lPath;
            bool lbBounds;
            lrCall.ReturnI32(0);
            if (!StoragePath(lrCall, lrCall.U32(0), lrCall.U32(1), lPath, lbBounds)) return lbBounds ? nullptr : OutOfBounds(lrCall);
            lrCall.ReturnI32(DeleteFileA(lPath.c_str()) ? 1 : 0);
            return nullptr;
        }

        // ---- brn.ui.v1 --------------------------------------------------------------------------

        wasm_trap_t* UiTextureCreateFn(HostCall& lrCall)
        {
            const int32_t liWidth = lrCall.I32(1), liHeight = lrCall.I32(2);
            lrCall.ReturnI32(0);
            if (liWidth <= 0 || liHeight <= 0 || liWidth > BRN_UI_TEXTURE_LIMIT || liHeight > BRN_UI_TEXTURE_LIMIT) return nullptr;
            const uint8_t* lpRgba = GuestMemory(lrCall.mpCaller).At(lrCall.U32(0), static_cast<uint64_t>(liWidth) * liHeight * 4u);
            if (lpRgba == nullptr) return OutOfBounds(lrCall);
            lrCall.ReturnI32(static_cast<int32_t>(lrCall.Runtime().mTextures.Create(lpRgba, liWidth, liHeight)));
            return nullptr;
        }
        wasm_trap_t* UiTextureUpdateFn(HostCall& lrCall)
        {
            const int32_t liX = lrCall.I32(1), liY = lrCall.I32(2), liWidth = lrCall.I32(3), liHeight = lrCall.I32(4);
            if (liX < 0 || liY < 0 || liWidth <= 0 || liHeight <= 0 || liWidth > BRN_UI_TEXTURE_LIMIT || liHeight > BRN_UI_TEXTURE_LIMIT)
                return Trap(lrCall, "bad rectangle");
            const uint8_t* lpRgba = GuestMemory(lrCall.mpCaller).At(lrCall.U32(5), static_cast<uint64_t>(liWidth) * liHeight * 4u);
            if (lpRgba == nullptr) return OutOfBounds(lrCall);
            if (!lrCall.Runtime().mTextures.Update(lrCall.U32(0), liX, liY, liWidth, liHeight, lpRgba))
                return Trap(lrCall, "unknown texture, or the rectangle leaves it");
            return nullptr;
        }
        wasm_trap_t* UiTextureDestroyFn(HostCall& lrCall) { lrCall.Runtime().mTextures.Destroy(lrCall.U32(0)); return nullptr; }

        template <typename Index>
        wasm_trap_t* UiSubmit(HostCall& lrCall)
        {
            WasmRuntime& lrRuntime = lrCall.Runtime();
            if (!lrRuntime.mbInRender) return Trap(lrCall, "only valid inside brn_mod_render");
            const uint32_t luVertices = lrCall.U32(1), luIndices = lrCall.U32(3);
            if (luIndices == 0) return nullptr;
            if (luIndices % 3 != 0) return Trap(lrCall, "index count is not a multiple of 3");
            Ui::DrawList& lrList = lrRuntime.mDrawList;
            if (luVertices > Ui::DrawList::KU_MAX_VERTICES - lrList.maVertices.size() ||
                luIndices > Ui::DrawList::KU_MAX_INDICES - lrList.maIndices.size())
                return Trap(lrCall, "draw list too large for one frame");
            const GuestMemory lMemory(lrCall.mpCaller);
            const uint8_t* lpVertices = lMemory.At(lrCall.U32(0), static_cast<uint64_t>(luVertices) * sizeof(brn_vertex));
            const uint8_t* lpIndices = lMemory.At(lrCall.U32(2), static_cast<uint64_t>(luIndices) * sizeof(Index));
            if (lpVertices == nullptr || lpIndices == nullptr) return OutOfBounds(lrCall);
            const uint32_t luTexture = lrCall.U32(4);
            if (luTexture != 0 && lrRuntime.mTextures.Find(luTexture) == nullptr) return Trap(lrCall, "unknown texture");

            const size_t luFirstVertex = lrList.maVertices.size();
            const size_t luFirstIndex = lrList.maIndices.size();
            lrList.maVertices.resize(luFirstVertex + luVertices);
            std::memcpy(&lrList.maVertices[luFirstVertex], lpVertices, static_cast<size_t>(luVertices) * sizeof(brn_vertex));
            lrList.maIndices.resize(luFirstIndex + luIndices);
            for (uint32_t lu = 0; lu < luIndices; ++lu)
            {
                Index lIndex;
                std::memcpy(&lIndex, lpIndices + lu * sizeof(Index), sizeof(Index));
                if (lIndex >= luVertices)   // the GPU would read outside the mod's vertices
                {
                    lrList.maVertices.resize(luFirstVertex);
                    lrList.maIndices.resize(luFirstIndex);
                    return Trap(lrCall, "index past the vertex count");
                }
                lrList.maIndices[luFirstIndex + lu] = lIndex;
            }
            Ui::DrawCommand lCommand;
            lCommand.muTexture = luTexture;
            lCommand.mClip = brn_rect{ lrCall.F32(5), lrCall.F32(6), lrCall.F32(7), lrCall.F32(8) };
            lCommand.muVertexOffset = static_cast<uint32_t>(luFirstVertex);
            lCommand.muVertexCount = luVertices;
            lCommand.muIndexOffset = static_cast<uint32_t>(luFirstIndex);
            lCommand.muIndexCount = luIndices;
            lCommand.muFlags = lrCall.U32(9);
            lrList.maCommands.push_back(lCommand);
            return nullptr;
        }
        wasm_trap_t* UiSubmit16Fn(HostCall& lrCall) { return UiSubmit<uint16_t>(lrCall); }
        wasm_trap_t* UiSubmit32Fn(HostCall& lrCall) { return UiSubmit<uint32_t>(lrCall); }

        wasm_trap_t* UiSetClipboardTextFn(HostCall& lrCall)
        {
            if (lrCall.U32(1) > 1024u * 1024u) return nullptr;
            const uint8_t* lpText = GuestMemory(lrCall.mpCaller).At(lrCall.U32(0), lrCall.U32(1));
            if (lpText == nullptr) return OutOfBounds(lrCall);
            const int liWide = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(lpText), static_cast<int>(lrCall.U32(1)), nullptr, 0);
            if (!OpenClipboard(nullptr)) return nullptr;
            EmptyClipboard();
            if (const HGLOBAL lhMemory = GlobalAlloc(GMEM_MOVEABLE, (static_cast<size_t>(liWide) + 1) * sizeof(wchar_t)))
            {
                wchar_t* lpTarget = static_cast<wchar_t*>(GlobalLock(lhMemory));
                if (lpTarget != nullptr)
                {
                    MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(lpText), static_cast<int>(lrCall.U32(1)), lpTarget, liWide);
                    lpTarget[liWide] = L'\0';
                    GlobalUnlock(lhMemory);
                    if (SetClipboardData(CF_UNICODETEXT, lhMemory) == nullptr) GlobalFree(lhMemory);
                }
                else GlobalFree(lhMemory);
            }
            CloseClipboard();
            return nullptr;
        }

        // ---- wasi_snapshot_preview1: just enough for a C/C++ libc to start and to print --------
        // No file system (no preopened directory: fd_prestat_get says EBADF), no sockets, no
        // environment, no arguments. stdout / stderr lines go to BrnGame.log.

        const int32_t KI_WASI_SUCCESS = 0, KI_WASI_EBADF = 8, KI_WASI_EINVAL = 28, KI_WASI_ENOSYS = 52;

        wasm_trap_t* WasiFdWriteFn(HostCall& lrCall)
        {
            const int32_t liFd = lrCall.I32(0);
            const GuestMemory lMemory(lrCall.mpCaller);
            const uint32_t luCount = lrCall.U32(2);
            if (luCount > 1024) return Trap(lrCall, "too many iovecs");
            const uint8_t* lpIovs = lMemory.At(lrCall.U32(1), static_cast<uint64_t>(luCount) * 8u);
            uint8_t* lpWritten = lMemory.At(lrCall.U32(3), 4);
            if (lpIovs == nullptr || lpWritten == nullptr) return OutOfBounds(lrCall);
            if (liFd != 1 && liFd != 2) { lrCall.ReturnI32(KI_WASI_EBADF); return nullptr; }
            std::string lText;
            uint32_t luTotal = 0;
            for (uint32_t lu = 0; lu < luCount; ++lu)
            {
                uint32_t luBuffer, luLength;
                std::memcpy(&luBuffer, lpIovs + lu * 8u, 4);
                std::memcpy(&luLength, lpIovs + lu * 8u + 4u, 4);
                const uint8_t* lpBuffer = lMemory.At(luBuffer, luLength);
                if (lpBuffer == nullptr) return OutOfBounds(lrCall);
                if (lText.size() < KU_MAX_TEXT) lText.append(reinterpret_cast<const char*>(lpBuffer), std::min<size_t>(luLength, KU_MAX_TEXT));
                luTotal += luLength;
            }
            while (!lText.empty() && lText.back() == '\n') lText.pop_back();
            if (!lText.empty()) CoreLog(&lrCall.Runtime().mrMod, lText.data(), lText.size());
            std::memcpy(lpWritten, &luTotal, 4);
            lrCall.ReturnI32(KI_WASI_SUCCESS);
            return nullptr;
        }
        wasm_trap_t* WasiFdFdstatGetFn(HostCall& lrCall)
        {
            const int32_t liFd = lrCall.I32(0);
            uint8_t* lpOut = GuestMemory(lrCall.mpCaller).At(lrCall.U32(1), 24);
            if (lpOut == nullptr) return OutOfBounds(lrCall);
            if (liFd < 0 || liFd > 2) { lrCall.ReturnI32(KI_WASI_EBADF); return nullptr; }
            std::memset(lpOut, 0, 24);
            lpOut[0] = 2;   // __WASI_FILETYPE_CHARACTER_DEVICE
            const uint64_t luRights = liFd == 0 ? 0x2u /* fd_read */ : 0x40u /* fd_write */;
            std::memcpy(lpOut + 8, &luRights, 8);
            lrCall.ReturnI32(KI_WASI_SUCCESS);
            return nullptr;
        }
        wasm_trap_t* WasiBadFdFn(HostCall& lrCall) { lrCall.ReturnI32(KI_WASI_EBADF); return nullptr; }
        wasm_trap_t* WasiNoSysFn(HostCall& lrCall) { lrCall.ReturnI32(KI_WASI_ENOSYS); return nullptr; }
        wasm_trap_t* WasiSizesGetFn(HostCall& lrCall)   // environ_sizes_get / args_sizes_get: none
        {
            const GuestMemory lMemory(lrCall.mpCaller);
            uint8_t* lpCount = lMemory.At(lrCall.U32(0), 4);
            uint8_t* lpBytes = lMemory.At(lrCall.U32(1), 4);
            if (lpCount == nullptr || lpBytes == nullptr) return OutOfBounds(lrCall);
            std::memset(lpCount, 0, 4);
            std::memset(lpBytes, 0, 4);
            lrCall.ReturnI32(KI_WASI_SUCCESS);
            return nullptr;
        }
        wasm_trap_t* WasiEmptyGetFn(HostCall& lrCall) { lrCall.ReturnI32(KI_WASI_SUCCESS); return nullptr; }
        wasm_trap_t* WasiClockTimeGetFn(HostCall& lrCall)
        {
            uint8_t* lpOut = GuestMemory(lrCall.mpCaller).At(lrCall.U32(2), 8);
            if (lpOut == nullptr) return OutOfBounds(lrCall);
            uint64_t luNanoseconds;
            if (lrCall.I32(0) == 0)   // realtime
            {
                FILETIME lNow;
                GetSystemTimeAsFileTime(&lNow);
                const uint64_t lu100ns = (static_cast<uint64_t>(lNow.dwHighDateTime) << 32) | lNow.dwLowDateTime;
                luNanoseconds = (lu100ns - 116444736000000000ull) * 100u;   // 1601 -> 1970
            }
            else
                luNanoseconds = static_cast<uint64_t>(HostTimeSeconds() * 1e9);
            std::memcpy(lpOut, &luNanoseconds, 8);
            lrCall.ReturnI32(KI_WASI_SUCCESS);
            return nullptr;
        }
        wasm_trap_t* WasiClockResGetFn(HostCall& lrCall)
        {
            uint8_t* lpOut = GuestMemory(lrCall.mpCaller).At(lrCall.U32(1), 8);
            if (lpOut == nullptr) return OutOfBounds(lrCall);
            const uint64_t luResolution = 1000;   // 1 us
            std::memcpy(lpOut, &luResolution, 8);
            lrCall.ReturnI32(KI_WASI_SUCCESS);
            return nullptr;
        }
        wasm_trap_t* WasiRandomGetFn(HostCall& lrCall)
        {
            if (lrCall.U32(1) > 1024u * 1024u) { lrCall.ReturnI32(KI_WASI_EINVAL); return nullptr; }
            uint8_t* lpOut = GuestMemory(lrCall.mpCaller).At(lrCall.U32(0), lrCall.U32(1));
            if (lpOut == nullptr) return OutOfBounds(lrCall);
            const bool lbOk = BCryptGenRandom(nullptr, lpOut, lrCall.U32(1), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
            lrCall.ReturnI32(lbOk ? KI_WASI_SUCCESS : KI_WASI_ENOSYS);
            return nullptr;
        }
        wasm_trap_t* WasiProcExitFn(HostCall& lrCall)
        {
            const std::string lText = "the mod called exit(" + std::to_string(lrCall.I32(0)) + ")";
            return wasmtime_trap_new(lText.c_str(), lText.size());
        }
        wasm_trap_t* WasiSchedYieldFn(HostCall& lrCall) { lrCall.ReturnI32(KI_WASI_SUCCESS); return nullptr; }

        const HostImport KA_IMPORTS[] = {
            { "brn.core.v1", "log",               "ii>",  CoreLogFn },
            { "brn.core.v1", "has_module",        "ii>i", CoreHasModuleFn },
            { "brn.core.v1", "time",              ">F",   CoreTimeFn },
            { "brn.core.v1", "set_input_capture", "i>",   CoreSetInputCaptureFn },
            { "brn.core.v1", "mod_count",         ">i",   CoreModCountFn },
            { "brn.core.v1", "get_mod_info",      "ii>i", CoreGetModInfoFn },
            { "brn.core.v1", "set_mod_enabled",   "ii>",  CoreSetModEnabledFn },

            { "brn.player.v1", "get_state",          "i>i",      PlayerGetStateFn },
            { "brn.player.v1", "teleport",           "ffffff>",  PlayerTeleportFn },
            { "brn.player.v1", "reset_on_track",     "f>",       PlayerResetOnTrackFn },
            { "brn.player.v1", "place_on_track",     "fffffff>", PlayerPlaceOnTrackFn },
            { "brn.player.v1", "set_infinite_boost", "i>",       PlayerSetInfiniteBoostFn },
            { "brn.player.v1", "set_invulnerable",   "i>",       PlayerSetInvulnerableFn },
            { "brn.player.v1", "change_car",         "II>i",     PlayerChangeCarFn },

            { "brn.vehicles.v1",  "count", ">i",   VehiclesCountFn },
            { "brn.vehicles.v1",  "get",   "ii>i", VehiclesGetFn },
            { "brn.locations.v1", "count", ">i",   LocationsCountFn },
            { "brn.locations.v1", "get",   "ii>i", LocationsGetFn },
            { "brn.racecars.v1",  "count", ">i",   RaceCarsCountFn },
            { "brn.racecars.v1",  "get",   "ii>i", RaceCarsGetFn },
            { "brn.racecars.v1",  "remove_all_rivals", ">", RaceCarsRemoveAllRivalsFn },
            { "brn.traffic.v1",   "get",   "i>i",  TrafficGetFn },
            { "brn.traffic.v1",   "set",   "i>",   TrafficSetFn },
            { "brn.traffic.v1",   "kill_near_player", ">", TrafficKillNearPlayerFn },

            { "brn.debugvars.v1", "refresh",      ">i",      DebugVarsRefreshFn },
            { "brn.debugvars.v1", "count",        ">i",      DebugVarsCountFn },
            { "brn.debugvars.v1", "get",          "ii>i",    DebugVarsGetFn },
            { "brn.debugvars.v1", "option_count", "i>i",     DebugVarsOptionCountFn },
            { "brn.debugvars.v1", "get_option",   "iiiii>i", DebugVarsGetOptionFn },
            { "brn.debugvars.v1", "set",          "iiii>",   DebugVarsSetFn },
            { "brn.debugvars.v1", "call",         "ii>",     DebugVarsCallFn },

            { "brn.debugini.v1", "set", "iiii>i", DebugIniSetFn },

            { "brn.storage.v1", "read",   "iiii>i", StorageReadFn },
            { "brn.storage.v1", "write",  "iiii>i", StorageWriteFn },
            { "brn.storage.v1", "remove", "ii>i",   StorageRemoveFn },

            { "brn.ui.v1", "texture_create",     "iii>i",      UiTextureCreateFn },
            { "brn.ui.v1", "texture_update",     "iiiiii>",     UiTextureUpdateFn },
            { "brn.ui.v1", "texture_destroy",    "i>",          UiTextureDestroyFn },
            { "brn.ui.v1", "submit",             "iiiiiffffi>", UiSubmit16Fn },
            { "brn.ui.v1", "submit32",           "iiiiiffffi>", UiSubmit32Fn },
            { "brn.ui.v1", "set_clipboard_text", "ii>",         UiSetClipboardTextFn },

            { "wasi_snapshot_preview1", "fd_write",           "iiii>i", WasiFdWriteFn },
            { "wasi_snapshot_preview1", "fd_fdstat_get",      "ii>i",   WasiFdFdstatGetFn },
            { "wasi_snapshot_preview1", "fd_close",           "i>i",    WasiBadFdFn },
            { "wasi_snapshot_preview1", "fd_seek",            "iIii>i", WasiBadFdFn },
            { "wasi_snapshot_preview1", "fd_read",            "iiii>i", WasiBadFdFn },
            { "wasi_snapshot_preview1", "fd_prestat_get",     "ii>i",   WasiBadFdFn },
            { "wasi_snapshot_preview1", "fd_prestat_dir_name", "iii>i", WasiBadFdFn },
            { "wasi_snapshot_preview1", "path_open",          "iiiiiIIii>i", WasiNoSysFn },
            { "wasi_snapshot_preview1", "environ_sizes_get",  "ii>i",   WasiSizesGetFn },
            { "wasi_snapshot_preview1", "environ_get",        "ii>i",   WasiEmptyGetFn },
            { "wasi_snapshot_preview1", "args_sizes_get",     "ii>i",   WasiSizesGetFn },
            { "wasi_snapshot_preview1", "args_get",           "ii>i",   WasiEmptyGetFn },
            { "wasi_snapshot_preview1", "clock_time_get",     "iIi>i",  WasiClockTimeGetFn },
            { "wasi_snapshot_preview1", "clock_res_get",      "ii>i",   WasiClockResGetFn },
            { "wasi_snapshot_preview1", "random_get",         "ii>i",   WasiRandomGetFn },
            { "wasi_snapshot_preview1", "proc_exit",          "i>",     WasiProcExitFn },
            { "wasi_snapshot_preview1", "sched_yield",        ">i",     WasiSchedYieldFn },
            { "wasi_snapshot_preview1", "poll_oneoff",        "iiii>i", WasiNoSysFn },
        };

        wasm_valtype_t* ValType(char lcCode)
        {
            switch (lcCode)
            {
            case 'i': return wasm_valtype_new_i32();
            case 'I': return wasm_valtype_new_i64();
            case 'f': return wasm_valtype_new_f32();
            default:  return wasm_valtype_new_f64();
            }
        }

        wasm_functype_t* FuncType(const char* lpcSignature)
        {
            const char* lpcArrow = std::strchr(lpcSignature, '>');
            const size_t luParams = static_cast<size_t>(lpcArrow - lpcSignature);
            const size_t luResults = std::strlen(lpcArrow + 1);
            wasm_valtype_vec_t lParams, lResults;
            wasm_valtype_vec_new_uninitialized(&lParams, luParams);
            wasm_valtype_vec_new_uninitialized(&lResults, luResults);
            for (size_t lu = 0; lu < luParams; ++lu) lParams.data[lu] = ValType(lpcSignature[lu]);
            for (size_t lu = 0; lu < luResults; ++lu) lResults.data[lu] = ValType(lpcArrow[1 + lu]);
            return wasm_functype_new(&lParams, &lResults);   // takes both vectors
        }

        wasm_trap_t* Trampoline(void* lpEnv, wasmtime_caller_t* lpCaller, const wasmtime_val_t* lpArgs, size_t,
                                wasmtime_val_t* lpResults, size_t luResults)
        {
            const HostImport& lrImport = *static_cast<const HostImport*>(lpEnv);
            // Results start as zero of the declared type, so a body that returns early is well-formed.
            const char* lpcResults = std::strchr(lrImport.mpcSignature, '>') + 1;
            for (size_t lu = 0; lu < luResults; ++lu)
            {
                lpResults[lu] = {};
                lpResults[lu].kind = lpcResults[lu] == 'i' ? WASMTIME_I32 : lpcResults[lu] == 'I' ? WASMTIME_I64 :
                                     lpcResults[lu] == 'f' ? WASMTIME_F32 : WASMTIME_F64;
            }
            HostCall lCall = { &lrImport, lpCaller, lpArgs, lpResults };
            try
            {
                return lrImport.mpfnBody(lCall);
            }
            catch (...)   // a host failure (out of memory) must not unwind through wasm frames
            {
                return Trap(lCall, "host error");
            }
        }
    }

    wasmtime_error_t* DefineHostImports(wasmtime_linker_t* lpLinker)
    {
        for (const HostImport& lrImport : KA_IMPORTS)
        {
            wasm_functype_t* lpType = FuncType(lrImport.mpcSignature);
            wasmtime_error_t* lpError = wasmtime_linker_define_func(
                lpLinker, lrImport.mpcModule, std::strlen(lrImport.mpcModule), lrImport.mpcName, std::strlen(lrImport.mpcName),
                lpType, Trampoline, const_cast<HostImport*>(&lrImport), nullptr);
            wasm_functype_delete(lpType);
            if (lpError != nullptr) return lpError;
        }
        return nullptr;
    }

    bool IsHostImportModule(const char* lpcName, size_t luLength)
    {
        for (const HostImport& lrImport : KA_IMPORTS)
            if (std::strncmp(lrImport.mpcModule, "brn.", 4) == 0 && std::strlen(lrImport.mpcModule) == luLength &&
                std::memcmp(lrImport.mpcModule, lpcName, luLength) == 0)
                return true;
        return false;
    }
} }

#endif // BRN_PC_MODS
