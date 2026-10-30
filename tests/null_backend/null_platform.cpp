#include "null_backend/null_platform.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace opennova::null_backend {

// --- the window -----------------------------------------------------------------------------

bool NullWindow::create(const platform::WindowDesc &desc, std::string &error) {
	if (created_) {
		error = "the null window is up already";
		return false;
	}
	if (desc.width <= 0 || desc.height <= 0) {
		error = "the null window needs a size";
		return false;
	}
	created_ = true;
	title_ = desc.title;
	windowed_ = { desc.width, desc.height };
	mode_ = desc.mode;
	focused_ = true; // a new window takes the focus
	return true;
}

void NullWindow::set_size(int32_t width, int32_t height) {
	if (width > 0 && height > 0)
		windowed_ = { width, height };
}

platform::WindowSize NullWindow::size() const {
	if (!created_)
		return {};
	return mode_ == platform::WindowMode::Fullscreen
			? platform::WindowSize{ kDisplayWidth, kDisplayHeight }
			: windowed_;
}

bool NullWindow::take_close_request() {
	const bool requested = close_requested_;
	close_requested_ = false;
	return requested;
}

// --- the input device -----------------------------------------------------------------------

bool NullInputDevice::poll_event(platform::InputEvent &out) {
	if (queue_.empty())
		return false;
	out = queue_.front();
	queue_.pop_front();
	return true;
}

void NullInputDevice::set_cursor_mode(platform::CursorMode mode) {
	mode_ = mode;
	state_.cursor_captured = mode == platform::CursorMode::Captured;
}

void NullInputDevice::warp_cursor(int32_t x, int32_t y) {
	state_.cursor_x = x;
	state_.cursor_y = y;
}

bool NullInputDevice::set_lock_key(platform::LockKey key, bool on) {
	bool &word = key == platform::LockKey::CapsLock ? state_.caps_lock
			: key == platform::LockKey::NumLock     ? state_.num_lock
													: state_.scroll_lock;
	if (word == on) return false;
	word = on;
	lock_sets_.push_back({ key, on });
	return true;
}

void NullInputDevice::set_lock_state(bool caps_lock, bool num_lock, bool scroll_lock) {
	state_.caps_lock = caps_lock;
	state_.num_lock = num_lock;
	state_.scroll_lock = scroll_lock;
}

void NullInputDevice::deliver(const platform::InputEvent &event) {
	queue_.push_back(event);
	switch (event.type) {
		case platform::InputEvent::Type::MouseButton: {
			const uint8_t bit =
					static_cast<uint8_t>(1u << static_cast<unsigned>(event.button.button));
			state_.buttons = event.button.down ? static_cast<uint8_t>(state_.buttons | bit)
											   : static_cast<uint8_t>(state_.buttons & ~bit);
			break;
		}
		case platform::InputEvent::Type::MouseMotion:
			// A captured cursor is held; a free one travels with the mouse.
			if (!state_.cursor_captured) {
				state_.cursor_x += static_cast<int32_t>(std::lround(event.motion.dx));
				state_.cursor_y += static_cast<int32_t>(std::lround(event.motion.dy));
			}
			break;
		case platform::InputEvent::Type::Key:
			if (event.key.down && !event.key.repeat) {
				if (event.key.key == platform::KeyCode::CapsLock)
					state_.caps_lock = !state_.caps_lock;
				if (event.key.key == platform::KeyCode::NumLock)
					state_.num_lock = !state_.num_lock;
				if (event.key.key == platform::KeyCode::ScrollLock)
					state_.scroll_lock = !state_.scroll_lock;
			}
			break;
		default:
			break;
	}
}

// --- the clock ------------------------------------------------------------------------------

void NullClock::sleep_ms(uint32_t ms) {
	now_us_ += static_cast<uint64_t>(ms) * 1000u;
	slept_ms_ += ms;
}

void NullClock::pump_events() {
	++pumps_;
	platform_.pump();
}

// --- the files ------------------------------------------------------------------------------

