// This file in particular along with its header is public domain, use it for whatever you want.

#include <windows.h>
#if defined(__MINGW32__) || defined(__MINGW64__)
// mingw's hidsdi.h doesn't declare its functions inside `extern "C"`, so a
// C++ TU ends up calling the C++-mangled forms (HidD_GetHidGuid(_GUID*) etc.)
// which the import lib doesn't have. Wrap the include locally. Native-MSVC
// builds don't need this -- their SDK header has extern "C" already.
extern "C" {
#include <hidsdi.h>
}
#else
#include <hidsdi.h>
#endif
#include <setupapi.h>
#include <initguid.h>
#include <string>
#include <vector>

#include "Windows/Hid/HidInputDevice.h"
#include "Windows/Hid/SwitchPro.h"
#include "Windows/Hid/DualSense.h"
#include "Windows/Hid/DualShock.h"
#include "Windows/Hid/HidCommon.h"
#include "Common/CommonTypes.h"
#include "Common/TimeUtil.h"
#include "Common/Math/math_util.h"
#include "Common/Log.h"
#include "Common/Input/InputState.h"
#include "Common/Common.h"
#include "Common/System/NativeApp.h"
#include "Common/System/OSD.h"
#include "Core/KeyMap.h"
#include "Core/Config.h"

struct HidStickMapping {
	HidStickAxis stickAxis;
	InputAxis inputAxis;
};

// This is the same mapping as DInput etc.
static const HidStickMapping g_psStickMappings[] = {
	{HID_STICK_LX, JOYSTICK_AXIS_X},
	{HID_STICK_LY, JOYSTICK_AXIS_Y},
	{HID_STICK_RX, JOYSTICK_AXIS_Z},
	{HID_STICK_RY, JOYSTICK_AXIS_RX},
};

struct HidTriggerMapping {
	HidTriggerAxis triggerAxis;
	InputAxis inputAxis;
};

static const HidTriggerMapping g_psTriggerMappings[] = {
	{HID_TRIGGER_L2, JOYSTICK_AXIS_LTRIGGER},
	{HID_TRIGGER_R2, JOYSTICK_AXIS_RTRIGGER},
};

struct HIDControllerInfo {
	u16 vendorId;
	u16 productId;
	HIDControllerType type;
	const char *name;
};

constexpr u16 SONY_VID = 0x054C;
constexpr u16 NINTENDO_VID = 0x57e;
constexpr u16 SWITCH_PRO_PID = 0x2009;
constexpr u16 DS4_WIRELESS = 0x0BA0;
constexpr u16 PS_CLASSIC = 0x0CDA;

// We pick a few ones from here to support, let's add more later.
// https://github.com/ds4windowsapp/DS4Windows/blob/65609b470f53a4f832fb07ac24085d3e28ec15bd/DS4Windows/DS4Library/DS4Devices.cs#L126
static const HIDControllerInfo g_psInfos[] = {
	{SONY_VID, 0x05C4, HIDControllerType::DualShock, "DS4 v.1"},
	{SONY_VID, 0x09CC, HIDControllerType::DualShock, "DS4 v.2"},
	{SONY_VID, 0x0CE6, HIDControllerType::DualSense, "DualSense"},
	{SONY_VID, 0x0DF2, HIDControllerType::DualSense, "DualSense Edge"},
	{SONY_VID, PS_CLASSIC, HIDControllerType::DualShock, "PS Classic"},
	{NINTENDO_VID, SWITCH_PRO_PID, HIDControllerType::SwitchPro, "Switch Pro"},
	// {PSSubType::DS4, DS4_WIRELESS},
	// {PSSubType::DS5, DUALSENSE_WIRELESS},
	// {PSSubType::DS5, DUALSENSE_EDGE_WIRELESS},
};

static const HIDControllerInfo *GetGamepadInfo(const HIDD_ATTRIBUTES &attr) {
	for (const auto &info : g_psInfos) {
		if (attr.VendorID == info.vendorId && attr.ProductID == info.productId) {
			return &info;
		}
	}
	return nullptr;
}

void HidInputDevice::AddSupportedDevices(std::set<u32> *deviceVIDPIDs) {
	for (const auto &info : g_psInfos) {
		const u32 vidpid = MAKELONG(info.vendorId, info.productId);
		deviceVIDPIDs->insert(vidpid);
	}
}

// ---------------------------------------------------------------------------
// HidController -- one physical controller, polled with overlapped I/O.
// ---------------------------------------------------------------------------

