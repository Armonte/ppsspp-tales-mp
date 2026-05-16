#include <algorithm>
#include <sstream>

#include "Common/Math/math_util.h"
#include "Common/TimeUtil.h"
#include "Common/StringUtils.h"
#include "Common/Log.h"

#include "Core/HLE/sceCtrl.h"
#include "Core/KeyMap.h"
#include "Core/ControlMapper.h"
#include "Core/Config.h"
#include "Core/CoreParameter.h"
#include "Core/System.h"

using KeyMap::MultiInputMapping;

const float AXIS_BIND_THRESHOLD = 0.75f;
const float AXIS_BIND_THRESHOLD_MOUSE = 0.01f;

ControlMapper g_controlMapper;

// We reduce the threshold of some axes when another axis on the same stick is active.
// This makes it easier to hit diagonals if you bind an analog stick to four face buttons or D-Pad.
static InputAxis GetCoAxis(InputAxis axis) {
	switch (axis) {
	case JOYSTICK_AXIS_X: return JOYSTICK_AXIS_Y;
	case JOYSTICK_AXIS_Y: return JOYSTICK_AXIS_X;

		// This looks weird, but it's simply how XInput axes are mapped.
	case JOYSTICK_AXIS_Z: return JOYSTICK_AXIS_RX;
	case JOYSTICK_AXIS_RX: return JOYSTICK_AXIS_Z;

		// Not sure if these two are used.
	case JOYSTICK_AXIS_RY: return JOYSTICK_AXIS_RZ;
	case JOYSTICK_AXIS_RZ: return JOYSTICK_AXIS_RY;

	default:
		return JOYSTICK_AXIS_MAX; // invalid
	}
}

float ControlMapper::GetDeviceAxisThreshold(int device, const InputMapping &mapping) {
	if (device == DEVICE_ID_MOUSE) {
		return AXIS_BIND_THRESHOLD_MOUSE;
	}
	if (mapping.IsAxis()) {
		switch (KeyMap::GetAxisType((InputAxis)mapping.Axis(nullptr))) {
		case KeyMap::AxisType::TRIGGER:
			return g_Config.fAnalogTriggerThreshold;
		case KeyMap::AxisType::STICK:
		{
			// Co-axis processing, see GetCoAxes comment.
			const InputAxis axis = (InputAxis)mapping.Axis(nullptr);
			const InputAxis coAxis = GetCoAxis(axis);
			if (coAxis != JOYSTICK_AXIS_MAX) {
				const float absCoValue = fabsf(rawAxisValue_[(int)coAxis]);
				if (absCoValue > 0.0f) {
					// Bias down the threshold if the other axis is active.
					const float biasedThreshold = g_Config.fAnalogStickThreshold * (1.0f - absCoValue * 0.35f);
					// INFO_LOG(Log::System, "coValue: %f  threshold: %f", absCoValue, biasedThreshold);
					return biasedThreshold;
				}
			}
			// Non-adjusted threshold.
			return g_Config.fAnalogStickThreshold;
		}
		default:
			break;
		}
	}
	return AXIS_BIND_THRESHOLD;
}

static int GetOppositeVKey(int vkey) {
	switch (vkey) {
	case VIRTKEY_AXIS_X_MIN: return VIRTKEY_AXIS_X_MAX; break;
	case VIRTKEY_AXIS_X_MAX: return VIRTKEY_AXIS_X_MIN; break;
	case VIRTKEY_AXIS_Y_MIN: return VIRTKEY_AXIS_Y_MAX; break;
	case VIRTKEY_AXIS_Y_MAX: return VIRTKEY_AXIS_Y_MIN; break;
	case VIRTKEY_AXIS_RIGHT_X_MIN: return VIRTKEY_AXIS_RIGHT_X_MAX; break;
	case VIRTKEY_AXIS_RIGHT_X_MAX: return VIRTKEY_AXIS_RIGHT_X_MIN; break;
	case VIRTKEY_AXIS_RIGHT_Y_MIN: return VIRTKEY_AXIS_RIGHT_Y_MAX; break;
	case VIRTKEY_AXIS_RIGHT_Y_MAX: return VIRTKEY_AXIS_RIGHT_Y_MIN; break;
	default:
		return 0;
	}
}

static bool IsUnsignedMapping(int vkey) {
	return vkey == VIRTKEY_SPEED_ANALOG;
}

