#pragma once

#include <cstdint>

// GekkoNet rollback session driving the emulated PSP.
//
// While a session runs, RunSessionFrame() replaces PSP_RunLoopWhileState() on the emu thread: it hands the
// local player's input to GekkoNet and plays GekkoNet's events, where each advance applies the synced inputs
// (through __CtrlSetNetplayInput) and runs one PSP frame, and saves/loads go through NetplayState.
//
// The stress role runs locally with two local actors and no network: GekkoNet rolls back check_distance
// frames, re-simulates and compares checksums continuously, which catches save/load non-determinism.

namespace Rollback {

enum class Role {
	StressTest,
};

struct SessionConfig {
	Role role = Role::StressTest;
	// Stress: frames rolled back and compared each time.
	int checkDistance = 16;
	int inputDelay = 0;
	// Stress: replace the local input with a deterministic pseudo-random sequence, so rollbacks re-simulate
	// frames with changing input.
	bool fuzzInput = false;
};

struct SessionStats {
	bool active = false;
	Role role = Role::StressTest;
	int frame = 0;
	int64_t rollbackFrames = 0;
	int desyncFrame = -1;
	uint32_t desyncLocal = 0;
	uint32_t desyncRemote = 0;
	uint32_t stateSize = 0;
	size_t stateHighWatermark = 0;
	double avgSaveMs = 0.0;
	double avgLoadMs = 0.0;
	int saveFailures = 0;
	int loadFailures = 0;
};

// Starts a session with the current game running. Enters the determinism lockdown.
bool StartSession(const SessionConfig &config);
void StopSession();
bool IsSessionActive();

// Emu thread, once per host frame, instead of PSP_RunLoopWhileState(). Leaves coreState as the last advanced
// frame left it (CORE_NEXTFRAME when a frame completed).
void RunSessionFrame();

SessionStats GetSessionStats();

}  // namespace Rollback
