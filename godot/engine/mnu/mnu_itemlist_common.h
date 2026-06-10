#pragma once

#include <godot_cpp/classes/item_list.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

namespace godot {

// Shared ItemList runtime-data helpers for NovaMnuList / NovaMnuMulti, which are
// ~identical apart from select mode and which selection signal they relay. Kept as
// free functions (not a GDCLASS base) because both already subclass ItemList and
// GDExtension types cannot insert a shared base between them and that parent.

// Replace all rows with `items` (the common "here is the whole list" call).
inline void mnu_itemlist_set_items(ItemList *list, const PackedStringArray &items) {
	list->clear();
	for (int i = 0; i < items.size(); ++i) {
		list->add_item(items[i]);
	}
}

// First selected row, or -1 when nothing is selected.
inline int mnu_itemlist_first_selected(ItemList *list) {
	const PackedInt32Array sel = list->get_selected_items();
	return sel.is_empty() ? -1 : sel[0];
}

} // namespace godot
