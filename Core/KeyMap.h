// Copyright (c) 2013- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#pragma once

#include <string>
#include <string_view>
#include <map>
#include <vector>
#include <set>

#include "Common/Input/InputState.h" // InputMapping
#include "Common/Input/KeyCodes.h"     // keyboard keys
#include "Common/Data/Collections/TinySet.h"
#include "Core/KeyMapDefaults.h"

#define KEYMAP_ERROR_KEY_ALREADY_USED -1
#define KEYMAP_ERROR_UNKNOWN_KEY 0

// Don't change any of these - it'll break backwards compatibility with configs.
enum VirtKey : u32 {
	VIRTKEY_FIRST = 0x40000001,
	VIRTKEY_AXIS_X_MIN = 0x40000001,
	VIRTKEY_AXIS_Y_MIN = 0x40000002,
	VIRTKEY_AXIS_X_MAX = 0x40000003,
	VIRTKEY_AXIS_Y_MAX = 0x40000004,
	VIRTKEY_RAPID_FIRE = 0x40000005,
	VIRTKEY_FASTFORWARD = 0x40000006,
	VIRTKEY_PAUSE = 0x40000007,
	VIRTKEY_SPEED_TOGGLE = 0x40000008,
	VIRTKEY_AXIS_RIGHT_X_MIN = 0x40000009,
	VIRTKEY_AXIS_RIGHT_Y_MIN = 0x4000000a,
	VIRTKEY_AXIS_RIGHT_X_MAX = 0x4000000b,
	VIRTKEY_AXIS_RIGHT_Y_MAX = 0x4000000c,
	VIRTKEY_REWIND = 0x4000000d,
	VIRTKEY_SAVE_STATE = 0x4000000e,
	VIRTKEY_LOAD_STATE = 0x4000000f,
	VIRTKEY_NEXT_SLOT = 0x40000010,
	VIRTKEY_TOGGLE_FULLSCREEN = 0x40000011,
	VIRTKEY_ANALOG_LIGHTLY = 0x40000012,
	VIRTKEY_AXIS_SWAP_TOGGLE = 0x40000013,
	VIRTKEY_DEVMENU = 0x40000014,
	VIRTKEY_FRAME_ADVANCE = 0x40000015,
	VIRTKEY_RECORD = 0x40000016,
	VIRTKEY_SPEED_CUSTOM1 = 0x40000017,
	VIRTKEY_SPEED_CUSTOM2 = 0x40000018,
	VIRTKEY_TEXTURE_DUMP = 0x40000019,
	VIRTKEY_TEXTURE_REPLACE = 0x4000001A,
	VIRTKEY_SCREENSHOT = 0x4000001B,
	VIRTKEY_MUTE_TOGGLE = 0x4000001C,
	VIRTKEY_OPENCHAT = 0x4000001D,
	VIRTKEY_ANALOG_ROTATE_CW = 0x4000001E,
	VIRTKEY_ANALOG_ROTATE_CCW = 0x4000001F,
	VIRTKEY_SCREEN_ROTATION_VERTICAL = 0x40000020,
	VIRTKEY_SCREEN_ROTATION_VERTICAL180 = 0x40000021,
	VIRTKEY_SCREEN_ROTATION_HORIZONTAL = 0x40000022,
	VIRTKEY_SCREEN_ROTATION_HORIZONTAL180 = 0x40000023,
	VIRTKEY_SPEED_ANALOG = 0x40000024,
	VIRTKEY_VR_CAMERA_ADJUST = 0x40000025,
	VIRTKEY_VR_CAMERA_RESET = 0x40000026,
	VIRTKEY_PREVIOUS_SLOT = 0x40000027,
	VIRTKEY_TOGGLE_WLAN = 0x40000028,
	VIRTKEY_EXIT_APP = 0x40000029,
	VIRTKEY_TOGGLE_MOUSE = 0x40000030,
	VIRTKEY_TOGGLE_TOUCH_CONTROLS =  0x40000031,
	VIRTKEY_RESET_EMULATION = 0x40000032,
	VIRTKEY_TOGGLE_DEBUGGER = 0x40000033,
	VIRTKEY_PAUSE_NO_MENU = 0x40000034,
	VIRTKEY_TOGGLE_TILT = 0x40000035,
	VIRTKEY_AXIS_SWAP_HOLD = 0x40000036,
	VIRTKEY_LAST,
	VIRTKEY_COUNT = VIRTKEY_LAST - VIRTKEY_FIRST
};

struct MappedAnalogAxis {
	int axisId;
	int direction;
};

struct MappedAnalogAxes {
	MappedAnalogAxis leftX;
	MappedAnalogAxis leftY;
	MappedAnalogAxis rightX;
	MappedAnalogAxis rightY;
};

// KeyMap
// A translation layer for key assignment. Provides
// integration with Core's config state.
//
// Does not handle input state managment.
//
// Platform ports should map their platform's keys to KeyMap's keys (NKCODE_*).
//
// Then have KeyMap transform those into psp buttons.

class IniFile;

namespace KeyMap {
	// Combo of InputMappings.
	struct MultiInputMapping {
		MultiInputMapping() {}
		explicit MultiInputMapping(const InputMapping &mapping) {
			mappings.push_back(mapping);
		}
		MultiInputMapping(const InputMapping &mapping, int padIdx) {
			mappings.push_back(mapping);
			padIndex = padIdx;
		}
		
		static MultiInputMapping FromConfigString(std::string_view str);
		std::string ToConfigString() const;
		std::string ToVisualString() const;

