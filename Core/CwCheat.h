// Rough and ready CwCheats implementation, disabled by default.

#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <iostream>
#include <sstream>

#include "Common/File/Path.h"
#include "Core/MemMap.h"

class PointerWrap;

void __CheatInit();
void __CheatShutdown();
void __CheatDoState(PointerWrap &p);

// Return whether cheats are enabled and in effect.
bool CheatsInEffect();

// Called from sceKernelModule when a PSP module is loaded or unloaded.
// Drives the `_M <module>` scope-matching plus `_O` apply-once-on-load
// behavior. Modules without any matching gated cheats are still tracked
// — cost is one std::string per loaded module, negligible.
void CheatNotifyModuleLoaded(std::string_view moduleName);
void CheatNotifyModuleUnloaded(std::string_view moduleName);

struct CheatLine {
	uint32_t part1;
	uint32_t part2;
};

bool DetectCheatTitle(std::string_view name, std::string_view *title);
bool DetectCheatPostComment(std::string_view name, std::string_view *title);

struct CheatCode {
	std::string name;
	std::vector<CheatLine> lines;
	// Optional substring match against the PSP module name reported at
	// __KernelLoadExec. Empty = no module gating (apply globally, the
	// classic CWcheat behavior). Non-empty = this cheat only fires while
	// a module whose name contains the substring is currently loaded.
	// Used to disambiguate multi-game discs where DISC_ID alone is the
	// same for two different modules (e.g. Tales of Phantasia NDX:
	// `TOP_NARIKIRI_DUNGEON_R` and `TOP_PHANTASIA_R` share the disc).
	std::string moduleMatch;
	// If true, fire the cheat ONCE the moment its gating module loads
	// (or once at first frame if there's no moduleMatch), then stop
	// re-applying per frame. Ideal for installing big payloads that
	// don't need refreshing every frame — saves CPU and avoids drift
	// for non-idempotent op types.
	bool applyOnce = false;
	// Runtime state — set true once Run() (or a module-load hook) has
	// dispatched this cheat. Reset on re-parse and when the gating
	// module unloads (so a re-load re-fires the cheat).
	mutable bool firedAlready = false;
};

struct CheatFileInfo {
	int lineNum;
	std::string name;
	bool enabled;

	bool IsTitle(std::string_view *title) const {
		return DetectCheatTitle(name, title);
	}
	bool IsPostComment(std::string_view *comment) const {
		return DetectCheatPostComment(name, comment);
	}
};

struct CheatOperation;

class CWCheatEngine {
public:
	CWCheatEngine(std::string_view gameID);
	std::vector<CheatFileInfo> FileInfo() const;
	void ParseCheats();
	void CreateCheatFile();
	const Path &CheatFilename() const {
		return filename_;
	}
	void Run();
	// Fire every `_O` cheat whose `_M` matches `moduleName` immediately,
	// then mark them so per-frame Run() doesn't re-fire them. Called from
	// the module-load hook so apply-once-on-load patches land BEFORE the
	// module's entry point executes — same install-timing guarantee the
	// pre-CWcheat ModularPatch system provided.
	void RunOnceForModule(std::string_view moduleName);
	// Reset the firedAlready flag on every `_O` cheat whose `_M` matches
	// the unloading module. A subsequent re-load fires the cheat again.
	void OnModuleUnloaded(std::string_view moduleName);
	bool HasCheats();
	static u32 GetAddress(u32 value) {
		// TODO: This comment is weird:
		// Returns static address used by ppsspp. Some games may not like this, and causes cheats to not work without offset
		u32 address = (value + 0x08800000) & 0x3FFFFFFF;
		return address;
	}

private:
	void InvalidateICache(u32 addr, int size) const;

	CheatOperation InterpretNextCwCheat(const CheatCode &cheat, size_t &i);

	void ExecuteOp(const CheatOperation &op, const CheatCode &cheat, size_t &i);
	inline void ApplyMemoryOperator(const CheatOperation &op, uint32_t(*oper)(uint32_t, uint32_t));
	inline bool TestIf(const CheatOperation &op, bool(*oper)(int a, int b)) const;
	inline bool TestIfAddr(const CheatOperation &op, bool(*oper)(int a, int b)) const;

	std::vector<CheatCode> cheats_;
	std::string gameID_;
	Path filename_;
};

class CheatFileParser {
public:
	CheatFileParser(const Path &filename, std::string_view gameID = "");
	~CheatFileParser();

	bool Parse();

	const std::vector<std::string> &GetErrors() const { return errors_; }
	const std::vector<CheatCode> &GetCheats() const { return cheats_; }
	const std::vector<CheatFileInfo> &GetFileInfo() const { return cheatInfo_; }

protected:
	void Flush();
	void FlushCheatInfo();
	void AddError(const std::string &msg, int lineNumber);
	void ParseLine(const std::string &line, int lineNumber);
	void ParseDataLine(const std::string &line, int lineNumber);
	bool ValidateGameID(std::string_view gameID);

	FILE *fp_ = nullptr;
	std::string validGameID_;

	int games_ = 0;
	std::vector<std::string> errors_;
	std::vector<CheatFileInfo> cheatInfo_;
	std::vector<CheatCode> cheats_;
	std::vector<CheatLine> pendingLines_;
	CheatFileInfo lastCheatInfo_;
	bool gameEnabled_ = true;
	bool gameRiskyEnabled_ = false;
	bool cheatEnabled_ = false;
	// Current `_M <substring>` scope. Set by an _M line, applied to every
	// _C block flushed afterwards. Empty = global (default / explicit reset
	// via a bare `_M`).
	std::string currentModuleMatch_;
	// Whether the cheat currently being assembled saw an _O directive.
	bool currentApplyOnce_ = false;
};
