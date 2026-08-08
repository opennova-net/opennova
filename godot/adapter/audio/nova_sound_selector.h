#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>

#include "audio/sound_selector.h"

namespace godot {

// Thin Godot binding over opennova::audio::SoundSelector: the portable sound-set member-selection
// state machine (FIRST/RANDOM/SEQUENTIAL/RANDOM_SEQ), pushed down out of nova_sound_bank.gd so the
// engine core stays C++ and a headless embedder could resolve the same member. The GDScript sound bank
// holds ONE of these per loaded bank set and asks it for a member index per (bank, set, layer) when
// it spawns a voice; the bank still owns the .lwf data access and the AudioStreamPlayer spawning.
class NovaSoundSelector : public RefCounted {
	GDCLASS(NovaSoundSelector, RefCounted)

	opennova::audio::SoundSelector selector_;

protected:
	static void _bind_methods();

public:
	// Pick a member index in [0, member_count) for the (bank, set, layer) layer in the given mode
	// (NovaSoundBank.SELECTION_*), or -1 when the layer is empty. State (sequence cursor / shuffle
	// bag) persists across calls per layer.
	int select_member(int bank, int set_index, int layer_index, int member_count, int mode);

	// Drop all per-layer selection state (e.g. when reloading banks).
	void reset();
};

} // namespace godot
