#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Core/Rollback/DirtyTracker.h"

// Frame-versioned copies of the PSP's RAM and VRAM, in 4 KiB pages, for rollback netplay.
//
// A full savestate copies ~34 MiB every frame. Most of it doesn't change from one frame to the next, so the
// rollback state instead keeps RAM and VRAM here: a save records only the pages that changed since the previous
// save (tagged with the frame), and a load writes back, page by page, the content each page had at the loaded
// frame - only for pages that differ from what's in memory now.
//
// Changed pages come from the DirtyTracker (write faults) where it works, otherwise from hashing every page.
// Versions newer than a loaded frame belong to the abandoned timeline and are dropped; Trim() drops versions older
// than any frame GekkoNet can still load, keeping one per page as its base.

namespace Rollback {

class PageStore {
public:
	static constexpr size_t kPageSize = 4096;

	// Takes the current RAM and VRAM as the base version of every page. trackWrites: use the DirtyTracker instead of
	// hashing (falls back to hashing if it can't arm). validate: with the tracker, also hash everything and report
	// pages the tracker missed (slow; for testing).
	void Reset(bool trackWrites, bool validate);
	bool TrackingWrites() const { return tracker_.Armed(); }
	// Pages that changed without the tracker noticing (validate mode). Must stay 0.
	uint64_t MissedPages() const { return missedPages_; }
	uint64_t Faults() const { return tracker_.Faults(); }

	// Records the pages that changed since the last Save/Load as frame's versions. Returns a hash of all pages
	// (the content as of frame), for the netplay checksum.
	uint64_t Save(int frame);
	// Writes back each page's content as of frame (which must be a saved frame >= the oldest kept one), and drops
	// the versions after it. Returns false if some page has no version that old.
	bool Load(int frame);
	// Drops versions only needed for frames before oldestFrame.
	void Trim(int oldestFrame);

	size_t NumPages() const { return pages_.size(); }
	size_t StoredVersions() const { return storedVersions_; }
	// Pages copied by the last Save / written back by the last Load.
	size_t LastSavedPages() const { return lastSavedPages_; }
	size_t LastLoadedPages() const { return lastLoadedPages_; }
	// Guest addresses of the pages the last Load wrote back.
	const std::vector<uint32_t> &LastRestoredAddresses() const { return restored_; }

private:
	struct Version {
		int frame;
		uint64_t hash;
		uint32_t slot;
	};
	struct Page {
		uint32_t addr;  // guest address
		uint8_t *mem;  // live memory
		uint8_t *writeMem;  // the same memory through a mapping the tracker doesn't protect (or mem)
		uint64_t hash;  // hash of the content as of the last Save/Load
		std::vector<Version> versions;  // ascending frame
	};

	uint32_t AllocSlot();
	void FreeSlot(uint32_t slot);
	uint8_t *SlotData(uint32_t slot) { return &slotData_[(size_t)slot * kPageSize]; }

	std::vector<Page> pages_;
	std::vector<uint8_t> slotData_;
	std::vector<uint32_t> freeSlots_;
	std::vector<uint64_t> hashScratch_;
	std::vector<uint32_t> restored_;
	std::vector<uint32_t> dirtyScratch_;
	DirtyTracker tracker_;
	struct Alias {
		uint8_t *ptr;
		uint32_t guestAddress;
		uint32_t size;
	};
	std::vector<Alias> aliases_;
	void ReleaseAliases();

public:
	~PageStore() { tracker_.Disarm(); ReleaseAliases(); }

private:
	bool validate_ = false;
	uint64_t missedPages_ = 0;

	// Records page's current content as frame's version if it changed. Returns whether it did.
	bool SavePage(Page &page, int frame);
	size_t storedVersions_ = 0;
	size_t lastSavedPages_ = 0;
	size_t lastLoadedPages_ = 0;
};

}  // namespace Rollback
