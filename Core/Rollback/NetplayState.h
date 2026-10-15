#pragma once

#include <cstddef>
#include <cstdint>

// Saves and loads the emulated PSP for GekkoNet's save/load events.
//
// GekkoNet allocates every state buffer with one fixed size (GekkoConfig::state_size), so Initialize() measures
// the current state once and adds headroom. A save writes a full savestate (SaveState::SaveToBuffer) into the
// event's buffer; a load reads it back. Checksums are lazy: GekkoNet asks for one only for the frames it compares.

namespace Rollback {

class NetplayState {
public:
	// Measures the current state and fixes the buffer size GekkoNet must use. Call with the game running.
	bool Initialize();

	uint32_t StateSize() const { return stateSize_; }
	// Largest state written so far, against StateSize().
	size_t HighWatermark() const { return highWatermark_; }
	// Average milliseconds per save / load since Initialize().
	double AverageSaveMs() const { return saves_ ? saveMsTotal_ / saves_ : 0.0; }
	double AverageLoadMs() const { return loads_ ? loadMsTotal_ / loads_ : 0.0; }

	// GekkoSaveEvent: write the current state into state (capacity StateSize()), set *stateLen.
	bool Save(int frame, uint8_t *state, unsigned int *stateLen, unsigned int *checksum);
	// GekkoLoadEvent.
	bool Load(int frame, const uint8_t *state, unsigned int stateLen);

	// The checksum GekkoNet compares between peers (gekko_set_checksum_callback).
	static unsigned int ChecksumCallback(void *user, int frame, const unsigned char *state, unsigned int stateLen);
	static uint32_t Checksum(const uint8_t *state, size_t stateLen);

private:
	uint32_t stateSize_ = 0;
	size_t highWatermark_ = 0;
	double saveMsTotal_ = 0.0;
	double loadMsTotal_ = 0.0;
	int saves_ = 0;
	int loads_ = 0;
};

}  // namespace Rollback
