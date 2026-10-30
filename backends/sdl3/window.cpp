#include "window.h"

#include <cstdint>

namespace opennova::sdl3 {

Window::~Window() {
	if (window_ != nullptr) SDL_DestroyWindow(window_);
}

bool Window::create(const platform::WindowDesc &desc, std::string &error) {
	if (window_ != nullptr) {
		error = "the window exists";
		return false;
	}
	// The size is the client area's in pixels; a fullscreen window covers its display at the
	// display's own mode (no exclusive mode), which set_mode applies once it stands.
	window_ = SDL_CreateWindow(desc.title.c_str(), desc.width, desc.height, 0);
	if (window_ == nullptr) {
		error = std::string("SDL_CreateWindow: ") + SDL_GetError();
		return false;
	}
	// Typed characters arrive as text-input events (input.cpp attaches each to its key-down).
	SDL_StartTextInput(window_);
	if (desc.mode != platform::WindowMode::Windowed) set_mode(desc.mode);
	return true;
}

void Window::set_title(const std::string &title) {
	if (window_ != nullptr) SDL_SetWindowTitle(window_, title.c_str());
}

void Window::set_size(int32_t width, int32_t height) {
	if (window_ != nullptr) SDL_SetWindowSize(window_, width, height);
}

platform::WindowSize Window::size() const {
	platform::WindowSize out;
	if (window_ != nullptr) {
		int w = 0;
		int h = 0;
		if (SDL_GetWindowSizeInPixels(window_, &w, &h)) {
			out.width = w;
			out.height = h;
		}
	}
	return out;
}

void Window::set_mode(platform::WindowMode mode) {
	if (window_ == nullptr) return;
	if (mode == platform::WindowMode::Fullscreen) {
		// A null mode is SDL's borderless fullscreen desktop: the display keeps its own mode.
		SDL_SetWindowFullscreenMode(window_, nullptr);
		SDL_SetWindowFullscreen(window_, true);
	} else {
		SDL_SetWindowFullscreen(window_, false);
	}
}

platform::WindowMode Window::mode() const {
	if (window_ != nullptr && (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0)
		return platform::WindowMode::Fullscreen;
	return platform::WindowMode::Windowed;
}

bool Window::focused() const {
	return window_ != nullptr && (SDL_GetWindowFlags(window_) & SDL_WINDOW_INPUT_FOCUS) != 0;
}

bool Window::take_close_request() {
	const bool requested = close_requested_;
	close_requested_ = false;
	return requested;
}

platform::NativeWindowHandle Window::native_handle() const {
	platform::NativeWindowHandle out;
	if (window_ == nullptr) return out;
	const SDL_PropertiesID props = SDL_GetWindowProperties(window_);
	if (void *hwnd = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr)) {
		out.kind = platform::NativeWindowHandle::Kind::Win32;
		out.window = hwnd;
		return out;
	}
	if (void *surface = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr)) {
		out.kind = platform::NativeWindowHandle::Kind::Wayland;
		out.window = surface;
		out.display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
		return out;
	}
	if (void *display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr)) {
		// The X11 Window is an XID, held as an integer in the pointer (platform/window.h).
		const Sint64 xid = SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
		out.kind = platform::NativeWindowHandle::Kind::X11;
		out.window = reinterpret_cast<void *>(static_cast<uintptr_t>(xid));
		out.display = display;
	}
	return out;
}

float Window::refresh_rate_hz() const {
	if (window_ == nullptr) return 0.0f;
	const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window_));
	return mode != nullptr ? mode->refresh_rate : 0.0f;
}

void Window::on_event(const SDL_Event &event) {
	// The OS's quit (the last window's close, a logoff, a console's Ctrl+C) is the window's close
	// request too: the engine answers both the same way.
	if (event.type == SDL_EVENT_QUIT ||
			(event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && window_ != nullptr &&
					event.window.windowID == SDL_GetWindowID(window_)))
		close_requested_ = true;
}

} // namespace opennova::sdl3
