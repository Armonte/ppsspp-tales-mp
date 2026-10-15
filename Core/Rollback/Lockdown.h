#pragma once

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

void EnterLockdown();
void ExitLockdown();

}  // namespace Rollback
