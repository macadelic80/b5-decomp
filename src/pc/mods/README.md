# Burnout_PC.exe mods

FLAG PC divergence: there is no console counterpart. This folder is the PC build's mod host.

A mod is a folder `mods/<name>/` next to `Burnout_PC.exe`, holding a `mod.ini` and one
**WebAssembly** module:

```ini
[Mod]
Name=Debug menu
Wasm=debug_menu.wasm
Version=0.2
```

The same `.wasm` runs on every OS and CPU the game is built for. It runs in a sandbox: it sees
its own memory and the functions in [`api/brn_mod_api.h`](api/brn_mod_api.h), nothing else.

## Writing a mod

Any language that compiles to `wasm32` works. The mod exports a few functions and imports the
host's services; [`api/brn_mod_api.h`](api/brn_mod_api.h) is the contract (C, no dependency) and
[`api/brn_mod_api.wit`](api/brn_mod_api.wit) restates it in WIT for reference.

| Export | Required | Called |
| --- | --- | --- |
| `memory` | yes | (the linear memory; every toolchain exports it) |
| `brn_mod_api_version() -> u32` | yes | at load; must return `BRN_MOD_API_VERSION` (2) |
| `brn_mod_init() -> i32` | yes | once; non-zero = loaded |
| `brn_mod_input_buffer() -> ptr` + `brn_mod_on_input(count)` | no | render thread, before render: up to 64 `brn_input_event` per call |
| `brn_mod_render(w, h, dpi_scale, dt, frame)` | no | render thread, every frame: submit the UI here |
| `brn_mod_tick()` | no | update thread, once per game update |
| `brn_mod_shutdown()` | no | when switched off from the mods list |
| `_initialize` | no | first, if present (WASI reactors: libc and C++ constructors) |

A mod's exports never run at the same time: the host serialises them, so a mod needs no locks.

| Import module | What |
| --- | --- |
| `brn.core.v1` | log, time, input capture, the mods list (switch a mod on/off; switching a wasm mod on reloads its `.wasm`) |
| `brn.player.v1` | the player's car: state, teleport, reset / place on track, boost, invulnerability, car change |
| `brn.vehicles.v1`, `brn.locations.v1`, `brn.racecars.v1`, `brn.traffic.v1` | game lists and traffic switches |
| `brn.debugvars.v1` | the game's own CgsDev debug menu tree: list, set, call |
| `brn.debugini.v1` | `config.ini [Debug]` lines (applied by the game at start-up) |
| `brn.storage.v1` | the mod's own files in `mods/<name>/data/` |
| `brn.ui.v1` | textures (RGBA8) and textured triangle lists |

Getters return a copy made at the last game tick; commands are queued and applied at the next
one, through the game's own channels (the debug game-event queue, game actions) -- see
`ModServices.cpp`. A module never changes once published: a new shape gets a new name
(`brn.player.v2`) and the old one stays. An import this build lacks does not stop a mod from
loading; it traps if called, so check `brn_has_module` when you target several versions.

### UI

