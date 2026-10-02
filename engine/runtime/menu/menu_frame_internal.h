#pragma once

// Private storage shared by the menu-frame compiler translation units.
// [orig: CUIElement_Draw @ 0x64a8a0; CStaticWnd_Render @ 0x657b10]

#include <runtime/menu/menu_frame.h>

#include <base/io/strutil.h>

namespace opennova::menu {

inline mnu::RectEdges offset_rect(const mnu::RectEdges &rect, int dx, int dy) {
	return { rect.left + dx, rect.top + dy, rect.right + dx, rect.bottom + dy };
}

// The per-node shown gate the draw walk, the hit walk and the ancestor query share: the
// authored hidden flag, overridden by a widget state's hide and then its show.
inline bool node_shown(const mnu::Window &w, const MenuWidgetState *ws) {
	bool shown = !w.hidden;
	if (ws != nullptr) {
		if (ws->hide) {
			shown = false;
		}
		if (ws->show) {
			shown = true;
		}
	}
	return shown;
}

// The state slot an APPEARANCE STATE attribute parses to, -1 for a token the parse does
// not know [orig: CUIElement_ParseXMLDefinition @ 0x6483d4..0x64845e — DEFAULT=0,
// DISABLED=1, MOUSEOVER=2, SELECTED=3; an unknown STATE fails the row @ 0x648511, so it
// sets nothing].
inline int appearance_state_slot(const std::string &state) {
	if (strutil::iequals(state, "default")) {
		return kStateDefault;
	}
	if (strutil::iequals(state, "disabled")) {
		return kStateDisabled;
	}
	if (strutil::iequals(state, "mouseover")) {
		return kStateMouseover;
	}
	if (strutil::iequals(state, "selected")) {
		return kStateSelected;
	}
	return -1;
}

// Per-widget resolved build info. Rects stay in the 800x600 design space
// (ints); scaling + the per-element int truncation happen at emit
// [orig: CUIElement_DrawStretchedTexture @ 0x647d40].
struct MenuFrameCompiler::WidgetNode {
	const mnu::Window *window = nullptr;
	int parent = -1;
	// The root window's node (a root's own index): the own-or-root walks end
	// there [orig: CWnd_GetInheritedTextRsrc @ 0x646AB0 and
	// CWnd_GetInheritedCursorTexture @ 0x646AD0 return the widget's own value,
	// else the TOPMOST ancestor's: the loop re-tests the widget's own field].
	int root = -1;
	// A SPINUP / SPINDOWN arrow: a whole button window the spin list parses
	// before attaching it [orig: CUISpinList_ParseXMLDefinition @ 0x64bd10 ->
	// CButtonWnd_ParseTooltipXML @ 0x658170; CSpinListWnd_CreateUpDownChildren
	// @ 0x64b8b0 attaches it]. Parts sit past the document's index space.
	bool part = false;
	int part_kind = 0;    // 1 SPINUP, 2 SPINDOWN
	int spin_up = -1;     // a spin list's arrow nodes
	int spin_down = -1;
	// The per-visual-state appearance records [orig: the 28-byte state
	// records at elem+8; flags bit0 COLOR / bit1 IMAGE / bit2 CUSTOM / bit3
	// OUTLINE].
	StatePass states[4];
	// Every IMAGE row of the window's APPEARANCE as parsed, for the rect's
	// fallback extents [orig: @ 0x6485cd..0x648634].
	struct ImageRow {
		int32_t texture = kMenuTexNone;
		int32_t height = -1; // HEIGHT, -1 when absent
	};
	std::vector<ImageRow> image_rows;
	// The ITEMS per-state appearance records (list-row highlight)
	// [orig: CListWnd_DrawItems @ 0x643f30 — the row style records at +828].
	StatePass items_states[4];
	// The drawing FONT slot and its 8 colors, from the nearest self-or-ancestor
	// whose FONT loaded [orig: CWnd_GetFontAndColors @ 0x646a70 — font and
	// colors from the same widget; none up the chain: slot 0, nothing measured
	// or drawn]. `measure_font` is the one the parse measured the text size
	// with: the same, except a part parsed before it is attached reaches only
	// its own FONT.
	int32_t font = 0;
	int32_t measure_font = 0;
	uint32_t colors[4] = { 0, 0, 0, 0 };
	// The string table the widget's ids read: its own TEXT_RSRC, else its root
	// window's; a part parsed before it is attached reads only its own. Null:
	// neither authors one (ids show raw).
	const std::string *text_table = nullptr;
	// The effective FRAME block [orig: CWnd_FindInheritedFrameBlock
	// @ 0x647190 — nearest ancestor carrying a frame; the draw gate stays the
	// widget's own DRAW_FRAME].
	int frame_owner = -1;
	int32_t frame_stencil = kMenuTexNone;
	int32_t frame_brush = kMenuTexNone;
	// The widget's own CURSOR texture (inherited_cursor_ picks own-or-root
	// by what loaded).
	int32_t cursor = kMenuTexNone;
	// Item rows [orig: CUISpinList_ParseXMLDefinition @ 0x64bd10].
	struct ItemVisual {
		enum Kind { kText,
			kImage,
			kColor } kind = kText;
		std::string text;
		int32_t texture = kMenuTexNone;
		uint32_t color = 0;
	};
	std::vector<ItemVisual> items;
	std::vector<ItemVisual> popup_items; // combo LIST_BOX rows when authored
	StatePass popup_states[4]; // LIST_BOX background appearances
	StatePass popup_items_states[4]; // LIST_BOX ITEMS row appearances
	// Authored sprite scrollbars. `scrollbar` is the standalone type=scroll
	// visual, `embedded_scrollbar` is the direct SCROLLBAR used by list/table/
	// multiline widgets, and `popup_scrollbar` belongs to a combo LIST_BOX.
	struct ScrollbarVisual {
		bool present = false;
		bool has_position = false;
		mnu::Position position;
		bool vertical = true;
		int part_extent = 20;
		int edge_pad = 0;
		StatePass track[4];
		StatePass shuttle[4];
		StatePass up[4];
		StatePass down[4];
	};
	ScrollbarVisual scrollbar;
	ScrollbarVisual embedded_scrollbar;
	ScrollbarVisual popup_scrollbar;
	// TABLE: the column set-up and SPACING (menu_frame_table.cpp
	// build_table_columns_).
	std::vector<TableColumnSetup> table_columns;
	int table_spacing = 0;
};

} // namespace opennova::menu
