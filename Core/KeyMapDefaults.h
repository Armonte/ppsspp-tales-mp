#pragma once

#include "Common/Input/InputState.h"  // InputDeviceID, DEVICE_ID_INVALID

namespace KeyMap {

enum DefaultMaps {
	DEFAULT_MAPPING_KEYBOARD,
	DEFAULT_MAPPING_PAD,
	DEFAULT_MAPPING_ANDROID_PAD,
	DEFAULT_MAPPING_IOS_PAD,
	DEFAULT_MAPPING_XINPUT,
	DEFAULT_MAPPING_ANDROID_XBOX,  // XBox controller or similar on Android
	DEFAULT_MAPPING_SHIELD,
	DEFAULT_MAPPING_XPERIA_PLAY,
	DEFAULT_MAPPING_MOQI_I7S,
	DEFAULT_MAPPING_RETROID_CONTROLLER,
	DEFAULT_MAPPING_VR_HEADSET,
};

// `targetPadIndex` chooses which PSP virtual pad slot the default mappings
// drive. Default 0 = main pad (existing behavior). 1..N-1 = extra pads
// (multiplayer via the EXTRA_PAD MMIO mirror).
//
// `deviceIdOverride` lets callers force a specific physical device id
// instead of the per-profile hardcoded primary (DEVICE_ID_XINPUT_0,
// DEVICE_ID_PAD_0, etc). Necessary when the user has multiple controllers
// of the same type and wants to bind one of them specifically — without
// the override, autoconfigure would dump every Xbox profile into XINPUT_0
// no matter which pad they actually picked.
void SetDefaultKeyMap(DefaultMaps dmap, bool replace, int targetPadIndex = 0, InputDeviceID deviceIdOverride = DEVICE_ID_ANY);

}  // namespace