static bool IsSignedAxis(int axis) {
	switch (axis) {
	case JOYSTICK_AXIS_X:
	case JOYSTICK_AXIS_Y:
	case JOYSTICK_AXIS_Z:
	case JOYSTICK_AXIS_RX:
	case JOYSTICK_AXIS_RY:
	case JOYSTICK_AXIS_RZ:
		return true;
	default:
		return false;
	}
}

// This is applied on the circular radius, not directly on the axes.
// TODO: Share logic with tilt?

static float MapAxisValue(float v) {
	const float deadzone = g_Config.fAnalogDeadzone;
	const float invDeadzone = g_Config.fAnalogInverseDeadzone;
	const float sensitivity = g_Config.fAnalogSensitivity;
	const float sign = v >= 0.0f ? 1.0f : -1.0f;

	// Apply deadzone.
	v = Clamp((fabsf(v) - deadzone) / (1.0f - deadzone), 0.0f, 1.0f);

	// Apply sensitivity and inverse deadzone.
	if (v != 0.0f) {
		v = Clamp(invDeadzone + v * (sensitivity - invDeadzone), 0.0f, 1.0f);
	}

	return sign * v;
}

void ConvertAnalogStick(float x, float y, float *outX, float *outY) {
	const bool isCircular = g_Config.bAnalogIsCircular;

	float norm = std::max(fabsf(x), fabsf(y));
	if (norm == 0.0f) {
		*outX = x;
		*outY = y;
		return;
	}

	if (isCircular) {
		float newNorm = sqrtf(x * x + y * y);
		float factor = newNorm / norm;
		x *= factor;
		y *= factor;
		norm = newNorm;
	}

	float mappedNorm = MapAxisValue(norm);
	*outX = Clamp(x / norm * mappedNorm, -1.0f, 1.0f);
	*outY = Clamp(y / norm * mappedNorm, -1.0f, 1.0f);
}

void ControlMapper::SetPSPAxis(int device, int padIndex, int stick, char axis, float value) {
	const int axisId = axis == 'X' ? 0 : 1;
	if (stick != 0 && stick != 1) {
		return;
	}

	if (padIndex < 0 || padIndex >= NUM_VIRTUAL_PADS) return;
	float position[2];
	position[0] = history_[padIndex][stick][0];
	position[1] = history_[padIndex][stick][1];

	position[axisId] = value;

	const float x = position[0];
	const float y = position[1];

	for (auto listener : listeners_) {
		listener->SetRawAnalog(stick, x, y);
	}

	// NOTE: We need to use single-axis checks, since the other axis might be from another device,
	// so we'll add a little leeway.
	bool inDeadZone = fabsf(value) < g_Config.fAnalogDeadzone * 0.7f;

	bool ignore = false;
	if (inDeadZone && lastNonDeadzoneDeviceID_[stick] != device) {
		// Ignore this event! See issue #15465
		ignore = true;
	}

	if (!inDeadZone) {
		lastNonDeadzoneDeviceID_[stick] = device;
	}

	if (!ignore) {
		history_[padIndex][stick][axisId] = value;
		UpdateAnalogOutput(padIndex, stick);

	}
}

void ControlMapper::UpdateAnalogOutput(int padIndex, int stick) {
	float x, y;
	ConvertAnalogStick(history_[padIndex][stick][0], history_[padIndex][stick][1], &x, &y);
	// ANALOG_LIGHTLY is a system modifier — any pad holding it applies the limiter
	// to every pad's analog. IsVirtKeyOn ORs across pads internally.
	if (IsVirtKeyOn(VIRTKEY_ANALOG_LIGHTLY)) {
		x *= g_Config.fAnalogLimiterDeadzone;
		y *= g_Config.fAnalogLimiterDeadzone;
	}
	converted_[padIndex][stick][0] = x;
	converted_[padIndex][stick][1] = y;
	for (auto listener : listeners_) {
		listener->SetPSPAnalog(padIndex, iInternalScreenRotationCached_, stick, x, y);
	}
}

void ControlMapper::ForceReleaseVKey(int vkey) {
	// Note: This one is called from an onVKey_ handler, which already holds mutex_.

	KeyMap::LockMappings();
	std::vector<KeyMap::MultiInputMapping> multiMappings;
	if (KeyMap::InputMappingsFromPspButtonNoLock(vkey, &multiMappings, true)) {
		double now = time_now_d();
		for (const auto &entry : multiMappings) {
			for (const auto &mapping : entry.mappings) {
				curInput_[mapping] = { 0.0f, now };
				// Different logic for signed axes?
				UpdatePSPState(mapping, now);
			}
		}
	}
	KeyMap::UnlockMappings();
}

