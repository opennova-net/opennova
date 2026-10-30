// The headless test backend (ADR 0077 d8): every interface of engine/base/platform implemented
// with no OS window, device, process or disk. Time is simulated: the clock moves only by the
// sleeps the engine asks for and by advance_us, the backend's own time passing (a present's
// swap). The input device and the window take scripted samples, each at a time on that clock,
// and the pump delivers the due ones in their order, as an OS queue would. Files are an in-memory
// tree, a spawned child exits with a scripted code, and the audio sink plays at its rate on the
// simulated clock. The ctest null_backend_mission runs a mission through these seams; any ctest
// that wants a platform with no OS behind it links opennova_null_backend.
#pragma once

#include <base/platform/platform.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace opennova::null_backend {

class NullPlatform;

class NullWindow final : public platform::Window {
public:
	// The null display: what a fullscreen window covers.
	static constexpr int32_t kDisplayWidth = 1920;
	static constexpr int32_t kDisplayHeight = 1080;
	static constexpr float kRefreshRateHz = 60.0f;

	bool create(const platform::WindowDesc &desc, std::string &error) override;
	void set_title(const std::string &title) override { title_ = title; }
	void set_size(int32_t width, int32_t height) override;
	platform::WindowSize size() const override;
	void set_mode(platform::WindowMode mode) override { mode_ = mode; }
	platform::WindowMode mode() const override { return mode_; }
	bool focused() const override { return created_ && focused_; }
	bool take_close_request() override;
	platform::NativeWindowHandle native_handle() const override { return {}; }
	float refresh_rate_hz() const override { return kRefreshRateHz; }

	bool created() const { return created_; }
	const std::string &title() const { return title_; }

private:
	friend class NullPlatform;
	bool created_ = false;
	std::string title_;
	platform::WindowSize windowed_;
	platform::WindowMode mode_ = platform::WindowMode::Windowed;
	bool focused_ = false;
	bool close_requested_ = false;
};

class NullInputDevice final : public platform::InputDevice {
public:
	bool poll_event(platform::InputEvent &out) override;
	platform::InputState state() const override { return state_; }
	void set_cursor_mode(platform::CursorMode mode) override;
	void warp_cursor(int32_t x, int32_t y) override;

	platform::CursorMode cursor_mode() const { return mode_; }

private:
	friend class NullPlatform;
	// One delivered event: queued, and the state it moves (the buttons, the cursor where it is
	// not captured, the lock keys' toggles on their key-downs), as the OS's state moves with
	// the messages it hands out.
	void deliver(const platform::InputEvent &event);

	std::deque<platform::InputEvent> queue_;
	platform::InputState state_;
	platform::CursorMode mode_ = platform::CursorMode::Visible;
};

class NullClock final : public platform::Clock {
public:
	explicit NullClock(NullPlatform &platform, uint64_t start_us) :
			platform_(platform), now_us_(start_us) {}
	uint64_t now_us() const override { return now_us_; }
	void sleep_ms(uint32_t ms) override;
	void pump_events() override;

	// The backend's own time passing (a present's swap, the work of a frame).
	void advance_us(uint64_t us) { now_us_ += us; }
	uint64_t slept_ms() const { return slept_ms_; }
	int pumps() const { return pumps_; }

private:
	NullPlatform &platform_;
	uint64_t now_us_;
	uint64_t slept_ms_ = 0;
	int pumps_ = 0;
};

class NullFiles final : public platform::Files {
public:
	static constexpr const char *kExecutableDir = "/null/bin";
	static constexpr const char *kUserDataDir = "/null/user";

	std::string executable_dir() const override { return kExecutableDir; }
	std::string user_data_dir() const override { return kUserDataDir; }
	std::unique_ptr<platform::FileStream> open(
			const std::string &path, platform::FileMode mode, std::string &error) override;
	bool read_file(const std::string &path, std::vector<uint8_t> &out, std::string &error) override;
	bool write_file(
			const std::string &path, const void *data, size_t size, std::string &error) override;

	// A file put in place (a test's fixture).
	void put(const std::string &path, std::vector<uint8_t> bytes);
	void put(const std::string &path, const std::string &text);
	bool exists(const std::string &path) const { return files_.count(path) != 0; }

private:
	std::map<std::string, std::vector<uint8_t>> files_;
};

