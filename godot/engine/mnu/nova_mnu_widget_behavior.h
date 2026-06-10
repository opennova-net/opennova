#pragma once

#include <godot_cpp/variant/string.hpp>

#include "nova_mnu_widget_common.h"

namespace godot {

class NovaMnuMenu;

// Shared, non-GDCLASS behavior carried by-value on each interactive MNU widget.
// GDExtension types cannot multiply-inherit and the widgets subclass different
// Godot Controls (LineEdit/TextEdit/ItemList/...), so the concerns common to all
// of them (the owning menu back-pointer, the edit_mode inert flag, and the
// per-state sound slots) live here as composition rather than a shared base.
//
// Mirrors the duplicated members of NovaMnuButton / NovaMnuCheckBox; the builder
// fills these in and the widget's _ready() consumes them (skipping all signal
// wiring when edit_mode is set, exactly like the button).
struct MnuWidgetBehavior {
	NovaMnuMenu *menu = nullptr;
	bool edit_mode = false;
	MnuWidgetSounds sounds;

	void set_menu(NovaMnuMenu *p_menu) { menu = p_menu; }
	void set_edit_mode(bool p_edit) { edit_mode = p_edit; }
	void set_sounds(const MnuWidgetSounds &p_sounds) { sounds = p_sounds; }

	// Fire one state slot through the owning menu (no-op without a menu or when
	// the slot is unset). States: MNU_SOUND_MOUSEIN on pointer enter,
	// MNU_SOUND_MOUSEOUT on pointer exit, MNU_SOUND_SELECTED on activation.
	// [orig: widget_process_mouse_event @ 0x647a00 fires the slots on visual-state
	//  transitions, mask-gated at elem+0x9C]
	// Defined in the .cpp so nova_mnu_menu.h is not pulled into every widget
	// translation unit.
	void play_state(int p_state) const;

	// Event-site conveniences mapping the Godot signals onto the state slots.
	void play_hover() const { play_state(MNU_SOUND_MOUSEIN); }
	void play_mouseout() const { play_state(MNU_SOUND_MOUSEOUT); }
	void play_click() const { play_state(MNU_SOUND_SELECTED); }

	// Emit the menu's aggregate widget_value_changed signal (no-op without a menu).
	void notify_value(const String &p_widget_name, const String &p_kind, int p_index,
			const String &p_value) const;
};

} // namespace godot