void ControlMapper::ReleaseAll() {
	std::vector<AxisInput> axes;
	std::vector<KeyInput> keys;

	{
		std::lock_guard<std::mutex> guard(mutex_);

		for (const auto &input : curInput_) {
			if (input.first.IsAxis()) {
				if (input.second.value != 0.0f) {
					AxisInput axis;
					axis.deviceId = input.first.deviceId;
					int dir;
					axis.axisId = (InputAxis)input.first.Axis(&dir);
					axis.value = 0.0;
					axes.push_back(axis);
				}
			} else {
				if (input.second.value != 0.0) {
					KeyInput key;
					key.deviceId = input.first.deviceId;
					key.flags = KeyInputFlags::UP;
					key.keyCode = (InputKeyCode)input.first.keyCode;
					keys.push_back(key);
				}
			}
		}
	}

	Axis(axes.data(), axes.size());;
	for (const auto &key : keys) {
		Key(key, nullptr);
	}
}


static int RotatePSPKeyCode(int x) {
	switch (x) {
	case CTRL_UP: return CTRL_RIGHT;
	case CTRL_RIGHT: return CTRL_DOWN;
	case CTRL_DOWN: return CTRL_LEFT;
	case CTRL_LEFT: return CTRL_UP;
	default:
		return x;
	}
}

// Used to decay analog values when clashing with digital ones.
static ControlMapper::InputSample ReduceMagnitude(ControlMapper::InputSample sample, double now) {
	float reduction = std::min(std::max(0.0f, (float)(now - sample.timestamp) - 2.0f), 1.0f);
	if (reduction > 0.0f) {
		sample.value *= (1.0f - reduction);
	}
	if ((sample.value > 0.0f && sample.value < 0.05f) || (sample.value < 0.0f && sample.value > -0.05f)) {
		sample.value = 0.0f;
	}
	return sample;
}

float ControlMapper::MapAxisValue(float value, int vkId, const InputMapping &mapping, const InputMapping &changedMapping, bool *oppositeTouched) {
	if (IsUnsignedMapping(vkId)) {
		// If a signed axis is mapped to an unsigned mapping,
		// convert it. This happens when mapping DirectInput triggers to analog speed,
		// for example.
		int direction = 0;
		if (IsSignedAxis(mapping.Axis(&direction))) {
			// The value has been split up into two curInput values, so we need to go fetch the other
			// and put them back together again. Kind of awkward, but at least makes the regular case simple...
			InputMapping other = mapping.FlipDirection();
			if (other == changedMapping) {
				*oppositeTouched = true;
			}
			float valueOther = curInput_[other].value;
			float signedValue = value - valueOther;
			float ranged = (signedValue + 1.0f) * 0.5f;
			if (direction == -1) {
				ranged = 1.0f - ranged;
			}
			// NOTICE_LOG(Log::System, "rawValue: %f other: %f signed: %f ranged: %f", iter->second, valueOther, signedValue, ranged);
			return ranged;
		} else {
			return value;
		}
	} else {
		return value;
	}
}

static bool IsSwappableVKey(uint32_t vkey) {
	switch (vkey) {
	case CTRL_UP:
	case CTRL_LEFT:
	case CTRL_DOWN:
	case CTRL_RIGHT:
	case VIRTKEY_AXIS_X_MIN:
	case VIRTKEY_AXIS_X_MAX:
	case VIRTKEY_AXIS_Y_MIN:
	case VIRTKEY_AXIS_Y_MAX:
		return true;
	default:
		return false;
	}
}

void ControlMapper::SwapMappingIfEnabled(uint32_t *vkey) {
	if (swapAxes_ || IsVirtKeyOn(VIRTKEY_AXIS_SWAP_HOLD)) {
		switch (*vkey) {
		case CTRL_UP: *vkey = VIRTKEY_AXIS_Y_MAX; break;
		case VIRTKEY_AXIS_Y_MAX: *vkey = CTRL_UP; break;
		case CTRL_DOWN: *vkey = VIRTKEY_AXIS_Y_MIN; break;
		case VIRTKEY_AXIS_Y_MIN: *vkey = CTRL_DOWN; break;
		case CTRL_LEFT: *vkey = VIRTKEY_AXIS_X_MIN; break;
		case VIRTKEY_AXIS_X_MIN: *vkey = CTRL_LEFT; break;
		case CTRL_RIGHT: *vkey = VIRTKEY_AXIS_X_MAX; break;
		case VIRTKEY_AXIS_X_MAX: *vkey = CTRL_RIGHT; break;
		}
	}
}

