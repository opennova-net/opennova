#pragma once

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <base/vfs/file_source.h>
#include <formats/rtxt/rtxt.h>
#include <runtime/menu/menu_frame.h>
#include <runtime/menu/menu_frame_assets.h>

#include "resource_index/resource_root_file_source.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace godot {

class MenuDrawListStats;

class MnuDocument;
class MnsStyleSheet;
class RtxtStringFile;

// The Godot half of the menu textures the engine's loader keeps (MenuFrameAssets): the
// pixels decoded by the format retail's dispatch picked (TGA / DDS / PNG through Godot's
// decoders, PCX through the engine's port of retail's loader) and their upload, kept
// under the loader's key until it lets them go.
class MenuFrameTextures : public opennova::menu::MenuTextureDecoder {
public:
	struct Entry {
		Ref<Image> image;
		Ref<Texture2D> texture;
	};
	bool decode(const std::string &p_key, opennova::menu::MenuTextureFormat p_format,
			const std::vector<uint8_t> &p_bytes, int &r_width, int &r_height) override;
	void release(const std::string &p_key) override;
	const Entry *find(const std::string &p_key) const;

private:
	std::map<std::string, Entry> entries_;
};

// The compiled-menu device leg (ADR 0033 R2) over the engine's
// MenuFrameCompiler (engine/runtime/menu): the engine owns the witnessed .mnu
// screen draw walk — widget order, state-driven appearance/color selection,
// text layout, the edit caret, frames, the mouse pump, the interaction
// geometry queries, and the edit-input module (witness record:
// docs/mnu/menu-re.md); this Control keeps only what a device leg may keep:
// VFS texture/.fnt upload, the typed per-widget state marshalling, and
// rasterizing the compiled MenuDrawList with CanvasItem draw calls. The game
// menu shell (the MenuDriver binding over engine/runtime/menu + menu_shell.gd) and test
// Menus canvas both drive this one surface.
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
	// last of that name; empty = the first screen), loading through the mounted
	// VFS root what the screen reads: the widget art (by retail's extension
	// dispatch, engine menu_assets.h), every FONT's .fnt, and every TEXT_RSRC
	// string table; `style` supplies the %VAR% stylesheet; `override_text` is
	// the expansion's table every string lookup tries first (null: none). The
	// document Ref is retained; re-configure after document edits. A null
	// document lets everything go.
	bool configure(const Ref<MnuDocument> &p_document,
			const String &p_screen_name, const Ref<ResourceRoot> &p_root,
			const Ref<MnsStyleSheet> &p_style, const Ref<RtxtStringFile> &p_override_text);
	// The C++ core (not bound): `p_screen` of `p_document` through any file source
	// (the game's root, the editor's project files), the engine's loader
	// (runtime/menu/menu_frame_assets.h) keeping what it loaded by (file, stamp)
	// across configures. The document, the screen, the file source and the
	// override table are borrowed until the next configure.
	bool configure_screen(const opennova::mnu::Document *p_document,
			const opennova::mnu::Screen *p_screen, const opennova::FileSource &p_files,
			const std::map<std::string, std::string> &p_vars,
			const opennova::rtxt::File *p_override_text);
	// Let everything go (configure over nothing).
	void clear_screen();
	bool is_configured() const;
	// Forget the texture loads earlier configures made (the band height a
	// texture's first load fixed; engine menu_frame.h note_texture_loads): the
	// editor preview's every configure is a first load; the game keeps them for
	// the session, as retail keeps its texture cache.
	void reset_loads();
	// C++ siblings (not bound): the configured compiler, its frame state and the
	// loader, for the editor preview's picking, rects and JSON.
	const opennova::menu::MenuFrameCompiler &native_compiler() const { return compiler_; }
	const opennova::menu::MenuFrameState &native_state() const { return state_; }
	const opennova::menu::MenuFrameAssets &native_assets() const { return assets_; }
	// The editor viewport's frame state (it never pumps): what its options hold on the
	// configured screen (editor/preview apply_menu_options), set after each configure.
	void set_native_state(const opennova::menu::MenuFrameState &p_state);

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
	// CWnd_SetRect: the widget's own rect (parent-relative design units).
	void set_widget_rect(int p_index, const Rect2i &p_rect);
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
	// TABLE rows (menu_table_row.h) and the table's custom-draw handler (the
	// CUSTOM_DRAW cells' control callback), C++ only.
	void set_widget_table_rows(int p_index,
			const std::vector<opennova::menu::MenuTableRow> &p_rows);
	// The same rows from GDScript (a test's, a GDScript shell's): each an Array or a
	// PackedStringArray of its cells' texts, the rest of the row as CTableWnd_AddRow
	// leaves it (column 0's value 0, state 0, no flags).
	void set_widget_table_cells(int p_index, const Array &p_rows);
	void set_table_cell_painter(int p_index, opennova::menu::MenuTableCellPainter p_painter);
	// CWnd_SetClipRect (absolute design units); `p_enabled` false removes it.
	void set_widget_clip_rect(int p_index, bool p_enabled, const Rect2i &p_rect);
	// The runtime's installed table columns and a marquee's credits (C++ only: the
	// menu driver's frame seam).
	void set_widget_table_columns(int p_index, bool p_installed,
			const std::vector<opennova::menu::MenuTableColumn> &p_columns, int p_sort_column);
	void set_widget_marquee(int p_index, const opennova::menu::MarqueeCredits &p_credits);
	// Whether a menu texture name loads through the configured file source
	// (retail's dispatch, menu_assets.h): the marquee's image nodes ask.
	bool texture_loads(const String &p_name);
	// A texture the next configure over `p_files` names, decoded and kept ahead of it (C++ only:
	// the editor's menu device spreads a screen's first configure over its steps, ADR 0046 S13 V6;
	// native_assets().texture_kept says what is not kept yet).
	bool load_texture_ahead(const std::string &p_name, const opennova::FileSource &p_files);

	// Widget queries over the configured screen (design-space rects; the
	// pre-order index space matches a document DFS of the same screen).
	int widget_count() const;
	String widget_name(int p_index) const;
	int widget_kind(int p_index) const;
	// A widget's text as the game draws it: each byte a glyph of the font's code page,
	// Windows-1252 (a retail VERSION line's copyright sign is its 0xA9 byte), decoded as such.
	String widget_authored_text(int p_index) const;
	bool is_widget_disabled(int p_index) const;
	// Effective draw/hit visibility (own flag + ancestors + overrides) —
	// shell overlays mounted over a widget must follow it.
	bool is_widget_shown(int p_index) const;
	Rect2 widget_rect(int p_index) const;
	// The widget's own rect relative to its parent's origin (CWnd_GetRect).
	Rect2 widget_local_rect(int p_index) const;
	// The widget a companion mounts a Control over (-1 none): the widgets
	// after it paint on the menu-top overlay, above the mount.
	void set_mount_widget(int p_index);
	int get_mount_widget() const { return state_.mount_index; }
	// The widget whose CUSTOM appearance pass a companion draws (-1 none):
	// its canvas item sits between the ops before and after that pass, and
	// is visible only while the pass ran this frame (engine
	// MenuDrawList::custom_slot_op).
	void set_custom_slot_widget(int p_index);
	int get_custom_slot_widget() const { return state_.custom_slot_index; }
	RID get_custom_slot_canvas_item();
	bool is_custom_slot_drawn() const { return custom_slot_drawn_; }
	int item_count(int p_index) const;
	String get_widget_text(int p_index) const; // effective: runtime else authored; cp1252 as authored
	int get_widget_caret(int p_index) const;

	// Interaction geometry (positions in this control's local coordinates;
	// the engine scales them like process_mouse does).
	int hit_test(const Vector2 &p_position) const;
	int list_row_at(int p_index, const Vector2 &p_position) const;
	bool combo_popup_contains(int p_index, const Vector2 &p_position) const;
	int combo_popup_row_at(int p_index, const Vector2 &p_position) const;
	int spin_arrow_at(int p_index, const Vector2 &p_position) const; // 0/1 up/2 down
	// A list-like widget's displayed row text (C++ only).
	std::string item_display_text(int p_index, int p_row) const;
	// The table hit test (MenuFrameCompiler::table_hit, CTableWnd_HitTest): the
	// data row (-1 the header strip) and column (-1 none); false where retail fails.
	bool table_hit(int p_index, const Vector2 &p_position, int *r_row, int *r_column) const;
	// A widget's parse-time {hot} mnemonic (MenuFrameCompiler::widget_mnemonic).
	std::string widget_mnemonic(int p_index) const;
	// The open popup (a shown MODAL window's index, -1 none): the pump serves
	// its subtree alone (MenuFrameState::popup_root).
	void set_open_popup(int p_index);

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
	// The cursor pass at a point with no pump (C++ only: the editor's picture,
	// which never pumps, draws the game's pointer by it): the claim the pump would
	// make there stamps the cursor (engine MenuFrameCompiler::claim_at), no hover
	// or press written; drawn again only when it moved. Hidden: no claim.
	void place_cursor(bool p_visible, const Vector2 &p_position);

	// Feed one raw-mouse sample through the engine pump (menu_frame.h
	// pump_mouse carries the witness): updates every row's hover/press, the
	// cursor position, and returns the claimed widget index (-1 = none).
	// Local control coordinates; the pump scales by this control's size.
	int process_mouse(const Vector2 &p_position, bool p_button_down);
	// True when a scrollbar part took the last process_mouse sample (its press
	// never reaches the owner widget).
	bool last_sample_scrolled() const { return last_sample_scrolled_; }

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
	// The menu items' shader (the font pages' MODULATE2X stage on a flagged
	// glyph run), and the test seam's copy of each glyph run as submitted:
	// [{page, uvs, colors}].
	static String glyph_shader_code();
	Array get_glyph_submissions();

	void _draw() override;

