#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Finds the RAM/VRAM pages written since the last check, for the rollback page store, without hashing.
//
// Every host mapping of guest RAM and VRAM is made read-only (PPSSPP maps each mirror - cached/uncached,
// user/kernel - as its own view of the same memory, so all of them). The first write to a page faults; the fault
// handler marks the page dirty and makes it writable in the view that faulted, and the write goes ahead. TakeDirty()
// hands over the dirty pages and makes them read-only again. Windows uses a vectored exception handler registered
// ahead of PPSSPP's own; elsewhere a SIGSEGV handler that passes everything it doesn't own to the previous one.
//
// Pages written in several saves in a row become hot: they stay writable and are reported dirty every time
// (the page store's hash tells whether they really changed), which saves a fault and a re-protect per page per
// frame for the stack and the game's per-frame variables. A hot page that stops changing is protected again.
//
// Pages are numbered like PageStore's: RAM (from 0x08000000) first, then VRAM (from 0x04000000).

namespace Rollback {

class DirtyTracker {
public:
	~DirtyTracker();

	// Protects the views and installs the fault handler. False if that isn't possible here (callers then hash).
	bool Arm();
	void Disarm();
	bool Armed() const { return armed_; }

	// Appends the pages written since the last call, and the hot pages, to pages (ascending); clears them and
	// protects them again (except hot ones).
	void TakeDirty(std::vector<uint32_t> *pages);
	// Whether the page store found a page from TakeDirty changed: a hot page that keeps not changing goes cold.
	void NoteSaved(uint32_t page, bool changed);
	size_t HotPages() const { return hotCount_; }

	// Faults handled since Arm().
	uint64_t Faults() const;

	// Called from the fault handler: true if the address was a write to a tracked page, now made writable.
	bool HandleWriteFault(uintptr_t address);

private:
	struct View {
		uint8_t *host;
		uint32_t size;
		uint32_t firstPage;  // page number of the view's first page
		std::vector<uint64_t> writable;  // pages this view has made writable, bit per page in the view
	};

	bool Protect(uint8_t *host, size_t size, bool writable);

	std::vector<View> views_;
	size_t numPages_ = 0;
	std::vector<uint64_t> dirty_;  // bit per page number
	std::vector<uint64_t> hot_;  // bit per page number
	std::vector<uint8_t> dirtyStreak_;  // consecutive TakeDirty calls a page was written in
	std::vector<uint8_t> unchangedStreak_;  // consecutive NoteUnchanged calls for a hot page
	size_t hotCount_ = 0;
	bool armed_ = false;
};

}  // namespace Rollback
