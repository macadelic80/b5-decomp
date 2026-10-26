#pragma once

// FLAG PC divergence: shared between the mod host (ModHost.cpp) and the game-state services
// (ModServices.cpp). Not seen by the game or by the mods.

#include "pc/mods/api/brn_mod_api.h"

namespace BrnGameState { class GameStateModule; namespace GameStateModuleIO { struct OutputBuffer; } }
namespace BrnWorld { class WorldModule; }

namespace CgsPC { namespace Mods
{
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
