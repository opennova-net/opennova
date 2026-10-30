#include "world/tick_accumulator.h"

namespace godot {

void TickAccumulator::_bind_methods() {
	ClassDB::bind_method(D_METHOD("bank", "delta"), &TickAccumulator::bank);
	ClassDB::bind_method(D_METHOD("reset"), &TickAccumulator::reset);
}

} // namespace godot
