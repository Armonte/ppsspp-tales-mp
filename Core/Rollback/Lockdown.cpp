#include "Core/Rollback/Lockdown.h"

#include "Common/Log.h"
#include "Common/Thread/ThreadManager.h"
#include "Core/Config.h"

namespace Rollback {

namespace {

bool g_lockedDown = false;

// The user's values of the forced settings, put back by ExitLockdown().
struct SavedSettings {
	bool bSeparateSASThread;
	bool bSaveNewTextures;
	int iFrameSkip;
	bool bAutoFrameSkip;
	bool bEnableWlan;
};
SavedSettings g_saved;

}  // namespace

bool IsLockedDown() {
	return g_lockedDown;
}

static const std::vector<uint32_t> *g_restoredPages = nullptr;

const std::vector<uint32_t> *RollbackLoadRestoredPages() {
	return g_restoredPages;
}

void SetRollbackLoadRestoredPages(const std::vector<uint32_t> *pages) {
	g_restoredPages = pages;
}

bool RollbackLoadRestoredRange(uint32_t start, uint32_t end) {
	if (!g_restoredPages)
		return true;
	if (start >= end)
		return false;
	for (uint32_t page : *g_restoredPages) {
		if (page < end && page + 4096 > start)
			return true;
	}
	return false;
}

void EnterLockdown() {
	if (g_lockedDown)
		return;

	g_saved.bSeparateSASThread = g_Config.bSeparateSASThread;
	g_saved.bSaveNewTextures = g_Config.bSaveNewTextures;
	g_saved.iFrameSkip = g_Config.iFrameSkip;
	g_saved.bAutoFrameSkip = g_Config.bAutoFrameSkip;
	g_saved.bEnableWlan = g_Config.bEnableWlan;

	// SAS mixing on its own thread finishes at host-dependent times.
	g_Config.bSeparateSASThread = false;
	// Texture dumping writes files from host threads while the game runs.
	g_Config.bSaveNewTextures = false;
	// Frame skipping depends on host speed. Rollback re-simulation skips rendering on its own.
	// (sceDisplay ends a host frame at every vblank while locked down, see __DisplayFlip.)
	g_Config.iFrameSkip = 0;
	g_Config.bAutoFrameSkip = false;
	// Ad hoc networking talks to the real network.
	g_Config.bEnableWlan = false;

	g_threadManager.SetInlineMode(true);

	g_lockedDown = true;
	INFO_LOG(Log::System, "Rollback: determinism lockdown on");
}

void ExitLockdown() {
	if (!g_lockedDown)
		return;

	g_threadManager.SetInlineMode(false);

	g_Config.bSeparateSASThread = g_saved.bSeparateSASThread;
	g_Config.bSaveNewTextures = g_saved.bSaveNewTextures;
	g_Config.iFrameSkip = g_saved.iFrameSkip;
	g_Config.bAutoFrameSkip = g_saved.bAutoFrameSkip;
	g_Config.bEnableWlan = g_saved.bEnableWlan;

	g_lockedDown = false;
	INFO_LOG(Log::System, "Rollback: determinism lockdown off");
}

}  // namespace Rollback
