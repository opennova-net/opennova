#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <vector>

#include <mnu/mnu.h>

namespace godot {

// Godot-facing wrapper around a parsed MNU menu document (NovaLogic's XML-like
// UI markup). All format behavior lives in libs/mnu; this Resource adds the
// editor-facing read + mutation surface, change signal (Resource::emit_changed),
// and Godot byte/file I/O.
//
// Widgets are addressed by a STABLE integer id assigned by this class and kept
// in sync across add/delete/reparent, so the editor's selection and undo survive
// re-renders and tree mutation (array indices would shift). Ids are unique
// across both screens and windows within one document instance.
class NovaMnuDocument : public Resource {
	GDCLASS(NovaMnuDocument, Resource)

public:
	// Mirrors mnu::WindowType (same order). -1 is used for screen containers,
	// which are not widgets.
	enum WidgetType {
		TYPE_WINDOW = 0,
		TYPE_STATIC,
		TYPE_BUTTON,
		TYPE_EDIT,
		TYPE_MULTILINE_EDIT,
		TYPE_LIST,
		TYPE_CHECKBOX,
		TYPE_RADIO,
		TYPE_COMBO,
		TYPE_SCROLL,
		TYPE_TABLE,
		TYPE_SPINLIST,
		TYPE_MULTI,
		TYPE_MAP,
		TYPE_GLOBE,
		TYPE_LABEL,
		TYPE_GOTO,
		TYPE_MARQUEE,
		TYPE_GLB_TABLE,
		TYPE_RADIOEDIT,
		TYPE_LAN_LIST,
		TYPE_GOPHER,
		TYPE_UNKNOWN,
	};

	// Color slots map to fields of mnu::Font. Values are raw strings (hex like
	// "FF8000" or a "%VAR%" stylesheet reference); conversion to a displayable
	// Color is the caller's job so variable references survive a round-trip.
	enum ColorSlot {
		COLOR_DEFAULT_FG = 0,
		COLOR_DEFAULT_BG,
		COLOR_MOUSEOVER_FG,
		COLOR_MOUSEOVER_BG,
		COLOR_SELECTED_FG,
		COLOR_SELECTED_BG,
		COLOR_DISABLED_FG,
		COLOR_DISABLED_BG,
	};

	// Texture slots map to mnu::Appearance entries by state.
	enum TextureSlot {
		TEX_DEFAULT = 0,
		TEX_MOUSEOVER,
		TEX_SELECTED,
		TEX_DISABLED,
	};

	// Window boolean flags (bitmask).
	enum WidgetFlags {
		FLAG_HIDDEN = 1 << 0,
		FLAG_DISABLED = 1 << 1,
		FLAG_CHECKED = 1 << 2,
		FLAG_DRAW_FRAME = 1 << 3,
		FLAG_MODAL = 1 << 4,
		FLAG_READONLY = 1 << 5,
	};

private:
	// Parallel id tree, kept structurally identical to doc_ so a lockstep walk
	// maps an id to its mnu::Window/Screen. One IdWindow mirrors one mnu::Window.
	struct IdWindow {
		int id = 0;
		std::vector<IdWindow> children;
	};
	struct IdScreen {
		int id = 0;
		IdWindow root;
	};

	mnu::Document doc_;
	std::vector<IdScreen> ids_;
	int next_id_ = 1;
	Vector2i menu_size_ = Vector2i(640, 480);

	// Build a fresh id tree mirroring doc_ (used after load / structural rebuild).
	void rebuild_ids();
	IdWindow make_id_window(const mnu::Window &w);

	// Snapshot helpers. collect_ids walks the id tree in the SAME pre-order
	// rebuild_ids assigns (screen id, then its root window subtree depth-first);
	// build_id_window_from_list rebuilds an IdWindow against doc_ but reads ids
	// from a stored list instead of minting new ones, so apply_state restores
	// byte-identical ids. Returns false on list under-run (desync).
	void collect_id_window(const IdWindow &node, PackedInt32Array &out) const;
	PackedInt32Array collect_ids() const;
	bool build_id_window_from_list(const mnu::Window &w, const PackedInt32Array &ids, int &k, IdWindow &out) const;

