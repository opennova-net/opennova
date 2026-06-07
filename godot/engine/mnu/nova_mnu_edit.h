#pragma once

#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#include "nova_mnu_widget_behavior.h"

namespace godot {

class NovaMnuMenu;

// A single-line MNU text-entry field (type="edit"). Subclasses LineEdit and adds
// the shared MNU widget behavior (owning menu, edit_mode inert gate, sounds). The
// .mnu is a template: the field seeds its STRING text and a runtime host drives the
// content through LineEdit's set_text()/get_text(). On change/submit it routes the
// value to the owning menu's aggregate widget_value_changed signal; hosts that hold
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

	void on_text_changed(const String &p_text);
	void on_text_submitted(const String &p_text);

protected:
	static void _bind_methods();

public:
	void _ready() override;

	// --- Build-time configuration (called by nova_mnu_builder) ---
	void set_menu(NovaMnuMenu *p_menu) { behavior_.set_menu(p_menu); }
	void set_edit_mode(bool p_edit) { behavior_.set_edit_mode(p_edit); }
	void set_hover_sound(const String &p_trigger, const String &p_file) {
		behavior_.set_hover_sound(p_trigger, p_file);
	}
	void set_click_sound(const String &p_trigger, const String &p_file) {
		behavior_.set_click_sound(p_trigger, p_file);
	}
	void set_hotkey(const String &p_hotkey, bool p_virtual) {
		hotkey_ = p_hotkey;
		hotkey_virtual_ = p_virtual;
	}
	String get_hotkey() const { return hotkey_; }
};

} // namespace godot
