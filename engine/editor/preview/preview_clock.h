#pragma once

#include <cmath>
#include <cstdint>

#include <base/io/tick_rate.h>

namespace opennova::editor {

// The viewports' one clock (ADR 0046 S13 V5; CONTEXT.md "Preview clock"): what plays in any
// viewport reads it, a model's part animations, flipbooks and colour generators by its
// milliseconds and a clip by its game ticks (io::kTickHz), and later a particle effect, a menu's
// animations and an environment's time of day. It runs while it plays, `rate` times as fast as the
// Shell's frames pass (Viewports::advance); a SetViewport's `clock` plays, pauses, sets the rate or
// seeks it, and a viewport's follow seeks it (a clip newly chosen starts at tick 0, a clip event
// selected holds the clock on the tick the clip first samples it). Its milliseconds wrap as the
// game's millisecond clock does; its ticks run from 0 and hold at their largest.
class PreviewClock {
public:
	bool playing() const { return playing_; }
	double rate() const { return rate_; }
	uint32_t ms() const { return ms_; }
	int32_t ticks() const { return ticks_; }

	// `seconds` of the Shell's frames pass: while it plays, `seconds * rate` of the clock.
	void advance(double seconds) {
		const double run = seconds * rate_;
		if (!playing_ || !(run > 0.0)) return;
		ms_carry_ += run * 1000.0;
		const double whole = std::floor(ms_carry_);
		ms_carry_ -= whole;
		// It wraps as the game's millisecond clock does.
		ms_ += static_cast<uint32_t>(std::fmod(whole, 4294967296.0));
		tick_carry_ += run * io::kTickHz;
		const double ticks = std::floor(tick_carry_);
		tick_carry_ -= ticks;
		ticks_ = ticks < double(INT32_MAX - ticks_) ? ticks_ + int32_t(ticks) : INT32_MAX;
	}
	void set_playing(bool playing) { playing_ = playing; }
	// A rate of 0 or more (0 holds it where it is while it plays).
	void set_rate(double rate) { rate_ = rate > 0.0 ? rate : 0.0; }
	void seek_ms(uint32_t ms) {
		ms_ = ms;
		ms_carry_ = 0.0;
	}
	// A tick of 0 or more (a negative one is 0).
	void seek_ticks(int32_t ticks) {
		ticks_ = ticks > 0 ? ticks : 0;
		tick_carry_ = 0.0;
	}

private:
	bool playing_ = true;
	double rate_ = 1.0;
	uint32_t ms_ = 0;
	double ms_carry_ = 0.0; // the fraction of a millisecond not taken yet
	int32_t ticks_ = 0;
	double tick_carry_ = 0.0;
};

} // namespace opennova::editor
