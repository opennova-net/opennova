#include <runtime/world/tick_accumulator.h>

#include <cmath>

namespace opennova::world {

int TickAccumulator::bank(double delta) {
	return policy_ == TickBankPolicy::WallClock ? bank_wall_clock(delta)
	                                            : bank_retail_main_loop(delta);
}

int TickAccumulator::bank_wall_clock(double delta) {
	accum_ += delta;
	int n = static_cast<int>(accum_ / kTickDt);
	if (n <= 0) return 0;
	accum_ -= static_cast<double>(n) * kTickDt;
	if (n > kMaxCatchupTicks) {
		n = kMaxCatchupTicks;
		// Drop the backlog so a load hitch doesn't spiral into the next frames.
		accum_ = 0.0;
	}
	return n;
}

int TickAccumulator::bank_retail_main_loop(double delta) {
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
