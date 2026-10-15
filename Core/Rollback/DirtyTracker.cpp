#include "ppsspp_config.h"

#include "Core/Rollback/DirtyTracker.h"

#include <atomic>
#include <cstring>

#include "Common/Log.h"
#include "Core/MemMap.h"

#ifdef _WIN32
#include "Common/CommonWindows.h"
#else
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace Rollback {

namespace {

constexpr size_t kPageSize = 4096;

DirtyTracker *g_tracker = nullptr;
std::atomic<uint64_t> g_faults{ 0 };

void SetBit(std::vector<uint64_t> &bits, size_t index) {
#ifdef _MSC_VER
	_InterlockedOr64((volatile long long *)&bits[index / 64], (long long)(1ULL << (index % 64)));
#else
	__atomic_fetch_or(&bits[index / 64], 1ULL << (index % 64), __ATOMIC_RELAXED);
#endif
}

bool TestBit(const std::vector<uint64_t> &bits, size_t index) {
	return (bits[index / 64] >> (index % 64)) & 1;
}

#ifdef _WIN32
PVOID g_handlerHandle = nullptr;

LONG CALLBACK WriteFaultHandler(PEXCEPTION_POINTERS info) {
	const EXCEPTION_RECORD *record = info->ExceptionRecord;
	// ExceptionInformation[0] is 1 for a write.
	if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2 || record->ExceptionInformation[0] != 1)
		return EXCEPTION_CONTINUE_SEARCH;
	DirtyTracker *tracker = g_tracker;
	if (tracker && tracker->HandleWriteFault((uintptr_t)record->ExceptionInformation[1]))
		return EXCEPTION_CONTINUE_EXECUTION;
	return EXCEPTION_CONTINUE_SEARCH;
}
#elif !PPSSPP_PLATFORM(MAC) && !PPSSPP_PLATFORM(IOS)
struct sigaction g_previousAction;
bool g_handlerInstalled = false;

void WriteFaultSignal(int sig, siginfo_t *info, void *context) {
	DirtyTracker *tracker = g_tracker;
	// A tracked page is readable, so any fault on one (SEGV_ACCERR) is a write.
	if (tracker && info->si_code == SEGV_ACCERR && tracker->HandleWriteFault((uintptr_t)info->si_addr))
		return;
	if (g_previousAction.sa_flags & SA_SIGINFO) {
		g_previousAction.sa_sigaction(sig, info, context);
	} else if (g_previousAction.sa_handler == SIG_DFL || g_previousAction.sa_handler == SIG_IGN) {
		// Let the default action happen: reinstall it and return to re-raise on the same access.
		sigaction(SIGSEGV, &g_previousAction, nullptr);
	} else {
		g_previousAction.sa_handler(sig);
	}
}
#endif

}  // namespace

DirtyTracker::~DirtyTracker() {
	Disarm();
}

bool DirtyTracker::Protect(uint8_t *host, size_t size, bool writable) {
#ifdef _WIN32
	DWORD old;
	return VirtualProtect(host, size, writable ? PAGE_READWRITE : PAGE_READONLY, &old) != 0;
#elif !PPSSPP_PLATFORM(MAC) && !PPSSPP_PLATFORM(IOS)
	return mprotect(host, size, writable ? (PROT_READ | PROT_WRITE) : PROT_READ) == 0;
#else
	return false;
#endif
}

