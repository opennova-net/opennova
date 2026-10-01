#pragma once

// Private storage shared by the menu-frame compiler translation units.
// [orig: CUIElement_Draw @ 0x64a8a0; CStaticWnd_Render @ 0x657b10]

#include <runtime/menu/menu_frame.h>

namespace opennova::menu {

inline mnu::RectEdges offset_rect(const mnu::RectEdges &rect, int dx, int dy) {
	return { rect.left + dx, rect.top + dy, rect.right + dx, rect.bottom + dy };
}

// Per-widget resolved build info. Rects stay in the 800x600 design space
// (ints); scaling + the per-element int truncation happen at emit
// [orig: CUIElement_DrawStretchedTexture @ 0x647d40].
struct MenuFrameCompiler::WidgetNode {
	const mnu::Window *window = nullptr;
	int parent = -1;
	// The per-visual-state appearance records [orig: the 28-byte state
	// records at elem+8; flags bit0 COLOR / bit1 IMAGE / bit3 OUTLINE].
	StatePass states[4];
	// The ITEMS per-state appearance records (list-row highlight)
	// [orig: CListWnd_DrawItems @ 0x643f30 — the row style records at +828].
	StatePass items_states[4];
	// The effective FONT + per-state fg colors, resolved like the draw-time
	// walk [orig: CWnd_GetFontAndColors @ 0x646a70 — self-then-parent to the
	// first widget with a font; colors in parse order, only fg is drawn].
	int32_t font = 0;
	uint32_t colors[4] = { 0, 0, 0, 0 };
	// The effective FRAME block [orig: CWnd_FindInheritedFrameBlock
	// @ 0x647190 — nearest ancestor carrying a frame; the draw gate stays the
	// widget's own DRAW_FRAME].
	int frame_owner = -1;
	int32_t frame_stencil = kMenuTexNone;
	int32_t frame_brush = kMenuTexNone;
	// The inherited CURSOR texture [orig: CWnd_ProcessMouseEvent
	// @ 0x647a00 walks parents for +276 into g_UIFrameCursorTexture].
	int32_t cursor = kMenuTexNone;
	// Spin arrows: default-state art (their independent hover states are
	// child-widget state the compiled path defers — D-MNU-13).
	StatePass spin_up;
	StatePass spin_down;
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
