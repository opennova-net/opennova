#pragma once

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>

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
// text layout, the edit caret, frames, the mouse pump, the interaction
// geometry queries, and the edit-input module (witness record:
// docs/mnu/menu-re.md); this Control keeps only what a device leg may keep:
// VFS texture/.fnt upload, the typed per-widget state marshalling, and
// rasterizing the compiled MenuDrawList with CanvasItem draw calls. The game
// menu shell (godot/game/menu_driver.gd + nova_menu_shell.gd) and the ONED
// Menus canvas both drive this one surface.
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
	// Runtime content channels (the Control-tree path's set_items /
	// add_row_values / marquee content, now engine state).
	void set_widget_items(int p_index, const PackedStringArray &p_items);
	void clear_widget_items(int p_index);
	void set_widget_selected_set(int p_index, const PackedInt32Array &p_rows);
	void set_widget_table_rows(int p_index, const TypedArray<PackedStringArray> &p_rows);
	void clear_widget_table_rows(int p_index);
	void set_widget_marquee_lines(int p_index, const PackedStringArray &p_lines);
	void reset_widget_marquee(int p_index);

	// Widget queries over the configured screen (design-space rects; the
	// pre-order index space matches a document DFS of the same screen).
	int widget_count() const;
	String widget_name(int p_index) const;
	int widget_kind(int p_index) const;
	String widget_authored_text(int p_index) const;
	bool is_widget_disabled(int p_index) const;
	Rect2 widget_rect(int p_index) const;
	int item_count(int p_index) const;
	String get_widget_text(int p_index) const; // effective: runtime else authored
	int get_widget_caret(int p_index) const;

	// Interaction geometry (positions in this control's local coordinates;
	// the engine scales them like process_mouse does).
	int hit_test(const Vector2 &p_position) const;
	int list_row_at(int p_index, const Vector2 &p_position) const;
	int list_visible_rows(int p_index) const;
	Rect2 combo_popup_rect(int p_index) const;
	bool combo_popup_contains(int p_index, const Vector2 &p_position) const;
	int combo_popup_row_at(int p_index, const Vector2 &p_position) const;
	int spin_arrow_at(int p_index, const Vector2 &p_position) const; // 0/1 up/2 down
	int table_row_at(int p_index, const Vector2 &p_position) const;
	int hotkey_widget(const String &p_key, bool p_virtual) const;
	// Multiline wrapped-line counts: x = rows that fit, y = total rows —
	// scroll range = [0, y - x] (engine multiline_line_counts).
	Vector2i multiline_line_counts(int p_index) const;

	// Edit-input routing over the engine module (menu/menu_edit.h): applies
	// the witnessed insert/key ops to the widget's effective text/caret
	// state. edit_key returns 0 none / 1 changed / 2 commit.
	bool edit_char(int p_index, int p_unicode);
	int edit_key(int p_index, int p_key, bool p_shift);

	// The claim cursor of the last process_mouse (inherited widget CURSOR
	// else the screen default); null when neither resolves.
	Ref<Texture2D> get_cursor_texture() const;
	// Distinct texture/font names configure() could not resolve.
	int get_unresolved_asset_count() const;

	// The blink clock and the cursor pass [orig: the cursor draws last at the
	// raw mouse position, unscaled — CUIScene_DrawScreensAndCursor @ 0x63bf60].
	void set_time_ms(int64_t p_ms);
	void set_cursor_state(bool p_visible, const Vector2 &p_position);

	// Feed one raw-mouse sample through the engine pump (menu_frame.h
	// pump_mouse carries the witness): updates every row's hover/press, the
	// cursor position, and returns the claimed widget index (-1 = none).
	// Local control coordinates; the pump scales by this control's size.
	int process_mouse(const Vector2 &p_position, bool p_button_down);

	// The companions' name->pre-order-index seam (case-insensitive authored
	// widget NAME; -1 = absent). Valid after configure().
	int widget_index(const String &p_name) const;

	// Activation edges, emitted by process_mouse: "widget_pressed(index)" on
	// the button-down edge over a claimed widget; "widget_clicked(index)" on
	// the release edge while the SAME widget still owns the claim (the
	// standard control-activation contract the Control-tree buttons had).

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
	int mouse_claim_ = -1;      // last pump claim (activation edge tracking)
	int press_claim_ = -1;      // widget owning the current press, -1 = none
	bool mouse_button_down_ = false;
	int32_t cursor_slot_ = -1;  // last claim's cursor texture slot
	int unresolved_assets_ = 0;
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
