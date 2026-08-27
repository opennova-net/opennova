#include <runtime/world/tick_accumulator.h>

namespace opennova::world {

int TickAccumulator::bank(double delta) {
	accum_ += delta;
	int n = static_cast<int>(accum_ / kTickDt);
	if (n <= 0) return 0;
	accum_ -= static_cast<double>(n) * kTickDt;
	if (n > kMaxCatchupTicks) {
		n = kMaxCatchupTicks;
		// Drop the backlog so a load hitch doesn't spiral into the next
		// frames [orig: Game_MainLoop @ 0x52b630].
		accum_ = 0.0;
	}
	return n;
}

} // namespace opennova::world
