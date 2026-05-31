#pragma once

#include <godot_cpp/classes/item_list.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include "nova_mnu_widget_behavior.h"

namespace godot {

class NovaMnuMenu;

// A multi-select MNU list (type="multi"). Identical to NovaMnuList except the
// select mode is SELECT_MULTI and per-row selection relays through ItemList's
// multi_selected(index, selected) signal. get_selected_items() (inherited) returns
// the full selection. Inert + non-focusable in edit_mode.
class NovaMnuMulti : public ItemList {
	GDCLASS(NovaMnuMulti, ItemList)

private:
	MnuWidgetBehavior behavior_;

	void on_multi_selected(int p_index, bool p_selected);
	void on_item_activated(int p_index);

protected:
	static void _bind_methods();

public:
	void _ready() override;

	void set_menu(NovaMnuMenu *p_menu) { behavior_.set_menu(p_menu); }
	void set_edit_mode(bool p_edit) { behavior_.set_edit_mode(p_edit); }
	void set_hover_sound(const String &p_trigger, const String &p_file) {
		behavior_.set_hover_sound(p_trigger, p_file);
	}
	void set_click_sound(const String &p_trigger, const String &p_file) {
		behavior_.set_click_sound(p_trigger, p_file);
	}

	void set_items(const PackedStringArray &items);
};

} // namespace godot
