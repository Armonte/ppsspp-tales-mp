#pragma once

#include "Common/CommonWindows.h"
#include "Windows/Hid/HidInputDevice.h"

bool InitializeDualSense(HANDLE handle, int outReportSize);
bool ShutdownDualsense(HANDLE handle, int outReportSize);
bool ParseDualSenseInput(const BYTE *inputReport, DWORD bytesRead, HIDControllerState *state, int inReportSize);
