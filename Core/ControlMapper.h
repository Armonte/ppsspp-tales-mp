#pragma once

#include "Common/Input/InputState.h"
#include "Core/KeyMap.h"

#include "Core/HLE/sceCtrl.h"   // NUM_VIRTUAL_PADS
#include <functional>
#include <cstring>
#include <mutex>
#include <vector>

struct DisplayLayoutConfig;

// Pure interface.
class ControlListener {
public:
	virtual ~ControlListener() = default;
	virtual void OnVKey(VirtKey vkey, bool down) {}
	virtual void OnVKeyAnalog(VirtKey vkey, float value) {}
	// padIndex: 0 for the real PSP pad (sceCtrl-visible), 1..NUM_VIRTUAL_PADS-1
	// for the virtual extra pads mirrored at 0x0E000000.
	virtual void UpdatePSPButtons(int padIndex, uint32_t buttonMask, uint32_t changedMask) {}
	virtual void SetPSPAnalog(int padIndex, int rotation, int stick, float x, float y) {}
	virtual void SetRawAnalog(int stick, float x, float y) {}
};

// Utilities for mapping input events to PSP inputs and virtual keys.
// Main use is of course from EmuScreen.cpp, but also useful from control settings etc.
class ControlMapper {
public:
	void UpdateConfig(const DisplayLayoutConfig &config);
	void UpdateAutoMovements(double now);

	// Inputs to the table-based mapping
	// These functions are free-threaded.
	bool Key(const KeyInput &key, bool *pauseTrigger);
	void Axis(const AxisInput *axes, size_t count);

	// Required callbacks.
	// TODO: These are so many now that a virtual interface might be more appropriate..
	void AddListener(ControlListener *listener) {
		listeners_.push_back(listener);
	}
	void RemoveListener(ControlListener *listener);

	// Inject raw PSP key input directly, such as from touch screen controls.
	// Combined with the mapped input. Unlike __Ctrl APIs, this supports
	// virtual key codes, including analog mappings.
	void PSPKey(int deviceId, int pspKeyCode, KeyInputFlags flags);

	// Toggle swapping DPAD and Analog. Useful on some input devices with few buttons.
	void ToggleSwapAxes();

	// Call this when a Vkey press triggers leaving the screen you're using the controlmapper on. This can cause
	// the loss of key-up events, which will confuse things later when you're back.
	// Might replace this later by allowing through "key-up" and similar events to lower screens.
	void ForceReleaseVKey(int vkey);

	// Call when the emu screen gets pushed behind some other screen, like the pause screen, to release all "down" inputs.
	void ReleaseAll();

	void GetDebugString(char *buffer, size_t bufSize) const;

	struct InputSample {
		float value;
		double timestamp;
	};

private:
	void UpdateSwapAxes();
	bool UpdatePSPState(const InputMapping &changedMapping, double now);
	float MapAxisValue(float value, int vkId, const InputMapping &mapping, const InputMapping &changedMapping, bool *oppositeTouched);
	void SwapMappingIfEnabled(uint32_t *vkey);

	void SetPSPAxis(int deviceId, int padIndex, int stick, char axis, float value);
	void UpdateAnalogOutput(int padIndex, int stick);

	void onVKey(VirtKey vkey, bool down);
	// `padIndex` selects which PSP pad slot this analog event drives.
	// Virtkey-driven analog inputs (where a stick is mapped via
	// VIRTKEY_AXIS_X_MIN/MAX etc) used to be hardcoded to pad 0; this
	// param closes that hole so a P2 controller's stick reaches P2.
	void onVKeyAnalog(int deviceId, int padIndex, VirtKey vkey, float value);

	void UpdateCurInputAxis(const InputMapping &mapping, float value, double timestamp);
	float GetDeviceAxisThreshold(int device, const InputMapping &mapping);

	// "Is this virtkey currently on for ANY pad?" Used by system-level
	// vkey callers (FASTFORWARD, PAUSE, ANALOG_LIGHTLY etc) where the
	// answer is "any controller pressed it" regardless of pad index.
	bool IsVirtKeyOn(VirtKey key) const {
		int index = key - VIRTKEY_FIRST;
		if (index < 0 || index >= VIRTKEY_COUNT) {
			return false;
		}
		for (int pad = 0; pad < NUM_VIRTUAL_PADS; ++pad) {
			if (virtKeyOn_[pad][index]) return true;
		}
		return false;
	}

	// To track mappable virtual keys. We can have as many as we want.
	// Indexed by [padIndex][vkey_index]. Per-pad because analog stick
	// virtkeys need separate state per virtual PSP pad — without this,
	// pad 2's stick deflection would get summed with pad 0's and dumped
	// onto whichever single pad was last fixed by the dispatcher.
	float virtKeys_[NUM_VIRTUAL_PADS][VIRTKEY_COUNT]{};
	bool virtKeyOn_[NUM_VIRTUAL_PADS][VIRTKEY_COUNT]{};  // Track boolean output separately since thresholds may differ.

	// This is only used for co-axis (analog stick to buttons), so not bothering to track separately
	// per device.
	float rawAxisValue_[JOYSTICK_AXIS_MAX]{};

	double deviceTimestamps_[(size_t)DEVICE_ID_COUNT]{};

	int lastNonDeadzoneDeviceID_[2]{};

	// [padIndex][stick][X/Y]. Pad 0 is the real PSP pad; pads 1..3 are virtual
	// (driven only by per-binding padIndex in MultiInputMapping).
	float history_[NUM_VIRTUAL_PADS][2][2]{};
	float converted_[NUM_VIRTUAL_PADS][2][2]{};  // for debug display

	// Mappable auto-rotation. Useful for keyboard/dpad->analog in a few games.
	bool autoRotatingAnalogCW_ = false;
	bool autoRotatingAnalogCCW_ = false;

	bool swapAxes_ = false;

	int iInternalScreenRotationCached_ = 0;

	// Protects basically all the state.
	// TODO: Maybe we should piggyback on the screenmanager mutex - it's always locked
	// when events come in here.
	std::mutex mutex_;

	std::map<InputMapping, InputSample> curInput_;

	// Callbacks
	std::vector<ControlListener *> listeners_;
};

void ConvertAnalogStick(float x, float y, float *outX, float *outY);
float GetDeviceAxisThreshold(int device, const InputMapping &mapping);

extern ControlMapper g_controlMapper;
