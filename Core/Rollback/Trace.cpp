#include "Core/Rollback/Trace.h"

#include <cstdarg>
#include <cstdio>

#include "Core/CoreTiming.h"
#include "Core/MIPS/MIPS.h"

namespace Rollback {

bool g_tracing = false;

static std::string g_trace;

void TraceBegin() {
	g_trace.clear();
	g_tracing = true;
}

void TraceLine(const char *format, ...) {
	char line[512];
	int len = snprintf(line, sizeof(line), "%llu ", (unsigned long long)CoreTiming::GetTicks(currentMIPS));
	va_list args;
	va_start(args, format);
	vsnprintf(line + len, sizeof(line) - len, format, args);
	va_end(args);
	g_trace += line;
	g_trace += '\n';
}

std::string TraceEnd() {
	g_tracing = false;
	return std::move(g_trace);
}

}  // namespace Rollback
