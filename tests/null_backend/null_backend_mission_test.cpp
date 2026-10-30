// The headless test backend's proof (ADR 0077 d8): a mission runs through the engine's platform
// seams with no Godot and no OS device, and the platform changes nothing.
//
// Two legs:
//   devices   each null device through its interface: the window's create, mode and latched
//             close request; the input device's ordered queue, its state and its cursor modes;
//             the clock's sleep and pump; the files' stream and atomic write; the OS's
//             arguments, child, URL and locale; the audio sink's queue on the clock and its peak.
//   mission   the synthetic mission (common/tick_digest.h) end to end over the platform: its
//             files read through platform::Files, its load the R2 executor's headless legs
//             (mission::run_mission_load, the session's start leg the kernel's boot), its frames
//             banked by inmatch::Session from the platform Clock (inmatch::MainLoopClock and the
//             session's frame lock over inmatch::PlatformFrameLockClock, game.cfg's lock on, so
//             each frame's lock sleeps through the platform), each update pumping the platform
//             and taking the device's scripted samples through the engine's conversions
//             (controls::queued_key, mouse_button_event, wheel_delta_units), the loop ending at
//             the window's close request. Asserts the tick chain equals the bare no-net rig's
//             committed kSyntheticDigest (tick_digest), the samples arrive in their order at
//             the first update past their time and convert as retail's window procedure
//             would, and the audio sink kept pace with the clock.
#include "common/file_io.h"
#include "common/test_paths.h"
#include "common/tick_digest.h"
#include "null_backend/null_platform.h"

#include <base/io/strutil.h>
#include <base/resource_index/resource_index.h>
#include <base/vfs/file_source.h>
#include <formats/gamecfg/game_cfg.h>
#include <runtime/assets/asset_store.h>
#include <runtime/controls/device_messages.h>
#include <runtime/controls/key_event_queue.h>
#include <runtime/controls/player_actions.h> // WheelRemainder
#include <runtime/inmatch/local_role.h>
#include <runtime/inmatch/main_loop_clock.h>
#include <runtime/inmatch/session.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/mission/mission_load.h>
#include <runtime/mission/runtime_boot.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace opennova;
namespace nb = opennova::null_backend;
namespace pf = opennova::platform;

static int failures = 0;
#define CHECK(c)                                                                                   \
	do {                                                                                           \
		if (!(c)) {                                                                                \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                               \
			++failures;                                                                            \
		}                                                                                          \
	} while (0)

