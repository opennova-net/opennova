#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <base/io/json.h>

namespace opennova::editor {

// What one of the Shell's frames spent its time on, the measure the editor's speed is judged by (the
// perf query; ADR 0046's budgets): the frame's whole time from the start of the one before (Wall), the
// windows' layout pass, the requests the frame served, the session's poll and, inside it, the validation's
// steps, the Problems rows' composition, the running operation's steps and the game's own data's validation,
// then the viewport devices' work (their sync, their frame legs and their builds).
enum class FramePhase : uint8_t {
	Wall,
	Layout,
	Requests,
	Poll,
	Validation,
	Compose,
	Operation,
	Originals,
	Devices,
	kCount,
};

inline constexpr size_t kFramePhaseCount = static_cast<size_t>(FramePhase::kCount);

// A phase's token on the wire ("wall", "layout"...).
const char *frame_phase_token(FramePhase phase);

// The last kFrames frames' phases, in microseconds, and how many frames ran slow since the first: the
// Shell begins a frame, adds what each phase took as it runs (a phase run twice in a frame adds twice),
// and ends it; the session adds its poll's parts to the frame under way. Nothing is kept outside a frame
// (a test's or the command line's session, which has no frames, measures nothing).
class FrameTimes {
public:
	static constexpr size_t kFrames = 512;
	// A frame slower than these is counted (their totals never fall back: a client reads them twice and
	// takes the difference).
	static constexpr int64_t kSlowUs = 50000;
	static constexpr int64_t kStallUs = 100000;

	using Sample = std::array<int64_t, kFramePhaseCount>;

	void begin_frame();
	void add(FramePhase phase, int64_t us);
	// What drawing one window took inside the frame's layout (the ImGui pass's window_times), by its title.
	void add_window(const char *title, int64_t us);
	// The Shell's frame pacing after the frame: its state ("off", "active", "idle"), the cap it set, and each
	// reason it found the frame active by (a frame active for several counts under each).
	void note_pacing(const char *state, int max_fps, const std::vector<const char *> &reasons);
	void end_frame();
	bool in_frame() const { return open_; }

	// The frames the ring holds (kFrames at most), and every frame ended since the first.
	size_t frames() const { return held_; }
	uint64_t total_frames() const { return total_; }
	// The value at `fraction` (0.5 the median, 1 the most) of a phase over the frames held, nearest rank;
	// 0 with none held.
	int64_t percentile(FramePhase phase, double fraction) const;
	// The frame held whose Wall took longest (every phase of it); zeros with none held.
	Sample worst() const;

	// The summary on the wire: frames, total_frames, slow_total and stall_total (frames over 50 and
	// 100 ms since the first), slow and stall (of the frames held), phases (each phase's mean, p50, p95
	// and max over the frames held, by token), worst (the slowest frame's phases, by token), windows
	// (each window's title, draws, mean and max since the first frame, the costliest first),
	// slowest (the slowest frame since the first: its wall and its windows' times, by title) and pacing
	// (the last state and cap, and the frames each reason kept active since the first: active_by).
	io::JsonValue json() const;

private:
	std::array<Sample, kFrames> ring_{};
	size_t next_ = 0;
	size_t held_ = 0;
	Sample current_{};
	bool open_ = false;
	uint64_t total_ = 0;
	uint64_t slow_total_ = 0;
	uint64_t stall_total_ = 0;
	struct WindowCost {
		uint64_t draws = 0;
		int64_t total_us = 0;
		int64_t max_us = 0;
	};
	std::map<std::string, WindowCost> windows_;
	std::vector<std::pair<std::string, int64_t>> current_windows_; // the frame under way's
	int64_t slowest_wall_ = -1;
	std::vector<std::pair<std::string, int64_t>> slowest_windows_;
	std::string pacing_state_ = "off";
	int pacing_fps_ = 0;
	std::map<std::string, uint64_t> active_by_;
};

// Microseconds on the steady clock, the one the phases are timed by.
int64_t steady_clock_us();

} // namespace opennova::editor
