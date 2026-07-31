#pragma once

#include <godot_cpp/classes/base_button.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/string.hpp>

#include "nova_mnu_widget_behavior.h"

namespace godot {

class NovaMnuMenu;

// A single-line MNU text-entry field (type="edit"). Subclasses LineEdit and adds
// the shared MNU widget behavior (owning menu, edit_mode inert gate, sounds). The
// .mnu is a template: the field seeds its STRING text and a runtime shell drives the
// content through LineEdit's set_text()/get_text(). On change/submit it routes the
// value to the owning menu's aggregate widget_value_changed signal; shells that hold
// the field directly use LineEdit's own text_changed / text_submitted signals.
//
// In edit_mode the builder leaves it non-editable and it connects no signals, so the
// ONED preview canvas never accepts input while authoring.
class NovaMnuEdit : public LineEdit {
	GDCLASS(NovaMnuEdit, LineEdit)

private:
	MnuWidgetBehavior behavior_;
	String hotkey_;
	bool hotkey_virtual_ = false;
	bool number_ = false;
	bool has_min_ = false;
	int min_ = 0;
	bool has_max_ = false;
	int max_ = 0;
	bool normalizing_ = false;
	BaseButton *radio_button_ = nullptr;
	Label *radio_label_ = nullptr;
	bool radio_was_selected_on_press_ = false;
	bool radio_editing_ = false;
	bool runtime_enabled_ = true;
	bool has_text_state_colors_ = false;
	Color normal_text_color_;
	Color disabled_text_color_;

	void on_text_changed(const String &p_text);
	void on_sound_mouse_entered();
	void on_sound_mouse_exited();
	void on_text_submitted(const String &p_text);
	void on_focus_exited();
	void on_radio_button_down();
	void on_radio_pressed();
	void on_radio_toggled(bool p_pressed);
	void enter_radio_edit();
	void leave_radio_edit();
	String filter_numeric(const String &p_text) const;
	bool commit_numeric();
	void update_text_state_color();

protected:
	static void _bind_methods();

public:
	void _ready() override;

	// --- Build-time configuration (called by nova_mnu_builder) ---
	void set_menu(NovaMnuMenu *p_menu) { behavior_.set_menu(p_menu); }
	void set_edit_mode(bool p_edit) { behavior_.set_edit_mode(p_edit); }
	void set_sounds(const MnuWidgetSounds &p_sounds) { behavior_.set_sounds(p_sounds); }
	void set_hotkey(const String &p_hotkey, bool p_virtual) {
		hotkey_ = p_hotkey;
		hotkey_virtual_ = p_virtual;
	}
	String get_hotkey() const { return hotkey_; }
	void set_numeric_constraints(bool p_number, bool p_has_min, int p_min,
			bool p_has_max, int p_max) {
		number_ = p_number;
		has_min_ = p_has_min;
		min_ = p_min;
		has_max_ = p_has_max;
		max_ = p_max;
	}
	bool is_numeric_only() const { return number_; }
	void set_text_state_colors(const Color &p_normal, const Color &p_disabled);
	void set_radio_parts(BaseButton *p_button, Label *p_label);
	bool is_radio_editing() const { return radio_editing_; }
	void set_runtime_enabled(bool p_enabled);
	bool trigger_hotkey();
};

} // namespace godot
