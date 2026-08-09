#pragma once

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/label_settings.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/color.hpp>

namespace godot {

// One spinlist/combo item's resolved visual. `text` is always set (the value, the
// label, and the fallback when an image fails to resolve); `texture`/`color` carry the
// image or swatch. Mirrors the original item model parsed in
// CUISpinList_ParseXMLDefinition @ 0x64bd10 (type ID/IMAGE/COLOR) and rendered in
// CSpinListWnd_Render @ 0x64b220 (image = native-size aligned, color = full-rect swatch
// from the RRGGBB element text).
struct MnuItemVisual {
	enum Kind { TEXT, IMAGE, COLOR };
	Kind kind = TEXT;
	String text;
	// The item's `value=` attribute — the semantic value the original reads (e.g. SERVERTYPE
	// 0/1, GAME_TYPE COOP=2), distinct from the localized display `text`. The original parses
	// it with wcstoul [orig: CUISpinList_ParseXMLDefinition @ 0x64bd10]; a shell reads it to map
	// the selection to behavior. Empty for shell-supplied (set_values) text lists.
	String value;
	Ref<Texture2D> texture;
	Color color = Color(1, 1, 1, 1);
};

// Build a reusable item-cell under `mount`: three full-rect, mouse-ignoring children —
// a Label (text/id items), a native-size centered TextureRect (image items), and a
// ColorRect swatch (color items). Exactly one is shown per item via mnu_show_item_cell.
// `halign` aligns the text; `label_settings` styles it (font/colour) when valid.
void mnu_build_item_cell(Control *mount, HorizontalAlignment halign,
		const Ref<LabelSettings> &label_settings);

// Toggle the cell built by mnu_build_item_cell to show `visual`. Text items show the
// Label; image items show the native-size TextureRect (centered), the menu root's
// anamorphic scale handling screen scaling; color items show the full-rect swatch.
void mnu_show_item_cell(Control *mount, const MnuItemVisual &visual);

} // namespace godot