namespace {

// --- devices --------------------------------------------------------------------------------

pf::InputEvent key(pf::KeyCode code, uint16_t scancode, bool down, bool repeat = false) {
	pf::InputEvent e;
	e.type = pf::InputEvent::Type::Key;
	e.key.key = code;
	e.key.scancode = static_cast<pf::Scancode>(scancode);
	e.key.down = down;
	e.key.repeat = repeat;
	return e;
}

pf::InputEvent button(pf::MouseButton which, bool down, uint8_t clicks = 1) {
	pf::InputEvent e;
	e.type = pf::InputEvent::Type::MouseButton;
	e.button.button = which;
	e.button.down = down;
	e.button.clicks = clicks;
	return e;
}

pf::InputEvent wheel(float notches) {
	pf::InputEvent e;
	e.type = pf::InputEvent::Type::MouseWheel;
	e.wheel.y = notches;
	return e;
}

pf::InputEvent motion(float dx, float dy) {
	pf::InputEvent e;
	e.type = pf::InputEvent::Type::MouseMotion;
	e.motion.dx = dx;
	e.motion.dy = dy;
	return e;
}

pf::InputEvent focus(bool gained) {
	pf::InputEvent e;
	e.type = pf::InputEvent::Type::Focus;
	e.focus.gained = gained;
	return e;
}

void devices_leg() {
	nb::NullPlatform backend;
	const pf::Platform platform = backend.record();
	CHECK(platform.clock && platform.files && platform.os && platform.window && platform.input &&
			platform.audio);

	// The window.
	std::string error;
	pf::Window &window = *platform.window;
	CHECK(!window.focused());
	CHECK(window.create({ "OpenNova", 800, 600, pf::WindowMode::Windowed }, error));
	CHECK(!window.create({ "again", 800, 600, pf::WindowMode::Windowed }, error));
	CHECK(window.size().width == 800 && window.size().height == 600);
	CHECK(window.focused());
	window.set_mode(pf::WindowMode::Fullscreen);
	CHECK(window.size().width == nb::NullWindow::kDisplayWidth);
	window.set_mode(pf::WindowMode::Windowed);
	window.set_size(1024, 768);
	CHECK(window.size().width == 1024 && window.size().height == 768);
	CHECK(window.refresh_rate_hz() == nb::NullWindow::kRefreshRateHz);
	CHECK(window.native_handle().kind == pf::NativeWindowHandle::Kind::None);

	// The clock, the pump and the input device: a sample is reported only by the first pump at
	// or past its time, in script order, the focus among the keys.
	pf::Clock &clock = *platform.clock;
	const uint64_t t0 = clock.now_us();
	CHECK(clock.now_ms() == t0 / 1000u);
	backend.script(t0 + 2000, key(pf::KeyCode::Space, 0x39, true));
	backend.script(t0 + 2000, focus(false));
	backend.script(t0 + 1000, button(pf::MouseButton::Right, true));
	backend.script_close_request(t0 + 3000);
	pf::InputDevice &input = *platform.input;
	pf::InputEvent event;
	clock.pump_events();
	CHECK(!input.poll_event(event));
	clock.sleep_ms(1);
	CHECK(clock.now_us() == t0 + 1000);
	CHECK(input.state().buttons == 0); // nothing pumped yet
	clock.pump_events();
	CHECK(input.state().buttons == (1u << static_cast<unsigned>(pf::MouseButton::Right)));
	CHECK(input.poll_event(event) && event.type == pf::InputEvent::Type::MouseButton);
	CHECK(!input.poll_event(event));
	clock.sleep_ms(1);
	clock.pump_events();
	CHECK(input.poll_event(event) && event.type == pf::InputEvent::Type::Key && event.key.down);
	CHECK(input.poll_event(event) && event.type == pf::InputEvent::Type::Focus &&
			!event.focus.gained);
	CHECK(!window.focused());
	CHECK(!window.take_close_request());
	clock.sleep_ms(1);
	clock.pump_events();
	CHECK(window.take_close_request());
	CHECK(!window.take_close_request()); // the read cleared it
	CHECK(backend.clock().pumps() == 4 && backend.clock().slept_ms() == 3);

	// The cursor: a free cursor travels with the mouse, a captured one holds and reports only
	// motion; the warp moves it.
	input.warp_cursor(100, 100);
	backend.script(clock.now_us(), motion(5.0f, -3.0f));
	clock.pump_events();
	CHECK(input.state().cursor_x == 105 && input.state().cursor_y == 97);
	input.set_cursor_mode(pf::CursorMode::Captured);
	CHECK(input.state().cursor_captured);
	backend.script(clock.now_us(), motion(7.0f, 7.0f));
	clock.pump_events();
	CHECK(input.state().cursor_x == 105 && input.state().cursor_y == 97);
	CHECK(input.poll_event(event) && event.type == pf::InputEvent::Type::MouseMotion);
	CHECK(input.poll_event(event) && event.motion.dx == 7.0f);
	input.set_cursor_mode(pf::CursorMode::Hidden);
	CHECK(!input.state().cursor_captured);

	// The files: the directories, a write stream that lands as it closes, a read stream's seek,
	// the whole-file read and the atomic write.
	pf::Files &files = *platform.files;
	CHECK(files.executable_dir() == nb::NullFiles::kExecutableDir);
	CHECK(files.user_data_dir() == nb::NullFiles::kUserDataDir);
	const std::string path = files.user_data_dir() + "/player.sav";
	CHECK(files.open(path, pf::FileMode::Read, error) == nullptr);
	{
		std::unique_ptr<pf::FileStream> out = files.open(path, pf::FileMode::Write, error);
		CHECK(out != nullptr);
		if (out)
			CHECK(out->write("abcdef", 6) == 6);
	}
	{
		std::unique_ptr<pf::FileStream> in = files.open(path, pf::FileMode::Read, error);
		CHECK(in != nullptr);
		if (in) {
			char got[4] = {};
			CHECK(in->size() == 6);
			CHECK(in->seek(-2, pf::SeekFrom::End) && in->tell() == 4);
			CHECK(in->read(got, sizeof(got)) == 2 && std::memcmp(got, "ef", 2) == 0);
			CHECK(in->write("x", 1) == 0); // a read stream writes nothing
		}
	}
	CHECK(files.write_file(path, "xyz", 3, error));
	std::vector<uint8_t> bytes;
	CHECK(files.read_file(path, bytes, error) && bytes == std::vector<uint8_t>({ 'x', 'y', 'z' }));
	CHECK(!files.read_file(path + ".missing", bytes, error) && bytes.empty());

	// The OS: the launch, the child's exit code at its poll, a kill's, the URL, the locale.
	nb::NullOs &os = backend.os();
	os.set_arguments({ "/game", "jo" });
	os.set_command_line("opennova.exe /game jo");
	CHECK(platform.os->arguments().size() == 2 && platform.os->command_line().size() == 21);
	CHECK(platform.os->process_id() == nb::NullOs::kProcessId);
	proc::SpawnOptions spawn;
	CHECK(platform.os->spawn(spawn, error) == nullptr);
	spawn.argv = { "update.exe", "--check" };
	os.set_child_exit_code(10);
	std::unique_ptr<pf::Process> child = platform.os->spawn(spawn, error);
	CHECK(child != nullptr && child->pid() != 0);
	if (child) {
		const std::optional<uint32_t> code = child->poll();
		CHECK(code && *code == 10);
		CHECK(child->pid() == 0);
	}
	std::unique_ptr<pf::Process> killed = platform.os->spawn(spawn, error);
	if (killed) {
		killed->kill();
		const std::optional<uint32_t> code = killed->poll();
		CHECK(code && *code == 1);
	}
	CHECK(os.spawned().size() == 2 && os.spawned()[0].argv[1] == "--check");
	CHECK(platform.os->open_url("https://opennova.net") && os.opened_urls().size() == 1);
	CHECK(pf::locale_tag(platform.os->locale()) == "en_US");
	CHECK(pf::locale_tag({ "de", "" }) == "de" && pf::locale_tag({}).empty());

	// The audio sink: it plays at its rate on the clock, silence once it runs dry, and the peak
	// is the pushed frames' largest sample before the gain.
	pf::AudioDevice &audio = *platform.audio;
	CHECK(!audio.push(nullptr, 0));
	CHECK(audio.open({ 44100, 2, pf::SampleFormat::S16 }, error) && audio.is_open());
	CHECK(pf::audio_frame_bytes({ 44100, 2, pf::SampleFormat::S16 }) == 4);
	std::vector<int16_t> frames(2 * 4410, 0);
	frames[7] = -16384;
	CHECK(audio.push(frames.data(), 4410));
	audio.set_gain(0.5f);
	CHECK(audio.queued_frames() == 4410);
	CHECK(audio.take_peak() == 0.5f && audio.take_peak() == 0.0f);
	clock.sleep_ms(50); // 2205 frames played
	CHECK(audio.queued_frames() == 2205);
	clock.sleep_ms(100); // dry
	CHECK(audio.queued_frames() == 0);
	audio.close();
	CHECK(!audio.is_open() && !audio.push(frames.data(), 1));
}

// --- mission --------------------------------------------------------------------------------

// The mission's resource directory on the platform's files.
const std::string kResourceDir = std::string(nb::NullFiles::kExecutableDir) + "/resource";

// The mounted resource root over the platform's files: a flat name, its case aside, at the
// resource directory (the clips' FileSource).
class PlatformFileSource final : public FileSource {
public:
	explicit PlatformFileSource(pf::Files &files) : files_(files) {}
	bool read(const std::string &name, std::vector<uint8_t> &out) const override {
		std::string error;
		return files_.read_file(kResourceDir + "/" + strutil::to_lower(name), out, error);
	}
	uint64_t stamp(const std::string &name) const override {
		std::string error;
		return files_.open(kResourceDir + "/" + strutil::to_lower(name), pf::FileMode::Read,
					   error) != nullptr
				? 1
				: 0;
	}

private:
	pf::Files &files_;
};

// The mission document's own files over the platform's (the kernel's boot source), by their
// exact names, as the rig's in-memory map answers them.
mission::BootFileSource boot_source(pf::Files &files) {
	mission::BootFileSource source;
	source.has_file = [&files](const std::string &name) {
		std::string error;
		return files.open(kResourceDir + "/" + name, pf::FileMode::Read, error) != nullptr;
	};
	source.read_file = [&files](const std::string &name, std::vector<uint8_t> &out) {
		std::string error;
		return files.read_file(kResourceDir + "/" + name, out, error);
	};
	return source;
}

// One scripted sample and the time it is due.
struct Sample {
	uint64_t at_us;
	pf::InputEvent event;
};

// What the engine's conversions made of one sample, and when the update took it.
struct Taken {
	pf::InputEvent::Type type = pf::InputEvent::Type::None;
	uint32_t value = 0; // the queued key's VK | flags << 16, the mouse event, the notches
	uint64_t taken_us = 0;
	uint64_t previous_pump_us = 0;
};

// The update's input frame over the platform: the update's message pump (Clock::pump_events,
// where retail's update pumps its window messages ahead of its input frame:
// TickInputSource carries the witness), then every event the device reports through the
// engine's conversions: a key-down's queue entry with Ctrl and Shift as the key state holds
// them, a button's mouse event, a wheel turn's notches, a focus change's key-state clear. No
// binding table is loaded, so no conversion reaches a row and every update's sample is empty:
// the session ticks as the bare rig's frames do.
class PlatformInputSource final : public inmatch::TickInputSource {
public:
	explicit PlatformInputSource(const pf::Platform &platform) : platform_(platform) {}

