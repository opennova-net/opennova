#include "nova_mnu_edit.h"

#include <godot_cpp/variant/callable_method_pointer.hpp>

using namespace godot;

void NovaMnuEdit::_ready() {
	if (behavior_.edit_mode) {
		// Inert while authoring: stay non-editable, wire nothing.
		return;
	}
	connect("text_changed", callable_mp(this, &NovaMnuEdit::on_text_changed));
	connect("text_submitted", callable_mp(this, &NovaMnuEdit::on_text_submitted));
	connect("focus_exited", callable_mp(this, &NovaMnuEdit::on_focus_exited));
	connect("mouse_entered", callable_mp(this, &NovaMnuEdit::on_sound_mouse_entered));
	connect("mouse_exited", callable_mp(this, &NovaMnuEdit::on_sound_mouse_exited));
	if (radio_button_ != nullptr) {
		radio_button_->connect("button_down",
				callable_mp(this, &NovaMnuEdit::on_radio_button_down));
		radio_button_->connect("pressed",
				callable_mp(this, &NovaMnuEdit::on_radio_pressed));
		radio_button_->connect("toggled",
				callable_mp(this, &NovaMnuEdit::on_radio_toggled));
	}
}
void NovaMnuEdit::on_sound_mouse_entered() {
	behavior_.play_hover();
}

void NovaMnuEdit::on_sound_mouse_exited() {
	behavior_.play_mouseout();
}

void NovaMnuEdit::on_text_changed(const String &p_text) {
	if (normalizing_) {
		return;
	}
	String value = p_text;
	if (number_) {
		value = filter_numeric(p_text);
		if (value != p_text) {
			const int caret = get_caret_column();
			normalizing_ = true;
			set_text(value);
			set_caret_column(MIN(caret, value.length()));
			normalizing_ = false;
		}
	}
	behavior_.notify_value(String(get_name()), "edit", -1, value);
}

void NovaMnuEdit::on_text_submitted(const String &p_text) {
	commit_numeric();
	behavior_.play_click();
	// Preserve LineEdit's submitted payload for ordinary fields. Numeric fields
	// publish the normalized/clamped text that commit_numeric() leaves behind.
	const String value = number_ ? get_text() : p_text;
	behavior_.notify_value(String(get_name()), "edit", -1, value);
}

void NovaMnuEdit::on_focus_exited() {
	const bool clamped = commit_numeric();
	if (clamped) {
		// set_text() is guarded while normalizing, so explicitly publish the
		// committed value; otherwise shells retain the preceding out-of-range text.
		behavior_.notify_value(String(get_name()), "edit", -1, get_text());
	}
	if (radio_editing_) {
		leave_radio_edit();
	}
}

void NovaMnuEdit::set_radio_parts(BaseButton *p_button, Label *p_label) {
	radio_button_ = p_button;
	radio_label_ = p_label;
	radio_editing_ = false;
	set_editable(false);
	if (radio_button_ != nullptr) {
		radio_button_->set_visible(true);
		radio_button_->set_disabled(!runtime_enabled_ || behavior_.edit_mode);
	}
	if (radio_label_ != nullptr) {
		radio_label_->set_text(get_text());
	}
}

void NovaMnuEdit::set_text_state_colors(
		const Color &p_normal, const Color &p_disabled) {
	normal_text_color_ = p_normal;
	disabled_text_color_ = p_disabled;
	has_text_state_colors_ = true;
	update_text_state_color();
}

void NovaMnuEdit::update_text_state_color() {
	if (!has_text_state_colors_) {
		return;
	}
	add_theme_color_override("font_uneditable_color",
			runtime_enabled_ ? normal_text_color_ : disabled_text_color_);
}

