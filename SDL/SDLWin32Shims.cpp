// Rollback fork: pieces of the Win32 frontend that shared code (UI settings, camera) calls on Windows,
// provided for the SDL frontend when it is built on Windows (PPSSPP_SDL_ON_WINDOWS).

#ifdef _WIN32

#include "Common/CommonWindows.h"
#include <SDL3/SDL.h>

namespace MainWindow {

// The first SDL window is the main window; its native handle parents dialogs such as the folder picker.
HWND GetHWND() {
	int count = 0;
	SDL_Window **windows = SDL_GetWindows(&count);
	HWND hwnd = nullptr;
	if (windows && count > 0) {
		hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(windows[0]), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
	}
	SDL_free(windows);
	return hwnd;
}

}  // namespace MainWindow

#endif  // _WIN32
