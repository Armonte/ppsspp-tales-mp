#include "Common/CommonWindows.h"

#if defined(__MINGW32__) || defined(__MINGW64__)
extern "C" {
#include <hidsdi.h>
}
#else
#include <hidsdi.h>
#endif

#include "HidCommon.h"

bool WriteReport(HANDLE handle, const u8 *data, size_t size) {
	// HID handles are opened with FILE_FLAG_OVERLAPPED so reads can be made
	// non-blocking, which means writes have to go through an OVERLAPPED struct
	// as well. Writes complete quickly, so we just wait for them here.
	OVERLAPPED ov{};
	ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
	DWORD written = 0;
	bool result = WriteFile(handle, data, (DWORD)size, &written, &ov) != FALSE;
	if (!result && GetLastError() == ERROR_IO_PENDING) {
		result = GetOverlappedResult(handle, &ov, &written, TRUE) != FALSE;
	}
	u32 errorCode = result ? 0 : GetLastError();
	if (ov.hEvent) {
		CloseHandle(ov.hEvent);
	}
	if (!result) {
		if (errorCode == ERROR_INVALID_PARAMETER) {
			if (!HidD_SetOutputReport(handle, (PVOID)data, (DWORD)size)) {
				errorCode = GetLastError();
			}
		}
		WARN_LOG(Log::UI, "WriteReport: Failed initializing: %08x", errorCode);
		return false;
	}
	return true;
}
