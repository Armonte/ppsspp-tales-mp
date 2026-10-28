#pragma once

#include <cstdint>
#include <string>

// Per-stage timing of rollback saves and loads, to see where the time goes (SaveStart::DoState's subsystems,
// the measure pass, the page store). Only collected while the determinism lockdown is on.

namespace Rollback {

enum class ProfileStage {
	PagesSave,
	PagesLoad,
	Header,
	Memory,
	CoreTiming,
	MemoryStick,
	MIPS,
	HLE,
	Kernel,
	Achievements,
	FileSystem,
	Count,
};

// Which pass of a savestate operation is running.
enum class ProfilePass {
	Measure,
	Write,
	Read,
	Count,
};

void ProfileAdd(ProfileStage stage, ProfilePass pass, double seconds);
// Average milliseconds per operation of each stage and pass since the last reset, as one line.
std::string ProfileSummary(int saves, int loads);
void ProfileReset();

// Finer timers inside a stage, by name (a string literal), for drilling down. Only the read pass is kept.
void ProfileAddNamed(const char *name, double seconds);
// The named timers, slowest first, average milliseconds per load.
std::string ProfileNamedSummary(int loads);

class ProfileNamedScope {
public:
	explicit ProfileNamedScope(const char *name, bool reading);
	~ProfileNamedScope();

private:
	const char *name_;
	double start_ = 0.0;
	bool active_ = false;
};

// Times a scope and adds it to stage/pass, when the lockdown is on.
class ProfileScope {
public:
	ProfileScope(ProfileStage stage, ProfilePass pass);
	~ProfileScope();

private:
	ProfileStage stage_;
	ProfilePass pass_;
	double start_ = 0.0;
	bool active_ = false;
};

}  // namespace Rollback
