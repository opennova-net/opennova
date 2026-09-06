#pragma once

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/image.hpp>
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

#include <formats/fnt/fnt.h>
#include <runtime/menu/menu_frame.h>

#include <map>
#include <memory>
#include <vector>

namespace godot {

class MenuDrawListStats;

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
// menu shell (godot/game/menu_driver.gd + menu_shell.gd) drives this one
// surface; menu authoring was removed (ADR 0037).
class MenuFrame : public Control {
	GDCLASS(MenuFrame, Control)

public:
	MenuFrame();
	~MenuFrame();

	// Re-exports of the engine's menu constants (values and witnesses live at
	// the engine homes: menu/menu_frame.h design space, menu/menu_edit.h edit
	// keys/results) so GDScript consumers carry no literals of their own.
	enum {
		// The fixed 800x600 authoring design space design_scale_() maps the
		// control size onto.
		DESIGN_WIDTH = opennova::menu::kMenuDesignWidth,
		DESIGN_HEIGHT = opennova::menu::kMenuDesignHeight,
		// The special-key codes edit_key consumes.
		EDIT_KEY_BACKSPACE = opennova::menu::kEditKeyBackspace,
		EDIT_KEY_ENTER = opennova::menu::kEditKeyEnter,
		EDIT_KEY_END = opennova::menu::kEditKeyEnd,
		EDIT_KEY_HOME = opennova::menu::kEditKeyHome,
		EDIT_KEY_LEFT = opennova::menu::kEditKeyLeft,
		EDIT_KEY_RIGHT = opennova::menu::kEditKeyRight,
		EDIT_KEY_DELETE = opennova::menu::kEditKeyDelete,
		// edit_key's return values (EditKeyResult).
		EDIT_RESULT_NONE = static_cast<int>(opennova::menu::EditKeyResult::kNone),
		EDIT_RESULT_CHANGED =
				static_cast<int>(opennova::menu::EditKeyResult::kChanged),
		EDIT_RESULT_COMMIT =
				static_cast<int>(opennova::menu::EditKeyResult::kCommit),
		// scroll_hit_at's part codes (the engine ScrollHit map).
		SCROLL_HIT_NONE = opennova::menu::MenuFrameCompiler::kScrollHitNone,
		SCROLL_HIT_UP = opennova::menu::MenuFrameCompiler::kScrollHitUp,
		SCROLL_HIT_DOWN = opennova::menu::MenuFrameCompiler::kScrollHitDown,
		SCROLL_HIT_SHUTTLE = opennova::menu::MenuFrameCompiler::kScrollHitShuttle,
		SCROLL_HIT_TRACK_BEFORE =
				opennova::menu::MenuFrameCompiler::kScrollHitTrackBefore,
		SCROLL_HIT_TRACK_AFTER =
				opennova::menu::MenuFrameCompiler::kScrollHitTrackAfter,
	};

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
	void set_widget_shown_override(int p_index, bool p_shown);
	void set_widget_disabled(int p_index, bool p_disabled);
	// The open dropdown's hovered ROW (style 2) — the driver's popup-exclusive
	// pump updates it per move (the witness map lives at the engine compiler,
	// engine/runtime/menu/menu_frame.h).
	void set_widget_hover_item(int p_index, int p_row);
	// Scrollbar-part geometry probe (the SCROLL_HIT_* codes above). The
	// interaction itself — arrows, track pages, shuttle drag — lives in the
	// engine pump and reports through the scroll_value_changed signal.
	int scroll_hit_at(int p_index, const Vector2 &p_position) const;
	int get_widget_hover_item(int p_index) const;
	void set_widget_checked(int p_index, bool p_checked);
	void set_widget_focused(int p_index, bool p_focused);
	void set_widget_caret(int p_index, int p_caret);
	void set_widget_text(int p_index, const String &p_text);
	void set_widget_selection(int p_index, int p_selected_item, int p_hover_item,
			int p_scroll_row);
	// Standalone type=scroll range/page/value. Page is the original inclusive
	// page field (visible count - 1).
	void set_widget_scroll_range(int p_index, int p_minimum, int p_maximum,
			int p_page, int p_value);
	void set_widget_popup_open(int p_index, bool p_open);
	// Runtime content channels (the Control-tree path's set_items /
	// add_row_values / marquee content, now engine state).
	void set_widget_items(int p_index, const PackedStringArray &p_items);
	void set_widget_selected_set(int p_index, const PackedInt32Array &p_rows);
	void set_widget_table_rows(int p_index, const TypedArray<PackedStringArray> &p_rows);
	void set_widget_marquee_lines(int p_index, const PackedStringArray &p_lines);