HidController::HidController(HANDLE handle, HIDControllerType subType, int pad,
	int inReportSize, int outReportSize, std::wstring devicePath)
	: handle_(handle), subType_(subType), pad_(pad),
	  inReportSize_(inReportSize), outReportSize_(outReportSize),
	  devicePath_(std::move(devicePath)) {
	readEvent_ = CreateEvent(nullptr, TRUE, FALSE, nullptr);  // manual-reset

	int rs = (subType_ == HIDControllerType::SwitchPro) ? SwitchPro_INPUT_REPORT_LEN : inReportSize_;
	if (rs <= 0 || rs > (int)sizeof(readBuffer_)) {
		rs = (int)sizeof(readBuffer_);
	}
	readSize_ = rs;
}

HidController::~HidController() {
	if (readPending_ && handle_ && handle_ != INVALID_HANDLE_VALUE) {
		// Cancel the in-flight read and wait for it to actually finish before
		// the handle / OVERLAPPED go away.
		CancelIoEx(handle_, &overlapped_);
		DWORD bytesRead = 0;
		GetOverlappedResult(handle_, &overlapped_, &bytesRead, TRUE);
		readPending_ = false;
	}
	if (handle_ && handle_ != INVALID_HANDLE_VALUE) {
		switch (subType_) {
		case HIDControllerType::DualShock:
			ShutdownDualShock(handle_, outReportSize_);
			break;
		case HIDControllerType::DualSense:
			ShutdownDualsense(handle_, outReportSize_);
			break;
		default:
			break;
		}
		CloseHandle(handle_);
		handle_ = nullptr;
	}
	if (readEvent_) {
		CloseHandle(readEvent_);
		readEvent_ = nullptr;
	}
}

bool HidController::HasAccelerometer() const {
	switch (subType_) {
	case HIDControllerType::DualSense:
	case HIDControllerType::SwitchPro:
		return true;
	default:
		return false;
	}
}

void HidController::ReleaseAllKeys(const ButtonInputMapping *buttonMappings, int count) {
	const InputDeviceID deviceID = DeviceID();
	for (int i = 0; i < count; i++) {
		const auto &mapping = buttonMappings[i];
		KeyInput key;
		key.deviceId = deviceID;
		key.flags = KeyInputFlags::UP;
		key.keyCode = mapping.keyCode;
		NativeKey(key);
	}

	static const InputAxis allAxes[6] = {
		JOYSTICK_AXIS_X,
		JOYSTICK_AXIS_Y,
		JOYSTICK_AXIS_Z,
		JOYSTICK_AXIS_RX,
		JOYSTICK_AXIS_LTRIGGER,
		JOYSTICK_AXIS_RTRIGGER,
	};

	for (const auto axisId : allAxes) {
		AxisInput axis;
		axis.deviceId = deviceID;
		axis.axisId = axisId;
		axis.value = 0;
		NativeAxis(&axis, 1);
	}
}

bool HidController::IssueRead() {
	overlapped_ = {};
	overlapped_.hEvent = readEvent_;
	if (readEvent_) {
		ResetEvent(readEvent_);
	}
	DWORD bytesRead = 0;
	if (ReadFile(handle_, readBuffer_, (DWORD)readSize_, &bytesRead, &overlapped_)) {
		// Completed synchronously; GetOverlappedResult will still report it.
		readPending_ = true;
		return true;
	}
	if (GetLastError() == ERROR_IO_PENDING) {
		readPending_ = true;
		return true;
	}
	return false;  // genuine device error
}

