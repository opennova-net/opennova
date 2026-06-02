#pragma once

#include <godot_cpp/variant/string.hpp>

namespace godot {

class NovaMnuMenu;

// Shared, non-GDCLASS behavior carried by-value on each interactive MNU widget.
// GDExtension types cannot multiply-inherit and the widgets subclass different
// Godot Controls (LineEdit/TextEdit/ItemList/...), so the concerns common to all
// of them (the owning menu back-pointer, the edit_mode inert flag, and the hover/
// click sound triggers) live here as composition rather than a shared base.
//
// Mirrors the duplicated members of NovaMnuButton / NovaMnuCheckBox; the builder
// fills these in and the widget's _ready() consumes them (skipping all signal
// wiring when edit_mode is set, exactly like the button).
struct MnuWidgetBehavior {
	NovaMnuMenu *menu = nullptr;
	bool edit_mode = false;
	String hover_trigger;
	String hover_file;
	String click_trigger;
	String click_file;

	void set_menu(NovaMnuMenu *p_menu) { menu = p_menu; }
	void set_edit_mode(bool p_edit) { edit_mode = p_edit; }
	void set_hover_sound(const String &p_trigger, const String &p_file) {
		hover_trigger = p_trigger;
		hover_file = p_file;
	}
	void set_click_sound(const String &p_trigger, const String &p_file) {
		click_trigger = p_trigger;
		click_file = p_file;
	}

	// Route a hover/click sound through the owning menu (no-op without a menu or
	// when neither trigger nor file is set). Defined in the .cpp so nova_mnu_menu.h
	// is not pulled into every widget translation unit.
	void play_hover() const;
	void play_click() const;

	// Emit the menu's aggregate widget_value_changed signal (no-op without a menu).
	void notify_value(const String &p_widget_name, const String &p_kind, int p_index,
			const String &p_value) const;
};

} // namespace godot