void NovaMnuEdit::set_runtime_enabled(bool p_enabled) {
	runtime_enabled_ = p_enabled;
	if (!runtime_enabled_ && radio_editing_) {
		leave_radio_edit();
	}
	if (radio_button_ != nullptr) {
		radio_button_->set_disabled(!runtime_enabled_ || behavior_.edit_mode);
		set_editable(runtime_enabled_ && radio_editing_ && !behavior_.edit_mode);
	} else {
		set_editable(runtime_enabled_ && !behavior_.edit_mode);
	}
	update_text_state_color();
}

bool NovaMnuEdit::trigger_hotkey() {
	if (!runtime_enabled_ || behavior_.edit_mode) {
		return true;
	}
	if (radio_button_ == nullptr) {
		grab_focus();
		return true;
	}
	if (radio_button_->is_pressed()) {
		enter_radio_edit();
	} else {
		radio_button_->set_pressed(true);
		radio_button_->emit_signal("pressed");
	}
	return true;
}

void NovaMnuEdit::on_radio_button_down() {
	radio_was_selected_on_press_ =
			radio_button_ != nullptr && radio_button_->is_pressed();
}

void NovaMnuEdit::on_radio_pressed() {
	if (radio_button_ == nullptr) {
		return;
	}
	// [orig: RadioEditWnd_handle_event @ 0x65d540] The first activation selects
	// the radio; activating the already-selected radio swaps its radio child for
	// the edit child and focuses the edit.
	if (radio_was_selected_on_press_) {
		enter_radio_edit();
	}
}

void NovaMnuEdit::on_radio_toggled(bool p_pressed) {
	if (!p_pressed && radio_editing_) {
		// Selecting another radio in the same group dismisses this one's editor.
		leave_radio_edit();
	}
}

void NovaMnuEdit::enter_radio_edit() {
	if (radio_button_ == nullptr || !runtime_enabled_ || behavior_.edit_mode ||
			radio_button_->is_disabled()) {
		return;
	}
	radio_editing_ = true;
	radio_button_->set_visible(false);
	set_editable(true);
	grab_focus();
	set_caret_column(get_text().length());
}

void NovaMnuEdit::leave_radio_edit() {
	commit_numeric();
	radio_editing_ = false;
	set_editable(false);
	if (radio_label_ != nullptr) {
		// [orig: RadioEditWnd_handle_event @ 0x65d540] Returning to radio mode
		// copies the edit child's current text into the radio presentation.
		radio_label_->set_text(get_text());
	}
	if (radio_button_ != nullptr) {
		radio_button_->set_pressed_no_signal(true);
		radio_button_->set_visible(true);
	}
}

String NovaMnuEdit::filter_numeric(const String &p_text) const {
	String out;
	for (int i = 0; i < p_text.length(); ++i) {
		const char32_t c = p_text[i];
		if (c >= U'0' && c <= U'9') {
			out += String::chr(c);
		} else if (c == U'-' && i == 0 && (has_min_ ? min_ < 0 : true)) {
			out += "-";
		}
	}
	return out;
}

bool NovaMnuEdit::commit_numeric() {
	if (!number_ || normalizing_) {
		return false;
	}
	const String current = filter_numeric(get_text());
	if (current.is_empty() || current == "-") {
		return false;
	}
	int64_t value = current.to_int();
	if (has_min_ && value < min_) {
		value = min_;
	}
	if (has_max_ && value > max_) {
		value = max_;
	}
	const String clamped = String::num_int64(value);
	if (clamped != get_text()) {
		normalizing_ = true;
		set_text(clamped);
		normalizing_ = false;
		return true;
	}
	return false;
}

void NovaMnuEdit::_bind_methods() {
	// text/get_text and the text_changed/text_submitted signals come from LineEdit;
	// we only expose the MNU-specific hotkey accessor.
	ClassDB::bind_method(D_METHOD("get_hotkey"), &NovaMnuEdit::get_hotkey);
	ClassDB::bind_method(D_METHOD("is_numeric_only"), &NovaMnuEdit::is_numeric_only);
	ClassDB::bind_method(D_METHOD("is_radio_editing"), &NovaMnuEdit::is_radio_editing);
}
