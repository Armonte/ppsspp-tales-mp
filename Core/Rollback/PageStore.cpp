#include "Core/Rollback/PageStore.h"

#include <climits>
#include <cstring>

#include "Common/Log.h"
#include "Core/MemMap.h"
#include "ext/xxhash.h"

namespace Rollback {

namespace {

constexpr int kBaseFrame = INT_MIN;
// Slot storage grows in chunks of this many pages.
constexpr size_t kSlotChunk = 1024;

uint64_t HashPage(const uint8_t *mem) {
	return XXH3_64bits(mem, PageStore::kPageSize);
}

}  // namespace

uint32_t PageStore::AllocSlot() {
	if (freeSlots_.empty()) {
		const size_t oldSlots = slotData_.size() / kPageSize;
		slotData_.resize((oldSlots + kSlotChunk) * kPageSize);
		for (size_t i = oldSlots + kSlotChunk; i > oldSlots; i--)
			freeSlots_.push_back((uint32_t)(i - 1));
	}
	const uint32_t slot = freeSlots_.back();
	freeSlots_.pop_back();
	storedVersions_++;
	return slot;
}

void PageStore::FreeSlot(uint32_t slot) {
	freeSlots_.push_back(slot);
	storedVersions_--;
}

void PageStore::Reset() {
	pages_.clear();
	slotData_.clear();
	freeSlots_.clear();
	storedVersions_ = 0;

	struct Region {
		uint32_t addr;
		uint32_t size;
	};
	const Region regions[] = {
		{ PSP_GetKernelMemoryBase(), Memory::g_MemorySize },
		{ PSP_GetVidMemBase(), Memory::VRAM_SIZE },
	};
	for (const Region &r : regions) {
		uint8_t *base = Memory::GetPointerWriteUnchecked(r.addr);
		for (uint32_t off = 0; off < r.size; off += kPageSize) {
			Page page;
			page.mem = base + off;
			page.hash = HashPage(page.mem);
			const uint32_t slot = AllocSlot();
			memcpy(SlotData(slot), page.mem, kPageSize);
			page.versions.push_back(Version{ kBaseFrame, page.hash, slot });
			pages_.push_back(std::move(page));
		}
	}
	INFO_LOG(Log::SaveState, "Rollback: page store tracks %zu pages (%zu KiB)", pages_.size(), pages_.size() * kPageSize / 1024);
}

uint64_t PageStore::Save(int frame) {
	lastSavedPages_ = 0;
	for (Page &page : pages_) {
		const uint64_t h = HashPage(page.mem);
		if (h != page.hash) {
			if (page.versions.back().frame == frame) {
				// Saved twice without a load in between: the newer content wins.
				page.versions.back().hash = h;
				memcpy(SlotData(page.versions.back().slot), page.mem, kPageSize);
			} else {
				const uint32_t slot = AllocSlot();
				memcpy(SlotData(slot), page.mem, kPageSize);
				page.versions.push_back(Version{ frame, h, slot });
			}
			page.hash = h;
			lastSavedPages_++;
		}
	}
	hashScratch_.resize(pages_.size());
	for (size_t i = 0; i < pages_.size(); i++)
		hashScratch_[i] = pages_[i].hash;
	return XXH3_64bits(hashScratch_.data(), hashScratch_.size() * sizeof(uint64_t));
}

bool PageStore::Load(int frame) {
	lastLoadedPages_ = 0;
	bool ok = true;
	for (Page &page : pages_) {
		while (page.versions.size() > 1 && page.versions.back().frame > frame) {
			FreeSlot(page.versions.back().slot);
			page.versions.pop_back();
		}
		const Version &target = page.versions.back();
		if (target.frame > frame) {
			// Only the base is left and it's newer than the frame: trimmed away.
			ok = false;
			continue;
		}
		// Hash the live page too: it may have changed since the last save.
		if (HashPage(page.mem) != target.hash) {
			memcpy(page.mem, SlotData(target.slot), kPageSize);
			lastLoadedPages_++;
		}
		page.hash = target.hash;
	}
	if (!ok)
		ERROR_LOG(Log::SaveState, "Rollback: page store can't go back to frame %d (trimmed)", frame);
	return ok;
}

void PageStore::Trim(int oldestFrame) {
	for (Page &page : pages_) {
		// Keep the newest version at or before oldestFrame as the page's base.
		size_t keepFrom = 0;
		for (size_t i = 1; i < page.versions.size() && page.versions[i].frame <= oldestFrame; i++)
			keepFrom = i;
		for (size_t i = 0; i < keepFrom; i++)
			FreeSlot(page.versions[i].slot);
		if (keepFrom)
			page.versions.erase(page.versions.begin(), page.versions.begin() + keepFrom);
	}
}

}  // namespace Rollback