	// Locate an id. screen_index == -1 means "not found". is_screen marks a
	// screen container; otherwise path is the child-index chain from the
	// screen's root_window (empty path == the root_window itself).
	struct Locator {
		int screen_index = -1;
		std::vector<int> path;
		bool is_screen = false;
		bool valid() const { return screen_index >= 0; }
	};
	Locator locate(int id) const;
	static bool find_in_id_window(const IdWindow &node, int id, std::vector<int> &path);

	mnu::Window *window_at(const Locator &loc);
	const mnu::Window *window_at(const Locator &loc) const;
	IdWindow *id_window_at(const Locator &loc);

	// Find/format the mnu::Appearance for a texture slot's state string.
	static const char *state_for_slot(int slot);
	mnu::Appearance *find_appearance(mnu::Window &w, const char *state, bool create);

	void touch(); // emit_changed()

protected:
	static void _bind_methods();

public:
	NovaMnuDocument();

	// --- I/O ---
	Error load_from_bytes(const PackedByteArray &p_bytes);
	PackedByteArray to_byte_array() const;
	Error load_from_path(const String &p_path);
	Error save_to_path(const String &p_path) const;
	void create_empty();

	// --- Authoring canvas size ---
	Vector2i get_menu_size() const { return menu_size_; }
	void set_menu_size(const Vector2i &p_size);

	// --- Tree read ---
	int get_screen_count() const;
	PackedInt32Array get_screen_ids() const;
	int get_screen_root_id(int p_screen_id) const;
	bool is_screen(int p_id) const;
	bool widget_exists(int p_id) const;
	int get_parent_id(int p_id) const;
	PackedInt32Array get_child_ids(int p_id) const;
	int get_widget_type(int p_id) const; // WidgetType, or -1 for a screen
	String get_widget_type_name(int p_type) const;
	String get_widget_name(int p_id) const;
	Rect2 get_window_rect(int p_id) const;

	// --- Screen properties ---
	String get_screen_name(int p_screen_id) const;
	bool get_screen_has_music_var(int p_screen_id) const;
	int get_screen_music_var(int p_screen_id) const;
	String get_screen_text_rsrc(int p_screen_id) const;
	String get_screen_cursor_file(int p_screen_id) const;
	String get_screen_cursor_flags(int p_screen_id) const;
	void set_screen_property(int p_screen_id, const String &p_key, const Variant &p_value);

	// Which POSITION extents are explicitly authored (get_window_rect_flags
	// bitmask). Shipped menus omit RIGHT/BOTTOM for auto-size widgets (the
	// original engine stretches appearance art across an explicit width).
	enum RectFlags {
		RECT_HAS_LEFT = 1,
		RECT_HAS_TOP = 2,
		RECT_HAS_RIGHT = 4,
		RECT_HAS_BOTTOM = 8,
	};

	// --- Widget property read/write (setters emit changed) ---
	void set_widget_name(int p_id, const String &p_name);
	// A negative rect width/height means auto-size: clears has_right/has_bottom
	// so the writer omits RIGHT/BOTTOM, the shipped auto-size spelling.
	void set_window_rect(int p_id, const Rect2 &p_rect);
	int get_window_rect_flags(int p_id) const;

	String get_widget_text(int p_id) const;       // string_data.value
	void set_widget_text(int p_id, const String &p_text);
	String get_widget_string_type(int p_id) const; // "id" or "" (literal)
	void set_widget_string_type(int p_id, const String &p_type);

	String get_widget_font(int p_id) const;
	void set_widget_font(int p_id, const String &p_font);

	// Scalar template fields surfaced by the inspector (M9): the marquee data
	// source file and the scroll/marquee orientation.
	String get_widget_datasource(int p_id) const;
	void set_widget_datasource(int p_id, const String &p_value);
	String get_widget_orientation(int p_id) const;
	void set_widget_orientation(int p_id, const String &p_value);

