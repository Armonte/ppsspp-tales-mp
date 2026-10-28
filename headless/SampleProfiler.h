#pragma once

#include <string>

// Statistical CPU profile of one thread (Windows): a sampler thread suspends it every millisecond, reads its
// instruction pointer, and the report resolves those through the PDB. Code outside any module (the jit's
// output) is one bucket. For finding where emulation time goes, e.g. in rollback re-simulation.

namespace SampleProfiler {

// Starts sampling the calling thread.
bool Start();
// Stops, and returns the top functions by samples (self time).
std::string StopAndReport(int topCount);

}  // namespace SampleProfiler
