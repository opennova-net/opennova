#pragma once

#include <cstdint>
#include <string>

// The window the game draws in (ADR 0077 d2: the engine's platform API, backend-neutral). The
// backend owns it: the engine creates it once, sets its title, size and mode, reads its size, its
// focus, its display's refresh rate and its OS handle, and takes the user's request to close it.
// What any of that means for the game (the quit a close request starts, the key-state reset a
// focus change runs, the target the render frame sizes) is the engine's.
//
// The design follows SDL3's SDL_Window (SDL_CreateWindow, SDL_SetWindowTitle,
// SDL_GetWindowSizeInPixels, SDL_SetWindowFullscreen, SDL_WINDOW_INPUT_FOCUS,
// SDL_EVENT_WINDOW_CLOSE_REQUESTED, the window's HWND / X11 / Wayland properties, the display
// mode's refresh_rate), GLFW's GLFWwindow (glfwCreateWindow, glfwGetFramebufferSize,
// glfwSetWindowMonitor, GLFW_FOCUSED, glfwWindowShouldClose, glfwGetWin32Window, GLFWvidmode's
// refreshRate) and sokol_app's one window (sapp_width / sapp_height, sapp_toggle_fullscreen,
// SAPP_EVENTTYPE_QUIT_REQUESTED, sapp_win32_get_hwnd): one window, its state polled, its close
// request latched until read. The focus change is also an input event (input_sample.h
// FocusEvent), since the engine must see it in order with the keys.
namespace opennova::platform {

// Windowed, or covering its display at the display's own mode (SDL3's fullscreen with no
// exclusive mode set, GLFW's windowed full screen, sokol_app's fullscreen): the engine renders at
// its own resolution and scales, so it never changes the display's mode.
enum class WindowMode : uint8_t { Windowed, Fullscreen };

struct WindowDesc {
	std::string title; // UTF-8
	// The windowed client size in pixels.
	int32_t width = 640;
	int32_t height = 480;
	WindowMode mode = WindowMode::Windowed;
};

struct WindowSize {
	int32_t width = 0;
	int32_t height = 0;
};

// The OS's own window, for an API outside the backend that takes it (a second GPU backend, a
// native dialog): `kind` names what the two pointers are. Win32: `window` the HWND, `display`
// null. X11: `window` the Window id (an XID, held as an integer in the pointer, as bgfx's
// PlatformData::nwh takes it), `display` the Display*. Wayland: `window` the wl_surface*,
// `display` the wl_display*. None: no OS window (a headless backend).
struct NativeWindowHandle {
	enum class Kind : uint8_t { None, Win32, X11, Wayland };
	Kind kind = Kind::None;
	void *window = nullptr;
	void *display = nullptr;
};

class Window {
public:
	virtual ~Window() = default;

	// Brings the window up as `desc` asks (title, client size, mode). False with `error` when the
	// OS refused it; the window does not exist then. Called once.
	virtual bool create(const WindowDesc &desc, std::string &error) = 0;

	virtual void set_title(const std::string &title) = 0;
	// The windowed client size in pixels (a fullscreen window keeps it for its return).
	virtual void set_size(int32_t width, int32_t height) = 0;
	// The client area's size in pixels now (the drawable's: a fullscreen window's is its display's).
	virtual WindowSize size() const = 0;

	virtual void set_mode(WindowMode mode) = 0;
	virtual WindowMode mode() const = 0;

	// The window holds the keyboard focus now.
	virtual bool focused() const = 0;

	// Whether the user asked to close the window (its close button, Alt+F4, the OS's quit) since
	// the last read; reading clears it. The window stays up: closing is the engine's answer.
	virtual bool take_close_request() = 0;

	virtual NativeWindowHandle native_handle() const = 0;

	// The refresh rate of the display the window is on, in Hz; 0 when the backend cannot tell.
	virtual float refresh_rate_hz() const = 0;
};

} // namespace opennova::platform
