#include "nova_mnu_list.h"

#include "mnu_itemlist_common.h"

#include <godot_cpp/variant/callable_method_pointer.hpp>

using namespace godot;

void NovaMnuList::_ready() {
	if (behavior_.edit_mode) {
		// Inert while authoring.
		return;
	}
	connect("item_selected", callable_mp(this, &NovaMnuList::on_item_selected));
	connect("item_activated", callable_mp(this, &NovaMnuList::on_item_activated));
	connect("mouse_entered", callable_mp(this, &NovaMnuList::on_sound_mouse_entered));
	connect("mouse_exited", callable_mp(this, &NovaMnuList::on_sound_mouse_exited));
}
void NovaMnuList::on_sound_mouse_entered() {
	behavior_.play_hover();
}

void NovaMnuList::on_sound_mouse_exited() {
	behavior_.play_mouseout();
}

void NovaMnuList::on_item_selected(int p_index) {
	behavior_.notify_value(String(get_name()), "list", p_index, get_item_text(p_index));
}

void NovaMnuList::on_item_activated(int p_index) {
	behavior_.play_click();
	behavior_.notify_value(String(get_name()), "list", p_index, get_item_text(p_index));
}

void NovaMnuList::set_items(const PackedStringArray &items) {
	mnu_itemlist_set_items(this, items);
}

int NovaMnuList::get_selected_index() const {
	return mnu_itemlist_first_selected(const_cast<NovaMnuList *>(this));
}

void NovaMnuList::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_items", "items"), &NovaMnuList::set_items);
	ClassDB::bind_method(D_METHOD("get_selected_index"), &NovaMnuList::get_selected_index);
}
