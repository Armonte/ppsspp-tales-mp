// Tales of Phantasia: Narikiri Dungeon X — multiplayer patch installer.
//
// Detects the game by DISC_ID and applies MIPS patches to redirect input
// reads in battle_input_dispatch through our per-pad routing. See hooking.md
// in repo root for the full strategy.
#pragma once

#include <string_view>

#include "Common/CommonTypes.h"

namespace TalesMp {

// Returns true if the given DISC_ID matches a supported Tales-NDX release.
bool IsSupportedDiscId(std::string_view disc_id);

// Apply MIPS patches to the loaded EBOOT in RAM. Must be called AFTER the
// EBOOT is loaded into memory and BEFORE the game's threads start running.
// Logs every action; returns true on success, false if anything looked wrong
// (e.g. instruction bytes don't match what we expect at the patch sites).
bool ApplyPatches();

// Read the hook counter (incremented each time the patched hook fires).
// Returns 0 if patches aren't installed or counter address is unreadable.
u32 ReadHookCounter();

// One-line log of patch state (counter value, etc.) — call on shutdown to
// see whether the hook actually fired during the run.
void LogHookStatus();

}  // namespace TalesMp
