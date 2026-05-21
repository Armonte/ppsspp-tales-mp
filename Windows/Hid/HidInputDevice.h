// This file in particular along with its cpp file is public domain, use it for whatever you want.

// Internal common header for Hid input stuff.

#pragma once

#include <set>
#include <string>
#include <memory>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Input/InputState.h"
#include "Windows/InputDevice.h"
#include "Common/CommonWindows.h"

enum class HIDControllerType {
	DualShock,
	DualSense,
	SwitchPro,
};

struct HIDControllerState {
	// Analog sticks
	s8 stickAxes[4];  // LX LY RX RY
	// Analog triggers
	u8 triggerAxes[2];
	// Buttons. Here the mapping is specific to the controller type and resolved
	// later.
	u32 buttons;  // Bitmask, PSButton enum

	bool accValid = false;
	bool gyroValid = false;
	float accelerometer[3];  // X, Y, Z
	float gyro[3];
};

struct ButtonInputMapping {
	u32 button;
	InputKeyCode keyCode;
};

// One physical HID controller (DualShock / DualSense / Switch Pro). Owns its
// device handle and reports input under its own DEVICE_ID_HID_<pad> id.
class HidController {
public:
	HidController(HANDLE handle, HIDControllerType subType, int pad,
		int inReportSize, int outReportSize, std::wstring devicePath);
	~HidController();

	HidController(const HidController &) = delete;
	HidController &operator=(const HidController &) = delete;

	// Reads one input report and emits events. Returns false if the read
	// failed, which we treat as the controller having been disconnected.
	bool UpdateState(bool sendInput);
	bool HasAccelerometer() const;

	int Pad() const { return pad_; }
	const std::wstring &DevicePath() const { return devicePath_; }

private:
	void ReleaseAllKeys(const ButtonInputMapping *buttonMappings, int count);
	InputDeviceID DeviceID() const { return (InputDeviceID)(DEVICE_ID_HID_0 + pad_); }

	HANDLE handle_;
	HIDControllerType subType_;
	HIDControllerState prevState_{};
	int pad_;
	int inReportSize_;
	int outReportSize_;
	std::wstring devicePath_;
};

// Meta-device that discovers and polls every supported HID controller, up to
// DEVICE_ID_HID_5 (6 slots). Controllers are picked up as they appear and
// dropped when their reads start failing. Originally this only ever opened the
// first controller it found.
class HidInputDevice : public InputDevice {
public:
	void Init() override;
	int UpdateState() override;
	void Shutdown() override;

	static void AddSupportedDevices(std::set<u32> *deviceVIDPIDs);

	bool HasAccelerometer() const override;

private:
	// Enumerates HID devices and opens any supported ones not already open,
	// assigning each the lowest free pad slot. Only called when a slot is free.
	void ScanForNewControllers();
	int FirstFreePadSlot() const;

	std::vector<std::unique_ptr<HidController>> controllers_;
	int pollCount_ = 0;
	enum {
		POLL_FREQ = 709,  // a prime number.
		// HID controllers get device ids DEVICE_ID_HID_0 .. DEVICE_ID_HID_5.
		MAX_HID_CONTROLLERS = DEVICE_ID_HID_5 - DEVICE_ID_HID_0 + 1,
	};
};
