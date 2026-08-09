#include "nova_mnu_multiline_edit.h"

#include "nova_mnu_scroll.h"

#include <godot_cpp/classes/v_scroll_bar.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>

using namespace godot;

void MnuMultilineEdit::_ready() {
	if (authored_scrollbar_ != nullptr) {
		VScrollBar *native = get_v_scroll_bar();
		if (native != nullptr) {
			authored_scrollbar_->link_range_target(authored_scrollbar_->get_path_to(native));
			native->set_modulate(Color(1, 1, 1, 0));
			native->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		}
	}
	if (behavior_.edit_mode) {
		// Inert while authoring.
		return;
	}
	connect("text_changed", callable_mp(this, &MnuMultilineEdit::on_text_changed));
}

void MnuMultilineEdit::on_text_changed() {
	behavior_.notify_value(String(get_name()), "multiline", -1, get_text());
}

void MnuMultilineEdit::set_text_state_colors(
		const Color &p_normal, const Color &p_disabled) {
	normal_text_color_ = p_normal;
	disabled_text_color_ = p_disabled;
	has_text_state_colors_ = true;
	update_text_state_color();
}

void MnuMultilineEdit::set_runtime_enabled(bool p_enabled) {
	runtime_enabled_ = p_enabled;
	set_editable(runtime_enabled_ && !readonly_ && !behavior_.edit_mode);
	update_text_state_color();
}

void MnuMultilineEdit::update_text_state_color() {
	if (!has_text_state_colors_) {
		return;
	}
	add_theme_color_override("font_readonly_color",
			runtime_enabled_ ? normal_text_color_ : disabled_text_color_);
}

void MnuMultilineEdit::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_readonly", "readonly"), &MnuMultilineEdit::set_readonly);
	ClassDB::bind_method(D_METHOD("get_readonly"), &MnuMultilineEdit::get_readonly);
}
