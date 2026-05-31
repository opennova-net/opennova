#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/string.hpp>
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
	int get_screen_music_var(int p_screen_id) const;
	String get_screen_text_rsrc(int p_screen_id) const;
	String get_screen_cursor_file(int p_screen_id) const;
	void set_screen_property(int p_screen_id, const String &p_key, const Variant &p_value);

	// --- Widget property read/write (setters emit changed) ---
	void set_widget_name(int p_id, const String &p_name);
	void set_window_rect(int p_id, const Rect2 &p_rect);

	String get_widget_text(int p_id) const;       // string_data.value
	void set_widget_text(int p_id, const String &p_text);
	String get_widget_string_type(int p_id) const; // "id" or "" (literal)
	void set_widget_string_type(int p_id, const String &p_type);

	String get_widget_font(int p_id) const;
	void set_widget_font(int p_id, const String &p_font);

	String get_widget_color(int p_id, int p_slot) const; // raw string, preserves %VAR%
	void set_widget_color(int p_id, int p_slot, const String &p_value);

	String get_widget_texture(int p_id, int p_slot) const;
	void set_widget_texture(int p_id, int p_slot, const String &p_value);

	int get_widget_flags(int p_id) const;
	void set_widget_flags(int p_id, int p_flags);
	int get_widget_group(int p_id) const;
	void set_widget_group(int p_id, int p_group);
	PackedStringArray get_flag_labels() const;

	// --- Structural mutation (emit changed; return the affected id) ---
	int add_widget(int p_parent_id, int p_type, const Rect2 &p_rect);
	void delete_widget(int p_id);
	int add_screen(const String &p_name);
	void delete_screen(int p_screen_id);

	// Native access for the loader/saver.
	void set_native(const mnu::Document &p_doc);
	const mnu::Document &get_native() const { return doc_; }
};

} // namespace godot

VARIANT_ENUM_CAST(godot::NovaMnuDocument::WidgetType);
VARIANT_ENUM_CAST(godot::NovaMnuDocument::ColorSlot);
VARIANT_ENUM_CAST(godot::NovaMnuDocument::TextureSlot);
VARIANT_ENUM_CAST(godot::NovaMnuDocument::WidgetFlags);
