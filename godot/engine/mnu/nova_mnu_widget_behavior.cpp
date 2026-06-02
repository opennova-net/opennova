#include "nova_mnu_widget_behavior.h"

#include "nova_mnu_menu.h"

using namespace godot;

void MnuWidgetBehavior::play_hover() const {
	if (menu == nullptr) {
		return;
	}
	if (hover_file.is_empty() && hover_trigger.is_empty()) {
		return;
	}
	menu->play_widget_sound(hover_trigger, hover_file);
}

void MnuWidgetBehavior::play_click() const {
	if (menu == nullptr) {
		return;
	}
	if (click_file.is_empty() && click_trigger.is_empty()) {
		return;
	}
	menu->play_widget_sound(click_trigger, click_file);
}

void MnuWidgetBehavior::notify_value(const String &p_widget_name, const String &p_kind,
		int p_index, const String &p_value) const {
	if (menu == nullptr) {
		return;
	}
	menu->notify_widget_value(p_widget_name, p_kind, p_index, p_value);
}
