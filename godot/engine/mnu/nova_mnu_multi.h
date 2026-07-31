#pragma once

#include <godot_cpp/classes/item_list.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include "mnu_itemlist_common.h"
#include "nova_mnu_widget_behavior.h"

namespace godot {

class NovaMnuMenu;
class NovaMnuScroll;

// A multi-select MNU list (type="multi"). Identical to NovaMnuList except the
// select mode is SELECT_MULTI and per-row selection relays through ItemList's
// multi_selected(index, selected) signal. get_selected_items() (inherited) returns
// the full selection. Inert + non-focusable in edit_mode.
class NovaMnuMulti : public ItemList {
	GDCLASS(NovaMnuMulti, ItemList)

private:
	MnuWidgetBehavior behavior_;
	MnuItemListTextLayout item_text_layout_;
	NovaMnuScroll *authored_scrollbar_ = nullptr;

	void on_multi_selected(int p_index, bool p_selected);
	void on_sound_mouse_entered();
	void on_sound_mouse_exited();
	void on_item_activated(int p_index);

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void _ready() override;

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

	void set_items(const PackedStringArray &items);
};

} // namespace godot
