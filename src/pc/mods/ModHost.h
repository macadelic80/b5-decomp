#pragma once

// FLAG PC divergence: the DLL mod host (mods/<name>/mod.ini + a DLL speaking
// pc/mods/api/brn_mod_api.h). No console counterpart, no third-party library: the host
// loads the DLLs, forwards the window messages, gives them the D3D9 device at the end of
// each frame, and serves game state through versioned C tables (ModServices.cpp).
//
// The game reaches it through the four hooks below only. config.ini [Mods] Enabled=0
// (or BRN_PC_MODS=0 at build time) leaves every hook a no-op and loads nothing.

#ifndef BRN_PC_MODS
#define BRN_PC_MODS 1
#endif

struct IDirect3DDevice9;
struct HWND__;
namespace BrnGameState { class GameStateModule; namespace GameStateModuleIO { struct OutputBuffer; } }
namespace BrnWorld { class WorldModule; }

namespace CgsPC { namespace Mods
{
#if BRN_PC_MODS
    // Render thread, inside the scene, just before EndScene (renderengine::Device::ShowPixelBuffer).
    void RenderFrame(IDirect3DDevice9* lpDevice, HWND__* lhWindow);

    // Window thread (windowProc). Queues the message for the mods; true = the mods hold the
    // keyboard/mouse and the game's own handling of this message must be skipped.
    bool OnWindowMessage(HWND__* lhWindow, unsigned int luMessage, unsigned long long luWParam,
                         long long liLParam);

    // Input readers (keyboard pads, debug controller): a mod asked for the input.
    bool IsCapturingKeyboard();
    bool IsCapturingGamepad();

    // Update thread, right after GameStateModule::PreWorldUpdate (BrnGameModule.cpp):
    // apply the mods' queued commands, refresh the snapshots, run on_game_tick.
    void OnGameTick(BrnGameState::GameStateModule& lrGameState, BrnWorld::WorldModule& lrWorld,
                    BrnGameState::GameStateModuleIO::OutputBuffer* lpGameStateOutput);
#else
    inline void RenderFrame(IDirect3DDevice9*, HWND__*) {}
    inline bool OnWindowMessage(HWND__*, unsigned int, unsigned long long, long long) { return false; }
    inline bool IsCapturingKeyboard() { return false; }
    inline bool IsCapturingGamepad() { return false; }
    inline void OnGameTick(BrnGameState::GameStateModule&, BrnWorld::WorldModule&,
                           BrnGameState::GameStateModuleIO::OutputBuffer*) {}
#endif
} }
