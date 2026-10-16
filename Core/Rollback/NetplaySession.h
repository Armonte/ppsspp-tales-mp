#pragma once

#include <cstdint>
#include <string>

// GekkoNet rollback session driving the emulated PSP.
//
// While a session runs, RunSessionFrame() replaces PSP_RunLoopWhileState() on the emu thread: it hands the
// local player's input to GekkoNet and plays GekkoNet's events, where each advance applies the synced inputs
// (player 1 through __CtrlSetNetplayInput, player 2 through the game's hidden 2P path where there is one) and runs one PSP frame, and saves/loads go through NetplayState.
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
	// Diagnostics: dump the first mismatching re-simulated state and its first run into this directory. With paged
	// states the dumps hold the state without RAM/VRAM (the page hash in the header covers those); combine with
	// fullStates to diff RAM byte for byte.
	std::string diagDumpDir;
	// Use full savestates instead of the paged RAM/VRAM store (slower; for comparison).
	bool fullStates = false;
	// Paged states: find changed pages by write faults (default) or by hashing every page.
	bool trackWrites = true;
	// With write tracking, also hash every page and count pages the tracker missed (slow; testing).
	bool validatePages = false;
	// Stress: take both players' input from SetScriptedInput (a recording) instead of the pad and a neutral player.
	bool scriptedInput = false;
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
	// Paged states: pages copied by the last save, written back by the last load, versions kept.
	size_t lastSavedPages = 0;
	size_t lastLoadedPages = 0;
	size_t storedPageVersions = 0;
	bool trackingWrites = false;
	uint64_t pageFaults = 0;
	size_t hotPages = 0;
	uint64_t missedPages = 0;
	// Average per-stage save/load times (Rollback::ProfileSummary).
	std::string profile;
	int loads = 0;
	int saveFailures = 0;
	int loadFailures = 0;
	// --rollback-diag: first re-simulated frame whose state differs from its first run, and where.
	// Vblanks per advanced (non-rollback) frame: should always be 1 under the lockdown.
	int minVblanksPerFrame = 0;
	int maxVblanksPerFrame = 0;
	// Whether the last advanced (non-rollback) frame finished a game frame (__DisplayGetGameFrames).
	bool lastFrameEndedGameFrame = false;
	int lastFrameVblanks = 0;
	int diagMismatchFrame = -1;
	size_t diagFirstDiff = 0;
};

// Starts a session with the current game running. Enters the determinism lockdown.
bool StartSession(const SessionConfig &config);
void StopSession();
bool IsSessionActive();

// Emu thread, once per host frame, instead of PSP_RunLoopWhileState(). Leaves coreState as the last advanced
// frame left it (CORE_NEXTFRAME when a frame completed).
void RunSessionFrame();

SessionStats GetSessionStats();

// With SessionConfig::scriptedInput: both players' input (CTRL_* bits, stick centered) for the next host frame.
void SetScriptedInput(uint32_t player1Buttons, uint32_t player2Buttons);

}  // namespace Rollback
