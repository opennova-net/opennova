#include <runtime/world/tick_accumulator.h>

#include <cmath>

namespace opennova::world {

int TickAccumulator::bank(double delta, FrameLock *lock) {
	// GetTickCount is a millisecond clock; keep the sub-unit remainder so a
	// steady sub-millisecond frame stream still conserves time.
	const double units = delta * 1000.0 * kUnitsPerMs + carry_;
	const double whole = std::floor(units);
	carry_ = units - whole;
	bank_ += static_cast<int32_t>(whole); // [orig: @0x52B7B2]

	// Clamp, else smooth in place [orig: @0x52B83C..0x52B866].
	if (bank_ <= kBankClampUnits) {
		if (smoothed_ != 0)
			bank_ = (7 * smoothed_ + bank_ + 4) >> 3;
	} else {
		bank_ = kBankClampUnits;
	}
	smoothed_ = bank_;

	// The frame lock, after the history's store: 1 ms sleeps, each adding the
	// milliseconds since the last stamp, until 16 ms stand banked or 500 ran
	// [orig: @0x52B860..0x52B8DF].
	if (lock != nullptr && lock->clock != nullptr) {
		int sleeps = 0;
		while (bank_ < kFrameLockUnits) {
			if (sleeps >= kFrameLockMaxSleeps) break;
			lock->clock->sleep_one_ms();
			const uint32_t now = lock->clock->now_ms();
			bank_ += kUnitsPerMs * static_cast<int32_t>(now - lock->stamp_ms);
			lock->stamp_ms = now;
			++sleeps;
		}
		lock->sleeps = sleeps;
	}

	// The FR counter: count the frame and sum its smoothed bank; a full window
	// publishes frames * 16000 / sum (unsigned) and restarts both counters
	// [orig: @0x52B8ED, @0x52B8F4, @0x52B91A, @0x52B952..0x52B98F].
	++frame_count_;
	frame_time_sum_ += static_cast<uint32_t>(bank_);
	if (frame_time_sum_ >= kFrameRateWindowUnits) {
		// The CPU share first, over the updates the window's drains ran
		// [orig: @0x52b926..0x52b948 — `shl edi, 4` of max(count, 1)].
		const uint32_t updates = update_count_ != 0 ? update_count_ : 1u;
		cpu_percent_ = static_cast<int32_t>(update_ms_ * 100u / (updates * 16u));
		average_fps_ = static_cast<int32_t>(frame_count_ * kUnitsPerSecond / frame_time_sum_);
		frame_count_ = 0;
		frame_time_sum_ = 0;
		update_count_ = 0;
		update_ms_ = 0;
	}

	// Drain 4 ms quanta while at least 4 ms remain; the logic update runs on
	// every fourth phase [orig: @0x52BA08..0x52BA6B].
	int ticks = 0;
	while ((bank_ & ~0xF) > 0x30) {
		bank_ -= kQuantumUnits;
		if ((phase_ & 3) == 0) ++ticks;
		++phase_;
	}
	return ticks;
}

void TickAccumulator::scale_smoothing_history(float factor) {
	// [orig: Game_ScaleSmoothedFrameTime @0x52B5D0 — fild + the 2^32 bias of a
	//  negative word @0x52B5D5..0x52B5E2, fmul @0x52B5E8, fistp under RC = chop
	//  @0x52B5F5..0x52B600, the store @0x52B606]
	const double scaled = static_cast<double>(static_cast<uint32_t>(smoothed_)) *
			static_cast<double>(factor);
	smoothed_ = static_cast<int32_t>(static_cast<int64_t>(scaled));
}

} // namespace opennova::world
