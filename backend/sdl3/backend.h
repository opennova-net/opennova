#pragma once

#include <base/platform/platform.h>

#include <memory>
#include <string>
#include <vector>

// The SDL3 platform backend (ADR 0077 d9): the engine's platform API (engine/base/platform) over
// SDL3, one per process. It brings SDL up, owns every device of the platform record (the clock and
// its event pump, the files, the OS services, the window, the keyboard and mouse, the audio sink)
// and takes SDL down with it. What any device's output means is the engine's; this backend only
// converts SDL's events, states and calls into the engine's own codes and back. Platform plumbing,
// not a port.
//
// No SDL type reaches past backend/sdl3 (scripts/lint/include_graph_check.py rule 11): this header
// and gpu.h are the backend's public face, and the devices' SDL halves live in its own .cpp files.
namespace opennova::sdl3 {

struct BackendOptions {
	// The arguments the game was started with, the program's own token left out
	// (platform::Os::arguments).
	std::vector<std::string> arguments;
	// SDL's dummy video and audio drivers: a window with no screen, an audio sink with no device and
	// no GPU (a CI smoke run, a machine with no display). Set as SDL's hints ahead of its init,
	// never through its environment variables.
	bool headless = false;
	// The organisation and application names the user's data directory is filed under
	// (SDL_GetPrefPath's: %APPDATA%\<org>\<app> on Windows, ~/.local/share/<app> on Linux).
	std::string org = "OpenNova";
	std::string app = "OpenNova";
};

class Gpu;
class Window;

class Backend final {
public:
	// SDL brought up with the video, audio and event subsystems; null with `error` when it does not
	// come up.
	static std::unique_ptr<Backend> create(const BackendOptions &options, std::string &error);
	~Backend();
	Backend(const Backend &) = delete;
	Backend &operator=(const Backend &) = delete;

	// The platform record the engine takes: every device present (the window is created by the
	// engine through it), each owned here and living as long as this backend.
	const platform::Platform &platform() const;

	// The drivers SDL runs ("windows", "x11", "wayland", "dummy"; "wasapi", "pulseaudio",
	// "dummy"), for the log.
	std::string video_driver() const;
	std::string audio_driver() const;
	bool headless() const;

private:
	Backend();
	// The GPU device claims the window.
	friend class Gpu;
	Window &window();
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace opennova::sdl3