		bool operator <(const MultiInputMapping &other) const {
			if (padIndex != other.padIndex)
				return padIndex < other.padIndex;
			for (size_t i = 0; i < mappings.capacity(); i++) {
				// If one ran out of entries, the other wins.
				if (mappings.size() == i && other.mappings.size() > i) return true;
				if (mappings.size() >= i && other.mappings.size() == i) return false;
				if (mappings[i] < other.mappings[i]) return true;
				if (mappings[i] > other.mappings[i]) return false;
			}
			return false;
		}

		// Which virtual PSP pad this binding drives (0 = real pad, 1..3 = MMIO mirror).
		// Default 0 preserves existing configs and behavior.
		int padIndex = 0;

		bool operator ==(const MultiInputMapping &other) const {
			return padIndex == other.padIndex && mappings == other.mappings;
		}

		bool EqualsSingleMapping(const InputMapping &other) const {
			// Only pad-0 single-mapping bindings count as "equal" to a raw InputMapping.
			return padIndex == 0 && mappings.size() == 1 && mappings[0] == other;
		}

		bool empty() const {
			return mappings.empty();
		}

		bool HasMouse() const {
			for (auto &m : mappings) {
				return m.deviceId == DEVICE_ID_MOUSE;
			}
			return false;
		}

		FixedVec<InputMapping, 3> mappings;
	};

	typedef std::map<int, std::vector<MultiInputMapping>> KeyMapping;

	// Once the multimappings are inserted here, they must not be empty.
	// If one would be, delete the whole entry from the map instead.
	// This is automatically handled by SetInputMapping.
	extern std::set<InputDeviceID> g_seenDeviceIds;
	extern int g_controllerMapGeneration;
	// Key & Button names
	struct KeyMap_IntStrPair {
		int key;
		const char *name;
	};

	// Use if you need to display the textual name
	std::string GetKeyName(InputKeyCode keyCode);
	std::string GetKeyOrAxisName(const InputMapping &mapping);
	std::string GetAxisName(int axisId);
	std::string GetPspButtonName(int btn);
	const char *GetPspButtonNameCharPointer(int btn);

	const KeyMap_IntStrPair *GetMappableKeys(size_t *count);

	// Use to translate input mappings to and from PSP buttons. You should have already translated
	// your platform's keys to InputMapping keys.

	// Note that this one does not handle combos, since there's only one input.
	bool InputMappingToPspButton(const InputMapping &mapping, std::vector<int> *pspButtons);
	bool InputMappingsFromPspButton(int btn, std::vector<MultiInputMapping> *keys, bool ignoreMouse);

	// Careful with these.
	bool InputMappingsFromPspButtonNoLock(int btn, std::vector<MultiInputMapping> *keys, bool ignoreMouse);
	void LockMappings();
	void UnlockMappings();

	// Simplified check.
	bool PspButtonHasMappings(int btn);

	// Configure the key or axis mapping.
	// Any configuration will be saved to the Core config.
	void SetInputMapping(int psp_key, const MultiInputMapping &key, bool replace);
	// Return false if bind was a duplicate and got removed
	bool ReplaceSingleKeyMapping(int btn, int index, const MultiInputMapping &key);

	MappedAnalogAxes MappedAxesForDevice(InputDeviceID deviceId);

	void LoadFromIni(IniFile &iniFile);
	void SaveToIni(IniFile &iniFile);
	void ClearAllMappings();
	void DeleteNthMapping(int key, int number);

	void SetDefaultKeyMap(DefaultMaps dmap, bool replace, int targetPadIndex, InputDeviceID deviceIdOverride);

	void RestoreDefault();

	void UpdateNativeMenuKeys();

	void NotifyPadConnected(InputDeviceID deviceId, std::string_view name);
	void NotifyPadDisconnected(InputDeviceID deviceId);

	bool IsNvidiaShield(std::string_view name);
	bool IsNvidiaShieldTV(std::string_view name);
	bool IsXperiaPlay(std::string_view name);
	bool IsMOQII7S(std::string_view name);
	bool IsRetroid(std::string_view name);
	bool HasBuiltinController(std::string_view name);

	const std::set<std::string> &GetSeenPads();
	// Returns every seen physical device by id+name, NOT deduplicated by name.
	// This is what autoconfigure UIs should iterate when the user might have
	// multiple identically-named controllers (two Xbox pads, two DualShocks
	// etc) and needs to bind each one independently.
	std::vector<std::pair<InputDeviceID, std::string>> GetSeenPadDevices();
	std::string PadName(InputDeviceID deviceId);
	// Apply a default mapping profile for the given controller name.
	// `targetPadIndex` chooses which PSP virtual pad (0 = primary, 1..N-1 =
	// extra pads) the resulting bindings drive. Existing bindings on that
	// device that already target a DIFFERENT pad index are preserved — only
	// the slot we're configuring gets cleared.
	//
	// Looks up the device id by name, which is only useful when there's a
	// single controller of that type. For multi-controller setups (two
	// Xbox pads etc), use AutoConfForPadDevice instead.
	void AutoConfForPad(std::string_view name, int targetPadIndex = 0);

	// Same as AutoConfForPad but operates directly on a specific device id,
	// so identically-named duplicate controllers can each be bound to their
	// own PSP pad slot. The mapping profile is still picked by the device's
	// name (Xbox -> xinput defaults, etc), but the bindings written go to
	// the actual device, not the hardcoded "first of its type".
	void AutoConfForPadDevice(InputDeviceID deviceId, int targetPadIndex = 0);

	bool IsKeyMapped(InputDeviceID device, int key);

	bool HasChanged(int &prevGeneration);

	// Used for setting thresholds. Technically we could allow a setting per axis, but this is a reasonable compromise.
	enum class AxisType {
		TRIGGER,
		STICK,
		OTHER,
	};

	AxisType GetAxisType(InputAxis axis);
}  // namespace KeyMap
