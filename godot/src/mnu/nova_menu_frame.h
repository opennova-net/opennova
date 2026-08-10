#pragma once

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <fnt/fnt.h>
#include <menu/menu_frame.h>

#include <memory>
#include <vector>

namespace godot {

class MnuDocument;
class MnsStyleSheet;
class ResourceRoot;

// The compiled-menu device leg (ADR 0033 R2) over the engine's
// MenuFrameCompiler (engine/runtime/menu): the engine owns the witnessed .mnu
// screen draw walk — widget order, state-driven appearance/color selection,
// text layout, the edit caret, frames (witness record: docs/mnu/menu-re.md
// "Widget render dispatch"); this Control keeps only what a device leg may
// keep: VFS texture/.fnt upload, the typed per-widget state marshalling, and
// rasterizing the compiled MenuDrawList with CanvasItem draw calls. The
// interactive game menu stays on the MnuMenu Control tree; this class is the
// compiled render surface (preview/cutover seam).
class MenuFrame : public Control {
	GDCLASS(MenuFrame, Control)

public:
	MenuFrame();
	~MenuFrame();

	// Build the compiler against a parsed document's screen (empty name = the
	// first screen), loading widget art and every referenced .fnt through the
	// mounted VFS root. `text_lookup` maps string-table ids to display text
	// (String/Item type=="id"); `style` supplies the %VAR% stylesheet. The
	// document Ref is retained; re-configure after document edits.
	bool configure(const Ref<MnuDocument> &p_document,
			const String &p_screen_name, const Ref<ResourceRoot> &p_root,
			const Ref<MnsStyleSheet> &p_style, const Dictionary &p_text_lookup);
	bool is_configured() const;

	// Typed per-widget per-frame state, keyed by the widget's pre-order index
	// in the screen tree (0 = the root window; children in authored order).
	// State persists until cleared; each setter queues a redraw.
	void clear_widget_states();
	void set_widget_shown_override(int p_index, bool p_shown);
	void clear_widget_shown_override(int p_index);
	void set_widget_disabled(int p_index, bool p_disabled);
	void set_widget_hovered(int p_index, bool p_hovered);
	void set_widget_pressed(int p_index, bool p_pressed);
	void set_widget_checked(int p_index, bool p_checked);
	void set_widget_focused(int p_index, bool p_focused);
	void set_widget_caret(int p_index, int p_caret);
	void set_widget_text(int p_index, const String &p_text);
	void clear_widget_text(int p_index);
	void set_widget_selection(int p_index, int p_selected_item,
			int p_hover_item, int p_scroll_row);
	void set_widget_popup_open(int p_index, bool p_open);

	// The blink clock and the cursor pass [orig: the cursor draws last at the
	// raw mouse position, unscaled — CUIScene_DrawScreensAndCursor @ 0x63bf60].
	void set_time_ms(int64_t p_ms);
	void set_cursor_state(bool p_visible, const Vector2 &p_position);

	// Feed one raw-mouse sample through the engine pump (menu_frame.h
	// pump_mouse carries the witness): updates every row's hover/press, the
	// cursor position, and returns the claimed widget index (-1 = none).
	// Local control coordinates; the pump scales by this control's size.
	int process_mouse(const Vector2 &p_position, bool p_button_down);

	// Debug/test accessor: compile at the current size and report counts.
	Dictionary get_draw_list_stats();

	void _draw() override;

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	struct LoadedFont {
		fnt_font_t font = {};
		bool valid = false;
		std::vector<Ref<Texture2D>> pages;
		~LoadedFont() {
			if (valid) {
				fnt_free(&font);
			}
		}
	};

	opennova::menu::MenuWidgetState &widget_(int p_index);
	void collect_font_names_(const void *p_window,
			std::vector<String> &r_names) const;
	void load_assets_();
	void free_fonts_();
	Vector2 design_scale_() const;

	Ref<MnuDocument> document_;
	Ref<ResourceRoot> root_;
	bool configured_ = false;
	opennova::menu::MenuFrameCompiler compiler_;
	opennova::menu::MenuFrameState state_;
	std::vector<Ref<Texture2D>> textures_;
	// The parsed .fnt storage the compiler borrows.
	std::vector<std::unique_ptr<LoadedFont>> owned_fonts_;
	// fonts_[i] backs the compiler's font slot i (slot 0 = the default).
	std::vector<LoadedFont *> fonts_;
};

} // namespace godot
