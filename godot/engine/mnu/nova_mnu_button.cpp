#include "nova_mnu_button.h"

#include "nova_mnu_menu.h"

#include <godot_cpp/classes/label_settings.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/node_path.hpp>

using namespace godot;

void NovaMnuButton::_ready() {
	if (edit_mode_) {
		// Inert while authoring: stay disabled, wire nothing.
		return;
	}
	connect("pressed", callable_mp(this, &NovaMnuButton::on_pressed));
	connect("mouse_entered", callable_mp(this, &NovaMnuButton::on_mouse_entered));
	connect("mouse_exited", callable_mp(this, &NovaMnuButton::on_mouse_exited));
	label_ = Object::cast_to<Label>(get_node_or_null(NodePath("Label")));
	set_label_color(font_color_);
}

void NovaMnuButton::on_pressed() {
	play_state_sound(MNU_SOUND_SELECTED);
	for (const MnuActionData &action : actions_) {
		execute_action(action);
	}
}

void NovaMnuButton::on_mouse_entered() {
	play_state_sound(MNU_SOUND_MOUSEIN);
	set_label_color(font_hover_color_);
}

void NovaMnuButton::on_mouse_exited() {
	// [orig: widget_process_mouse_event @ 0x647a00 — leaving a hovered widget
	//  fires the MOUSEOUT slot (momentary visual state 2)]
	play_state_sound(MNU_SOUND_MOUSEOUT);
	set_label_color(font_color_);
}

void NovaMnuButton::play_state_sound(int p_state) {
	if (menu_ == nullptr || !sounds_.has(p_state)) {
		return;
	}
	const MnuSoundSlot &slot = sounds_.slots[p_state];
	menu_->play_widget_sound(slot.trigger, slot.file);
}

void NovaMnuButton::set_label_color(const Color &p_color) {
	if (label_ == nullptr) {
		return;
	}
	Ref<LabelSettings> settings = label_->get_label_settings();
	if (settings.is_valid()) {
		settings->set_font_color(p_color);
	}
}

void NovaMnuButton::execute_action(const MnuActionData &p_action) {
	if (menu_ == nullptr) {
		return;
	}
	menu_->dispatch_widget_action(p_action);
}

void NovaMnuButton::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_action_count"), &NovaMnuButton::get_action_count);
}
