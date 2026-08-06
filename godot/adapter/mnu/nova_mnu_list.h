#pragma once

#include <godot_cpp/classes/item_list.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include "mnu_itemlist_common.h"
#include "nova_mnu_widget_behavior.h"

namespace godot {

class NovaMnuMenu;
class NovaMnuScroll;

// A single-select MNU list (type="list"). Subclasses ItemList. The .mnu is a
// template: the file may seed rows (a static options list) but most lists are
// populated at runtime by the engine (mission lists, server browsers) through
// set_items()/add_item(). Selection relays to the owning menu's aggregate
// widget_value_changed signal; shells that hold the list use ItemList's own
// item_selected / item_activated signals. Inert + non-focusable in edit_mode.
class NovaMnuList : public ItemList {
	GDCLASS(NovaMnuList, ItemList)

private:
	MnuWidgetBehavior behavior_;
	MnuItemListTextLayout item_text_layout_;
	NovaMnuScroll *authored_scrollbar_ = nullptr;

	void on_item_selected(int p_index);
	void on_sound_mouse_entered();
	void on_sound_mouse_exited();
	void on_item_activated(int p_index);

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void _ready() override;

	// --- Build-time configuration (called by nova_mnu_builder) ---
	void set_menu(NovaMnuMenu *p_menu) { behavior_.set_menu(p_menu); }
	void set_edit_mode(bool p_edit) { behavior_.set_edit_mode(p_edit); }
	void set_sounds(const MnuWidgetSounds &p_sounds) { behavior_.set_sounds(p_sounds); }
	void set_authored_scrollbar(NovaMnuScroll *p_scrollbar) {
		authored_scrollbar_ = p_scrollbar;
	}
	void set_min_item_height(int p_height);
	void set_item_alignment(int p_horizontal, int p_vertical);
	void set_item_text_palette(const MnuItemListTextPalette &p_palette) {
		item_text_layout_.set_palette(p_palette);
	}
	void set_runtime_enabled(bool p_enabled) {
		item_text_layout_.set_widget_disabled(!p_enabled);
		queue_redraw();
	}
	Color get_item_text_color(int p_index, bool p_hovered = false) const;
	int get_item_horizontal_alignment() const {
		return static_cast<int>(item_text_layout_.horizontal);
	}
	int get_item_vertical_alignment() const {
		return static_cast<int>(item_text_layout_.vertical);
	}

	// --- Runtime data binding (shell-facing; add_item/clear/select/get_item_text/
	// set_item_metadata are inherited from ItemList) ---
	void set_items(const PackedStringArray &items);
	int get_selected_index() const;
};

} // namespace godot