// Can only be called from Key or Axis.
// mutex_ should be locked, and also KeyMap::LockMappings().
// TODO: We should probably make a batched version of this.
bool ControlMapper::UpdatePSPState(const InputMapping &changedMapping, double now) {
	// Instead of taking an input key and finding what it outputs, we loop through the OUTPUTS and
	// see if the input that corresponds to it has a value. That way we can easily implement all sorts
	// of crazy input combos if needed.

	int rotations = 0;
	switch (iInternalScreenRotationCached_) {
	case ROTATION_LOCKED_HORIZONTAL180: rotations = 2; break;
	case ROTATION_LOCKED_VERTICAL:      rotations = 1; break;
	case ROTATION_LOCKED_VERTICAL180:   rotations = 3; break;
	}

	// Per-pad button accumulators. Pad 0 is the real PSP, 1..3 are virtual.
	uint32_t buttonMask[NUM_VIRTUAL_PADS] = {};
	uint32_t changedButtonMask[NUM_VIRTUAL_PADS] = {};
	std::vector<MultiInputMapping> inputMappings;
	for (int i = 0; i < 32; i++) {
		uint32_t mask = 1 << i;
		if (!(mask & CTRL_MASK_USER)) {
			// Not a mappable button bit
			continue;
		}

		uint32_t mappingBit = mask;
		for (int i = 0; i < rotations; i++) {
			mappingBit = RotatePSPKeyCode(mappingBit);
		}

		SwapMappingIfEnabled(&mappingBit);
		if (!KeyMap::InputMappingsFromPspButtonNoLock(mappingBit, &inputMappings, false))
			continue;

		// If a mapping could consist of a combo, we could trivially check it here.
		for (auto &multiMapping : inputMappings) {
			int pad = multiMapping.padIndex;
			if (pad < 0 || pad >= NUM_VIRTUAL_PADS) continue;
			// Check if the changed mapping was involved in this PSP key.
			if (multiMapping.mappings.contains(changedMapping)) {
				changedButtonMask[pad] |= mask;
			}
			// Check if all inputs are "on".
			bool all = true;
			double curTime = 0.0;
			for (const auto &mapping : multiMapping.mappings) {
				auto iter = curInput_.find(mapping);
				if (iter == curInput_.end()) {
					all = false;
					continue;
				}
				// Stop reverse ordering from triggering.
				if (g_Config.bStrictComboOrder && iter->second.timestamp < curTime) {
					all = false;
					break;
				} else {
					curTime = iter->second.timestamp;
				}
				bool down = iter->second.value > 0.0f && iter->second.value > GetDeviceAxisThreshold(iter->first.deviceId, mapping);
				if (!down)
					all = false;
			}
			if (all) buttonMask[pad] |= mask;
		}
	}

	// We only request changing the buttons where the mapped input was involved.
	for (int p = 0; p < NUM_VIRTUAL_PADS; ++p) {
		if (!changedButtonMask[p]) continue;
		for (auto listener : listeners_) {
			listener->UpdatePSPButtons(p,
				buttonMask[p] & changedButtonMask[p],
				(~buttonMask[p]) & changedButtonMask[p]);
		}
	}

	bool keyInputUsed = false;
	for (int p = 0; p < NUM_VIRTUAL_PADS; ++p) {
		if (changedButtonMask[p]) { keyInputUsed = true; break; }
	}
	bool updateAnalogSticks = false;

	// OK, handle all the virtual keys next. For these we need to do deltas here and send events.
	// Note that virtual keys include the analog directions, as they are driven by them.
	for (int i = 0; i < VIRTKEY_COUNT; i++) {
		VirtKey vkId = (VirtKey)(i + VIRTKEY_FIRST);

		uint32_t idForMapping = vkId;
		SwapMappingIfEnabled(&idForMapping);

		if (!KeyMap::InputMappingsFromPspButtonNoLock(idForMapping, &inputMappings, false))
			continue;

		// If a mapping could consist of a combo, we could trivially check it here.
		// Save the first device ID so we can pass it into onVKeyDown, which in turn needs it for the analog
		// mapping which gets a little hacky.
		//
		// Per-pad bucketing: previously the inner loop summed every
		// multiMapping's product into a single `value` regardless of
		// multiMapping.padIndex, then dispatched to pad 0. That meant a
		// P2 binding's analog value got mixed with P1's contributions and
		// dumped onto P1's analog stick. Now we accumulate per pad and
		// dispatch per pad. Threshold stays single-valued since it's per-
		// mapping (axis-driven), not per-pad.
		float threshold = 1.0f;
		bool perPadTouched[NUM_VIRTUAL_PADS]{};
		float perPadValue[NUM_VIRTUAL_PADS]{};
		bool anyTouched = false;
		for (auto &multiMapping : inputMappings) {
			int pad = multiMapping.padIndex;
			if (pad < 0 || pad >= NUM_VIRTUAL_PADS) continue;

			if (multiMapping.mappings.contains(changedMapping)) {
				perPadTouched[pad] = true;
				anyTouched = true;
			}

			float product = 1.0f;  // We multiply the various inputs in a combo mapping with each other.
			double curTime = 0.0;
			for (auto mapping : multiMapping.mappings) {
				auto iter = curInput_.find(mapping);

				if (iter != curInput_.end()) {
					// Stop reverse ordering from triggering.
					if (g_Config.bStrictComboOrder && iter->second.timestamp < curTime) {
						product = 0.0f;
						break;
					} else {
						curTime = iter->second.timestamp;
					}

					if (mapping.IsAxis()) {
						threshold = GetDeviceAxisThreshold(iter->first.deviceId, mapping);
						bool axisTouched = false;
						float value = MapAxisValue(iter->second.value, idForMapping, mapping, changedMapping, &axisTouched);
						if (axisTouched) {
							perPadTouched[pad] = true;
							anyTouched = true;
						}
						product *= value;
					} else {
						product *= iter->second.value;
					}
				} else {
					product = 0.0f;
				}
			}

			perPadValue[pad] += product;
		}

		if (!anyTouched) {
			continue;
		}

		keyInputUsed = true;

		// Small values from analog inputs like gamepad sticks can linger around, which is bad here because we sum
		// up before applying deadzone etc. This means that it can be impossible to reach the min/max values with digital input!
		// So if non-analog events clash with analog ones mapped to the same input, decay the analog input,
		// which will quickly get things back to normal, while if it's intentional to use both at the same time for some reason,
		// that still works, though a bit weaker. We could also zero here, but you never know who relies on such strange tricks..
		// Note: This is an old problem, it didn't appear with the refactoring.
		// Decay is at the curInput_ level, which is global (per physical input), so it stays outside the per-pad loop.
		if (!changedMapping.IsAxis()) {
			for (auto &multiMapping : inputMappings) {
				for (auto &mapping : multiMapping.mappings) {
					if (mapping != changedMapping && curInput_[mapping].value > 0.0f) {
						// Note that this takes the time into account now - values will
						// decay after a while, not immediately.
						curInput_[mapping] = ReduceMagnitude(curInput_[mapping], now);
					}
				}
			}
		}

		// Aggregate ON state BEFORE per-pad updates, so system-level vkeys
		// (PAUSE / FASTFORWARD / ANALOG_LIGHTLY / AXIS_SWAP_HOLD etc) fire
		// the global onVKey edge once when any pad transitions from "none on"
		// to "any on", rather than once per pad.
		bool anyPrevOn = false;
		for (int pad = 0; pad < NUM_VIRTUAL_PADS; ++pad) {
			if (virtKeyOn_[pad][i]) { anyPrevOn = true; break; }
		}

		for (int pad = 0; pad < NUM_VIRTUAL_PADS; ++pad) {
			if (!perPadTouched[pad]) continue;

			float padValue = clamp_value(perPadValue[pad], 0.0f, 1.0f);
			bool bPrevValue = virtKeys_[pad][i] >= threshold;
			bool bValue = padValue >= threshold;

			if (virtKeys_[pad][i] != padValue) {
				onVKeyAnalog(changedMapping.deviceId, pad, vkId, padValue);
				virtKeys_[pad][i] = padValue;
			}

			if (!bPrevValue && bValue) {
				virtKeyOn_[pad][i] = true;
			} else if (bPrevValue && !bValue) {
				virtKeyOn_[pad][i] = false;
			}
		}

		// Now recompute aggregate ON and fire global onVKey on edge.
		bool anyNowOn = false;
		for (int pad = 0; pad < NUM_VIRTUAL_PADS; ++pad) {
			if (virtKeyOn_[pad][i]) { anyNowOn = true; break; }
		}

		if (!anyPrevOn && anyNowOn) {
			onVKey(vkId, true);
			if (vkId == VIRTKEY_ANALOG_LIGHTLY) {
				updateAnalogSticks = true;
			} else if (vkId == VIRTKEY_AXIS_SWAP_HOLD) {
				UpdateSwapAxes();
			}
		} else if (anyPrevOn && !anyNowOn) {
			onVKey(vkId, false);
			if (vkId == VIRTKEY_ANALOG_LIGHTLY) {
				updateAnalogSticks = true;
			} else if (vkId == VIRTKEY_AXIS_SWAP_HOLD) {
				UpdateSwapAxes();
			}
		}
	}

	if (updateAnalogSticks) {
		// If "lightly" (analog limiter) was toggled, we need to update both computed stick outputs.
		UpdateAnalogOutput(0, 0);
		UpdateAnalogOutput(0, 1);
	}

	return keyInputUsed;
}