	// Complete, presence-aware authoring state for one widget. apply_widget_patch
	// accepts any subset of the same grouped keys and applies it atomically,
	// preserving every field the caller did not mention. This is the deep seam
	// shared by the inspector and MCP; the scalar methods remain convenient
	// adapters for common edits.
	Dictionary get_widget_authoring_state(int p_id) const;
	bool apply_widget_patch(int p_id, const Dictionary &p_patch);

	// Per-widget interaction sounds (hover/click etc.). Each row is
	// {state, trigger, file}: file is the .lwf profile and trigger names a set in
	// it (MOUSE_OVER/CLICK_SELECT/...). set replaces the whole list (the editor's
	// natural undo unit), preserving any rows beyond hover/click the author leaves
	// in place (e.g. CLICK_VALUE on a combo).
	TypedArray<Dictionary> get_widget_sounds(int p_id) const;
	void set_widget_sounds(int p_id, const TypedArray<Dictionary> &p_sounds);

	// Per-widget navigation/window actions. Each row is
	// {type,target,state,file,source,field,test,has_target_form,target_form,
	// toggle,external_browser}: the lossless structured editor view of <ACTION>,
	// including retail shell-owned GLB/LAN/form/app-message verbs.
	TypedArray<Dictionary> get_widget_actions(int p_id) const;
	void set_widget_actions(int p_id, const TypedArray<Dictionary> &p_actions);

	// --- M10: structured authoring of nested list/table templates ---
	//
	// Item rows for list-like widgets (list / multi / spinlist / combo). Each row
	// is a Dictionary {type, value, text} so a row's typed/value fields survive a
	// round-trip verbatim (the items analog of the color %VAR% rule). The active
	// container mirrors the runtime builder: a combo with a LIST_BOX stores its
	// rows there; everything else uses the window's own <ITEMS>. Mutators no-op on
	// a widget that has no item list. Indices out of range are clamped / ignored.
	int get_item_count(int p_id) const;
	Dictionary get_item(int p_id, int p_index) const;
	TypedArray<Dictionary> get_items(int p_id) const;
	void set_item(int p_id, int p_index, const Dictionary &p_row);
	int add_item(int p_id, const Dictionary &p_row); // returns the new row index, or -1
	void remove_item(int p_id, int p_index);
	void move_item(int p_id, int p_from, int p_to); // p_to is the destination index

	// Table column template (header/body/subst definitions) for a Table widget.
	// Headers are {has_column,column,has_width,width,justify,vjustify,sort,type,
	// text}; bodies are {has_column,column,bitmap_draw,scale_bitmap,custom_draw,
	// bitmap_flags,justify,vjustify}; value->image SUBST rows are
	// {has_column,column,value,is_file,file}. Presence flags distinguish an
	// omitted attribute from an explicitly-authored zero. Getters return empty /
	// no-op for a non-table id.
	int get_table_column_count(int p_id) const;
	void set_table_column_count(int p_id, int p_count);
	int get_table_column_spacing(int p_id) const;
	void set_table_column_spacing(int p_id, int p_spacing);
	TypedArray<Dictionary> get_table_headers(int p_id) const;
	void set_table_header(int p_id, int p_index, const Dictionary &p_row);
	int add_table_header(int p_id, const Dictionary &p_row); // returns the new index, or -1
	void remove_table_header(int p_id, int p_index);
	TypedArray<Dictionary> get_table_bodies(int p_id) const;
	void set_table_body(int p_id, int p_index, const Dictionary &p_row);
	int add_table_body(int p_id, const Dictionary &p_row); // returns the new index, or -1
	void remove_table_body(int p_id, int p_index);
	TypedArray<Dictionary> get_table_substs(int p_id) const;
	void set_table_subst(int p_id, int p_index, const Dictionary &p_row);
	int add_table_subst(int p_id, const Dictionary &p_row); // returns the new index, or -1
	void remove_table_subst(int p_id, int p_index);

