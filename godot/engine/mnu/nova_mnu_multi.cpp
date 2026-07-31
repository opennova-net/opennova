#include "nova_mnu_multi.h"

#include "mnu_itemlist_common.h"
#include "nova_mnu_scroll.h"

#include <godot_cpp/classes/v_scroll_bar.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>

using namespace godot;

void NovaMnuMulti::_notification(int p_what) {
	if (p_what == NOTIFICATION_DRAW) {
		item_text_layout_.draw(this);
	}
}

void NovaMnuMulti::_ready() {
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
	connect("multi_selected", callable_mp(this, &NovaMnuMulti::on_multi_selected));
	connect("item_activated", callable_mp(this, &NovaMnuMulti::on_item_activated));
	connect("mouse_entered", callable_mp(this, &NovaMnuMulti::on_sound_mouse_entered));
	connect("mouse_exited", callable_mp(this, &NovaMnuMulti::on_sound_mouse_exited));
}

void NovaMnuMulti::set_min_item_height(int p_height) {
	if (p_height > 0) {
		add_theme_constant_override("v_separation", MAX(p_height - 16, 0));
	}
}

void NovaMnuMulti::set_item_alignment(int p_horizontal, int p_vertical) {
	item_text_layout_.configure(this, p_horizontal, p_vertical);
}

Color NovaMnuMulti::get_item_text_color(int p_index, bool p_hovered) const {
	return item_text_layout_.color_for(
			const_cast<NovaMnuMulti *>(this), p_index, p_hovered);
}

void NovaMnuMulti::on_sound_mouse_entered() {
	behavior_.play_hover();
}

void NovaMnuMulti::on_sound_mouse_exited() {
	behavior_.play_mouseout();
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
	ClassDB::bind_method(D_METHOD("get_item_text_color", "index", "hovered"),
			&NovaMnuMulti::get_item_text_color, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("set_item_alignment", "horizontal", "vertical"),
			&NovaMnuMulti::set_item_alignment);
	ClassDB::bind_method(D_METHOD("get_item_horizontal_alignment"),
			&NovaMnuMulti::get_item_horizontal_alignment);
	ClassDB::bind_method(D_METHOD("get_item_vertical_alignment"),
			&NovaMnuMulti::get_item_vertical_alignment);
}
