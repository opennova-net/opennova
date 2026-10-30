// opennova-game: the game over the SDL3 backend (ADR 0077 d9), the app shell that becomes
// opennova.exe once Godot retires (R8; until then the Godot build keeps that name). main() brings
// the SDL3 platform up (backends/sdl3: the window, the keyboard and mouse, the audio sink, the
// clock and its pump) and the SDL_GPU device over its window, then runs the engine's frame:
//
//   1. the platform pump: the OS's events into the devices, the input device's raw events into
//      the engine's input frame (local_input.h);
//   2. the engine frame: the main loop's clock (inmatch::MainLoopClock over the platform's
//      clock, retail's GetTickCount bank and frame lock) and the session's advance, its updates
//      running the logic ticks with the input frame's rows applied;
//   3. the GPU frame: R5's render frame encodes its passes here (runtime/gpu over SDL_GPU); until
//      then the swapchain is cleared;
//   4. present: the frame's commands submitted, then the render callback's re-base stamp.
//
// Until the engine frontend (R3's runtime/frontend) owns the game flow, the app boots the one
// mission its command line names (local_mission.h), with no menus and no HUD, and plays the
// engine's audio stream once the engine's mixer produces one (R4's mixer lane).

#include "local_input.h"
#include "local_mission.h"

#include <backends/sdl3/backend.h>
#include <backends/sdl3/gpu.h>

#include <base/io/log.h>
#include <base/io/strutil.h>
#include <base/resource_index/boot_policy.h>
#include <formats/gamecfg/game_cfg.h>
#include <runtime/inmatch/main_loop_clock.h>

#include <games/games.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace opennova;

const char kUsage[] =
		"usage: opennova-game --resource-dir <game dir> --mission <file.bms> [/game <code>]\n"
		"                     [/exp <name>] [--frames <n>] [--headless] [--log-debug]\n"
		"  --resource-dir   the game folder to play from (its PFF set, or a loose tree)\n"
		"  --mission        the mission to boot, by its file name in the folder\n"
		"  /game, /exp      the title the folder runs as (default: the game its UPDATE.WIZ\n"
		"                   names, else acidgambit) and the expansion to mount (/mod is /exp)\n"
		"  --frames <n>     run <n> frames, then exit (0 = until the window closes)\n"
		"  --headless       SDL's dummy video and audio drivers and no GPU: no screen, no sound\n"
		"                   device (a CI smoke run)\n"
		"  --log-debug      print the engine's debug log lines\n";

// The app's own options; the rest are the game's launch flags (base/resource_index/boot_policy.h
// parse_launch_flags), the same opennova.exe takes.
struct AppOptions {
	int64_t frames = 0;
	bool headless = false;
	bool log_debug = false;
	bool help = false;
};

// `args` without the app's own options, which `options` takes. False with `error` on a bad value.
bool take_app_options(std::vector<std::string> &args, AppOptions &options, std::string &error) {
	std::vector<std::string> rest;
	for (size_t i = 0; i < args.size(); ++i) {
		const std::string &arg = args[i];
		if (strutil::iequals(arg, "--frames")) {
			const std::optional<int> frames =
					i + 1 < args.size() ? strutil::parse_int(args[i + 1]) : std::nullopt;
			if (!frames || *frames < 0) {
				error = "--frames takes a frame count";
				return false;
			}
			options.frames = *frames;
			++i;
		} else if (strutil::iequals(arg, "--headless")) {
			options.headless = true;
		} else if (strutil::iequals(arg, "--log-debug")) {
			options.log_debug = true;
		} else if (arg == "--help" || arg == "-h") {
			options.help = true;
		} else {
			rest.push_back(arg);
		}
	}
	args = std::move(rest);
	return true;
}

bool g_log_debug = false;

void install_log_sink() {
	io::set_log_sink([](io::LogLevel level, const char *message) {
		if (level < io::LogLevel::kInfo && !g_log_debug) return;
		std::fprintf(level >= io::LogLevel::kWarn ? stderr : stdout, "%s\n", message);
		std::fflush(level >= io::LogLevel::kWarn ? stderr : stdout);
	});
}

