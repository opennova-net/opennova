#include "nova_mnu_widget_behavior.h"

#include "nova_mnu_menu.h"

using namespace godot;

void MnuWidgetBehavior::play_state(int p_state) const {
	if (menu == nullptr) {
		return;
	}
	if (!sounds.has(p_state)) {
		return;
	}
	const MnuSoundSlot &slot = sounds.slots[p_state];
	menu->play_widget_sound(slot.trigger, slot.file);
}

void MnuWidgetBehavior::notify_value(const String &p_widget_name, const String &p_kind,
		int p_index, const String &p_value) const {
	if (menu == nullptr) {
		return;
	}
	menu->notify_widget_value(p_widget_name, p_kind, p_index, p_value);
}
