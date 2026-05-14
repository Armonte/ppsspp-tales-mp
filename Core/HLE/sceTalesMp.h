// Tales of Phantasia: Narikiri Dungeon X — multiplayer patch installer.
//
// Detects the game by DISC_ID and applies MIPS patches to redirect input
// reads in battle_input_dispatch through our per-pad routing. See hooking.md
// in repo root for the full strategy.
#pragma once

#include <string_view>

namespace TalesMp {

// Returns true if the given DISC_ID matches a supported Tales-NDX release.
bool IsSupportedDiscId(std::string_view disc_id);

// Apply MIPS patches to the loaded EBOOT in RAM. Must be called AFTER the
// EBOOT is loaded into memory and BEFORE the game's threads start running.
// Logs every action; returns true on success, false if anything looked wrong
// (e.g. instruction bytes don't match what we expect at the patch sites).
bool ApplyPatches();

}  // namespace TalesMp
