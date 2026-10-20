#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

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
	// and max over the frames held, by token) and worst (the slowest frame's phases, by token).
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
};

// Microseconds on the steady clock, the one the phases are timed by.
int64_t steady_clock_us();

} // namespace opennova::editor