	String get_widget_color(int p_id, int p_slot) const; // raw string, preserves %VAR%
	void set_widget_color(int p_id, int p_slot, const String &p_value);

	String get_widget_texture(int p_id, int p_slot) const;
	void set_widget_texture(int p_id, int p_slot, const String &p_value);

	int get_widget_flags(int p_id) const;
	void set_widget_flags(int p_id, int p_flags);
	int get_widget_group(int p_id) const;
	void set_widget_group(int p_id, int p_group);
	PackedStringArray get_flag_labels() const;

	// Full per-state appearance rows {state, type, value, map_state, height};
	// set replaces the whole list. This is the only writer that can author the
	// stock empty-state rows (no type) and "custom" rows — the color/texture
	// slot setters always create type="image" rows.
	TypedArray<Dictionary> get_widget_appearances(int p_id) const;
	void set_widget_appearances(int p_id, const TypedArray<Dictionary> &p_rows);

	// FRAME assets {stencil, stencil_size, brush, monogram}: DRAW_FRAME children
	// render with the nearest ancestor's frame; shipped screens carry it on the
	// root window. insetx/insety round-trip data is preserved untouched.
	Dictionary get_window_frame(int p_id) const;
	void set_window_frame(int p_id, const Dictionary &p_frame);

	// Corpus-structure probes for the game-safety analyzer: every shipped LIST
	// has a SCROLLBAR subtree, every shipped SPINLIST has SPINUP+SPINDOWN.
	bool widget_has_scrollbar(int p_id) const;
	bool widget_has_spin_arrows(int p_id) const;

	// --- Structural mutation (emit changed; return the affected id) ---
	int add_widget(int p_parent_id, int p_type, const Rect2 &p_rect);
	void delete_widget(int p_id);
	int add_screen(const String &p_name);
	void delete_screen(int p_screen_id);
	// Lossless subtree clipboard. The payload is a self-contained MNU document,
	// so editor copy/cut/paste preserves every nested authored field while pasted
	// nodes receive fresh stable ids.
	PackedByteArray capture_widget_subtree(int p_id) const;
	int insert_widget_subtree(int p_parent_id, const PackedByteArray &p_payload,
			int p_index = -1, const Vector2i &p_offset = Vector2i());
	bool move_widget_to_index(int p_id, int p_index);
	int duplicate_screen(int p_screen_id, const String &p_name);
	bool move_screen_to_index(int p_screen_id, int p_index);

	// --- Snapshot + reparent (M8: structural undo) ---
	// capture_state returns an opaque state {mnu, ids, next_id, menu_size};
	// apply_state restores it with byte-identical widget ids, so editor undo ops
	// that reference ids stay valid across a structural undo (a plain
	// to_byte_array/load_from_bytes round-trip would renumber positionally after
	// an in-session delete). On id-list desync apply_state falls back to a
	// positional rebuild_ids.
	Dictionary capture_state() const;
	void apply_state(const Dictionary &p_state);
	// Move a widget subtree under a new parent (a screen targets its root window),
	// inserting at p_index. Refuses a screen / root-window source, an unknown
	// parent, or a cycle (new parent == the node or a descendant of it). Returns
	// false when refused; preserves the moved subtree's ids.
	bool reparent_widget(int p_id, int p_new_parent_id, int p_index);

	// Native access for the loader/saver.
	void set_native(const mnu::Document &p_doc);
	const mnu::Document &get_native() const { return doc_; }
};

} // namespace godot

VARIANT_ENUM_CAST(godot::NovaMnuDocument::WidgetType);
VARIANT_ENUM_CAST(godot::NovaMnuDocument::ColorSlot);
VARIANT_ENUM_CAST(godot::NovaMnuDocument::TextureSlot);
VARIANT_ENUM_CAST(godot::NovaMnuDocument::WidgetFlags);