A mod never touches the graphics device. It submits **draw lists** -- vertices `{x, y, u, v,
rgba8}` (20 bytes, the layout of both `ImDrawVert` and egui's `Vertex`), 16- or 32-bit indices,
a texture and a clip rect -- with `brn_ui_submit` / `brn_ui_submit32` during `brn_mod_render`,
in back buffer pixels. The host draws them over the finished frame (`ModUiD3D9.cpp`, alpha
blended, scissored; pass `BRN_UI_PREMULTIPLIED` for egui). The device state is saved and restored
around the mods. The backend is the only graphics-API-specific file: a Vulkan or Metal port adds
another one behind `Ui::Backend` (`ModUi.h`).

Input arrives as portable `brn_input_event`s (mouse in back buffer pixels, keys as `BRN_KEY_*`,
text as code points, wheel, focus, resize). While a mod holds `BRN_CAPTURE_KEYBOARD` /
`MOUSE` / `GAMEPAD`, the game ignores that input. The game hides the Windows cursor: draw your
own while your menu is open (ImGui: `io.MouseDrawCursor`).

### Examples

- **Rust + egui** -- [macadelic80/b5_mods](https://github.com/macadelic80/b5_mods): `b5_mod_sdk`
  (typed services, `export_mod!`, an egui overlay that submits draw lists) and `debug_menu`
  (vehicle, teleport, motion, stats, traffic, race cars, the mods list and the game's debug menu
  tree). Build: `rustup target add wasm32-unknown-unknown`, then `install_mods.bat <game dir>`
  (`cargo build --release` for `wasm32-unknown-unknown`).
- **C++ + Dear ImGui** -- `b5_mods_imgui` (its own repository, beside b5_mods): F8 opens the
  player's position and car, a teleport target and the debug menu tree. Build with
  [wasi-sdk](https://github.com/WebAssembly/wasi-sdk):
  `python build.py --wasi-sdk <wasi-sdk dir> --install <game dir>`
  (`--target=wasm32-wasip1 -mexec-model=reactor`; Dear ImGui is fetched pinned).
- **C** -- include `brn_mod_api.h` and build with clang: `clang --target=wasm32 -nostdlib
  -Wl,--no-entry -Wl,--export-dynamic` (or wasi-sdk for a libc).

Zig, AssemblyScript, TinyGo and interpreters compiled to wasm (QuickJS, Lua) work the same way:
declare the imports with their module/field names and export the functions above.

## Security model

What a WebAssembly mod can do is exactly the list of imports above. In particular:

- **No game memory.** The mod sees its own linear memory only. Data is copied in; game objects
  are named by index or id. Every pointer and length a mod passes is checked against its memory
  (`GuestMemory` in `ModWasm.h`); an out-of-bounds one traps.
- **No file system, network, environment or processes.** There is no WASI file system or socket;
  the few `wasi_snapshot_preview1` calls a C/C++ libc needs to start are stubs (stdout/stderr go
  to `BrnGame.log`, clocks, random; no preopened directory). Persistent data goes through
  `brn.storage.v1`, confined to `mods/<name>/data/` with plain file names. The only other file a
  mod can change is `config.ini [Debug]`, line by line, through `brn.debugini.v1`. The clipboard
  is write-only.
- **Faults stay in the mod.** A trap -- a crash, a Rust panic, an out-of-bounds access -- switches
  that mod off and names it in `BrnGame.log` with its wasm backtrace; the game and the other mods
  keep running. wasmtime runs with signals-based traps off, so a mod's fault never raises a
  Windows exception for the game's crash handler to see.
- **Bounded time and memory.** Each call has a wall-clock budget (epoch interruption,
  `config.ini [Mods] WasmTimeLimitMs`, default 200 ms; `WasmInitTimeLimitMs` for start-up) and a
  mod's memory is capped (`WasmMemoryLimitMB`, default 256). A stuck mod traps instead of
  freezing the frame. Draw lists and textures have per-frame / per-mod caps.

What it does not protect against: a mod can still use the services it is given (teleport the car,
flip debug variables, fill its own storage folder), and its commands queue until the next tick.
Install mods you trust to behave, not only to stay in their box.

## Native (DLL) mods

The previous DLL host is still here (`ModNative.cpp`, [`api/brn_native_mod_api.h`](api/brn_native_mod_api.h)),
**off by default**: a DLL runs with the game's full rights and is built per OS/CPU. It loads only
with `config.ini [Mods] AllowNativeMods=1` **and** `mods/mods.ini [Native] <folder>=1`, and the
log warns at every start. A native mod's `mod.ini` names `Dll=` instead of `Wasm=`.

## Configuration

```ini
; config.ini
[Mods]
Enabled=1               ; 0 (or building with BRN_PC_MODS=0) turns every hook into a no-op
AllowNativeMods=0
WasmTimeLimitMs=200
WasmInitTimeLimitMs=5000
WasmMemoryLimitMB=256

; mods/mods.ini (written by the mods list)
[Enabled]               ; WebAssembly mods, default on
debug_menu=1
[Native]                ; DLL mods, default off
```

## Host layout

| File | Role |
| --- | --- |
| `ModHost.cpp/.h` | the game's hooks (`RenderFrame`, `OnWindowMessage`, input gates, `OnGameTick`), config, `mods.ini`, capture, the frame's D3D9 state save/restore |
| `ModWasm.cpp/.h` | wasmtime engine, one store per mod, limits, traps |
| `ModWasmImports.cpp` | every host import, bounds-checked; the WASI stubs |
| `ModInput.cpp/.h` | Win32 messages -> `brn_input_event` |
| `ModUi.cpp/.h`, `ModUiD3D9.cpp` | textures + draw lists; the D3D9 backend |
| `ModNative.cpp` | the opt-in DLL tier |
| `ModServices.cpp`, `ModDebugVars.cpp` | the game-state services both tiers share |

The runtime is wasmtime's C API (`tools/build/fetch_wasmtime.py`, pinned; the build is x64, which
wasmtime supports with its Cranelift JIT, epoch interruption and store limits).