namespace {

// One file's bytes as a stream. A write stream starts empty and puts its bytes in the tree when
// it closes, as a closed file's are on disk.
class MemoryFileStream final : public platform::FileStream {
public:
	MemoryFileStream(std::vector<uint8_t> bytes, std::vector<uint8_t> *commit) :
			bytes_(std::move(bytes)), commit_(commit) {}
	~MemoryFileStream() override {
		if (commit_ != nullptr)
			*commit_ = std::move(bytes_);
	}

	size_t read(void *data, size_t size) override {
		const size_t left = pos_ < bytes_.size() ? bytes_.size() - pos_ : 0;
		const size_t n = std::min(size, left);
		if (n > 0)
			std::memcpy(data, bytes_.data() + pos_, n);
		pos_ += n;
		return n;
	}
	size_t write(const void *data, size_t size) override {
		if (commit_ == nullptr)
			return 0; // a read stream
		if (bytes_.size() < pos_ + size)
			bytes_.resize(pos_ + size);
		if (size > 0)
			std::memcpy(bytes_.data() + pos_, data, size);
		pos_ += size;
		return size;
	}
	bool seek(int64_t offset, platform::SeekFrom from) override {
		const int64_t base = from == platform::SeekFrom::Begin ? 0
				: from == platform::SeekFrom::Current		   ? static_cast<int64_t>(pos_)
													  : static_cast<int64_t>(bytes_.size());
		if (base + offset < 0)
			return false;
		pos_ = static_cast<size_t>(base + offset);
		return true;
	}
	int64_t tell() override { return static_cast<int64_t>(pos_); }
	int64_t size() override { return static_cast<int64_t>(bytes_.size()); }

private:
	std::vector<uint8_t> bytes_;
	std::vector<uint8_t> *commit_;
	size_t pos_ = 0;
};

} // namespace

std::unique_ptr<platform::FileStream> NullFiles::open(
		const std::string &path, platform::FileMode mode, std::string &error) {
	if (mode == platform::FileMode::Write)
		return std::make_unique<MemoryFileStream>(std::vector<uint8_t>(), &files_[path]);
	const auto found = files_.find(path);
	if (found == files_.end()) {
		error = "cannot open " + path;
		return nullptr;
	}
	return std::make_unique<MemoryFileStream>(found->second, nullptr);
}

bool NullFiles::read_file(const std::string &path, std::vector<uint8_t> &out, std::string &error) {
	out.clear();
	const auto found = files_.find(path);
	if (found == files_.end()) {
		error = "cannot open " + path;
		return false;
	}
	out = found->second;
	return true;
}

bool NullFiles::write_file(
		const std::string &path, const void *data, size_t size, std::string &error) {
	(void)error;
	const auto *bytes = static_cast<const uint8_t *>(data);
	files_[path].assign(bytes, bytes + size); // whole, in one step: the atomic write
	return true;
}

void NullFiles::put(const std::string &path, std::vector<uint8_t> bytes) {
	files_[path] = std::move(bytes);
}

void NullFiles::put(const std::string &path, const std::string &text) {
	files_[path].assign(text.begin(), text.end());
}

// --- the OS ---------------------------------------------------------------------------------

namespace {

// A child that exits with its code at its first poll; a kill ends it with TerminateProcess's 1.
class NullProcess final : public platform::Process {
public:
	NullProcess(int64_t pid, uint32_t exit_code) : pid_(pid), exit_code_(exit_code) {}
	int64_t pid() const override { return pid_; }
	std::optional<uint32_t> poll() override {
		if (pid_ != 0) {
			pid_ = 0; // reaped
			reaped_ = exit_code_;
		}
		return reaped_;
	}
	void kill() override {
		if (pid_ != 0)
			exit_code_ = 1;
	}
	bool release() override {
		pid_ = 0;
		return true;
	}

private:
	int64_t pid_;
	uint32_t exit_code_;
	std::optional<uint32_t> reaped_;
};

} // namespace

std::unique_ptr<platform::Process> NullOs::spawn(
		const proc::SpawnOptions &options, std::string &error) {
	if (options.argv.empty() || options.argv[0].empty()) {
		error = "no program to start";
		return nullptr;
	}
	spawned_.push_back(options);
	return std::make_unique<NullProcess>(next_pid_++, child_exit_code_);
}

