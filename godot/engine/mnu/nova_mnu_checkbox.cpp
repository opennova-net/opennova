#include "nova_mnu_checkbox.h"

#include "nova_mnu_menu.h"

#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/vector2.hpp>

using namespace godot;

void NovaMnuCheckBox::_ready() {
	if (edit_mode_) {
		// Inert while authoring.
		return;
	}

	// Cache the per-state art the build set so we can swap the displayed texture
	// with the checked/unchecked state (matches mnu_checkbox.gd).
	tex_unchecked_ = get_texture_normal();
	tex_checked_ = get_texture_pressed();

	underline_ = memnew(ColorRect);
	underline_->set_name("Underline");
	underline_->set_color(underline_color_);
	underline_->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	underline_->set_visible(false);
	add_child(underline_);

	connect("toggled", callable_mp(this, &NovaMnuCheckBox::on_toggled));
	connect("mouse_entered", callable_mp(this, &NovaMnuCheckBox::on_mouse_entered));

	callable_mp(this, &NovaMnuCheckBox::update_underline_position).call_deferred();
	update_appearance();
}

void NovaMnuCheckBox::on_toggled(bool p_pressed) {
	if (!click_sound_file_.is_empty() || !click_sound_trigger_.is_empty()) {
		if (menu_ != nullptr) {
			menu_->play_widget_sound(click_sound_trigger_, click_sound_file_);
		}
	}
	update_appearance();
}

void NovaMnuCheckBox::on_mouse_entered() {
	if (!hover_sound_file_.is_empty() || !hover_sound_trigger_.is_empty()) {
		if (menu_ != nullptr) {
			menu_->play_widget_sound(hover_sound_trigger_, hover_sound_file_);
		}
	}
}

void NovaMnuCheckBox::update_appearance() {
	if (is_pressed()) {
		if (tex_checked_.is_valid()) {
			set_texture_normal(tex_checked_);
		}
		if (underline_ != nullptr) {
			underline_->set_visible(true);
		}
	} else {
		if (tex_unchecked_.is_valid()) {
			set_texture_normal(tex_unchecked_);
		}
		if (underline_ != nullptr) {
			underline_->set_visible(false);
		}
	}
}

void NovaMnuCheckBox::update_underline_position() {
	if (underline_ == nullptr) {
		return;
	}
	const Vector2 sz = get_size();
	underline_->set_position(Vector2(0, sz.y - underline_thickness_));
	underline_->set_size(Vector2(sz.x, underline_thickness_));
	underline_->set_visible(is_pressed());
}

void NovaMnuCheckBox::_bind_methods() {
}
