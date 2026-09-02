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

#include <vector>

#include <formats/mnu/mnu.h>

namespace godot {

// Godot-facing wrapper around a parsed MNU menu document (NovaLogic's XML-like
// UI markup). All format behavior lives in engine/formats/mnu; this Resource adds
// the read surface the menu driver walks and Godot byte/file I/O. Documents are
// read-only here since ONED went run-only (ADR 0037); the authoring half was
// deleted 2026-09-02.
//
// Widgets are addressed by a positional integer id: screens and windows are
// numbered in one pre-order walk at load (rebuild_ids), unique across both
// screens and windows within one document instance.
class MnuDocument : public Resource {
	GDCLASS(MnuDocument, Resource)

public:
	// Mirrors opennova::mnu::WindowType (same order). -1 is used for screen containers,
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

	// Color slots map to fields of opennova::mnu::Font. Values are raw strings (hex like
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

	// Texture slots map to opennova::mnu::Appearance entries by state.
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
	// maps an id to its opennova::mnu::Window/Screen. One IdWindow mirrors one opennova::mnu::Window.
	struct IdWindow {
		int id = 0;
		std::vector<IdWindow> children;
	};
	struct IdScreen {
		int id = 0;
		IdWindow root;
	};

	opennova::mnu::Document doc_;
	std::vector<IdScreen> ids_;
	int next_id_ = 1;

	// Build a fresh id tree mirroring doc_ (used after load / structural rebuild).
	void rebuild_ids();
	IdWindow make_id_window(const opennova::mnu::Window &w);

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

	opennova::mnu::Window *window_at(const Locator &loc);
	const opennova::mnu::Window *window_at(const Locator &loc) const;
	IdWindow *id_window_at(const Locator &loc);

	// The opennova::mnu::Appearance state string of a texture slot.
	static const char *state_for_slot(int slot);

protected:
	static void _bind_methods();

public:
	MnuDocument();

	// --- I/O ---
	Error load_from_bytes(const PackedByteArray &p_bytes);
	PackedByteArray to_byte_array() const;
	Error load_from_path(const String &p_path);
	Error save_to_path(const String &p_path) const;

	// --- Tree read ---
	int get_screen_count() const;
	PackedInt32Array get_screen_ids() const;
	int get_screen_root_id(int p_screen_id) const;
	bool is_screen(int p_id) const;
	bool widget_exists(int p_id) const;
	int get_parent_id(int p_id) const;
	PackedInt32Array get_child_ids(int p_id) const;
	int get_widget_type(int p_id) const; // WidgetType, or -1 for a screen
	String get_widget_name(int p_id) const;
	Rect2 get_window_rect(int p_id) const;

	// --- Screen properties ---
	String get_screen_name(int p_screen_id) const;
	bool get_screen_has_music_var(int p_screen_id) const;
	int get_screen_music_var(int p_screen_id) const;
	String get_screen_text_rsrc(int p_screen_id) const;

	// Which POSITION extents are explicitly authored (the window-rect
	// bitmask). Shipped menus omit RIGHT/BOTTOM for auto-size widgets (the
	// original engine stretches appearance art across an explicit width).
	enum RectFlags {
		RECT_HAS_LEFT = 1,
		RECT_HAS_TOP = 2,
		RECT_HAS_RIGHT = 4,
		RECT_HAS_BOTTOM = 8,
	};

	// --- Widget property read ---
	String get_widget_text(int p_id) const;       // string_data.value
	String get_widget_string_type(int p_id) const; // "id" or "" (literal)

	String get_widget_font(int p_id) const;

	// The marquee data source file and the scroll/marquee orientation.
	String get_widget_datasource(int p_id) const;
	String get_widget_orientation(int p_id) const;

	// Per-widget interaction sounds (hover/click etc.). Each row is
	// {state, trigger, file}: file is the .lwf profile and trigger names a set in
	// it (MOUSE_OVER/CLICK_SELECT/...).
	TypedArray<Dictionary> get_widget_sounds(int p_id) const;

	// Per-widget navigation/window actions. Each row is
	// {type,target,state,file,source,field,test,has_target_form,target_form,
	// toggle,external_browser}: the structured view of <ACTION>, including
	// retail shell-owned GLB/LAN/form/app-message verbs.
	TypedArray<Dictionary> get_widget_actions(int p_id) const;

	// Item rows for list-like widgets (list / multi / spinlist / combo). Each row
	// is a Dictionary {type, value, text}. The active container mirrors the
	// runtime builder: a combo with a LIST_BOX stores its rows there; everything
	// else uses the window's own <ITEMS>.
	// The window-level <ITEMS multiselect=...> flag (the driver's CTRL-select gate).
	bool is_widget_multiselect(int p_id) const;
	int get_item_count(int p_id) const;
	// Retail's select-by-value seed: the row whose authored `value=` equals
	// p_value, row 0 on a miss; -1 only when the widget has no item list
	// (engine/runtime/menu/options_policy.h spinlist_row_for_value).
	int find_item_row_by_value(int p_id, const String &p_value) const;
	Dictionary get_item(int p_id, int p_index) const;

	String get_widget_color(int p_id, int p_slot) const; // raw string, preserves %VAR%

	String get_widget_texture(int p_id, int p_slot) const;

	int get_widget_flags(int p_id) const;
	int get_widget_group(int p_id) const;

	// Native access for the loader/saver.
	void set_native(const opennova::mnu::Document &p_doc);
	const opennova::mnu::Document &get_native() const { return doc_; }
};

} // namespace godot

VARIANT_ENUM_CAST(godot::MnuDocument::WidgetType);
VARIANT_ENUM_CAST(godot::MnuDocument::ColorSlot);
VARIANT_ENUM_CAST(godot::MnuDocument::TextureSlot);
VARIANT_ENUM_CAST(godot::MnuDocument::WidgetFlags);