bool NullOs::open_url(const std::string &url) {
	if (url.empty())
		return false;
	opened_urls_.push_back(url);
	return true;
}

// --- the audio sink -------------------------------------------------------------------------

bool NullAudioDevice::open(const platform::AudioSpec &spec, std::string &error) {
	if (spec.rate == 0 || spec.channels == 0) {
		error = "no such PCM format";
		return false;
	}
	close();
	open_ = true;
	spec_ = spec;
	played_us_ = clock_.now_us();
	return true;
}

void NullAudioDevice::close() {
	open_ = false;
	queued_ = 0;
	carry_ = 0;
	peak_ = 0.0f;
}

void NullAudioDevice::play() const {
	if (!open_)
		return;
	const uint64_t now = clock_.now_us();
	const uint64_t elapsed = now - played_us_;
	played_us_ = now;
	// The device plays `rate` frames a second, silence once the queue runs dry.
	const uint64_t frame_us = elapsed * spec_.rate + carry_;
	const uint64_t frames = frame_us / 1000000u;
	carry_ = frame_us % 1000000u;
	queued_ = frames >= queued_ ? 0 : queued_ - frames;
	if (queued_ == 0)
		carry_ = 0;
}

bool NullAudioDevice::push(const void *frames, uint32_t frame_count) {
	if (!open_ || (frames == nullptr && frame_count != 0))
		return false;
	play();
	const size_t samples = static_cast<size_t>(frame_count) * spec_.channels;
	if (spec_.format == platform::SampleFormat::S16) {
		const auto *s = static_cast<const int16_t *>(frames);
		for (size_t i = 0; i < samples; ++i)
			peak_ = std::max(peak_, std::fabs(static_cast<float>(s[i]) / 32768.0f));
	} else {
		const auto *s = static_cast<const float *>(frames);
		for (size_t i = 0; i < samples; ++i)
			peak_ = std::max(peak_, std::fabs(s[i]));
	}
	queued_ += frame_count;
	pushed_ += frame_count;
	return true;
}

uint32_t NullAudioDevice::queued_frames() const {
	play();
	return static_cast<uint32_t>(std::min<uint64_t>(queued_, UINT32_MAX));
}

float NullAudioDevice::take_peak() {
	const float peak = peak_;
	peak_ = 0.0f;
	return peak;
}

// --- the backend ----------------------------------------------------------------------------

NullPlatform::NullPlatform(uint64_t start_us) : clock_(*this, start_us), audio_(clock_) {}

platform::Platform NullPlatform::record() {
	platform::Platform out;
	out.clock = &clock_;
	out.files = &files_;
	out.os = &os_;
	out.window = &window_;
	out.input = &input_;
	out.audio = &audio_;
	return out;
}

void NullPlatform::script(uint64_t at_us, const platform::InputEvent &event) {
	Scripted entry;
	entry.at_us = at_us;
	entry.event = event;
	// Behind every entry due at or before it, so a time's entries keep their script order.
	const auto at = std::upper_bound(script_.begin() + static_cast<std::ptrdiff_t>(next_),
			script_.end(), at_us, [](uint64_t t, const Scripted &s) { return t < s.at_us; });
	script_.insert(at, entry);
}

void NullPlatform::script_close_request(uint64_t at_us) {
	Scripted entry;
	entry.at_us = at_us;
	entry.close_request = true;
	const auto at = std::upper_bound(script_.begin() + static_cast<std::ptrdiff_t>(next_),
			script_.end(), at_us, [](uint64_t t, const Scripted &s) { return t < s.at_us; });
	script_.insert(at, entry);
}

void NullPlatform::pump() {
	const uint64_t now = clock_.now_us();
	for (; next_ < script_.size() && script_[next_].at_us <= now; ++next_) {
		const Scripted &entry = script_[next_];
		if (entry.close_request) {
			window_.close_requested_ = true;
			continue;
		}
		if (entry.event.type == platform::InputEvent::Type::Focus)
			window_.focused_ = entry.event.focus.gained;
		input_.deliver(entry.event);
	}
}

} // namespace opennova::null_backend