bool ControlMapper::Key(const KeyInput &key, bool *pauseTrigger) {
	double now = time_now_d();
	InputMapping mapping(key.deviceId, key.keyCode);

	std::lock_guard<std::mutex> guard(mutex_);

	if (key.deviceId < DEVICE_ID_COUNT) {
		deviceTimestamps_[(int)key.deviceId] = now;
	}

	if (key.flags & KeyInputFlags::DOWN) {
		curInput_[mapping] = { 1.0f, now };
	} else if (key.flags & KeyInputFlags::UP) {
		curInput_[mapping] = { 0.0f, now};
	}

	// TODO: See if this can be simplified further somehow.
	if ((key.flags & KeyInputFlags::DOWN) && key.keyCode == NKCODE_BACK) {
		bool mappingFound = KeyMap::InputMappingToPspButton(mapping, nullptr);
		DEBUG_LOG(Log::System, "Key: %d DeviceId: %d", key.keyCode, key.deviceId);
		if (!mappingFound || key.deviceId == DEVICE_ID_DEFAULT) {
			*pauseTrigger = true;
			return true;
		}
	}

	KeyMap::LockMappings();
	bool retval = UpdatePSPState(mapping, now);
	KeyMap::UnlockMappings();
	return retval;
}

void ControlMapper::ToggleSwapAxes() {
	// Note: The lock is already locked here.
	swapAxes_ = !swapAxes_;

	UpdateSwapAxes();
}

