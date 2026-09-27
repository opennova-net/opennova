#include <runtime/world/tick_accumulator.h>

#include <cmath>

namespace opennova::world {

int TickAccumulator::bank(double delta) {
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

	// The FR counter: count the frame and sum its smoothed bank; a full window
	// publishes frames * 16000 / sum (unsigned) and restarts both counters
	// [orig: @0x52B8ED, @0x52B8F4, @0x52B91A, @0x52B952..0x52B98F].
	++frame_count_;
	frame_time_sum_ += static_cast<uint32_t>(bank_);
	if (frame_time_sum_ >= kFrameRateWindowUnits) {
		average_fps_ = static_cast<int32_t>(frame_count_ * kUnitsPerSecond / frame_time_sum_);
		frame_count_ = 0;
		frame_time_sum_ = 0;
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

} // namespace opennova::world
