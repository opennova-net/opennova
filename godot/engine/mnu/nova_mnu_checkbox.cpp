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
	connect("mouse_exited", callable_mp(this, &NovaMnuCheckBox::on_mouse_exited));

	callable_mp(this, &NovaMnuCheckBox::update_underline_position).call_deferred();
	update_appearance();
}

void NovaMnuCheckBox::on_toggled(bool p_pressed) {
	play_state_sound(MNU_SOUND_SELECTED);
	update_appearance();
}

void NovaMnuCheckBox::on_mouse_entered() {
	play_state_sound(MNU_SOUND_MOUSEIN);
}

void NovaMnuCheckBox::on_mouse_exited() {
	play_state_sound(MNU_SOUND_MOUSEOUT);
}

void NovaMnuCheckBox::play_state_sound(int p_state) {
	if (menu_ == nullptr || !sounds_.has(p_state)) {
		return;
	}
	const MnuSoundSlot &slot = sounds_.slots[p_state];
	menu_->play_widget_sound(slot.trigger, slot.file);
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
