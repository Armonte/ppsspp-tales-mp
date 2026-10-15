#include "Core/Rollback/NetplayState.h"

#include <algorithm>
#include <string>

#include "Common/Log.h"
#include "Common/TimeUtil.h"
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

uint32_t NetplayState::Checksum(const uint8_t *state, size_t stateLen) {
	const uint64_t h = XXH3_64bits(state, stateLen);
	return (uint32_t)(h ^ (h >> 32));
}

unsigned int NetplayState::ChecksumCallback(void *user, int frame, const unsigned char *state, unsigned int stateLen) {
	return Checksum(state, stateLen);
}

}  // namespace Rollback
