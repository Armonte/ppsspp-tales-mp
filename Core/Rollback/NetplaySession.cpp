#include "Core/Rollback/NetplaySession.h"

#include <cstring>
#include <mutex>

#include <gekkonet.h>

#include "Common/Log.h"
#include "Core/Core.h"
#include "Core/HLE/sceCtrl.h"
#include "Core/HLE/sceDisplay.h"
#include "Core/HW/Display.h"
#include "Core/Rollback/Lockdown.h"
#include "Core/Rollback/NetplayState.h"
#include "Core/Rollback/Profile.h"
#include "Core/Rollback/TekkenDR.h"
#include "Core/System.h"

namespace Rollback {

namespace {

// One player's input for one frame, as exchanged through GekkoNet. Fixed layout on every host.
struct NetInput {
	uint32_t buttons;
	uint8_t analogX;
	uint8_t analogY;
	uint8_t pad[2];
};
static_assert(sizeof(NetInput) == 8, "NetInput is part of the wire format");

struct Session {
	SessionConfig config;
	GekkoSession *gekko = nullptr;
	int localHandle = -1;
	int extraLocalHandle = -1;
	NetplayState state;
	uint32_t fuzzSeed = 0x2545F491;
	int fuzzHoldFrames = 0;
	NetInput fuzzInput{};
	NetInput scripted[2]{};
	// Player 2 plays through Tekken: Dark Resurrection's hidden local 2P path.
	bool tekkenDR = false;
};

std::mutex g_statsLock;
SessionStats g_stats;
Session *g_session = nullptr;

NetInput ReadLocalInput() {
	NetInput in{};
	in.buttons = __CtrlPeekButtons() & CTRL_MASK_USER;
	u8 analog[2][2];
	__CtrlPeekAnalogRaw(analog);
	in.analogX = analog[0][0];
	in.analogY = analog[0][1];
	return in;
}

// Deterministic input fuzzer for the stress test: random buttons and stick, each held for a few frames.
NetInput FuzzInput(Session &s) {
	if (s.fuzzHoldFrames-- <= 0) {
		auto next = [&]() {
			s.fuzzSeed = s.fuzzSeed * 1664525u + 1013904223u;
			return s.fuzzSeed >> 8;
		};
		const uint32_t userButtons = CTRL_UP | CTRL_DOWN | CTRL_LEFT | CTRL_RIGHT | CTRL_CROSS | CTRL_CIRCLE |
			CTRL_SQUARE | CTRL_TRIANGLE | CTRL_LTRIGGER | CTRL_RTRIGGER | CTRL_START;
		s.fuzzInput.buttons = next() & userButtons;
		s.fuzzInput.analogX = (uint8_t)next();
		s.fuzzInput.analogY = (uint8_t)next();
		s.fuzzHoldFrames = 2 + (int)(next() % 12);
	}
	return s.fuzzInput;
}

void ApplyInput(const Session &s, const NetInput *inputs) {
	const u8 analog[2][2] = { { inputs[0].analogX, inputs[0].analogY }, { 128, 128 } };
	__CtrlSetNetplayInput(true, inputs[0].buttons, analog);
	if (s.tekkenDR)
		TekkenDRApplyPlayer2(inputs[1].buttons, inputs[1].analogX, inputs[1].analogY);
}

void UpdateStats(Session &s) {
	std::lock_guard<std::mutex> guard(g_statsLock);
	g_stats.stateSize = s.state.StateSize();
	g_stats.stateHighWatermark = s.state.HighWatermark();
	g_stats.avgSaveMs = s.state.AverageSaveMs();
	g_stats.avgLoadMs = s.state.AverageLoadMs();
	g_stats.lastSavedPages = s.state.Pages().LastSavedPages();
	g_stats.lastLoadedPages = s.state.Pages().LastLoadedPages();
	g_stats.storedPageVersions = s.state.Pages().StoredVersions();
	g_stats.trackingWrites = s.state.Pages().TrackingWrites();
	g_stats.pageFaults = s.state.Pages().Faults();
	g_stats.missedPages = s.state.Pages().MissedPages();
	g_stats.profile = ProfileSummary(s.state.Saves(), s.state.Loads());
	g_stats.loads = s.state.Loads();
	g_stats.diagMismatchFrame = s.state.DiagMismatchFrame();
	g_stats.diagFirstDiff = s.state.DiagFirstDiffOffset();
}

}  // namespace

bool StartSession(const SessionConfig &config) {
	if (g_session) {
		WARN_LOG(Log::System, "Rollback: a session is already running");
		return false;
	}
	if (!PSP_IsInited()) {
		WARN_LOG(Log::System, "Rollback: start a game before starting a session");
		return false;
	}

	EnterLockdown();

	Session *s = new Session();
	s->config = config;
	s->tekkenDR = IsTekkenDR();
	for (NetInput &in : s->scripted)
		in.analogX = in.analogY = 128;
	const int checkDistance = config.checkDistance > 0 ? config.checkDistance : 16;
	// Loads go back at most the check distance (stress) or the prediction window; keep a margin on top.
	const int keepFrames = checkDistance + 8 + 32;
	if (!s->state.Initialize(config.fullStates, keepFrames, config.trackWrites, config.validatePages)) {
		delete s;
		ExitLockdown();
		return false;
	}

	if (!config.diagDumpDir.empty())
		s->state.EnableDiagnostics(config.diagDumpDir, (config.checkDistance > 0 ? config.checkDistance : 16) + 4);

	if (!gekko_create(&s->gekko, GekkoStressSession)) {
		ERROR_LOG(Log::System, "Rollback: gekko_create failed");
		delete s;
		ExitLockdown();
		return false;
	}

	GekkoConfig gcfg{};
	gcfg.num_players = 2;
	gcfg.max_spectators = 0;
	gcfg.max_input_prediction_window = 8;
	gcfg.input_size = sizeof(NetInput);
	gcfg.state_size = s->state.StateSize();
	gcfg.limited_saving = false;
	gcfg.desync_detection = true;
	gcfg.check_distance = config.checkDistance > 0 ? config.checkDistance : 16;
	gekko_start(s->gekko, &gcfg);
	gekko_set_checksum_callback(s->gekko, &NetplayState::ChecksumCallback, &s->state);

	// The stress session takes only local actors: the PSP's pad and a second, neutral player.
	s->localHandle = gekko_add_actor(s->gekko, GekkoLocalPlayer, nullptr);
	s->extraLocalHandle = gekko_add_actor(s->gekko, GekkoLocalPlayer, nullptr);
	if (s->localHandle < 0 || s->extraLocalHandle < 0) {
		ERROR_LOG(Log::System, "Rollback: gekko_add_actor failed");
		gekko_destroy(&s->gekko);
		delete s;
		ExitLockdown();
		return false;
	}
	gekko_set_local_delay(s->gekko, s->localHandle, (unsigned char)config.inputDelay);
	gekko_set_local_delay(s->gekko, s->extraLocalHandle, (unsigned char)config.inputDelay);

	g_session = s;
	{
		std::lock_guard<std::mutex> guard(g_statsLock);
		g_stats = SessionStats{};
		g_stats.active = true;
		g_stats.role = config.role;
	}
	UpdateStats(*s);
	INFO_LOG(Log::System, "Rollback: stress session started (check distance %u, state %u bytes, fuzz %s)",
		gcfg.check_distance, gcfg.state_size, config.fuzzInput ? "on" : "off");
	return true;
}

void StopSession() {
	Session *s = g_session;
	if (!s)
		return;
	g_session = nullptr;
	gekko_destroy(&s->gekko);
	__CtrlSetNetplayInput(false, 0, nullptr);
	ExitLockdown();
	{
		std::lock_guard<std::mutex> guard(g_statsLock);
		g_stats.active = false;
	}
	INFO_LOG(Log::System, "Rollback: session stopped after %d frames", g_stats.frame);
	delete s;
}

bool IsSessionActive() {
	return g_session != nullptr;
}

void RunSessionFrame() {
	Session *s = g_session;
	if (!s) {
		PSP_RunLoopWhileState();
		return;
	}

	NetInput local = s->config.scriptedInput ? s->scripted[0] : s->config.fuzzInput ? FuzzInput(*s) : ReadLocalInput();
	NetInput extra{};
	extra.analogX = extra.analogY = 128;
	if (s->config.scriptedInput)
		extra = s->scripted[1];
	gekko_add_local_input(s->gekko, s->localHandle, &local);
	gekko_add_local_input(s->gekko, s->extraLocalHandle, &extra);

	int sessionEventCount = 0;
	if (GekkoSessionEvent **events = gekko_session_events(s->gekko, &sessionEventCount)) {
		for (int i = 0; i < sessionEventCount; i++) {
			const GekkoSessionEvent *e = events[i];
			if (e->type == GekkoDesyncDetected) {
				std::lock_guard<std::mutex> guard(g_statsLock);
				if (g_stats.desyncFrame < 0) {
					g_stats.desyncFrame = e->data.desynced.frame;
					g_stats.desyncLocal = e->data.desynced.local_checksum;
					g_stats.desyncRemote = e->data.desynced.remote_checksum;
				}
				ERROR_LOG(Log::System, "Rollback: non-deterministic state at frame %d (%08x vs %08x)",
					e->data.desynced.frame, e->data.desynced.local_checksum, e->data.desynced.remote_checksum);
			}
		}
	}

	int gameEventCount = 0;
	GekkoGameEvent **events = gekko_update_session(s->gekko, &gameEventCount);
	bool advanced = false;
	for (int i = 0; events && i < gameEventCount; i++) {
		GekkoGameEvent *e = events[i];
		switch (e->type) {
		case GekkoSaveEvent:
			if (!s->state.Save(e->data.save.frame, e->data.save.state, e->data.save.state_len, e->data.save.checksum)) {
				std::lock_guard<std::mutex> guard(g_statsLock);
				g_stats.saveFailures++;
			}
			break;
		case GekkoLoadEvent:
			if (!s->state.Load(e->data.load.frame, e->data.load.state, e->data.load.state_len)) {
				std::lock_guard<std::mutex> guard(g_statsLock);
				g_stats.loadFailures++;
			}
			break;
		case GekkoAdvanceEvent: {
			// A previous advance in this batch left the frame finished; start the next one.
			if (advanced && coreState == CORE_NEXTFRAME)
				coreState = CORE_RUNNING_CPU;
			const NetInput *inputs = (const NetInput *)e->data.adv.inputs;
			ApplyInput(*s, inputs);
			SetResimulating(e->data.adv.rolling_back);
			const int vcountBefore = __DisplayGetVCount();
			const int gameFramesBefore = __DisplayGetGameFrames();
			// One advance is exactly one PSP frame: keep running until the frame ends (or the core stops).
			do {
				PSP_RunLoopWhileState();
			} while (coreState == CORE_RUNNING_CPU);
			SetResimulating(false);
			const int vblanks = __DisplayGetVCount() - vcountBefore;
			advanced = true;
			std::lock_guard<std::mutex> guard(g_statsLock);
			g_stats.frame = e->data.adv.frame;
			if (e->data.adv.rolling_back) {
				g_stats.rollbackFrames++;
			} else {
				g_stats.lastFrameEndedGameFrame = __DisplayGetGameFrames() != gameFramesBefore;
				g_stats.lastFrameVblanks = vblanks;
				if (g_stats.maxVblanksPerFrame == 0 || vblanks < g_stats.minVblanksPerFrame)
					g_stats.minVblanksPerFrame = vblanks;
				if (vblanks > g_stats.maxVblanksPerFrame)
					g_stats.maxVblanksPerFrame = vblanks;
			}
			break;
		}
		default:
			break;
		}
	}
	UpdateStats(*s);
}

void SetScriptedInput(uint32_t player1Buttons, uint32_t player2Buttons) {
	if (Session *s = g_session) {
		s->scripted[0].buttons = player1Buttons;
		s->scripted[1].buttons = player2Buttons;
	}
}

SessionStats GetSessionStats() {
	std::lock_guard<std::mutex> guard(g_statsLock);
	return g_stats;
}

}  // namespace Rollback