void ControlMapper::UpdateSwapAxes() {
	// Clear dpad on every pad — axis-swap toggle affects all virtual pads
	// since each pad has its own swapped-axes view of the same logical inputs.
	for (auto listener : listeners_) {
		for (int pad = 0; pad < NUM_VIRTUAL_PADS; ++pad) {
			listener->UpdatePSPButtons(pad, 0, CTRL_LEFT | CTRL_RIGHT | CTRL_UP | CTRL_DOWN);
		}
	}

	for (VirtKey vkey = VIRTKEY_FIRST; vkey < VIRTKEY_LAST; vkey = (VirtKey)(vkey + 1)) {
		if (!IsSwappableVKey(vkey)) continue;
		const int i = vkey - VIRTKEY_FIRST;

		// Clear per-pad first; fire the global OnVKey edge only once if
		// any pad transitioned from on to off.
		bool wasAnyOn = false;
		for (int pad = 0; pad < NUM_VIRTUAL_PADS; ++pad) {
			if (virtKeyOn_[pad][i]) {
				wasAnyOn = true;
				virtKeyOn_[pad][i] = false;
			}
		}
		if (wasAnyOn) {
			for (auto listener : listeners_) {
				listener->OnVKey(vkey, false);
			}
		}

		for (int pad = 0; pad < NUM_VIRTUAL_PADS; ++pad) {
			if (virtKeys_[pad][i] > 0.0f) {
				for (auto listener : listeners_) {
					listener->OnVKeyAnalog(vkey, 0.0f);
				}
				virtKeys_[pad][i] = 0.0f;
			}
		}
	}

	history_[0][0][0] = 0.0f;
	history_[0][0][1] = 0.0f;

	UpdateAnalogOutput(0, 0);
	UpdateAnalogOutput(0, 1);
}

void ControlMapper::UpdateCurInputAxis(const InputMapping &mapping, float value, double timestamp) {
	InputSample &input = curInput_[mapping];
	input.value = value;
	if (value >= GetDeviceAxisThreshold(mapping.deviceId, mapping)) {
		if (input.timestamp == 0.0) {
			input.timestamp = time_now_d();
		}
	} else {
		input.timestamp = 0.0;
	}
}

