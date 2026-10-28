#pragma once

#include <cstdint>
#include <vector>

// Rollback netplay determinism lockdown.
//
// While a netplay session is active, every peer must reach byte-identical emulated state from the same
// inputs. The lockdown removes the host-dependent parts of emulation:
// - settings that make emulation depend on host threads or host timing are forced (and restored afterwards),
// - the ThreadManager runs tasks inline on the calling thread,
// - local time comes from the configured PSP timezone instead of the host's.
//
// Settings that change emulation but are the same on every host (CPU core, IO timing, locked CPU speed, ...)
// are not forced here; the session handshake requires peers to agree on them instead.
//
// Several of the forced settings are only read when a game boots (the SAS thread, for one), so a session
// enters the lockdown before booting the game it plays.

namespace Rollback {

bool IsLockedDown();

// A range of guest memory [start, end) that a rollback load changed.
struct RestoredRange {
	uint32_t start;
	uint32_t end;
};

// While a rollback load is restoring the core state: the RAM/VRAM the page store changed (in 64-byte steps).
// Lets loaders skip rebuilding what can't have changed (compiled code, module links). Null outside a rollback load.
const std::vector<RestoredRange> *RollbackLoadRestored();
void SetRollbackLoadRestored(const std::vector<RestoredRange> *ranges);
// Whether the load changed anything in [start, end).
bool RollbackLoadRestoredRange(uint32_t start, uint32_t end);

void EnterLockdown();
void ExitLockdown();

}  // namespace Rollback