protected:
public:
	// The retail Options policy tables (engine/runtime/menu/options_policy.h),
	// re-exported for the shell's appliers: [{control, minimum, maximum, page}],
	// [{control, value}], the object-detail rows' names, the gamma reference, the
	// preset-button names, the not-yet-serviced control names and
	// [{control, checked}] their rows show.
	static Array options_scroll_ranges();
	static Array video_quality_controls();
	static PackedStringArray object_detail_controls();
	static int video_gamma_reference();
	static PackedStringArray video_preset_buttons();
	static PackedStringArray options_unsupported_controls();
	static Array options_forced_checks();

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	opennova::menu::MenuWidgetState &widget_(int p_index);
	Ref<Texture2D> texture_for_quad_(const opennova::menu::MenuQuad &p_quad);
	// A composed frame texture by key, kept across configures while drawn (the
	// cache lets go of what two configures in a row did not draw).
	Ref<Texture2D> cached_frame_texture_(const std::string &p_key) const;
	void keep_frame_texture_(const std::string &p_key, const Ref<Texture2D> &p_texture);
	// The uploaded pages of the font a compiler slot draws with (null: none).
	const std::vector<Ref<Texture2D>> *font_pages_(int32_t p_slot);
	void adopt_slots_();
	Vector2 design_scale_() const;

	Ref<MnuDocument> document_;
	// The press and the click over the claims (engine MenuClickLatch).
	opennova::menu::MenuClickLatch click_;
	bool last_sample_scrolled_ = false;
	int32_t cursor_slot_ = -1;  // last claim's cursor texture slot
	bool configured_ = false;
	opennova::menu::MenuFrameCompiler compiler_;
	opennova::menu::MenuFrameState state_;
	// The engine's loader: the string tables, the fonts (their parsed storage the
	// compiler borrows) and the textures, kept by (file, stamp).
	opennova::menu::MenuFrameAssets assets_;
	MenuFrameTextures texture_store_;
	// The root the bound configure reads through, and the file source of the last
	// configure (borrowed; a marquee's fonts and images load through it later).
	ResourceRootFileSource root_files_;
	const opennova::FileSource *files_ = nullptr;
	// Per texture slot of the configured screen: the loader's key and the kept
	// texture and pixels. Source pixels are retained for the frame material
	// adapter. Retail's border is a fixed-function two-texture material, not
	// either authored texture by itself; derived textures are cached by the
	// textures' keys, UV and destination.
	std::vector<std::string> texture_keys_;
	std::vector<Ref<Texture2D>> textures_;
	std::vector<Ref<Image>> texture_images_;
	struct FrameTexture {
		Ref<Texture2D> texture;
		uint64_t used = 0; // the configure that last drew it
	};
	mutable std::map<std::string, FrameTexture> frame_texture_cache_;
	uint64_t configure_count_ = 0;
	// Each kept font's pages, uploaded on first draw, by the load's serial.
	std::map<uint64_t, std::vector<Ref<Texture2D>>> font_pages_by_serial_;
	Ref<RtxtStringFile> override_text_;
	// The menu-top overlay: a child canvas item one z above this Control, so
	// the compiled draw list's popup + cursor ops (MenuDrawList
	// overlay_op_start) paint over any Control a companion mounts as a frame child
	// (the PLAYER_INFO preview / icon mounts) — retail draws the open dropdown
	// and the cursor after every screen widget (CUIScene_DrawScreensAndCursor
	// @ 0x63bf60; D-MNU-12 in docs/mnu/menu-re.md).
	RID overlay_canvas_item_;
	void ensure_overlay_canvas_item_();
	// The custom-draw slot's item (z 2: a companion draws into it) and the
	// overlay ops after the slot (z 3).
	RID slot_canvas_item_;
	RID overlay_upper_canvas_item_;
	bool custom_slot_drawn_ = false;
	// The material this Control's item and both overlay items draw under: a
	// glyph run's font page runs its MODULATE2X stage (glyph_shader_code).
	Ref<Shader> glyph_shader_;
	Ref<ShaderMaterial> glyph_material_;
	// Set while get_glyph_submissions records the glyph runs _draw submits.
	Array *glyph_record_ = nullptr;
};

} // namespace godot
