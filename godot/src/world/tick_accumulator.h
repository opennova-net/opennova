#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <runtime/world/tick_accumulator.h>

namespace godot {

// The engine's fixed 62.5 Hz frame-time bank (world::TickAccumulator carries the
// witness: Game_MainLoop's 1/16 ms bank, its 500 ms clamp, the 7/8 smoothing and
// the 4 ms drain) for a clock outside a session: the menu mode's own updates
// (the PLAYER_INFO portrait's motion steps once a drained update).
class TickAccumulator : public RefCounted {
	GDCLASS(TickAccumulator, RefCounted)

	opennova::world::TickAccumulator bank_;

protected:
	static void _bind_methods();

public:
	// Bank `p_delta` seconds of wall-clock; the logic updates due now (>= 0).
	int bank(double p_delta) { return bank_.bank(p_delta); }
	// Discard the banked time and the smoothing history (the mode's init).
	void reset() { bank_.reset(); }
};

} // namespace godot