void HidController::ProcessReport(DWORD bytesRead, bool sendInput) {
	HIDControllerState state{};
	bool parsed = false;
	const ButtonInputMapping *buttonMappings = nullptr;
	size_t buttonMappingsSize = 0;
	switch (subType_) {
	case HIDControllerType::DualShock:
		parsed = ParseDualShockInput(readBuffer_, bytesRead, &state);
		GetPSButtonInputMappings(&buttonMappings, &buttonMappingsSize);
		break;
	case HIDControllerType::DualSense:
		parsed = ParseDualSenseInput(readBuffer_, bytesRead, &state, inReportSize_);
		GetPSButtonInputMappings(&buttonMappings, &buttonMappingsSize);
		break;
	case HIDControllerType::SwitchPro:
		parsed = ParseSwitchProInput(readBuffer_, bytesRead, &state);
		GetSwitchButtonInputMappings(&buttonMappings, &buttonMappingsSize);
		break;
	}

	if (!parsed) {
		// Unrecognized or short packet -- ignore it, keep the previous state.
		return;
	}

	const InputDeviceID deviceID = DeviceID();
	const u32 downMask = state.buttons & (~prevState_.buttons);
	const u32 upMask = (~state.buttons) & prevState_.buttons;

	for (u32 i = 0; i < buttonMappingsSize; i++) {
		const ButtonInputMapping &mapping = buttonMappings[i];
		if ((downMask & mapping.button) && sendInput) {
			KeyInput key;
			key.deviceId = deviceID;
			key.flags = KeyInputFlags::DOWN;
			key.keyCode = mapping.keyCode;
			NativeKey(key);
		}
		if ((upMask & mapping.button) && sendInput) {
			KeyInput key;
			key.deviceId = deviceID;
			key.flags = KeyInputFlags::UP;
			key.keyCode = mapping.keyCode;
			NativeKey(key);
		}
	}

	for (const auto &mapping : g_psStickMappings) {
		if (state.stickAxes[mapping.stickAxis] != prevState_.stickAxes[mapping.stickAxis] && sendInput) {
			AxisInput axis;
			axis.deviceId = deviceID;
			axis.axisId = mapping.inputAxis;
			axis.value = (float)state.stickAxes[mapping.stickAxis] * (1.0f / 128.0f);
			NativeAxis(&axis, 1);
		}
	}

	for (const auto &mapping : g_psTriggerMappings) {
		if (state.triggerAxes[mapping.triggerAxis] != prevState_.triggerAxes[mapping.triggerAxis] && sendInput) {
			AxisInput axis;
			axis.deviceId = deviceID;
			axis.axisId = mapping.inputAxis;
			axis.value = (float)state.triggerAxes[mapping.triggerAxis] * (1.0f / 255.0f);
			NativeAxis(&axis, 1);
		}
	}

	if (state.accValid && sendInput) {
		NativeAccelerometer(state.accelerometer[0], state.accelerometer[1], state.accelerometer[2]);
	}

	prevState_ = state;
}

bool HidController::UpdateState(bool sendInput) {
	// Make sure a read is in flight.
	if (!readPending_) {
		if (!IssueRead())
			return false;
	}

	// Drain every report that has arrived since the last tick. GetOverlappedResult
	// with bWait == FALSE never blocks -- if nothing arrived we just return.
	for (;;) {
		DWORD bytesRead = 0;
		if (!GetOverlappedResult(handle_, &overlapped_, &bytesRead, FALSE)) {
			const DWORD err = GetLastError();
			if (err == ERROR_IO_INCOMPLETE) {
				// Read still in flight -- nothing new this tick, still connected.
				return true;
			}
			// Genuine error -- treat as disconnected.
			INFO_LOG(Log::System, "HID controller (slot %d) read failed - assuming disconnected.", pad_);
			KeyMap::NotifyPadDisconnected(DeviceID());
			return false;
		}
		readPending_ = false;
		ProcessReport(bytesRead, sendInput);
		if (!IssueRead()) {
			INFO_LOG(Log::System, "HID controller (slot %d) read failed - assuming disconnected.", pad_);
			KeyMap::NotifyPadDisconnected(DeviceID());
			return false;
		}
	}
}

// ---------------------------------------------------------------------------
// HidInputDevice -- meta-device managing every connected HID controller.
// ---------------------------------------------------------------------------

void HidInputDevice::Init() {}

void HidInputDevice::Shutdown() {
	controllers_.clear();
}

bool HidInputDevice::HasAccelerometer() const {
	for (const auto &controller : controllers_) {
		if (controller->HasAccelerometer()) {
			return true;
		}
	}
	return false;
}

int HidInputDevice::FirstFreePadSlot() const {
	for (int slot = 0; slot < MAX_HID_CONTROLLERS; slot++) {
		bool used = false;
		for (const auto &controller : controllers_) {
			if (controller->Pad() == slot) {
				used = true;
				break;
			}
		}
		if (!used) {
			return slot;
		}
	}
	return -1;
}

