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
	bool bRenderDuplicateFrames;
	bool bEnableWlan;
};
SavedSettings g_saved;

}  // namespace

bool IsLockedDown() {
	return g_lockedDown;
}

void EnterLockdown() {
	if (g_lockedDown)
		return;

	g_saved.bSeparateSASThread = g_Config.bSeparateSASThread;
	g_saved.bSaveNewTextures = g_Config.bSaveNewTextures;
	g_saved.iFrameSkip = g_Config.iFrameSkip;
	g_saved.bAutoFrameSkip = g_Config.bAutoFrameSkip;
	g_saved.bRenderDuplicateFrames = g_Config.bRenderDuplicateFrames;
	g_saved.bEnableWlan = g_Config.bEnableWlan;

	// SAS mixing on its own thread finishes at host-dependent times.
	g_Config.bSeparateSASThread = false;
	// Texture dumping writes files from host threads while the game runs.
	g_Config.bSaveNewTextures = false;
	// Frame skipping depends on host speed. Rollback re-simulation skips rendering on its own.
	g_Config.iFrameSkip = 0;
	g_Config.bAutoFrameSkip = false;
	g_Config.bRenderDuplicateFrames = false;
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
	g_Config.bRenderDuplicateFrames = g_saved.bRenderDuplicateFrames;
	g_Config.bEnableWlan = g_saved.bEnableWlan;

	g_lockedDown = false;
	INFO_LOG(Log::System, "Rollback: determinism lockdown off");
}

}  // namespace Rollback
