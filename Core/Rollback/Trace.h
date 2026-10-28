#pragma once

#include <string>

// Rollback desync diagnostics: a timeline of one frame (syscalls, thread switches, timing events, each with the
// CPU tick it happened at), recorded on its first run and on every re-simulation, so the passes can be diffed
// to find where they part. Enabled by PPSSPP_RB_TRACE_FRAME=<frame> with --rollback-diag (see NetplaySession).

namespace Rollback {

extern bool g_tracing;

inline bool Tracing() {
	return g_tracing;
}

void TraceBegin();
void TraceLine(const char *format, ...)
#ifdef __GNUC__
	__attribute__((format(printf, 1, 2)))
#endif
	;
// Ends the trace and returns it.
std::string TraceEnd();

}  // namespace Rollback