int run(const AppOptions &options, const LaunchFlags &flags, std::vector<std::string> arguments) {
	// The launch's product and the title it runs as, as the game's launch chooses them: /game when
	// given, else the folder's UPDATE.WIZ, a folder with none being OpenNova's own (ADR 0067 d2).
	const LaunchProduct product = launch_product(flags, folder_product(flags.resource_dir));
	if (!product.supported) {
		io::logf(io::LogLevel::kError, "opennova-game: %s holds no game OpenNova plays",
				flags.resource_dir.c_str());
		return 1;
	}

	std::string error;
	sdl3::BackendOptions backend_options;
	backend_options.arguments = std::move(arguments);
	backend_options.headless = options.headless;
	std::unique_ptr<sdl3::Backend> backend = sdl3::Backend::create(backend_options, error);
	if (backend == nullptr) {
		io::logf(io::LogLevel::kError, "opennova-game: %s", error.c_str());
		return 1;
	}
	const platform::Platform &platform = backend->platform();

	platform::WindowDesc window;
	window.title = product.name.empty() ? std::string("OpenNova") : product.name;
	if (!platform.window->create(window, error)) {
		io::logf(io::LogLevel::kError, "opennova-game: %s", error.c_str());
		return 1;
	}
	// The GPU device claims the window; a headless run has none (the dummy driver has no surface),
	// nor does a machine with no Vulkan, Direct3D 12 or Metal device, whose window stays blank.
	std::unique_ptr<sdl3::Gpu> gpu;
	if (!backend->headless()) {
		gpu = sdl3::Gpu::create(*backend, error);
		if (gpu == nullptr) io::logf(io::LogLevel::kWarn, "opennova-game: no GPU: %s", error.c_str());
	}
	// The engine's PCM sink, at the mixer's rate and form (16-bit stereo, as retail's mixer
	// writes): its stream is pushed at the engine frame's tail once the mixer produces one.
	platform::AudioSpec audio;
	if (!platform.audio->open(audio, error))
		io::logf(io::LogLevel::kWarn, "opennova-game: no audio: %s", error.c_str());
	io::logf(io::LogLevel::kInfo, "opennova-game: video %s, audio %s, gpu %s",
			backend->video_driver().c_str(), backend->audio_driver().c_str(),
			gpu != nullptr ? gpu->driver().c_str() : "none");

	game::LocalMission mission;
	game::MissionRequest request;
	request.root = flags.resource_dir;
	request.mission = flags.mission;
	request.expansion = flags.expansion;
	request.game = product.game;
	if (!mission.load(request, error)) {
		io::logf(io::LogLevel::kError, "opennova-game: %s", error.c_str());
		return 1;
	}
	game::LocalInput input(mission);
	if (!backend->headless()) platform.input->set_cursor_mode(platform::CursorMode::Captured);

	// The process's cfg block: Config_SetDefaults' until the frontend reads game.cfg (R3). Its
	// lock_framerate word (1) locks each frame to 16 ms of banked time.
	const gamecfg::GameCfg cfg = gamecfg::defaults();
	// The main loop's clock and the session's frame lock over the platform's clock (GetTickCount's
	// steps and Sleep(1), inmatch::PlatformFrameLockClock), one clock for the frame's stamp, its
	// lock and its CPU share; restarted past the load, which is never banked.
	inmatch::PlatformFrameLockClock lock_clock(*platform.clock);
	inmatch::MainLoopClock clock(&lock_clock);
	mission.session().set_frame_lock_clock(&lock_clock);
	clock.restart();
	int64_t frames = 0;
	int64_t ticks = 0;
	int code = 0;
	while (options.frames == 0 || frames < options.frames) {
		// 1. The platform pump.
		platform.clock->pump_events();
		if (platform.window->take_close_request()) break;
		input.pump(*platform.input);

		// 2. The engine frame.
		inmatch::FrameInput frame = clock.begin_frame(&cfg);
		input.begin_frame(frame);
		mission.kernel().profile.reset();
		const inmatch::FrameOutcome outcome = mission.session().advance(frame);
		clock.end_frame(outcome.frame_stamp_ms);
		ticks += outcome.ticks_run();
		if (outcome.terminal()) {
			io::logf(io::LogLevel::kError, "opennova-game: the session ended: %s",
					outcome.error.message.c_str());
			code = 3;
			break;
		}

		// 3. The GPU frame: R5's render frame records its passes in the clear's place.
		if (gpu != nullptr && gpu->begin_frame()) gpu->clear(0.0f, 0.0f, 0.0f, 1.0f);

		// 4. Present, then the render callback's end: the stamp a mission-start frame re-bases to.
		if (gpu != nullptr) gpu->present();
		clock.mark_rendered();
		++frames;
	}
	io::logf(io::LogLevel::kInfo, "opennova-game: %lld frames, %lld logic ticks",
			static_cast<long long>(frames), static_cast<long long>(ticks));
	(void)mission.session().close();
	return code;
}

} // namespace

int main(int argc, char **argv) {
	opennova::games::install(); // every game's titles, protocols and rules (games/games.h)
	std::vector<std::string> args(argv + 1, argv + argc);
	const std::vector<std::string> arguments = args;
	AppOptions options;
	std::string error;
	if (!take_app_options(args, options, error)) {
		std::fprintf(stderr, "opennova-game: %s\n%s", error.c_str(), kUsage);
		return 2;
	}
	if (options.help) {
		std::fputs(kUsage, stdout);
		return 0;
	}
	g_log_debug = options.log_debug;
	install_log_sink();
	const opennova::LaunchFlags flags = opennova::parse_launch_flags(args);
	if (flags.resource_dir.empty() || flags.mission.empty()) {
		std::fprintf(stderr, "opennova-game: --resource-dir and --mission name the mission\n%s",
				kUsage);
		return 2;
	}
	return run(options, flags, arguments);
}
