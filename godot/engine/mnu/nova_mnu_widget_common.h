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

// Per-state widget sound slots.
// [orig: CUIElement_ParseXMLDefinition @ 0x648120 — a <SOUND STATE=... TRIGGER=...>
//  element stores {trigger, sound-bank id} in a per-state slot at elem+0xA0 and
//  ORs 1<<state into the mask at elem+0x9C. STATE tokens: MOUSEIN=1, MOUSEOUT=2,
//  SELECTED=3. The element TEXT names the .lwf bank file; TRIGGER names the sound
//  set inside it (SoundBank_FindTriggerByName @ 0x75be90).]
struct MnuSoundSlot {
	String trigger; // LWF sound-set name (e.g. MOUSE_OVER, CLICK_SELECT)
	String file; // .lwf bank file named by the <SOUND> element text (e.g. menu.lwf)

	bool is_empty() const { return trigger.is_empty() && file.is_empty(); }
};

// Slot indices mirror the original's sound_state values.
enum MnuSoundState {
	MNU_SOUND_MOUSEIN = 1,
	MNU_SOUND_MOUSEOUT = 2,
	MNU_SOUND_SELECTED = 3,
};

// The three state slots a widget can carry. Fired on visual-state transitions
// [orig: widget_process_mouse_event @ 0x647a00 — MOUSEIN latches while hovered;
//  MOUSEOUT and SELECTED reset to default after firing so they re-trigger].
struct MnuWidgetSounds {
	MnuSoundSlot slots[4]; // indexed by MnuSoundState; [0] unused
	int mask = 0; // 1<<state bits, set when a slot has a trigger

	void set_slot(int p_state, const String &p_trigger, const String &p_file) {
		if (p_state < 1 || p_state > 3) {
			return;
		}
		slots[p_state].trigger = p_trigger;
		slots[p_state].file = p_file;
		if (!p_trigger.is_empty()) {
			mask |= 1 << p_state;
		}
	}
	bool has(int p_state) const {
		return p_state >= 1 && p_state <= 3 && (mask & (1 << p_state)) != 0;
	}
};

} // namespace godot