	// Widget queries over the configured screen (design-space rects; the
	// pre-order index space matches a document DFS of the same screen).
	int widget_count() const;
	String widget_name(int p_index) const;
	int widget_kind(int p_index) const;
	String widget_authored_text(int p_index) const;
	bool is_widget_disabled(int p_index) const;
	// Effective draw/hit visibility (own flag + ancestors + overrides) —
	// shell overlays mounted over a widget must follow it.
	bool is_widget_shown(int p_index) const;
	Rect2 widget_rect(int p_index) const;
	int item_count(int p_index) const;
	String get_widget_text(int p_index) const; // effective: runtime else authored
	int get_widget_caret(int p_index) const;

	// Interaction geometry (positions in this control's local coordinates;
	// the engine scales them like process_mouse does).
	int hit_test(const Vector2 &p_position) const;
	int list_row_at(int p_index, const Vector2 &p_position) const;
	int list_visible_rows(int p_index) const;
	bool combo_popup_contains(int p_index, const Vector2 &p_position) const;
	int combo_popup_row_at(int p_index, const Vector2 &p_position) const;
	int spin_arrow_at(int p_index, const Vector2 &p_position) const; // 0/1 up/2 down
	int table_row_at(int p_index, const Vector2 &p_position) const;
	int hotkey_widget(const String &p_key, bool p_virtual) const;

	// Edit-input routing over the engine module (menu/menu_edit.h): applies
	// the witnessed insert/key ops to the widget's effective text/caret
	// state. edit_key takes an EDIT_KEY_* code and returns
	// EDIT_RESULT_NONE / EDIT_RESULT_CHANGED / EDIT_RESULT_COMMIT.
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

	// The open-dropdown sample: only the popup's scrollbar interaction runs,
	// restricted to the open combo. True when the scrollbar owns the sample
	// (the caller skips row hover/pick); value changes arrive on
	// "scroll_value_changed" like the main pump's.
	bool process_popup_mouse(int p_index, const Vector2 &p_position,
			bool p_button_down);

	// One wheel tick (steps > 0 = rows scroll down): the open popup
	// exclusively, else the front-most row owner under the point. True when
	// a scrollable target claimed the tick; value changes arrive on
	// "scroll_value_changed".
	bool process_mouse_wheel(const Vector2 &p_position, int p_steps);

	// The companions' name->pre-order-index seam (case-insensitive authored
	// widget NAME; -1 = absent). Valid after configure().
	int widget_index(const String &p_name) const;

	// Activation edge, emitted by process_mouse: "widget_clicked(index)" on
	// the release edge while the widget claimed on the button-down edge still
	// owns the claim (the standard control-activation contract the
	// Control-tree buttons had).

	// Debug/test accessor: compile at the current size and report counts.
	Ref<MenuDrawListStats> get_draw_list_stats();

	void _draw() override;

protected:
public:
	// The retail Options policy tables (engine/runtime/menu/options_policy.h),
	// re-exported for the shell's appliers: [{control, minimum, maximum, page}],
	// [{control, value}], the gamma reference, the preset-button names, the
	// not-yet-serviced control names and [{control, checked}] their rows show.
	static Array options_scroll_ranges();
	static Array video_quality_controls();
	static int video_gamma_reference();
	static PackedStringArray video_preset_buttons();
	static PackedStringArray options_unsupported_controls();
	static Array options_forced_checks();

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	struct LoadedFont {
		opennova::fnt::fnt_font_t font = {};
		bool valid = false;
		std::vector<Ref<Texture2D>> pages;
		~LoadedFont() {
			if (valid) {
				opennova::fnt::fnt_free(&font);
			}
		}
	};

	opennova::menu::MenuWidgetState &widget_(int p_index);
	Ref<Texture2D> texture_for_quad_(const opennova::menu::MenuQuad &p_quad);
	void collect_font_names_(const void *p_window,
			std::vector<String> &r_names) const;
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
	// Source pixels are retained for the frame material adapter. Retail's
	// border is a fixed-function two-texture material, not either authored
	// texture by itself; derived textures are cached by slots/UV/destination.
	std::vector<Ref<Image>> texture_images_;
	std::map<std::string, Ref<Texture2D>> frame_texture_cache_;
	// The parsed .fnt storage the compiler borrows.
	std::vector<std::unique_ptr<LoadedFont>> owned_fonts_;
	// fonts_[i] backs the compiler's font slot i (slot 0 = the default).
	std::vector<LoadedFont *> fonts_;
	// The menu-top overlay: a child canvas item one z above this Control, so
	// the compiled draw list's popup + cursor ops (MenuDrawList
	// overlay_op_start) paint over any Control a companion mounts as a frame child
	// (the PLAYER_INFO preview / icon mounts) — retail draws the open dropdown
	// and the cursor after every screen widget (CUIScene_DrawScreensAndCursor
	// @ 0x63bf60; D-MNU-12 in docs/mnu/menu-re.md).
	RID overlay_canvas_item_;
	void ensure_overlay_canvas_item_();
};

} // namespace godot
