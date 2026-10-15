#include "Core/Rollback/NetplayState.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "Common/File/FileUtil.h"
#include "Common/Log.h"
#include "Common/StringUtils.h"
#include "Common/TimeUtil.h"
#include "Core/Debugger/MemBlockInfo.h"
#include "Core/SaveState.h"
#include "ext/xxhash.h"

namespace Rollback {

namespace {

// The state grows a little during play (kernel objects, file handles, ...); this is the margin over the
// measured size. A save that outgrows it fails loudly instead of corrupting GekkoNet's buffers.
constexpr size_t kStateHeadroom = 4 * 1024 * 1024;
constexpr size_t kStateAlign = 1024 * 1024;

}  // namespace

bool NetplayState::Initialize() {
	const size_t measured = SaveState::MeasureSaveSize();
	if (measured == 0) {
		ERROR_LOG(Log::SaveState, "Rollback: could not measure the savestate size");
		return false;
	}
	const size_t size = (measured + kStateHeadroom + kStateAlign - 1) / kStateAlign * kStateAlign;
	stateSize_ = (uint32_t)size;
	highWatermark_ = 0;
	saveMsTotal_ = loadMsTotal_ = 0.0;
	saves_ = loads_ = 0;
	INFO_LOG(Log::SaveState, "Rollback: state measured at %zu bytes, buffers are %u bytes", measured, stateSize_);
	return true;
}

bool NetplayState::Save(int frame, uint8_t *state, unsigned int *stateLen, unsigned int *checksum) {
	const double start = time_now_d();
	size_t saved = 0;
	const auto err = SaveState::SaveToBuffer(state, stateSize_, &saved);
	saveMsTotal_ += (time_now_d() - start) * 1000.0;
	saves_++;
	if (err != CChunkFileReader::ERROR_NONE) {
		ERROR_LOG(Log::SaveState, "Rollback: save of frame %d failed (%d)%s", frame, (int)err,
			err == CChunkFileReader::ERROR_BAD_ALLOC ? ", the state outgrew the buffer" : "");
		*stateLen = 0;
		return false;
	}
	*stateLen = (unsigned int)saved;
	highWatermark_ = std::max(highWatermark_, saved);
	if (diag_)
		DiagCheck(frame, state, saved);
	// Lazy checksums: GekkoNet calls ChecksumCallback for the frames it compares.
	if (checksum)
		*checksum = 0;
	return true;
}

bool NetplayState::Load(int frame, const uint8_t *state, unsigned int stateLen) {
	const double start = time_now_d();
	std::string errorString;
	const auto err = SaveState::LoadFromBuffer(state, stateLen, &errorString);
	loadMsTotal_ += (time_now_d() - start) * 1000.0;
	loads_++;
	if (err != CChunkFileReader::ERROR_NONE) {
		ERROR_LOG(Log::SaveState, "Rollback: load of frame %d failed: %s", frame, errorString.c_str());
		return false;
	}
	return true;
}

void NetplayState::EnableDiagnostics(const std::string &dumpDir, int keepFrames) {
	// Record HLE writes to memory in detail, so a mismatch in RAM can name what wrote there.
	if (!diag_)
		MemBlockOverrideDetailed();
	diag_ = true;
	diagDir_ = dumpDir;
	diagKeep_ = keepFrames;
}

void NetplayState::DiagCheck(int frame, const uint8_t *state, size_t len) {
	auto it = diagFirstSaves_.find(frame);
	if (it == diagFirstSaves_.end()) {
		diagFirstSaves_[frame].assign(state, state + len);
		while ((int)diagFirstSaves_.size() > diagKeep_)
			diagFirstSaves_.erase(diagFirstSaves_.begin());
		return;
	}
	const std::vector<uint8_t> &first = it->second;
	if (diagMismatchFrame_ >= 0 || (first.size() == len && memcmp(first.data(), state, len) == 0))
		return;

	size_t diff = 0;
	const size_t common = std::min(first.size(), len);
	while (diff < common && first[diff] == state[diff])
		diff++;
	diagMismatchFrame_ = frame;
	diagFirstDiff_ = diff;
	ERROR_LOG(Log::SaveState, "Rollback diag: re-simulated frame %d differs from its first run (sizes %zu/%zu, first difference at +%zu)",
		frame, first.size(), len, diff);

	auto dump = [&](const char *which, const uint8_t *data, size_t size) {
		const std::string path = StringFromFormat("%s/rollback_desync_f%d_%s.bin", diagDir_.c_str(), frame, which);
		if (FILE *f = File::OpenCFile(Path(path), "wb")) {
			fwrite(data, 1, size, f);
			fclose(f);
			INFO_LOG(Log::SaveState, "Rollback diag: wrote %s", path.c_str());
		}
	};
	dump("first", first.data(), first.size());
	dump("resim", state, len);
	LogRamWriters(first.data(), state, std::min(first.size(), len));
}

// Name the recorded writers of the RAM ranges that differ. The Memory section starts with its 16-byte title, the
// section version, g_PSPModel, a marker and g_MemorySize; RAM (from 0x08000000) follows.
void NetplayState::LogRamWriters(const uint8_t *a, const uint8_t *b, size_t len) {
	static const char title[16] = "Memory";
	const uint8_t *found = (const uint8_t *)std::search(a, a + std::min<size_t>(len, 4096), title, title + sizeof(title));
	if (found == a + std::min<size_t>(len, 4096))
		return;
	const size_t marker = found - a;
	uint32_t memSize;
	memcpy(&memSize, a + marker + 28, sizeof(memSize));
	const size_t ramStart = marker + 32;
	if (ramStart + memSize > len)
		return;

	int logged = 0;
	size_t i = ramStart;
	while (i < ramStart + memSize && logged < 8) {
		if (a[i] == b[i]) {
			i++;
			continue;
		}
		// Merge differences closer than 64 bytes into one range.
		size_t end = i + 1, lastDiff = i;
		while (end < ramStart + memSize && end - lastDiff < 64) {
			if (a[end] != b[end])
				lastDiff = end;
			end++;
		}
		const uint32_t addr = 0x08000000 + (uint32_t)(i - ramStart);
		const uint32_t size = (uint32_t)(lastDiff + 1 - i);
		std::string writers;
		for (const MemBlockInfo &info : FindMemInfoByFlag(MemBlockFlags::WRITE, addr, size)) {
			writers += StringFromFormat(" [%s pc=%08x %08x+%u]", info.tag.c_str(), info.pc, info.start, info.size);
		}
		ERROR_LOG(Log::SaveState, "Rollback diag: RAM %08x..%08x differs, writers:%s", addr, addr + size, writers.empty() ? " (none recorded: CPU stores)" : writers.c_str());
		logged++;
		i = lastDiff + 1;
	}
}

uint32_t NetplayState::Checksum(const uint8_t *state, size_t stateLen) {
	const uint64_t h = XXH3_64bits(state, stateLen);
	return (uint32_t)(h ^ (h >> 32));
}

unsigned int NetplayState::ChecksumCallback(void *user, int frame, const unsigned char *state, unsigned int stateLen) {
	return Checksum(state, stateLen);
}

}  // namespace Rollback
