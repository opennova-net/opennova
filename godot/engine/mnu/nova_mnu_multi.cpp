#include "nova_mnu_multi.h"

#include "mnu_itemlist_common.h"

#include <godot_cpp/variant/callable_method_pointer.hpp>

using namespace godot;

void NovaMnuMulti::_ready() {
	if (behavior_.edit_mode) {
		// Inert while authoring.
		return;
	}
	connect("multi_selected", callable_mp(this, &NovaMnuMulti::on_multi_selected));
	connect("item_activated", callable_mp(this, &NovaMnuMulti::on_item_activated));
}

void NovaMnuMulti::on_multi_selected(int p_index, bool p_selected) {
	behavior_.notify_value(String(get_name()), "multi", p_index, get_item_text(p_index));
}

void NovaMnuMulti::on_item_activated(int p_index) {
	behavior_.play_click();
	behavior_.notify_value(String(get_name()), "multi", p_index, get_item_text(p_index));
}

void NovaMnuMulti::set_items(const PackedStringArray &items) {
	mnu_itemlist_set_items(this, items);
}

void NovaMnuMulti::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_items", "items"), &NovaMnuMulti::set_items);
}
