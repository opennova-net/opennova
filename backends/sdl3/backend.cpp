#include "backend.h"

#include "audio.h"
#include "input.h"
#include "window.h"

#include <base/platform/disk_files.h>
#include <base/platform/process.h>
#include <base/proc/child_process.h>

#include <SDL3/SDL.h>
// SDL_SetMainReady alone: the game's own main() is the entry point, so no SDL_main.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <string>
#include <utility>

namespace opennova::sdl3 {

namespace {

// SDL's own allocations a call hands back: freed with SDL_free.
std::string take_sdl_string(char *text) {
	std::string out = text != nullptr ? text : "";
	SDL_free(text);
	return out;
}

// A directory SDL names with its trailing separator, without it ('/'-separated).
std::string directory(std::string path) {
	for (char &c : path)
		if (c == '\\') c = '/';
	while (path.size() > 1 && path.back() == '/') path.pop_back();
	return path;
}

// The monotonic clock and the pump: SDL_GetTicksNS, SDL_Delay, and every pending SDL event
// handed to the window and the input device in the order SDL queued it.
class Clock final : public platform::Clock {
public:
	Clock(Window &window, InputDevice &input) : window_(window), input_(input) {}
	uint64_t now_us() const override { return SDL_GetTicksNS() / 1000u; }
	void sleep_ms(uint32_t ms) override { SDL_Delay(ms); }
	void pump_events() override {
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			window_.on_event(event);
			input_.on_event(event);
		}
	}

private:
	Window &window_;
	InputDevice &input_;
};

// The disk's files (base/platform DiskFiles) under SDL's two directories.
class Files final : public platform::DiskFiles {
public:
	Files(std::string executable_dir, std::string user_data_dir) :
			executable_dir_(std::move(executable_dir)), user_data_dir_(std::move(user_data_dir)) {}
	std::string executable_dir() const override { return executable_dir_; }
	std::string user_data_dir() const override { return user_data_dir_; }

private:
	std::string executable_dir_;
	std::string user_data_dir_;
};

// The OS's services: the arguments main() was handed, base/proc's command line, process id and
// child, SDL's URL handler and preferred locale.
class Os final : public platform::Os {
public:
	explicit Os(std::vector<std::string> arguments) : arguments_(std::move(arguments)) {}
	std::vector<std::string> arguments() const override { return arguments_; }
	std::string command_line() const override { return proc::own_command_line(); }
	int64_t process_id() const override { return proc::own_process_id(); }
	std::unique_ptr<platform::Process> spawn(const proc::SpawnOptions &options,
			std::string &error) override {
		return platform::ChildProcessHandle::spawn(options, error);
	}
	bool open_url(const std::string &url) override { return SDL_OpenURL(url.c_str()); }
	platform::Locale locale() const override {
		platform::Locale out;
		int count = 0;
		SDL_Locale **locales = SDL_GetPreferredLocales(&count);
		if (locales != nullptr && count > 0 && locales[0] != nullptr) {
			if (locales[0]->language != nullptr) out.language = locales[0]->language;
			if (locales[0]->country != nullptr) out.country = locales[0]->country;
		}
		SDL_free(locales);
		return out;
	}

private:
	std::vector<std::string> arguments_;
};

} // namespace

struct Backend::Impl {
	Impl(const BackendOptions &options, std::string executable_dir, std::string user_data_dir) :
			headless(options.headless),
			input(window),
			clock(window, input),
			files(std::move(executable_dir), std::move(user_data_dir)),
			os(options.arguments) {
		record.clock = &clock;
		record.files = &files;
		record.os = &os;
		record.window = &window;
		record.input = &input;
		record.audio = &audio;
	}

	bool headless = false;
	Window window;
	InputDevice input;
	AudioDevice audio;
	Clock clock;
	Files files;
	Os os;
	platform::Platform record;
};

Backend::Backend() = default;

Backend::~Backend() {
	// The devices go first (the audio stream, the window), then SDL.
	impl_.reset();
	SDL_Quit();
}

std::unique_ptr<Backend> Backend::create(const BackendOptions &options, std::string &error) {
	// The game's own main() is the entry point (no SDL_main).
	SDL_SetMainReady();
	if (options.headless) {
		SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
		SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
	}
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
		error = std::string("SDL_Init: ") + SDL_GetError();
		SDL_Quit();
		return nullptr;
	}
	const char *base = SDL_GetBasePath();
	std::string executable_dir = directory(base != nullptr ? base : "");
	std::string user_data_dir =
			directory(take_sdl_string(SDL_GetPrefPath(options.org.c_str(), options.app.c_str())));
	std::unique_ptr<Backend> backend(new Backend());
	backend->impl_ = std::make_unique<Impl>(options, std::move(executable_dir),
			std::move(user_data_dir));
	return backend;
}

const platform::Platform &Backend::platform() const {
	return impl_->record;
}

Window &Backend::window() {
	return impl_->window;
}

std::string Backend::video_driver() const {
	const char *name = SDL_GetCurrentVideoDriver();
	return name != nullptr ? name : "";
}

std::string Backend::audio_driver() const {
	const char *name = SDL_GetCurrentAudioDriver();
	return name != nullptr ? name : "";
}

bool Backend::headless() const {
	return impl_->headless;
}

} // namespace opennova::sdl3