	void run_update_rows(uint32_t logic_tick, bool new_sample) override {
		(void)logic_tick;
		(void)new_sample;
		const uint64_t previous = last_pump_us_;
		last_pump_us_ = platform_.clock->now_us();
		platform_.clock->pump_events();
		pf::InputEvent event;
		while (platform_.input->poll_event(event))
			take(event, previous);
	}
	inmatch::InputPacket sample_tick(uint32_t logic_tick) override {
		(void)logic_tick;
		return {};
	}

	std::vector<Taken> taken;
	controls::KeyEventQueue keys;

private:
	void take(const pf::InputEvent &event, uint64_t previous_pump_us) {
		Taken t;
		t.type = event.type;
		t.taken_us = last_pump_us_;
		t.previous_pump_us = previous_pump_us;
		switch (event.type) {
			case pf::InputEvent::Type::Key: {
				const int vk = static_cast<int>(event.key.key) & 0xFF;
				if (event.key.down) {
					key_down_[static_cast<size_t>(vk)] = true;
					const controls::QueuedKey queued =
							controls::queued_key(event.key, key_down_[0x11], key_down_[0x10]);
					if (queued.vk != 0)
						keys.push(queued.vk, queued.flags);
					t.value = static_cast<uint32_t>(queued.vk) |
							(static_cast<uint32_t>(queued.flags) << 16);
				} else {
					key_down_[static_cast<size_t>(vk)] = false;
				}
				break;
			}
			case pf::InputEvent::Type::MouseButton:
				t.value = controls::mouse_button_event(event.button);
				break;
			case pf::InputEvent::Type::MouseWheel:
				t.value = static_cast<uint32_t>(
						wheel_.feed(controls::wheel_delta_units(event.wheel)));
				break;
			case pf::InputEvent::Type::Focus:
				// The window procedure clears the key state alone on either change
				// (controls::KeyStateReset carries the witness).
				key_down_.fill(false);
				break;
			default:
				break;
		}
		taken.push_back(t);
	}

