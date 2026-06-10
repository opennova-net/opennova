#pragma once

#include <godot_cpp/classes/item_list.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include "nova_mnu_widget_behavior.h"

namespace godot {

class NovaMnuMenu;

// A single-select MNU list (type="list"). Subclasses ItemList. The .mnu is a
// template: the file may seed rows (a static options list) but most lists are
// populated at runtime by the engine (mission lists, server browsers) through
// set_items()/add_item(). Selection relays to the owning menu's aggregate
// widget_value_changed signal; hosts that hold the list use ItemList's own
// item_selected / item_activated signals. Inert + non-focusable in edit_mode.
class NovaMnuList : public ItemList {
	GDCLASS(NovaMnuList, ItemList)

private:
	MnuWidgetBehavior behavior_;

	void on_item_selected(int p_index);
	void on_sound_mouse_entered();
	void on_sound_mouse_exited();
	void on_item_activated(int p_index);

protected:
	static void _bind_methods();

public:
	void _ready() override;

	// --- Build-time configuration (called by nova_mnu_builder) ---
	void set_menu(NovaMnuMenu *p_menu) { behavior_.set_menu(p_menu); }
	void set_edit_mode(bool p_edit) { behavior_.set_edit_mode(p_edit); }
	void set_sounds(const MnuWidgetSounds &p_sounds) { behavior_.set_sounds(p_sounds); }

	// --- Runtime data binding (host-facing; add_item/clear/select/get_item_text/
	// set_item_metadata are inherited from ItemList) ---
	void set_items(const PackedStringArray &items);
	int get_selected_index() const;
};

} // namespace godot
