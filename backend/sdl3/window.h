#pragma once

#include <base/platform/window.h>

#include <SDL3/SDL.h>

#include <string>

// The platform window over an SDL_Window (platform/window.h): created once by the engine,
// its state read from SDL on demand, the close request latched from the event pump. Backend
// internal: the GPU device claims the SDL_Window it holds.
namespace opennova::sdl3 {

class Window final : public platform::Window {
public:
	Window() = default;
	~Window() override;
	Window(const Window &) = delete;
	Window &operator=(const Window &) = delete;

	bool create(const platform::WindowDesc &desc, std::string &error) override;
	void set_title(const std::string &title) override;
	void set_size(int32_t width, int32_t height) override;
	platform::WindowSize size() const override;
	void set_mode(platform::WindowMode mode) override;
	platform::WindowMode mode() const override;
	bool focused() const override;
	bool take_close_request() override;
	platform::NativeWindowHandle native_handle() const override;
	float refresh_rate_hz() const override;

	// One event of the pump: the close request (the window's close button, the OS's quit).
	void on_event(const SDL_Event &event);
	// The SDL window, null until create().
	SDL_Window *handle() const { return window_; }

private:
	SDL_Window *window_ = nullptr;
	bool close_requested_ = false;
};

} // namespace opennova::sdl3
