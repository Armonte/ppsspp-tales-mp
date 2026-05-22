#pragma once

#include "Common/CommonWindows.h"

#include "Windows/Hid/HidCommon.h"

// Switch Pro input reports are read in a fixed-size chunk.
constexpr int SwitchPro_INPUT_REPORT_LEN = 362;

bool InitializeSwitchPro(HANDLE handle);
void GetSwitchButtonInputMappings(const ButtonInputMapping **mappings, size_t *size);
bool ParseSwitchProInput(const BYTE *inputReport, DWORD bytesRead, HIDControllerState *state);