class NullOs final : public platform::Os {
public:
	static constexpr int64_t kProcessId = 4242;

	std::vector<std::string> arguments() const override { return arguments_; }
	std::string command_line() const override { return command_line_; }
	int64_t process_id() const override { return kProcessId; }
	std::unique_ptr<platform::Process> spawn(
			const proc::SpawnOptions &options, std::string &error) override;
	bool open_url(const std::string &url) override;
	platform::Locale locale() const override { return locale_; }

	// The launch the test describes.
	void set_arguments(std::vector<std::string> arguments) { arguments_ = std::move(arguments); }
	void set_command_line(std::string line) { command_line_ = std::move(line); }
	void set_locale(platform::Locale locale) { locale_ = std::move(locale); }
	// The code every child spawned from here on exits with, at its first poll.
	void set_child_exit_code(uint32_t code) { child_exit_code_ = code; }

	const std::vector<proc::SpawnOptions> &spawned() const { return spawned_; }
	const std::vector<std::string> &opened_urls() const { return opened_urls_; }

private:
	std::vector<std::string> arguments_;
	std::string command_line_;
	platform::Locale locale_{ "en", "US" };
	uint32_t child_exit_code_ = 0;
	int64_t next_pid_ = 1000;
	std::vector<proc::SpawnOptions> spawned_;
	std::vector<std::string> opened_urls_;
};

class NullAudioDevice final : public platform::AudioDevice {
public:
	explicit NullAudioDevice(const NullClock &clock) : clock_(clock) {}
	bool open(const platform::AudioSpec &spec, std::string &error) override;
	void close() override;
	bool is_open() const override { return open_; }
	bool push(const void *frames, uint32_t frame_count) override;
	uint32_t queued_frames() const override;
	void set_gain(float gain) override { gain_ = gain; }
	float take_peak() override;

	const platform::AudioSpec &spec() const { return spec_; }
	float gain() const { return gain_; }
	uint64_t pushed_frames() const { return pushed_; }

private:
	// The frames the device played since the last account, at its rate on the clock.
	void play() const;

	const NullClock &clock_;
	bool open_ = false;
	platform::AudioSpec spec_;
	float gain_ = 1.0f;
	float peak_ = 0.0f;
	uint64_t pushed_ = 0;
	mutable uint64_t queued_ = 0;
	mutable uint64_t played_us_ = 0; // the clock's reading the queue was last played to
	mutable uint64_t carry_ = 0; // played frame-microseconds not yet a whole frame
};

// The backend: every device, and the scripted samples the pump delivers.
class NullPlatform {
public:
	// The clock starts at `start_us` (one second: the tick count reads 1000).
	explicit NullPlatform(uint64_t start_us = 1000000);
	NullPlatform(const NullPlatform &) = delete;
	NullPlatform &operator=(const NullPlatform &) = delete;

	// The record the app hands the engine.
	platform::Platform record();

	// An input event the device reports at the first pump at or after `at_us` on the clock
	// (behind every event scripted earlier for the same time or before it). A focus event also
	// moves the window's focus.
	void script(uint64_t at_us, const platform::InputEvent &event);
	// The user's close request at the first pump at or after `at_us`.
	void script_close_request(uint64_t at_us);

	NullClock &clock() { return clock_; }
	NullWindow &window() { return window_; }
	NullInputDevice &input() { return input_; }
	NullFiles &files() { return files_; }
	NullOs &os() { return os_; }
	NullAudioDevice &audio() { return audio_; }

private:
	friend class NullClock;
	// The pump: every scripted sample due at the clock's reading, in script order.
	void pump();

	struct Scripted {
		uint64_t at_us = 0;
		bool close_request = false;
		platform::InputEvent event;
	};

	NullClock clock_;
	NullWindow window_;
	NullInputDevice input_;
	NullFiles files_;
	NullOs os_;
	NullAudioDevice audio_;
	std::vector<Scripted> script_; // ordered by at_us, then by the script call
	size_t next_ = 0;
};

} // namespace opennova::null_backend