void ControlMapper::Axis(const AxisInput *axes, size_t count) {
	double now = time_now_d();

	std::lock_guard<std::mutex> guard(mutex_);

	KeyMap::LockMappings();
	for (size_t i = 0; i < count; i++) {
		const AxisInput &axis = axes[i];

		if (axis.deviceId == DEVICE_ID_MOUSE && !g_Config.bMouseControl) {
			continue;
		}

		size_t deviceIndex = (size_t)axis.deviceId;  // this wraps -1 up high, so will get rejected on the next line.
		if (deviceIndex < (size_t)DEVICE_ID_COUNT) {
			deviceTimestamps_[deviceIndex] = now;
		}
		rawAxisValue_[axis.axisId] = axis.value;  // these are only used for co-axis mapping
		if (axis.value >= 0.0f) {
			InputMapping mapping(axis.deviceId, axis.axisId, 1);
			InputMapping opposite(axis.deviceId, axis.axisId, -1);
			UpdateCurInputAxis(mapping, axis.value, now);
			UpdateCurInputAxis(opposite, 0.0f, now);
			UpdatePSPState(mapping, now);
			UpdatePSPState(opposite, now);
		} else if (axis.value < 0.0f) {
			InputMapping mapping(axis.deviceId, axis.axisId, -1);
			InputMapping opposite(axis.deviceId, axis.axisId, 1);
			UpdateCurInputAxis(mapping, -axis.value, now);
			UpdateCurInputAxis(opposite, 0.0f, now);
			UpdatePSPState(mapping, now);
			UpdatePSPState(opposite, now);
		}
	}
	KeyMap::UnlockMappings();
}

void ControlMapper::UpdateConfig(const DisplayLayoutConfig &config) {
	iInternalScreenRotationCached_ = config.bRotateControlsWithScreen ? config.iInternalScreenRotation : ROTATION_LOCKED_HORIZONTAL;
}

void ControlMapper::UpdateAutoMovements(double now) {
	// Auto-rotate-analog is a dev/debug helper that drives pad 0's left stick
	// in a circle; it intentionally targets pad 0 only because there's no UX
	// for choosing a target pad here. Adding multi-pad support would need a
	// "which pad" config knob and isn't worth it for what's basically a test
	// feature.
	if (autoRotatingAnalogCW_) {
		// Clamp to a square
		float x = std::min(1.0f, std::max(-1.0f, 1.42f * (float)cos(now * -g_Config.fAnalogAutoRotSpeed)));
		float y = std::min(1.0f, std::max(-1.0f, 1.42f * (float)sin(now * -g_Config.fAnalogAutoRotSpeed)));

		for (auto listener : listeners_) {
			listener->SetPSPAnalog(0, iInternalScreenRotationCached_, 0, x, y);
		}
	} else if (autoRotatingAnalogCCW_) {
		float x = std::min(1.0f, std::max(-1.0f, 1.42f * (float)cos(now * g_Config.fAnalogAutoRotSpeed)));
		float y = std::min(1.0f, std::max(-1.0f, 1.42f * (float)sin(now * g_Config.fAnalogAutoRotSpeed)));

		for (auto listener : listeners_) {
			listener->SetPSPAnalog(0, iInternalScreenRotationCached_, 0, x, y);
		}
	}
}

void ControlMapper::PSPKey(int deviceId, int pspKeyCode, KeyInputFlags flags) {
	std::lock_guard<std::mutex> guard(mutex_);
	if (pspKeyCode >= VIRTKEY_FIRST) {
		int vk = pspKeyCode - VIRTKEY_FIRST;
		// PSPKey is the entry for synthetic / OSK / emu-injected presses, not
		// from a physical pad's mapping table — there's no padIndex context.
		// Route them to pad 0 (the primary), which preserves prior behavior.
		if (flags & KeyInputFlags::DOWN) {
			virtKeys_[0][vk] = 1.0f;
			onVKey((VirtKey)pspKeyCode, true);
			onVKeyAnalog(deviceId, 0, (VirtKey)pspKeyCode, 1.0f);
		}
		if (flags & KeyInputFlags::UP) {
			virtKeys_[0][vk] = 0.0f;
			onVKey((VirtKey)pspKeyCode, false);
			onVKeyAnalog(deviceId, 0, (VirtKey)pspKeyCode, 0.0f);
		}
	} else {
		// INFO_LOG(Log::System, "pspKey %d %d", pspKeyCode, flags);
		if (flags & KeyInputFlags::DOWN)
			for (auto listener : listeners_) {
				listener->UpdatePSPButtons(0, pspKeyCode, 0);
			}
		if (flags & KeyInputFlags::UP)
			for (auto listener : listeners_) {
				listener->UpdatePSPButtons(0, 0, pspKeyCode);
			}
	}
}