bool DirtyTracker::Arm() {
	if (armed_)
		return true;
#if PPSSPP_PLATFORM(MAC) || PPSSPP_PLATFORM(IOS)
	return false;
#else
#ifndef _WIN32
	if (sysconf(_SC_PAGESIZE) != (long)kPageSize)
		return false;
#endif
	// Group the views into RAM and VRAM by the memory they show (their arena offset).
	const std::vector<Memory::MappedView> mapped = Memory::GetMappedViews();
	auto arenaOf = [&](uint32_t guestAddress, size_t *offset) {
		for (const auto &v : mapped) {
			if (v.guestAddress == guestAddress) {
				*offset = v.arenaOffset;
				return true;
			}
		}
		return false;
	};
	struct Segment {
		uint32_t guestAddress;
		uint32_t size;
	};
	const Segment segments[] = {
		{ PSP_GetKernelMemoryBase(), Memory::g_MemorySize },
		{ PSP_GetVidMemBase(), Memory::VRAM_SIZE },
	};
	views_.clear();
	numPages_ = 0;
	for (const Segment &seg : segments) {
		size_t arena;
		if (!arenaOf(seg.guestAddress, &arena)) {
			ERROR_LOG(Log::MemMap, "Rollback: no view at %08x, not tracking dirty pages", seg.guestAddress);
			return false;
		}
		for (const auto &v : mapped) {
			// A view shows this segment if it starts inside it in the arena (VRAM has 2 MiB mirrors of 2 MiB).
			if (v.arenaOffset < arena || v.arenaOffset >= arena + seg.size)
				continue;
			const uint32_t size = std::min<uint32_t>(v.size, seg.size - (uint32_t)(v.arenaOffset - arena));
			View view{ v.host, size, (uint32_t)(numPages_ + (v.arenaOffset - arena) / kPageSize), {} };
			view.writable.assign((size / kPageSize + 63) / 64, 0);
			views_.push_back(std::move(view));
		}
		numPages_ += seg.size / kPageSize;
	}
	dirty_.assign((numPages_ + 63) / 64, 0);

	g_tracker = this;
#ifdef _WIN32
	g_handlerHandle = AddVectoredExceptionHandler(1, WriteFaultHandler);
	if (!g_handlerHandle) {
		g_tracker = nullptr;
		return false;
	}
#else
	struct sigaction action {};
	action.sa_sigaction = WriteFaultSignal;
	action.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGSEGV, &action, &g_previousAction) != 0) {
		g_tracker = nullptr;
		return false;
	}
	g_handlerInstalled = true;
#endif

	for (View &view : views_) {
		if (!Protect(view.host, view.size, false)) {
			ERROR_LOG(Log::MemMap, "Rollback: couldn't protect a view of guest memory, not tracking dirty pages");
			Disarm();
			return false;
		}
	}
	armed_ = true;
	g_faults = 0;
	INFO_LOG(Log::MemMap, "Rollback: tracking writes to %zu pages through %zu views", numPages_, views_.size());
	return true;
#endif
}

void DirtyTracker::Disarm() {
	for (View &view : views_)
		Protect(view.host, view.size, true);
#ifdef _WIN32
	if (g_handlerHandle) {
		RemoveVectoredExceptionHandler(g_handlerHandle);
		g_handlerHandle = nullptr;
	}
#elif !PPSSPP_PLATFORM(MAC) && !PPSSPP_PLATFORM(IOS)
	if (g_handlerInstalled) {
		sigaction(SIGSEGV, &g_previousAction, nullptr);
		g_handlerInstalled = false;
	}
#endif
	if (g_tracker == this)
		g_tracker = nullptr;
	views_.clear();
	armed_ = false;
}

bool DirtyTracker::HandleWriteFault(uintptr_t address) {
	for (View &view : views_) {
		const uintptr_t start = (uintptr_t)view.host;
		if (address < start || address >= start + view.size)
			continue;
		const size_t pageInView = (address - start) / kPageSize;
		SetBit(dirty_, view.firstPage + pageInView);
		SetBit(view.writable, pageInView);
		g_faults.fetch_add(1, std::memory_order_relaxed);
		return Protect(view.host + pageInView * kPageSize, kPageSize, true);
	}
	return false;
}

uint64_t DirtyTracker::Faults() const {
	return g_faults.load(std::memory_order_relaxed);
}

void DirtyTracker::TakeDirty(std::vector<uint32_t> *pages) {
	for (size_t word = 0; word < dirty_.size(); word++) {
		uint64_t bits = dirty_[word];
		if (!bits)
			continue;
		dirty_[word] = 0;
		while (bits) {
			const int bit = __builtin_ctzll(bits);
			bits &= bits - 1;
			pages->push_back((uint32_t)(word * 64 + bit));
		}
	}
	// Protect again what the faults opened, in runs.
	for (View &view : views_) {
		const size_t viewPages = view.size / kPageSize;
		size_t runStart = SIZE_MAX;
		for (size_t page = 0; page <= viewPages; page++) {
			const bool open = page < viewPages && TestBit(view.writable, page);
			if (open && runStart == SIZE_MAX) {
				runStart = page;
			} else if (!open && runStart != SIZE_MAX) {
				Protect(view.host + runStart * kPageSize, (page - runStart) * kPageSize, false);
				runStart = SIZE_MAX;
			}
		}
		std::fill(view.writable.begin(), view.writable.end(), 0);
	}
}

}  // namespace Rollback