	const pf::Platform &platform_;
	uint64_t last_pump_us_ = 0;
	std::array<bool, 256> key_down_{};
	controls::WheelRemainder wheel_;
};

// The tick chain, folded after every tick the session runs, to the rig's count.
class DigestObserver final : public inmatch::TickObserver {
public:
	explicit DigestObserver(const world::World &world) : world_(world) {}
	void after_tick() override {
		if (ticks >= test_digest::kSyntheticTicks)
			return;
		test_digest::hash_world(digest, world_);
		++ticks;
	}
	test_digest::Digest digest;
	int ticks = 0;

private:
	const world::World &world_;
};

// The frame's work and present on the backend's clock: under the lock's 16 ms, so every frame's
// lock sleeps the rest through the platform.
constexpr uint64_t kFrameWorkUs = 7000;
// The user closes the window past the rig's ticks (240 ticks of 16 ms: 3.84 s).
constexpr uint64_t kCloseAfterUs = 4500000;

void mission_leg() {
	nb::NullPlatform backend;
	const pf::Platform platform = backend.record();
	std::string error;

	// The mission's files on the platform's tree.
	pf::Files &files = *platform.files;
	backend.files().put(kResourceDir + "/synth.wac", std::string(test_digest::kSyntheticWac));
	backend.files().put(kResourceDir + "/items.def", std::string(test_digest::kPeopleItems));
	for (const char *name : test_digest::kPeopleClipFiles) {
		std::vector<uint8_t> clip =
				test_io::read_file(test_paths_fixture(std::string("anim/") + name));
		CHECK(!clip.empty());
		backend.files().put(kResourceDir + "/" + name, std::move(clip));
	}

	CHECK(platform.window->create({ "OpenNova", 800, 600, pf::WindowMode::Windowed }, error));
	CHECK(platform.audio->open({ 44100, 2, pf::SampleFormat::S16 }, error));

	// The samples: a Ctrl chord, an OS repeat, a keypad Enter under Shift, a focus loss with Ctrl
	// still down (an Alt+Tab's shape: its key state clears, so the next key queues bare), the
	// mouse's clicks (a double click's second press), the wheel's whole and half notches. Times
	// are after the load, inside the frames.
	const uint64_t start = backend.clock().now_us();
	const std::vector<Sample> samples = {
		{ start + 300000, key(pf::KeyCode::Control, 0x1D, true) },
		{ start + 300000, key(static_cast<pf::KeyCode>('A'), 0x1E, true) },
		{ start + 300000, key(static_cast<pf::KeyCode>('A'), 0x1E, true, /*repeat=*/true) },
		{ start + 400000, key(static_cast<pf::KeyCode>('A'), 0x1E, false) },
		{ start + 500000, key(pf::KeyCode::Shift, 0x2A, true) },
		{ start + 500000, key(pf::KeyCode::Return, pf::kScancodeExtended | 0x1C, true) },
		{ start + 600000, focus(false) },
		{ start + 700000, focus(true) },
		{ start + 700000, key(static_cast<pf::KeyCode>('A'), 0x1E, true) },
		{ start + 800000, button(pf::MouseButton::Left, true) },
		{ start + 810000, button(pf::MouseButton::Left, false) },
		{ start + 820000, button(pf::MouseButton::Left, true, /*clicks=*/2) },
		{ start + 900000, wheel(1.0f) },
		{ start + 1000000, wheel(-0.5f) },
		{ start + 1100000, wheel(-0.5f) },
	};
	for (const Sample &sample : samples)
		backend.script(sample.at_us, sample.event);
	backend.script_close_request(start + kCloseAfterUs);

	// The load: the R2 executor's headless legs. The session's start leg is the kernel's boot
	// (the bare local role runs no session); the device's one leg is the loading screen's
	// present, which takes a frame's time on the backend.
	ResourceIndex index;
	CHECK(index.mount_source(std::make_shared<PlatformFileSource>(files)));
	const assets::AssetStore store(&index);
	mission::MissionKernel kernel;
	kernel.set_assets(&store);
	kernel.open_document(test_digest::people_mission(), "synth", boot_source(files));
	inmatch::LocalRole role;
	role.bind(kernel);
	inmatch::Session session(role);
	CHECK(session.begin_load().applied());
	std::vector<int> presents;
	mission::MissionLoadLegs legs;
	legs.session.start_runtime = [&kernel, &error]() {
		mission::KernelBootOptions options;
		return kernel.boot(options, error) ? 0 : 1;
	};
	legs.session.role = [] { return mission::MissionLoadRole::None; };
	legs.device.present = [&](int percent) {
		presents.push_back(percent);
		backend.clock().advance_us(kFrameWorkUs);
	};
	const int load = mission::run_mission_load(mission::MissionLoadArm{}, legs);
	CHECK(load == 0);
	if (load != 0) {
		std::printf("FAIL: load: %s\n", error.c_str());
		return;
	}
	CHECK(kernel.root_motion.clip_count(0) != 0); // the people's table came through the files
	CHECK(!presents.empty() && presents.back() == mission::kMissionLoadProgressWorldReady);
	for (size_t i = 1; i < presents.size(); ++i)
		CHECK(presents[i] >= presents[i - 1]);
	CHECK(session.complete_load().applied());

	// The frames: the main loop's clock and the session's lock over the platform's clock, the
	// load's time never banked (the clock restarts across it), the player's cursor captured as
	// play holds it.
	inmatch::PlatformFrameLockClock lock_clock(*platform.clock);
	inmatch::MainLoopClock main_clock(&lock_clock);
	session.set_frame_lock_clock(&lock_clock);
	const gamecfg::GameCfg cfg = gamecfg::defaults();
	CHECK(inmatch::main_loop_frame_lock(&cfg));
	main_clock.restart();
	platform.input->set_cursor_mode(pf::CursorMode::Captured);
	CHECK(platform.input->state().cursor_captured);

	DigestObserver observer(kernel.world);
	test_digest::hash_world(observer.digest, kernel.world);
	session.set_tick_observer(&observer);
	PlatformInputSource source(platform);
	std::vector<int16_t> silence;
	const uint64_t frames_start_us = platform.clock->now_us();
	const uint64_t audio_start = backend.audio().pushed_frames();
	int frames = 0;
	bool closed = false;
	while (frames < 10000) {
		inmatch::FrameInput input = main_clock.begin_frame(&cfg);
		input.tick_source = &source;
		const inmatch::FrameOutcome out = session.advance(input);
		CHECK(!out.terminal());
		if (out.terminal())
			break;
		main_clock.end_frame(out.frame_stamp_ms);
		// The frame's mix keeps the sink a frame ahead (silence: the synthetic mission sounds
		// nothing), then the present.
		const uint32_t want = 44100 / 30;
		const uint32_t queued = platform.audio->queued_frames();
		if (queued < want) {
			silence.assign(static_cast<size_t>(want - queued) * 2, 0);
			platform.audio->push(silence.data(), want - queued);
		}
		backend.clock().advance_us(kFrameWorkUs);
		main_clock.mark_rendered();
		++frames;
		// The window's close request ends the loop, the engine's answer to it.
		if (platform.window->take_close_request()) {
			closed = true;
			break;
		}
	}
	session.set_tick_observer(nullptr);

	// The chain is the bare no-net rig's.
	std::printf("digest null_backend %d %016llx (%d frames, %llu ms slept)\n", observer.ticks,
			static_cast<unsigned long long>(observer.digest.h), frames,
			static_cast<unsigned long long>(backend.clock().slept_ms()));
	CHECK(closed);
	CHECK(observer.ticks == test_digest::kSyntheticTicks);
	CHECK(observer.digest.h == test_digest::kSyntheticDigest);
	// The lock slept every frame's rest through the platform.
	CHECK(backend.clock().slept_ms() > 0);
	const uint64_t frames_us = platform.clock->now_us() - frames_start_us;
	CHECK(frames > 0 && frames_us / static_cast<uint64_t>(frames) >= 15000);

	// The samples: each taken in script order by the first update pump at or past its time,
	// converted as the window procedure's messages are.
	CHECK(source.taken.size() == samples.size());
	for (size_t i = 0; i < source.taken.size() && i < samples.size(); ++i) {
		const Taken &t = source.taken[i];
		CHECK(t.type == samples[i].event.type);
		CHECK(t.taken_us >= samples[i].at_us);
		CHECK(t.previous_pump_us < samples[i].at_us);
	}
	auto queued_key = [](int vk, uint16_t flags) {
		return static_cast<uint32_t>(vk) | (static_cast<uint32_t>(flags) << 16);
	};
	namespace kf = controls::key_event_flag;
	if (source.taken.size() == samples.size()) {
		CHECK(source.taken[0].value == queued_key(0x11, kf::kCtrl)); // Ctrl's own press holds Ctrl
		CHECK(source.taken[1].value == queued_key('A', kf::kCtrl));
		CHECK(source.taken[2].value == queued_key('A', kf::kCtrl | kf::kRepeat));
		CHECK(source.taken[4].value == queued_key(0x10, kf::kCtrl | kf::kShift)); // Ctrl still held
		CHECK(source.taken[5].value ==
				queued_key(0x0D, kf::kCtrl | kf::kShift | kf::kExtended)); // keypad Enter
		CHECK(source.taken[8].value == queued_key('A', 0)); // the focus loss cleared Ctrl
		CHECK(source.taken[9].value == 2u); // WM_LBUTTONDOWN
		CHECK(source.taken[10].value == 4u); // WM_LBUTTONUP
		CHECK(source.taken[11].value == 0x400u); // WM_LBUTTONDBLCLK
		CHECK(static_cast<int32_t>(source.taken[12].value) == 1);
		CHECK(static_cast<int32_t>(source.taken[13].value) == 0); // half a notch
		CHECK(static_cast<int32_t>(source.taken[14].value) == -1);
	}
	controls::KeyEvent queued;
	int queued_count = 0;
	while (source.keys.pop(queued))
		++queued_count;
	CHECK(queued_count == 6); // every key-down's entry, the repeat's too
	CHECK(platform.window->focused());

	// The sink kept pace with the clock: what it played is the frames' time at its rate.
	const uint64_t played =
			backend.audio().pushed_frames() - audio_start - platform.audio->queued_frames();
	const uint64_t expected = frames_us * 44100u / 1000000u;
	CHECK(played + 44100 / 30 >= expected && played <= expected + 44100 / 30);
}

} // namespace

int main() {
	devices_leg();
	mission_leg();
	if (failures != 0) {
		std::printf("null_backend_mission: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("null_backend_mission: OK\n");
	return 0;
}
