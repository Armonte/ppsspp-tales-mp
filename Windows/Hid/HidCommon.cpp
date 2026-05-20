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
	DWORD written;
	bool result = WriteFile(handle, data, (DWORD)size, &written, NULL);
	if (!result) {
		u32 errorCode = GetLastError();
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
