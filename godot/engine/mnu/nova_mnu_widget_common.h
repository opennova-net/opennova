#pragma once

#include <godot_cpp/variant/string.hpp>

namespace godot {

// Plain data carried on interactive menu widgets, collapsed from the reference's
// MnuAction / MnuSound Resources (mnu_action.gd / mnu_sound.gd) into POD the
// builder fills in and the widget acts on at runtime. Keeping these as members
// (rather than bound Resources) matches this repo's "behavior in C++ subclasses"
// decision and avoids per-widget Resource churn.
struct MnuActionData {
	// Lowercased action verb as it appears in the MNU markup:
	// "screen" (navigate, optionally cross-file), "window" (show/hide/toggle a
	// named window), "pop_screen"/"pop" (back), "quit"/"quit_game".
	String type;
	String target; // screen or window name
	String file; // external .mnu file (type=="screen" cross-menu jump)
	String window_state; // "show", "hide", "toggle" (type=="window")
};

} // namespace godot