void HidInputDevice::ScanForNewControllers() {
	GUID hidGuid;
	HidD_GetHidGuid(&hidGuid);

	HDEVINFO deviceInfoSet = SetupDiGetClassDevs(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (deviceInfoSet == INVALID_HANDLE_VALUE)
		return;

	SP_DEVICE_INTERFACE_DATA interfaceData;
	interfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

	for (DWORD i = 0; SetupDiEnumDeviceInterfaces(deviceInfoSet, nullptr, &hidGuid, i, &interfaceData); ++i) {
		const int freeSlot = FirstFreePadSlot();
		if (freeSlot < 0)
			break;  // No room for more controllers.

		DWORD requiredSize = 0;
		SetupDiGetDeviceInterfaceDetail(deviceInfoSet, &interfaceData, nullptr, 0, &requiredSize, nullptr);

		std::vector<BYTE> buffer(requiredSize);
		auto *detailData = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA>(buffer.data());
		detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

		if (!SetupDiGetDeviceInterfaceDetail(deviceInfoSet, &interfaceData, detailData, requiredSize, nullptr, nullptr)) {
			continue;
		}

		// Skip devices we've already opened -- check by path before opening, so
		// we don't pointlessly CreateFile/HidD_GetAttributes a known device.
		std::wstring devicePath = detailData->DevicePath;
		bool alreadyOpen = false;
		for (const auto &controller : controllers_) {
			if (controller->DevicePath() == devicePath) {
				alreadyOpen = true;
				break;
			}
		}
		if (alreadyOpen) {
			continue;
		}

		// Opened overlapped so reads can be non-blocking.
		HANDLE handle = CreateFile(detailData->DevicePath, GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
		if (handle == INVALID_HANDLE_VALUE) {
			continue;
		}

		HIDD_ATTRIBUTES attr{sizeof(HIDD_ATTRIBUTES)};
		if (!HidD_GetAttributes(handle, &attr)) {
			CloseHandle(handle);
			continue;
		}

		const HIDControllerInfo *info = GetGamepadInfo(attr);
		if (!info) {
			// Some other HID device (keyboard, mouse, unsupported pad).
			CloseHandle(handle);
			continue;
		}

		INFO_LOG(Log::UI, "Found supported gamepad '%s' (PID %04x), assigning HID slot %d", info->name, info->productId, freeSlot);

		HIDP_CAPS caps;
		PHIDP_PREPARSED_DATA preparsedData;
		HidD_GetPreparsedData(handle, &preparsedData);
		HidP_GetCaps(preparsedData, &caps);
		HidD_FreePreparsedData(preparsedData);

		const int inReportSize = caps.InputReportByteLength;
		const int outReportSize = caps.OutputReportByteLength;

		INFO_LOG(Log::UI, "Initializing gamepad. out report size=%d", outReportSize);
		bool result = false;
		switch (info->type) {
		case HIDControllerType::DualSense:
			result = InitializeDualSense(handle, outReportSize);
			break;
		case HIDControllerType::DualShock:
			result = InitializeDualShock(handle, outReportSize);
			break;
		case HIDControllerType::SwitchPro:
			result = InitializeSwitchPro(handle);
			break;
		}
		if (!result) {
			// Initialization only sets things like the lightbar -- reads can
			// still work, so keep the controller rather than retrying forever.
			ERROR_LOG(Log::UI, "Controller initialization failed (continuing anyway)");
		}

		KeyMap::NotifyPadConnected((InputDeviceID)(DEVICE_ID_HID_0 + freeSlot), info->name);
		controllers_.push_back(std::make_unique<HidController>(handle, info->type, freeSlot,
			inReportSize, outReportSize, std::move(devicePath)));
	}

	SetupDiDestroyDeviceInfoList(deviceInfoSet);
}

int HidInputDevice::UpdateState() {
	// Throttled scan for newly-plugged controllers. We only enumerate when
	// there's a free slot -- the SetupDi scan isn't free, and running it on the
	// hot path every frame is exactly what stalls input reads.
	if (pollCount_ <= 0) {
		pollCount_ = POLL_FREQ;
		if (FirstFreePadSlot() >= 0) {
			ScanForNewControllers();
		}
	} else {
		pollCount_--;
	}

	const bool sendInput = g_Config.bAllowHIDInput;

	for (size_t i = 0; i < controllers_.size(); ) {
		if (controllers_[i]->UpdateState(sendInput)) {
			++i;
		} else {
			// Read failed -> treat as disconnected. Dropping it frees the pad
			// slot, so a later scan can pick the controller back up.
			controllers_.erase(controllers_.begin() + i);
		}
	}

	// Reads are non-blocking now, so HID no longer paces the input thread --
	// let it do its normal sleep.
	return 0;
}
