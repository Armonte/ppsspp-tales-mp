#include "Core/Rollback/Profile.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "Common/StringUtils.h"
#include "Common/TimeUtil.h"
#include "Core/Rollback/Lockdown.h"

namespace Rollback {

namespace {

double g_times[(int)ProfileStage::Count][(int)ProfilePass::Count];

const char *const g_stageNames[] = {
	"pagesSave", "pagesLoad", "header", "memory", "coretiming", "memstick", "mips", "hle", "kernel", "achievements", "fs",
};
static_assert(sizeof(g_stageNames) / sizeof(g_stageNames[0]) == (int)ProfileStage::Count, "stage names");

}  // namespace

static std::vector<std::pair<const char *, double>> g_named;

void ProfileAddNamed(const char *name, double seconds) {
	for (auto &entry : g_named) {
		if (entry.first == name) {
			entry.second += seconds;
			return;
		}
	}
	g_named.emplace_back(name, seconds);
}

std::string ProfileNamedSummary(int loads) {
	if (loads <= 0)
		return "";
	std::vector<std::pair<const char *, double>> sorted = g_named;
	std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
	std::string out;
	for (const auto &entry : sorted) {
		const double ms = entry.second * 1000.0 / loads;
		if (ms >= 0.01)
			out += StringFromFormat(" %s=%.3f", entry.first, ms);
	}
	return out;
}

ProfileNamedScope::ProfileNamedScope(const char *name, bool reading) : name_(name) {
	active_ = reading && IsLockedDown();
	if (active_)
		start_ = time_now_d();
}

ProfileNamedScope::~ProfileNamedScope() {
	if (active_)
		ProfileAddNamed(name_, time_now_d() - start_);
}

void ProfileAdd(ProfileStage stage, ProfilePass pass, double seconds) {
	g_times[(int)stage][(int)pass] += seconds;
}

std::string ProfileSummary(int saves, int loads) {
	std::string out;
	const char *const passNames[] = { "measure", "write", "read" };
	for (int pass = 0; pass < (int)ProfilePass::Count; pass++) {
		const int n = pass == (int)ProfilePass::Read ? loads : saves;
		if (n <= 0)
			continue;
		double total = 0.0;
		std::string parts;
		for (int stage = 0; stage < (int)ProfileStage::Count; stage++) {
			const double ms = g_times[stage][pass] * 1000.0 / n;
			total += ms;
			if (ms >= 0.005)
				parts += StringFromFormat(" %s=%.3f", g_stageNames[stage], ms);
		}
		out += StringFromFormat("[%s %.3fms:%s]", passNames[pass], total, parts.c_str());
	}
	return out;
}

void ProfileReset() {
	g_named.clear();
	for (auto &stage : g_times)
		for (double &t : stage)
			t = 0.0;
}

ProfileScope::ProfileScope(ProfileStage stage, ProfilePass pass) : stage_(stage), pass_(pass) {
	active_ = IsLockedDown();
	if (active_)
		start_ = time_now_d();
}

ProfileScope::~ProfileScope() {
	if (active_)
		ProfileAdd(stage_, pass_, time_now_d() - start_);
}

}  // namespace Rollback
