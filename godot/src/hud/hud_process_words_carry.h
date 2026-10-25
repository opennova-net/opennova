#pragma once

// The HUD's process words across the per-mission overlay rebuild (engine runtime/hud/hud_frame.h
// HudProcessWords carries the witness): the torn-down overlay hands them over
// (HudOverlay::get_process_words) and the presenter carries them into the next mission's
// (HudOverlay::carry_process_words). Opaque to script.

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <runtime/hud/hud_frame.h>

namespace godot {

class HudProcessWordsCarry : public RefCounted {
	GDCLASS(HudProcessWordsCarry, RefCounted)

public:
	opennova::hud::HudProcessWords words;

protected:
	static void _bind_methods() {}
};

} // namespace godot
