#include "ppsspp_config.h"

#include "headless/SampleProfiler.h"

#ifdef _WIN32

#include <algorithm>
#include <atomic>
#include <map>
#include <thread>
#include <vector>

#include "Common/CommonWindows.h"
#include "Common/StringUtils.h"

#include <dbghelp.h>
#include <timeapi.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")

namespace SampleProfiler {

static HANDLE g_target;
static std::thread g_sampler;
static std::atomic<bool> g_running{ false };
// Each sample: the instruction pointer, then return addresses unwound from it, then 0.
static std::vector<DWORD64> g_samples;
static size_t g_sampleCount;
constexpr int kMaxFrames = 32;

// Unwinds through code with unwind info (everything but the jit's output, where it stops).
static void RecordSample(CONTEXT ctx) {
	g_samples.push_back(ctx.Rip);
	for (int i = 1; i < kMaxFrames; i++) {
		DWORD64 imageBase = 0;
		PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
		if (!function)
			break;
		void *handlerData = nullptr;
		DWORD64 establisherFrame = 0;
		RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, function, &ctx, &handlerData, &establisherFrame, nullptr);
		if (!ctx.Rip)
			break;
		g_samples.push_back(ctx.Rip);
	}
	g_samples.push_back(0);
	g_sampleCount++;
}

bool Start() {
	if (g_running)
		return false;
	if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_target,
			THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
		return false;
	g_samples.clear();
	g_samples.reserve(1 << 24);
	g_sampleCount = 0;
	g_running = true;
	g_sampler = std::thread([] {
		timeBeginPeriod(1);
		while (g_running) {
			if (SuspendThread(g_target) != (DWORD)-1) {
				CONTEXT ctx{};
				ctx.ContextFlags = CONTEXT_FULL;
				const bool got = GetThreadContext(g_target, &ctx) != 0;
				// Unwinding reads the suspended thread's stack, so do it before resuming.
				if (got)
					RecordSample(ctx);
				ResumeThread(g_target);
			}
			Sleep(1);
		}
		timeEndPeriod(1);
	});
	return true;
}

std::string StopAndReport(int topCount) {
	if (!g_running)
		return "";
	g_running = false;
	g_sampler.join();
	CloseHandle(g_target);

	HANDLE process = GetCurrentProcess();
	SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
	SymInitialize(process, nullptr, TRUE);

	std::map<DWORD64, std::string> names;
	auto nameOf = [&](DWORD64 rip) -> const std::string & {
		auto it = names.find(rip);
		if (it != names.end())
			return it->second;
		std::string name;
		if (SymGetModuleBase64(process, rip) == 0) {
			name = "[jit code]";
		} else {
			alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
			SYMBOL_INFO *symbol = (SYMBOL_INFO *)buffer;
			symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
			symbol->MaxNameLen = 511;
			DWORD64 displacement = 0;
			name = SymFromAddr(process, rip, &displacement, symbol) ? std::string(symbol->Name) : StringFromFormat("?%llx", (unsigned long long)rip);
		}
		return names.emplace(rip, name).first->second;
	};
	std::map<std::string, int> self, inclusive;
	std::vector<const std::string *> stack;
	for (size_t i = 0; i < g_samples.size(); i++) {
		if (g_samples[i] == 0) {
			if (!stack.empty())
				self[*stack[0]]++;
			std::sort(stack.begin(), stack.end());
			stack.erase(std::unique(stack.begin(), stack.end()), stack.end());
			for (const std::string *name : stack)
				inclusive[*name]++;
			stack.clear();
			continue;
		}
		// Return addresses point after the call; step back into it for the right symbol.
		const bool leaf = i == 0 || g_samples[i - 1] == 0;
		stack.push_back(&nameOf(leaf ? g_samples[i] : g_samples[i] - 1));
	}
	SymCleanup(process);

	auto top = [&](const std::map<std::string, int> &counts, const char *title) {
		std::vector<std::pair<int, std::string>> sorted;
		for (auto &[name, count] : counts)
			sorted.emplace_back(count, name);
		std::sort(sorted.rbegin(), sorted.rend());
		const double total = (double)std::max<size_t>(g_sampleCount, 1);
		std::string out = StringFromFormat("%s (%zu samples):\n", title, g_sampleCount);
		for (int i = 0; i < (int)sorted.size() && i < topCount; i++)
			out += StringFromFormat("%6.2f%% %6d  %s\n", 100.0 * sorted[i].first / total, sorted[i].first, sorted[i].second.c_str());
		return out;
	};
	std::string report = top(self, "Sample profile, self") + top(inclusive, "Sample profile, inclusive");
	return report;
}

}  // namespace SampleProfiler

#else

namespace SampleProfiler {
bool Start() { return false; }
std::string StopAndReport(int) { return ""; }
}

#endif