void ControlMapper::onVKeyAnalog(int deviceId, int padIndex, VirtKey vkey, float value) {
	// Unfortunately, for digital->analog inputs to work sanely, we need to sum up
	// with the opposite value too.
	int stick = 0;
	int axis = 'X';
	int oppositeVKey = GetOppositeVKey(vkey);
	float sign = 1.0f;
	switch (vkey) {
	case VIRTKEY_AXIS_X_MIN: sign = -1.0f; break;
	case VIRTKEY_AXIS_X_MAX: break;
	case VIRTKEY_AXIS_Y_MIN: axis = 'Y'; sign = -1.0f; break;
	case VIRTKEY_AXIS_Y_MAX: axis = 'Y'; break;
	case VIRTKEY_AXIS_RIGHT_X_MIN: stick = CTRL_STICK_RIGHT; sign = -1.0f; break;
	case VIRTKEY_AXIS_RIGHT_X_MAX: stick = CTRL_STICK_RIGHT; break;
	case VIRTKEY_AXIS_RIGHT_Y_MIN: stick = CTRL_STICK_RIGHT; axis = 'Y'; sign = -1.0f; break;
	case VIRTKEY_AXIS_RIGHT_Y_MAX: stick = CTRL_STICK_RIGHT; axis = 'Y'; break;
	default:
		for (auto listener : listeners_) {
			listener->OnVKeyAnalog(vkey, value);
		}
		return;
	}
	if (oppositeVKey != 0) {
		// Same-pad lookup — the opposite virtkey contributes to THIS pad's
		// stick value only. Cross-pad subtraction would let P1's stick-left
		// cancel P2's stick-right, which is exactly the bug we just fixed.
		float oppVal = virtKeys_[padIndex][oppositeVKey - VIRTKEY_FIRST];
		if (oppVal != 0.0f) {
			value -= oppVal;
			// NOTICE_LOG(Log::sceCtrl, "Reducing %f by %f (from %08x : %s)", value, oppVal, oppositeVKey, KeyMap::GetPspButtonName(oppositeVKey).c_str());
		}
	}
	SetPSPAxis(deviceId, padIndex, stick, axis, sign * value);
}

void ControlMapper::onVKey(VirtKey vkey, bool down) {
	switch (vkey) {
	case VIRTKEY_ANALOG_ROTATE_CW:
		if (down) {
			autoRotatingAnalogCW_ = true;
			autoRotatingAnalogCCW_ = false;
		} else {
			autoRotatingAnalogCW_ = false;
			for (auto listener : listeners_) {
				listener->SetPSPAnalog(0, iInternalScreenRotationCached_, 0, 0.0f, 0.0f);
			}
		}
		break;
	case VIRTKEY_ANALOG_ROTATE_CCW:
		if (down) {
			autoRotatingAnalogCW_ = false;
			autoRotatingAnalogCCW_ = true;
		} else {
			autoRotatingAnalogCCW_ = false;
			for (auto listener : listeners_) {
				listener->SetPSPAnalog(0, iInternalScreenRotationCached_, 0, 0.0f, 0.0f);
			}
		}
		break;
	default:
		for (auto listener : listeners_) {
			listener->OnVKey(vkey, down);
		}
		break;
	}
}

void ControlMapper::GetDebugString(char *buffer, size_t bufSize) const {
	std::stringstream str;
	for (auto &iter : curInput_) {
		char temp[256];
		iter.first.FormatDebug(temp, sizeof(temp));
		str << temp << ": " << iter.second.value << std::endl;
	}
	for (int i = 0; i < VIRTKEY_COUNT; i++) {
		int vkId = VIRTKEY_FIRST + i;
		if ((vkId >= VIRTKEY_AXIS_X_MIN && vkId <= VIRTKEY_AXIS_Y_MAX) || vkId == VIRTKEY_ANALOG_LIGHTLY || vkId == VIRTKEY_SPEED_ANALOG) {
			str << KeyMap::GetPspButtonName(vkId);
			for (int pad = 0; pad < NUM_VIRTUAL_PADS; ++pad) {
				if (pad == 0 || virtKeys_[pad][i] != 0.0f) {
					str << " [P" << (pad + 1) << "=" << virtKeys_[pad][i] << "]";
				}
			}
			str << std::endl;
		}
	}
	str << "Lstick: " << converted_[0][0][0] << ", " << converted_[0][0][1] << std::endl;
	truncate_cpy(buffer, bufSize, str.str().c_str());
}

void ControlMapper::RemoveListener(ControlListener *listener) {
	std::lock_guard<std::mutex> guard(mutex_);
	auto it = std::find(listeners_.begin(), listeners_.end(), listener);
	if (it != listeners_.end()) {
		listeners_.erase(it);
	}
}
