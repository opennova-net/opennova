#include "nova_mnu_document.h"

#include "util/nova_string_convert.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstring>
#include <string>

using namespace godot;

namespace {

using opennova::to_gd;
using opennova::to_std;

// --- M10 helpers: active item container + dict <-> struct converters ---

// The canonical item-row container mirrors the authored format. Combo rows live
// inside LIST_BOX; treating win.items as a mirror is destructive because it can
// overwrite independently-authored popup alignment, appearances, and rows.
// List/Table and the other list-like controls use the window-level ITEMS block.
mnu::Items *items_container(mnu::Window *w) {
	if (w == nullptr) {
		return nullptr;
	}
	switch (w->type) {
		case mnu::WindowType::List:
		case mnu::WindowType::Table:
		case mnu::WindowType::GlbTable:
		case mnu::WindowType::Multi:
		case mnu::WindowType::LanList:
		case mnu::WindowType::SpinList:
			return &w->items;
		case mnu::WindowType::Combo:
			// LIST_BOX owns combo items when it actually authors an ITEMS
			// block under an authored LIST_BOX. A latent/disabled LIST_BOX or a
			// styling-only one falls back to top-level ITEMS, matching runtime.
			return w->list_box.present && w->list_box.items.present ?
					&w->list_box.items : &w->items;
		default:
			return nullptr;
	}
}

const mnu::Items *items_container(const mnu::Window *w) {
	return items_container(const_cast<mnu::Window *>(w));
}

Dictionary item_to_dict(const mnu::Item &it) {
	Dictionary d;
	d["type"] = to_gd(it.type);
	d["value"] = to_gd(it.value);
	d["text"] = to_gd(it.text);
	return d;
}

mnu::Item item_from_dict(const Dictionary &d) {
	mnu::Item it;
	it.type = to_std(String(d.get("type", "")));
	it.value = to_std(String(d.get("value", "")));
	it.text = to_std(String(d.get("text", "")));
	return it;
}

mnu::TableData *table_of(mnu::Window *w) {
	return (w != nullptr && (w->type == mnu::WindowType::Table ||
			w->type == mnu::WindowType::GlbTable)) ? &w->table_data : nullptr;
}

const mnu::TableData *table_of(const mnu::Window *w) {
	return table_of(const_cast<mnu::Window *>(w));
}

bool is_table_window(const mnu::Window &w) {
	return w.type == mnu::WindowType::Table ||
			w.type == mnu::WindowType::GlbTable;
}

void sync_items_selection_alias(mnu::Items &items) {
	items.selection_color.clear();
	for (const mnu::Appearance &appearance : items.appearances) {
		if (to_gd(appearance.state).nocasecmp_to("selected") == 0 &&
				to_gd(appearance.type).nocasecmp_to("color") == 0) {
			items.selection_color = appearance.value;
		}
	}
}

void set_items_selection_alias(mnu::Items &items, const std::string &value) {
	items.selection_color = value;
	// Clearing updates an existing winning row but never invents an empty row.
	if (!value.empty() ||
			items.find_appearance("selected", "color") != nullptr) {
		items.set_appearance_value("selected", "color", value);
	}
}

void sync_table_aliases_from_items(mnu::Window &w) {
	if (!is_table_window(w)) {
		return;
	}
	w.table_data.outline_color.clear();
	sync_items_selection_alias(w.items);
	for (const mnu::Appearance &appearance : w.items.appearances) {
		String state = to_gd(appearance.state).to_lower();
		String type = to_gd(appearance.type).to_lower();
		if (state == "default" && type == "outline") {
			w.table_data.outline_color = appearance.value;
		}
	}
	w.table_data.selection_color = w.items.selection_color;
}

void set_table_item_alias(mnu::Window &w, const char *state,
		const char *type, const std::string &value) {
	if (!is_table_window(w)) {
		return;
	}
	// Empty aliases should update an existing authored row but must not create
	// a new empty row solely because a scalar was cleared.
	if (!value.empty() || w.items.find_appearance(state, type) != nullptr) {
		w.items.set_appearance_value(state, type, value);
		// Table colors are serialized through the ordered ITEMS appearances.
		// Editing either scalar alias therefore authors that container too.
		w.items.present = true;
	}
}

Dictionary header_to_dict(const mnu::TableHeader &h) {
	Dictionary d;
	d["has_column"] = h.has_column;
	d["column"] = h.column;
	d["has_width"] = h.has_width;
	d["width"] = h.width;
	d["justify"] = to_gd(h.justify);
	d["vjustify"] = to_gd(h.vjustify);
	d["sort"] = to_gd(h.sort);
	d["type"] = to_gd(h.type);
	d["text"] = to_gd(h.text);
	return d;
}

mnu::TableHeader header_from_dict(const Dictionary &d) {
	mnu::TableHeader h;
	h.has_column = bool(d.get("has_column", d.has("column")));
	h.column = static_cast<int>(d.get("column", 0));
	h.has_width = bool(d.get("has_width", d.has("width")));
	h.width = static_cast<int>(d.get("width", 0));
	h.justify = to_std(String(d.get("justify", "")));
	h.vjustify = to_std(String(d.get("vjustify", "")));
	h.sort = to_std(String(d.get("sort", "")));
	h.type = to_std(String(d.get("type", "")));
	h.text = to_std(String(d.get("text", "")));
	return h;
}

Dictionary body_to_dict(const mnu::TableBody &b) {
	Dictionary d;
	d["has_column"] = b.has_column;
	d["column"] = b.column;
	d["justify"] = to_gd(b.justify);
	d["vjustify"] = to_gd(b.vjustify);
	d["bitmap_draw"] = b.bitmap_draw;
	d["scale_bitmap"] = b.scale_bitmap;
	d["bitmap_flags"] = to_gd(b.bitmap_flags);
	d["custom_draw"] = b.custom_draw;
	return d;
}

mnu::TableBody body_from_dict(const Dictionary &d) {
	mnu::TableBody b;
	b.has_column = bool(d.get("has_column", d.has("column")));
	b.column = static_cast<int>(d.get("column", 0));
	b.justify = to_std(String(d.get("justify", "")));
	b.vjustify = to_std(String(d.get("vjustify", "")));
	b.bitmap_draw = static_cast<bool>(d.get("bitmap_draw", false));
	b.scale_bitmap = static_cast<bool>(d.get("scale_bitmap", false));
	b.bitmap_flags = to_std(String(d.get("bitmap_flags", "")));
	b.custom_draw = static_cast<bool>(d.get("custom_draw", false));
	return b;
}

Dictionary subst_to_dict(const mnu::TableSubst &s) {
	Dictionary d;
	d["has_column"] = s.has_column;
	d["column"] = s.column;
	d["value"] = to_gd(s.value);
	d["is_file"] = s.is_file;
	d["file"] = to_gd(s.file);
	return d;
}

mnu::TableSubst subst_from_dict(const Dictionary &d) {
	mnu::TableSubst s;
	s.has_column = bool(d.get("has_column", d.has("column")));
	s.column = static_cast<int>(d.get("column", 0));
	s.value = to_std(String(d.get("value", "")));
	s.is_file = static_cast<bool>(d.get("is_file", false));
	s.file = to_std(String(d.get("file", "")));
	return s;
}

mnu::Screen make_default_screen(const std::string &name) {
	mnu::Screen screen;
	screen.name = name;
	// New documents and added screens must share the retail-safe root shape.
	// The original layout/render paths assume all four bounds and an appearance.
	screen.root_window.name = "MAIN";
	screen.root_window.type = mnu::WindowType::Window;
	mnu::Position &root_pos = screen.root_window.position;
	root_pos.left = 0;
	root_pos.top = 0;
	root_pos.right = 800;
	root_pos.bottom = 600;
	root_pos.has_left = root_pos.has_top = root_pos.has_right = root_pos.has_bottom = true;
	mnu::Appearance root_app;
	root_app.type = "custom";
	root_app.state = "default";
	screen.root_window.appearances.push_back(std::move(root_app));
	return screen;
}

Dictionary position_to_dict(const mnu::Position &p) {
	Dictionary d;
	d["left"] = p.left;
	d["top"] = p.top;
	d["right"] = p.right;
	d["bottom"] = p.bottom;
	d["has_left"] = p.has_left;
	d["has_top"] = p.has_top;
	d["has_right"] = p.has_right;
	d["has_bottom"] = p.has_bottom;
	return d;
}

void apply_position_patch(mnu::Position &p, const Dictionary &d) {
	if (d.has("left")) p.left = int(d["left"]);
	if (d.has("top")) p.top = int(d["top"]);
	if (d.has("right")) p.right = int(d["right"]);
	if (d.has("bottom")) p.bottom = int(d["bottom"]);
	if (d.has("has_left")) p.has_left = bool(d["has_left"]);
	if (d.has("has_top")) p.has_top = bool(d["has_top"]);
	if (d.has("has_right")) p.has_right = bool(d["has_right"]);
	if (d.has("has_bottom")) p.has_bottom = bool(d["has_bottom"]);
}

Dictionary appearance_to_dict(const mnu::Appearance &a) {
	Dictionary d;
	d["state"] = to_gd(a.state);
	d["type"] = to_gd(a.type);
	d["value"] = to_gd(a.value);
	d["has_map_state"] = a.has_map_state;
	d["map_state"] = a.map_state;
	d["has_height"] = a.has_height;
	d["height"] = a.height;
	return d;
}

TypedArray<Dictionary> appearances_to_array(const std::vector<mnu::Appearance> &appearances) {
	TypedArray<Dictionary> out;
	for (const mnu::Appearance &a : appearances) {
		out.push_back(appearance_to_dict(a));
	}
	return out;
}

std::vector<mnu::Appearance> appearances_from_array(const TypedArray<Dictionary> &rows) {
	std::vector<mnu::Appearance> out;
	out.reserve(static_cast<size_t>(rows.size()));
	for (int i = 0; i < rows.size(); ++i) {
		const Dictionary d = rows[i];
		mnu::Appearance a;
		a.state = to_std(String(d.get("state", "")));
		a.type = to_std(String(d.get("type", "")));
		a.value = to_std(String(d.get("value", "")));
		a.has_map_state = bool(d.get("has_map_state",
				d.has("map_state") && int(d.get("map_state", -1)) >= 0));
		a.map_state = int(d.get("map_state", -1));
		a.has_height = bool(d.get("has_height",
				d.has("height") && int(d.get("height", 0)) != 0));
		a.height = int(d.get("height", 0));
		out.push_back(std::move(a));
	}
	return out;
}

Dictionary sound_to_dict(const mnu::Sound &s) {
	Dictionary d;
	d["state"] = to_gd(s.state);
	d["trigger"] = to_gd(s.trigger);
	d["file"] = to_gd(s.file);
	return d;
}

TypedArray<Dictionary> sounds_to_array(const std::vector<mnu::Sound> &sounds) {
	TypedArray<Dictionary> out;
	for (const mnu::Sound &s : sounds) {
		out.push_back(sound_to_dict(s));
	}
	return out;
}

template <typename T>
Dictionary scrollbar_to_dict(const T &s) {
	Dictionary d;
	d["present"] = s.present;
	d["position"] = position_to_dict(s.position);
	d["track"] = appearances_to_array(s.track);
	d["shuttle"] = appearances_to_array(s.shuttle);
	d["scrollup"] = appearances_to_array(s.scrollup);
	d["scrolldown"] = appearances_to_array(s.scrolldown);
	d["sounds"] = sounds_to_array(s.sounds);
	return d;
}

template <typename T>
void apply_scrollbar_patch(T &s, const Dictionary &d) {
	// Child edits author the container unless this same atomic patch explicitly
	// chooses its presence. Explicit false retains the latent child values.
	if (!d.has("present") && !d.is_empty()) s.present = true;
	if (d.has("present")) s.present = bool(d["present"]);
	if (d.has("position")) apply_position_patch(s.position, Dictionary(d["position"]));
	if (d.has("track")) s.track = appearances_from_array(TypedArray<Dictionary>(d["track"]));
	if (d.has("shuttle")) s.shuttle = appearances_from_array(TypedArray<Dictionary>(d["shuttle"]));
	if (d.has("scrollup")) s.scrollup = appearances_from_array(TypedArray<Dictionary>(d["scrollup"]));
	if (d.has("scrolldown")) s.scrolldown = appearances_from_array(TypedArray<Dictionary>(d["scrolldown"]));
	if (d.has("sounds")) {
		const TypedArray<Dictionary> rows = d["sounds"];
		std::vector<mnu::Sound> next;
		next.reserve(static_cast<size_t>(rows.size()));
		for (int i = 0; i < rows.size(); ++i) {
			const Dictionary row = rows[i];
			mnu::Sound sound;
			sound.state = to_std(String(row.get("state", "")));
			sound.trigger = to_std(String(row.get("trigger", "")));
			sound.file = to_std(String(row.get("file", "")));
			next.push_back(std::move(sound));
		}
		s.sounds = std::move(next);
	}
}

Dictionary spin_button_to_dict(const mnu::SpinButton &button) {
	Dictionary d;
	d["present"] = button.present;
	d["position"] = position_to_dict(button.position);
	d["appearances"] = appearances_to_array(button.appearances);
	return d;
}

void apply_spin_button_patch(mnu::SpinButton &button, const Dictionary &d) {
	if (!d.has("present") && !d.is_empty()) button.present = true;
	if (d.has("present")) button.present = bool(d["present"]);
	if (d.has("position")) apply_position_patch(button.position, Dictionary(d["position"]));
	if (d.has("appearances")) {
		button.appearances = appearances_from_array(TypedArray<Dictionary>(d["appearances"]));
	}
}

Dictionary authoring_row_schema(const String &path) {
	Dictionary d;
	if (path.ends_with("appearances") || path.ends_with(".track") ||
			path.ends_with(".shuttle") || path.ends_with(".scrollup") ||
			path.ends_with(".scrolldown")) {
		d["state"] = String();
		d["type"] = String();
		d["value"] = String();
		d["has_map_state"] = false;
		d["map_state"] = 0;
		d["has_height"] = false;
		d["height"] = 0;
	} else if (path.ends_with("hotkeys")) {
		d["value"] = String();
		d["virtual"] = false;
	} else if (path.ends_with("sounds")) {
		d["state"] = String();
		d["trigger"] = String();
		d["file"] = String();
	} else if (path.ends_with("actions")) {
		for (const char *key : { "type", "state", "file", "source", "field",
					"test", "target" }) {
			d[key] = String();
		}
		d["has_target_form"] = false;
		d["target_form"] = 0;
		d["external_browser"] = false;
		d["toggle"] = false;
	} else if (path.ends_with(".rows")) {
		d["type"] = String();
		d["value"] = String();
		d["text"] = String();
	} else if (path.ends_with("headers")) {
		d["has_column"] = false;
		d["column"] = 0;
		d["has_width"] = false;
		d["width"] = 0;
		for (const char *key : { "justify", "vjustify", "sort", "type", "text" }) {
			d[key] = String();
		}
	} else if (path.ends_with("bodies")) {
		d["has_column"] = false;
		d["column"] = 0;
		d["justify"] = String();
		d["vjustify"] = String();
		d["bitmap_draw"] = false;
		d["scale_bitmap"] = false;
		d["bitmap_flags"] = String();
		d["custom_draw"] = false;
	} else if (path.ends_with("substitutions")) {
		d["has_column"] = false;
		d["column"] = 0;
		d["value"] = String();
		d["is_file"] = false;
		d["file"] = String();
	}
	return d;
}

bool authoring_type_matches(const Variant &actual, const Variant &expected) {
	const Variant::Type want = expected.get_type();
	const Variant::Type got = actual.get_type();
	if (want == Variant::INT) {
		return got == Variant::INT || got == Variant::FLOAT;
	}
	if (want == Variant::STRING) {
		return got == Variant::STRING || got == Variant::STRING_NAME;
	}
	return got == want;
}

bool validate_authoring_dictionary(const Dictionary &actual,
		const Dictionary &schema, const String &path);

bool validate_authoring_array(const Variant &value, const Variant &schema_value,
		const String &path) {
	if (value.get_type() != Variant::ARRAY) {
		return false;
	}
	const Array rows = value;
	const Array schema_rows = schema_value;
	Dictionary row_schema;
	if (!schema_rows.is_empty() &&
			Variant(schema_rows[0]).get_type() == Variant::DICTIONARY) {
		row_schema = schema_rows[0];
	} else {
		row_schema = authoring_row_schema(path);
	}
	if (row_schema.is_empty() && !rows.is_empty()) {
		return false;
	}
	for (int i = 0; i < rows.size(); ++i) {
		const Variant row_value = rows[i];
			if (row_value.get_type() != Variant::DICTIONARY ||
					!validate_authoring_dictionary(Dictionary(row_value), row_schema,
							path + String("[") + String::num_int64(i) +
									String("]"))) {
			return false;
		}
	}
	return true;
}

bool validate_authoring_dictionary(const Dictionary &actual,
		const Dictionary &schema, const String &path) {
	const Array keys = actual.keys();
	for (int i = 0; i < keys.size(); ++i) {
		const Variant key = keys[i];
		if (key.get_type() != Variant::STRING &&
				key.get_type() != Variant::STRING_NAME) {
			return false;
		}
		if (!schema.has(key)) {
			return false;
		}
		const Variant value = actual[key];
		const Variant expected = schema[key];
		const String child_path = path + String(".") + String(key);
		if (expected.get_type() == Variant::DICTIONARY) {
			if (value.get_type() != Variant::DICTIONARY ||
					!validate_authoring_dictionary(Dictionary(value),
							Dictionary(expected), child_path)) {
				return false;
			}
		} else if (expected.get_type() == Variant::ARRAY) {
			if (!validate_authoring_array(value, expected, child_path)) {
				return false;
			}
		} else if (!authoring_type_matches(value, expected)) {
			return false;
		}
	}
	return true;
}

} // namespace

NovaMnuDocument::NovaMnuDocument() {
	rebuild_ids();
}

// --- Id tree ---

NovaMnuDocument::IdWindow NovaMnuDocument::make_id_window(const mnu::Window &w) {
	IdWindow node;
	node.id = next_id_++;
	node.children.reserve(w.children.size());
	for (const auto &child : w.children) {
		node.children.push_back(make_id_window(child));
	}
	return node;
}

void NovaMnuDocument::rebuild_ids() {
	ids_.clear();
	next_id_ = 1;
	ids_.reserve(doc_.screens.size());
	for (const auto &screen : doc_.screens) {
		IdScreen s;
		s.id = next_id_++;
		s.root = make_id_window(screen.root_window);
		ids_.push_back(std::move(s));
	}
}

bool NovaMnuDocument::find_in_id_window(const IdWindow &node, int id, std::vector<int> &path) {
	if (node.id == id) {
		return true; // path holds the chain to this node (empty == root window)
	}
	for (size_t i = 0; i < node.children.size(); ++i) {
		path.push_back(static_cast<int>(i));
		if (find_in_id_window(node.children[i], id, path)) {
			return true;
		}
		path.pop_back();
	}
	return false;
}

NovaMnuDocument::Locator NovaMnuDocument::locate(int id) const {
	Locator loc;
	for (size_t s = 0; s < ids_.size(); ++s) {
		if (ids_[s].id == id) {
			loc.screen_index = static_cast<int>(s);
			loc.is_screen = true;
			return loc;
		}
		std::vector<int> path;
		if (find_in_id_window(ids_[s].root, id, path)) {
			loc.screen_index = static_cast<int>(s);
			loc.path = std::move(path);
			loc.is_screen = false;
			return loc;
		}
	}
	return loc;
}

mnu::Window *NovaMnuDocument::window_at(const Locator &loc) {
	if (!loc.valid() || loc.is_screen) {
		return nullptr;
	}
	mnu::Window *w = &doc_.screens[loc.screen_index].root_window;
	for (int idx : loc.path) {
		w = &w->children[idx];
	}
	return w;
}

const mnu::Window *NovaMnuDocument::window_at(const Locator &loc) const {
	if (!loc.valid() || loc.is_screen) {
		return nullptr;
	}
	const mnu::Window *w = &doc_.screens[loc.screen_index].root_window;
	for (int idx : loc.path) {
		w = &w->children[idx];
	}
	return w;
}

NovaMnuDocument::IdWindow *NovaMnuDocument::id_window_at(const Locator &loc) {
	if (!loc.valid() || loc.is_screen) {
		return nullptr;
	}
	IdWindow *n = &ids_[loc.screen_index].root;
	for (int idx : loc.path) {
		n = &n->children[idx];
	}
	return n;
}

const char *NovaMnuDocument::state_for_slot(int slot) {
	switch (slot) {
		case TEX_DEFAULT:
			return "default";
		case TEX_MOUSEOVER:
			return "mouseover";
		case TEX_SELECTED:
			return "selected";
		case TEX_DISABLED:
			return "disabled";
		default:
			return "default";
	}
}

mnu::Appearance *NovaMnuDocument::find_appearance(mnu::Window &w, const char *state, bool create) {
	for (auto &app : w.appearances) {
		if (app.state == state) {
			return &app;
		}
	}
	if (!create) {
		return nullptr;
	}
	mnu::Appearance app;
	app.state = state;
	app.type = "image";
	w.appearances.push_back(app);
	return &w.appearances.back();
}

void NovaMnuDocument::touch() {
	emit_changed();
}

// --- I/O ---

namespace {
// Absolute (screen-space) right/bottom extent of a window subtree. MNU child
// POSITIONs are parent-relative (the builder nests child controls under the
// parent node), so absolute coords accumulate ancestor left/top; right/bottom
// live in the same frame as left/top.
void accumulate_extent(const mnu::Window &w, int sum_x, int sum_y, int &max_r,
		int &max_b) {
	const mnu::Position &p = w.position;
	const int r = sum_x + (p.has_right ? p.right : (p.has_left ? p.left : 0));
	const int b = sum_y + (p.has_bottom ? p.bottom : (p.has_top ? p.top : 0));
	if (r > max_r) {
		max_r = r;
	}
	if (b > max_b) {
		max_b = b;
	}
	const int origin_x = sum_x + (p.has_left ? p.left : 0);
	const int origin_y = sum_y + (p.has_top ? p.top : 0);
	for (const mnu::Window &c : w.children) {
		accumulate_extent(c, origin_x, origin_y, max_r, max_b);
	}
}
} // namespace

Error NovaMnuDocument::load_from_bytes(const PackedByteArray &p_bytes) {
	std::vector<uint8_t> bytes(static_cast<size_t>(p_bytes.size()));
	if (!bytes.empty()) {
		std::memcpy(bytes.data(), p_bytes.ptr(), bytes.size());
	}
	mnu::Document parsed;
	std::string error;
	if (!mnu::parse(bytes.data(), bytes.size(), parsed, error)) {
		UtilityFunctions::push_warning("NovaMnuDocument: parse failed: ", error.c_str());
		return ERR_FILE_CORRUPT;
	}
	doc_ = std::move(parsed);
	rebuild_ids();
	// Derive the document's content extent (max authored right/bottom) for the editor
	// and MCP info. NOTE: this is informational only now — both the runtime and the
	// editor canvas scale a fixed 800x600 design space anamorphically to the screen
	// [orig: CUIScene_SetScreenScale @ 0x639480], so neither uses menu_size_ for the
	// fit. Leaves the default untouched if no window carries a position.
	{
		int max_r = 0;
		int max_b = 0;
		for (const mnu::Screen &s : doc_.screens) {
			accumulate_extent(s.root_window, 0, 0, max_r, max_b);
		}
		if (max_r > 0 && max_b > 0) {
			menu_size_ = Vector2i(max_r, max_b);
		}
	}
	touch();
	return OK;
}

PackedByteArray NovaMnuDocument::to_byte_array() const {
	std::vector<uint8_t> bytes;
	std::string error;
	if (!mnu::serialize_bytes(doc_, bytes, error, true, 2)) {
		UtilityFunctions::push_warning(String("NovaMnuDocument::to_byte_array: ") + to_gd(error));
		return PackedByteArray();
	}
	PackedByteArray out;
	out.resize(static_cast<int64_t>(bytes.size()));
	if (!bytes.empty()) {
		std::memcpy(out.ptrw(), bytes.data(), bytes.size());
	}
	return out;
}

Error NovaMnuDocument::load_from_path(const String &p_path) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null()) {
		return ERR_FILE_CANT_OPEN;
	}
	const PackedByteArray packed = file->get_buffer(file->get_length());
	file->close();
	return load_from_bytes(packed);
}

Error NovaMnuDocument::save_to_path(const String &p_path) const {
	const PackedByteArray packed = to_byte_array();
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		return ERR_FILE_CANT_WRITE;
	}
	file->store_buffer(packed);
	file->close();
	return OK;
}

void NovaMnuDocument::create_empty() {
	doc_ = mnu::Document{};
	doc_.screens.push_back(make_default_screen("SCREEN"));
	menu_size_ = Vector2i(800, 600);
	rebuild_ids();
	touch();
}

void NovaMnuDocument::set_menu_size(const Vector2i &p_size) {
	menu_size_ = p_size;
}

// --- Tree read ---

int NovaMnuDocument::get_screen_count() const {
	return static_cast<int>(doc_.screens.size());
}

PackedInt32Array NovaMnuDocument::get_screen_ids() const {
	PackedInt32Array out;
	out.resize(static_cast<int64_t>(ids_.size()));
	for (size_t i = 0; i < ids_.size(); ++i) {
		out.set(static_cast<int64_t>(i), ids_[i].id);
	}
	return out;
}

int NovaMnuDocument::get_screen_root_id(int p_screen_id) const {
	for (const auto &s : ids_) {
		if (s.id == p_screen_id) {
			return s.root.id;
		}
	}
	return -1;
}

bool NovaMnuDocument::is_screen(int p_id) const {
	const Locator loc = locate(p_id);
	return loc.valid() && loc.is_screen;
}

bool NovaMnuDocument::widget_exists(int p_id) const {
	return locate(p_id).valid();
}

int NovaMnuDocument::get_parent_id(int p_id) const {
	const Locator loc = locate(p_id);
	if (!loc.valid() || loc.is_screen) {
		return -1; // screens (and unknown ids) have no parent
	}
	if (loc.path.empty()) {
		return ids_[loc.screen_index].id; // root window's parent is its screen
	}
	// Walk the id tree to the parent of the located node.
	const IdWindow *node = &ids_[loc.screen_index].root;
	for (size_t i = 0; i + 1 < loc.path.size(); ++i) {
		node = &node->children[loc.path[i]];
	}
	return node->id;
}

PackedInt32Array NovaMnuDocument::get_child_ids(int p_id) const {
	PackedInt32Array out;
	const Locator loc = locate(p_id);
	if (!loc.valid()) {
		return out;
	}
	if (loc.is_screen) {
		out.push_back(ids_[loc.screen_index].root.id); // a screen's child is its root window
		return out;
	}
	const IdWindow *node = &ids_[loc.screen_index].root;
	for (int idx : loc.path) {
		node = &node->children[idx];
	}
	for (const auto &child : node->children) {
		out.push_back(child.id);
	}
	return out;
}

int NovaMnuDocument::get_widget_type(int p_id) const {
	const Locator loc = locate(p_id);
	if (!loc.valid() || loc.is_screen) {
		return -1;
	}
	return static_cast<int>(window_at(loc)->type);
}

String NovaMnuDocument::get_widget_type_name(int p_type) const {
	return to_gd(mnu::window_type_name(static_cast<mnu::WindowType>(p_type)));
}

String NovaMnuDocument::get_widget_name(int p_id) const {
	const Locator loc = locate(p_id);
	if (loc.is_screen) {
		return to_gd(doc_.screens[loc.screen_index].name);
	}
	const mnu::Window *w = window_at(loc);
	return w ? to_gd(w->name) : String();
}

Rect2 NovaMnuDocument::get_window_rect(int p_id) const {
	const Locator loc = locate(p_id);
	const mnu::Window *w = window_at(loc);
	if (!w) {
		return Rect2();
	}
	const mnu::Position &p = w->position;
	const float x = p.has_left ? static_cast<float>(p.left) : 0.0f;
	const float y = p.has_top ? static_cast<float>(p.top) : 0.0f;
	const float wd = (p.has_left && p.has_right) ? static_cast<float>(p.right - p.left) : 0.0f;
	const float ht = (p.has_top && p.has_bottom) ? static_cast<float>(p.bottom - p.top) : 0.0f;
	return Rect2(x, y, wd, ht);
}

// --- Screen properties ---

String NovaMnuDocument::get_screen_name(int p_screen_id) const {
	const Locator loc = locate(p_screen_id);
	if (!loc.valid() || !loc.is_screen) {
		return String();
	}
	return to_gd(doc_.screens[loc.screen_index].name);
}

bool NovaMnuDocument::get_screen_has_music_var(int p_screen_id) const {
	const Locator loc = locate(p_screen_id);
	return loc.valid() && loc.is_screen &&
			doc_.screens[loc.screen_index].has_music_var;
}

int NovaMnuDocument::get_screen_music_var(int p_screen_id) const {
	const Locator loc = locate(p_screen_id);
	if (!loc.valid() || !loc.is_screen) {
		return 0;
	}
	return doc_.screens[loc.screen_index].music_var;
}

String NovaMnuDocument::get_screen_text_rsrc(int p_screen_id) const {
	const Locator loc = locate(p_screen_id);
	if (!loc.valid() || !loc.is_screen) {
		return String();
	}
	return to_gd(doc_.screens[loc.screen_index].text_rsrc);
}

String NovaMnuDocument::get_screen_cursor_file(int p_screen_id) const {
	const Locator loc = locate(p_screen_id);
	if (!loc.valid() || !loc.is_screen) {
		return String();
	}
	return to_gd(doc_.screens[loc.screen_index].cursor_file);
}

String NovaMnuDocument::get_screen_cursor_flags(int p_screen_id) const {
	const Locator loc = locate(p_screen_id);
	if (!loc.valid() || !loc.is_screen) {
		return String();
	}
	return to_gd(doc_.screens[loc.screen_index].cursor_flags);
}

void NovaMnuDocument::set_screen_property(int p_screen_id, const String &p_key, const Variant &p_value) {
	const Locator loc = locate(p_screen_id);
	if (!loc.valid() || !loc.is_screen) {
		return;
	}
	mnu::Screen &s = doc_.screens[loc.screen_index];
	const String key = p_key.to_lower();
	if (key == "name") {
		s.name = to_std(p_value);
	} else if (key == "music_var") {
		s.music_var = static_cast<int>(p_value);
		s.has_music_var = true;
	} else if (key == "has_music_var") {
		s.has_music_var = static_cast<bool>(p_value);
	} else if (key == "text_rsrc") {
		s.text_rsrc = to_std(p_value);
	} else if (key == "cursor_file") {
		s.cursor_file = to_std(p_value);
	} else if (key == "cursor_flags") {
		s.cursor_flags = to_std(p_value);
	} else {
		return;
	}
	touch();
}

// --- Widget property read/write ---

void NovaMnuDocument::set_widget_name(int p_id, const String &p_name) {
	const Locator loc = locate(p_id);
	if (loc.is_screen) {
		doc_.screens[loc.screen_index].name = to_std(p_name);
		touch();
		return;
	}
	mnu::Window *w = window_at(loc);
	if (!w) {
		return;
	}
	w->name = to_std(p_name);
	touch();
}

void NovaMnuDocument::set_window_rect(int p_id, const Rect2 &p_rect) {
	mnu::Window *w = window_at(locate(p_id));
	if (!w) {
		return;
	}
	// A negative extent means "auto-size": the writer then omits RIGHT/BOTTOM,
	// which is how shipped menus spell art-sized toggles and auto-height labels
	// (the original engine stretches appearance art across an explicit width).
	const bool auto_width = p_rect.size.x < 0.0f;
	const bool auto_height = p_rect.size.y < 0.0f;
	mnu::Position &p = w->position;
	p.left = static_cast<int>(p_rect.position.x);
	p.top = static_cast<int>(p_rect.position.y);
	p.right = auto_width ? 0 : static_cast<int>(p_rect.position.x + p_rect.size.x);
	p.bottom = auto_height ? 0 : static_cast<int>(p_rect.position.y + p_rect.size.y);
	p.has_left = p.has_top = true;
	p.has_right = !auto_width;
	p.has_bottom = !auto_height;
	touch();
}

int NovaMnuDocument::get_window_rect_flags(int p_id) const {
	const mnu::Window *w = window_at(locate(p_id));
	if (!w) {
		return 0;
	}
	const mnu::Position &p = w->position;
	return (p.has_left ? RECT_HAS_LEFT : 0) | (p.has_top ? RECT_HAS_TOP : 0) |
			(p.has_right ? RECT_HAS_RIGHT : 0) | (p.has_bottom ? RECT_HAS_BOTTOM : 0);
}

String NovaMnuDocument::get_widget_text(int p_id) const {
	const mnu::Window *w = window_at(locate(p_id));
	return w ? to_gd(w->string_data.value) : String();
}

void NovaMnuDocument::set_widget_text(int p_id, const String &p_text) {
	mnu::Window *w = window_at(locate(p_id));
	if (!w) {
		return;
	}
	w->string_data.present = true;
	w->string_data.value = to_std(p_text);
	touch();
}

String NovaMnuDocument::get_widget_string_type(int p_id) const {
	const mnu::Window *w = window_at(locate(p_id));
	return w ? to_gd(w->string_data.type) : String();
}

void NovaMnuDocument::set_widget_string_type(int p_id, const String &p_type) {
	mnu::Window *w = window_at(locate(p_id));
	if (!w) {
		return;
	}
	w->string_data.present = true;
	w->string_data.type = to_std(p_type);
	touch();
}

String NovaMnuDocument::get_widget_font(int p_id) const {
	const mnu::Window *w = window_at(locate(p_id));
	return w ? to_gd(w->font.name) : String();
}

void NovaMnuDocument::set_widget_font(int p_id, const String &p_font) {
	mnu::Window *w = window_at(locate(p_id));
	if (!w) {
		return;
	}
	w->font.name = to_std(p_font);
	touch();
}

String NovaMnuDocument::get_widget_datasource(int p_id) const {
	const mnu::Window *w = window_at(locate(p_id));
	return w ? to_gd(w->datasource) : String();
}

void NovaMnuDocument::set_widget_datasource(int p_id, const String &p_value) {
	mnu::Window *w = window_at(locate(p_id));
	if (!w) {
		return;
	}
	w->datasource = to_std(p_value);
	touch();
}

String NovaMnuDocument::get_widget_orientation(int p_id) const {
	const mnu::Window *w = window_at(locate(p_id));
	return w ? to_gd(w->orientation) : String();
}

void NovaMnuDocument::set_widget_orientation(int p_id, const String &p_value) {
	mnu::Window *w = window_at(locate(p_id));
	if (!w) {
		return;
	}
	w->orientation = to_std(p_value);
	touch();
}

Dictionary NovaMnuDocument::get_widget_authoring_state(int p_id) const {
	Dictionary out;
	const mnu::Window *w = window_at(locate(p_id));
	if (!w) {
		return out;
	}
	out["id"] = p_id;
	out["type"] = static_cast<int>(w->type);
	out["type_token"] = to_gd(w->type_token);
	out["name"] = to_gd(w->name);
	out["text_rsrc"] = to_gd(w->text_rsrc);
	out["position"] = position_to_dict(w->position);
	out["flags"] = get_widget_flags(p_id);

	Dictionary string_data;
	string_data["present"] = w->string_data.present;
	string_data["type"] = to_gd(w->string_data.type);
	string_data["justify"] = to_gd(w->string_data.justify);
	string_data["vjustify"] = to_gd(w->string_data.vjustify);
	string_data["has_edge"] = w->string_data.has_edge;
	string_data["edge"] = w->string_data.edge;
	string_data["value"] = to_gd(w->string_data.value);
	out["string"] = string_data;

	Dictionary font;
	font["name"] = to_gd(w->font.name);
	font["default_fg"] = to_gd(w->font.default_fg);
	font["default_bg"] = to_gd(w->font.default_bg);
	font["mouseover_fg"] = to_gd(w->font.mouseover_fg);
	font["mouseover_bg"] = to_gd(w->font.mouseover_bg);
	font["selected_fg"] = to_gd(w->font.selected_fg);
	font["selected_bg"] = to_gd(w->font.selected_bg);
	font["disabled_fg"] = to_gd(w->font.disabled_fg);
	font["disabled_bg"] = to_gd(w->font.disabled_bg);
	out["font"] = font;

	Dictionary constraints;
	constraints["number"] = w->number;
	constraints["has_minval"] = w->has_minval;
	constraints["minval"] = w->minval;
	constraints["has_maxval"] = w->has_maxval;
	constraints["maxval"] = w->maxval;
	constraints["has_maxchar"] = w->has_maxchar;
	constraints["maxchar"] = w->maxchar;
	out["constraints"] = constraints;

	Dictionary behavior;
	behavior["as_button"] = w->as_button;
	behavior["has_group"] = w->has_group;
	behavior["group"] = w->group;
	behavior["has_form"] = w->has_form;
	behavior["form"] = w->form;
	behavior["global_var"] = w->global_var;
	behavior["password"] = w->password;
	behavior["datasource"] = to_gd(w->datasource);
	behavior["orientation"] = to_gd(w->orientation);
	out["behavior"] = behavior;

	Dictionary scroll_size;
	scroll_size["has_height"] = w->has_scroll_height;
	scroll_size["height"] = w->scroll_height;
	scroll_size["has_width"] = w->has_scroll_width;
	scroll_size["width"] = w->scroll_width;
	out["scroll_size"] = scroll_size;

	Dictionary cursor;
	cursor["file"] = to_gd(w->cursor.file);
	cursor["flags"] = to_gd(w->cursor.flags);
	out["cursor"] = cursor;

	TypedArray<Dictionary> hotkeys;
	for (const mnu::Hotkey &hotkey : w->hotkeys) {
		Dictionary row;
		row["value"] = to_gd(hotkey.value);
		row["virtual"] = hotkey.virtual_key;
		hotkeys.push_back(row);
	}
	out["hotkeys"] = hotkeys;

	Dictionary items;
	items["present"] = w->items.present;
	items["multiselect"] = w->items.multiselect;
	items["justify"] = to_gd(w->items.justify);
	items["vjustify"] = to_gd(w->items.vjustify);
	items["appearances"] = appearances_to_array(w->items.appearances);
	items["selection_color"] = to_gd(w->items.selection_color);
	TypedArray<Dictionary> item_rows;
	for (const mnu::Item &item : w->items.items) {
		item_rows.push_back(item_to_dict(item));
	}
	items["rows"] = item_rows;
	out["items"] = items;

	Dictionary list_box;
	list_box["present"] = w->list_box.present;
	list_box["position"] = position_to_dict(w->list_box.position);
	list_box["appearances"] = appearances_to_array(w->list_box.appearances);
	Dictionary list_string;
	list_string["present"] = w->list_box.string_data.present;
	list_string["type"] = to_gd(w->list_box.string_data.type);
	list_string["justify"] = to_gd(w->list_box.string_data.justify);
	list_string["vjustify"] = to_gd(w->list_box.string_data.vjustify);
	list_string["has_edge"] = w->list_box.string_data.has_edge;
	list_string["edge"] = w->list_box.string_data.edge;
	list_string["value"] = to_gd(w->list_box.string_data.value);
	list_box["string"] = list_string;
	Dictionary list_items;
	list_items["present"] = w->list_box.items.present;
	list_items["multiselect"] = w->list_box.items.multiselect;
	list_items["justify"] = to_gd(w->list_box.items.justify);
	list_items["vjustify"] = to_gd(w->list_box.items.vjustify);
	list_items["appearances"] = appearances_to_array(w->list_box.items.appearances);
	list_items["selection_color"] = to_gd(w->list_box.items.selection_color);
	TypedArray<Dictionary> list_item_rows;
	for (const mnu::Item &item : w->list_box.items.items) {
		list_item_rows.push_back(item_to_dict(item));
	}
	list_items["rows"] = list_item_rows;
	list_box["items"] = list_items;
	list_box["has_min_item_height"] = w->list_box.has_min_item_height;
	list_box["min_item_height"] = w->list_box.min_item_height;
	list_box["has_sb_edge_pad"] = w->list_box.has_sb_edge_pad;
	list_box["sb_edge_pad"] = w->list_box.sb_edge_pad;
	list_box["scrollbar"] = scrollbar_to_dict(w->list_box.scrollbar);
	out["list_box"] = list_box;

	out["spinup"] = spin_button_to_dict(w->spinup);
	out["spindown"] = spin_button_to_dict(w->spindown);
	Dictionary scroll_parts;
	scroll_parts["shuttle"] = appearances_to_array(w->shuttle);
	scroll_parts["scrollup"] = appearances_to_array(w->scrollup);
	scroll_parts["scrolldown"] = appearances_to_array(w->scrolldown);
	out["scroll_parts"] = scroll_parts;

	Dictionary table;
	table["has_count"] = w->table_data.column.has_count;
	table["count"] = w->table_data.column.count;
	table["has_spacing"] = w->table_data.column.has_spacing;
	table["spacing"] = w->table_data.column.spacing;
	table["headers"] = get_table_headers(p_id);
	table["bodies"] = get_table_bodies(p_id);
	table["substitutions"] = get_table_substs(p_id);
	table["has_min_item_height"] = w->table_data.has_min_item_height;
	table["min_item_height"] = w->table_data.min_item_height;
	table["outline_color"] = to_gd(w->table_data.outline_color);
	table["selection_color"] = to_gd(w->table_data.selection_color);
	table["multiselect"] = w->table_data.multiselect;
	table["scrollbar"] = scrollbar_to_dict(w->table_data.scrollbar);
	out["table"] = table;
	// Direct SCROLLBAR state is shared by LIST, MULTI, MULTILINE_EDIT, and
	// other scrollable widget classes. Table keeps the historical grouped view
	// above as well; both dictionaries describe the same native structure.
	out["scrollbar"] = scrollbar_to_dict(w->table_data.scrollbar);

	out["appearances"] = appearances_to_array(w->appearances);
	out["sounds"] = get_widget_sounds(p_id);
	out["actions"] = get_widget_actions(p_id);
	out["frame"] = get_window_frame(p_id);
	return out;
}

bool NovaMnuDocument::apply_widget_patch(int p_id, const Dictionary &p_patch) {
	mnu::Window *w = window_at(locate(p_id));
	if (!w || p_patch.is_empty()) {
		return false;
	}
	// Keep the bridge honest for direct callers as well as the editor/MCP:
	// ignored keys and idempotent assignments are not edits and must not dirty
	// the document or create an undo snapshot.
	const Dictionary before_state =
			get_widget_authoring_state(p_id).duplicate(true);
	if (!validate_authoring_dictionary(p_patch, before_state, "authoring")) {
		return false;
	}
	if (p_patch.has("type_token")) {
		w->type_token = to_std(String(p_patch["type_token"]));
		// A non-empty raw token is also the authoritative runtime type. Keep
		// document structure, inspector sections, and serialization consistent
		// immediately instead of waiting for a save/reload to reparse it.
		if (!w->type_token.empty()) {
			w->type = mnu::parse_window_type(w->type_token);
		}
	}
	if (p_patch.has("name")) w->name = to_std(String(p_patch["name"]));
	if (p_patch.has("text_rsrc")) w->text_rsrc = to_std(String(p_patch["text_rsrc"]));
	if (p_patch.has("position")) apply_position_patch(w->position, Dictionary(p_patch["position"]));
	if (p_patch.has("flags")) {
		const int flags = int(p_patch["flags"]);
		w->hidden = (flags & FLAG_HIDDEN) != 0;
		w->disabled = (flags & FLAG_DISABLED) != 0;
		w->checked = (flags & FLAG_CHECKED) != 0;
		w->draw_frame = (flags & FLAG_DRAW_FRAME) != 0;
		w->modal = (flags & FLAG_MODAL) != 0;
		w->readonly = (flags & FLAG_READONLY) != 0;
	}
	if (p_patch.has("string")) {
		const Dictionary d = p_patch["string"];
		if (!d.has("present") && !d.is_empty()) w->string_data.present = true;
		if (d.has("present")) w->string_data.present = bool(d["present"]);
		if (d.has("type")) w->string_data.type = to_std(String(d["type"]));
		if (d.has("justify")) w->string_data.justify = to_std(String(d["justify"]));
		if (d.has("vjustify")) w->string_data.vjustify = to_std(String(d["vjustify"]));
		if (d.has("has_edge")) w->string_data.has_edge = bool(d["has_edge"]);
		if (d.has("edge")) w->string_data.edge = int(d["edge"]);
		if (d.has("value")) w->string_data.value = to_std(String(d["value"]));
	}
	if (p_patch.has("font")) {
		const Dictionary d = p_patch["font"];
		if (d.has("name")) w->font.name = to_std(String(d["name"]));
		if (d.has("default_fg")) w->font.default_fg = to_std(String(d["default_fg"]));
		if (d.has("default_bg")) w->font.default_bg = to_std(String(d["default_bg"]));
		if (d.has("mouseover_fg")) w->font.mouseover_fg = to_std(String(d["mouseover_fg"]));
		if (d.has("mouseover_bg")) w->font.mouseover_bg = to_std(String(d["mouseover_bg"]));
		if (d.has("selected_fg")) w->font.selected_fg = to_std(String(d["selected_fg"]));
		if (d.has("selected_bg")) w->font.selected_bg = to_std(String(d["selected_bg"]));
		if (d.has("disabled_fg")) w->font.disabled_fg = to_std(String(d["disabled_fg"]));
		if (d.has("disabled_bg")) w->font.disabled_bg = to_std(String(d["disabled_bg"]));
	}
	if (p_patch.has("constraints")) {
		const Dictionary d = p_patch["constraints"];
		if (d.has("number")) w->number = bool(d["number"]);
		if (d.has("has_minval")) w->has_minval = bool(d["has_minval"]);
		if (d.has("minval")) w->minval = int(d["minval"]);
		if (d.has("has_maxval")) w->has_maxval = bool(d["has_maxval"]);
		if (d.has("maxval")) w->maxval = int(d["maxval"]);
		if (d.has("has_maxchar")) w->has_maxchar = bool(d["has_maxchar"]);
		if (d.has("maxchar")) w->maxchar = int(d["maxchar"]);
	}
	if (p_patch.has("behavior")) {
		const Dictionary d = p_patch["behavior"];
		if (d.has("as_button")) w->as_button = bool(d["as_button"]);
		if (d.has("has_group")) w->has_group = bool(d["has_group"]);
		if (d.has("group")) w->group = int(d["group"]);
		if (d.has("has_form")) w->has_form = bool(d["has_form"]);
		if (d.has("form")) w->form = int(d["form"]);
		if (d.has("global_var")) w->global_var = bool(d["global_var"]);
		if (d.has("password")) w->password = bool(d["password"]);
		if (d.has("datasource")) w->datasource = to_std(String(d["datasource"]));
		if (d.has("orientation")) w->orientation = to_std(String(d["orientation"]));
	}
	if (p_patch.has("scroll_size")) {
		const Dictionary d = p_patch["scroll_size"];
		if (d.has("has_height")) w->has_scroll_height = bool(d["has_height"]);
		if (d.has("height")) w->scroll_height = int(d["height"]);
		if (d.has("has_width")) w->has_scroll_width = bool(d["has_width"]);
		if (d.has("width")) w->scroll_width = int(d["width"]);
	}
	if (p_patch.has("cursor")) {
		const Dictionary d = p_patch["cursor"];
		if (d.has("file")) w->cursor.file = to_std(String(d["file"]));
		if (d.has("flags")) w->cursor.flags = to_std(String(d["flags"]));
	}
	if (p_patch.has("hotkeys")) {
		const TypedArray<Dictionary> rows = p_patch["hotkeys"];
		std::vector<mnu::Hotkey> hotkeys;
		hotkeys.reserve(static_cast<size_t>(rows.size()));
		for (int i = 0; i < rows.size(); ++i) {
			const Dictionary d = rows[i];
			mnu::Hotkey hotkey;
			hotkey.value = to_std(String(d.get("value", "")));
			hotkey.virtual_key = bool(d.get("virtual", false));
			hotkeys.push_back(std::move(hotkey));
		}
		w->hotkeys = std::move(hotkeys);
	}
	if (p_patch.has("items")) {
		const Dictionary d = p_patch["items"];
		if (!d.has("present") && !d.is_empty()) w->items.present = true;
		if (d.has("present")) w->items.present = bool(d["present"]);
		if (d.has("multiselect")) w->items.multiselect = bool(d["multiselect"]);
		if (d.has("justify")) w->items.justify = to_std(String(d["justify"]));
		if (d.has("vjustify")) w->items.vjustify = to_std(String(d["vjustify"]));
		const bool appearances_changed = d.has("appearances");
		const bool selection_changed = d.has("selection_color");
		if (appearances_changed) {
			w->items.appearances = appearances_from_array(TypedArray<Dictionary>(d["appearances"]));
			sync_items_selection_alias(w->items);
		}
		if (selection_changed) {
			set_items_selection_alias(w->items,
					to_std(String(d["selection_color"])));
		}
		if (d.has("rows")) {
			const TypedArray<Dictionary> rows = d["rows"];
			std::vector<mnu::Item> items;
			items.reserve(static_cast<size_t>(rows.size()));
			for (int i = 0; i < rows.size(); ++i) items.push_back(item_from_dict(rows[i]));
			w->items.items = std::move(items);
		}
		if (is_table_window(*w)) {
			if (appearances_changed) {
				sync_table_aliases_from_items(*w);
			}
			if (selection_changed) {
				w->table_data.selection_color = w->items.selection_color;
			}
		}
	}
	if (p_patch.has("list_box")) {
		const Dictionary d = p_patch["list_box"];
		if (!d.has("present") && !d.is_empty()) w->list_box.present = true;
		if (d.has("present")) w->list_box.present = bool(d["present"]);
		if (d.has("position")) apply_position_patch(w->list_box.position, Dictionary(d["position"]));
		if (d.has("appearances")) w->list_box.appearances = appearances_from_array(TypedArray<Dictionary>(d["appearances"]));
		if (d.has("string")) {
			const Dictionary sd = d["string"];
			if (!sd.has("present") && !sd.is_empty()) w->list_box.string_data.present = true;
			if (sd.has("present")) w->list_box.string_data.present = bool(sd["present"]);
			if (sd.has("type")) w->list_box.string_data.type = to_std(String(sd["type"]));
			if (sd.has("justify")) w->list_box.string_data.justify = to_std(String(sd["justify"]));
			if (sd.has("vjustify")) w->list_box.string_data.vjustify = to_std(String(sd["vjustify"]));
			if (sd.has("has_edge")) w->list_box.string_data.has_edge = bool(sd["has_edge"]);
			if (sd.has("edge")) w->list_box.string_data.edge = int(sd["edge"]);
			if (sd.has("value")) w->list_box.string_data.value = to_std(String(sd["value"]));
		}
		if (d.has("items")) {
			const Dictionary items = d["items"];
			if (!items.has("present") && !items.is_empty()) w->list_box.items.present = true;
			if (items.has("present")) w->list_box.items.present = bool(items["present"]);
			if (items.has("multiselect")) w->list_box.items.multiselect = bool(items["multiselect"]);
			if (items.has("justify")) w->list_box.items.justify = to_std(String(items["justify"]));
			if (items.has("vjustify")) w->list_box.items.vjustify = to_std(String(items["vjustify"]));
			if (items.has("appearances")) {
				w->list_box.items.appearances =
						appearances_from_array(TypedArray<Dictionary>(items["appearances"]));
				sync_items_selection_alias(w->list_box.items);
			}
			if (items.has("selection_color")) {
				set_items_selection_alias(w->list_box.items,
						to_std(String(items["selection_color"])));
			}
			if (items.has("rows")) {
				const TypedArray<Dictionary> rows = items["rows"];
				std::vector<mnu::Item> next;
				next.reserve(static_cast<size_t>(rows.size()));
				for (int i = 0; i < rows.size(); ++i) next.push_back(item_from_dict(rows[i]));
				w->list_box.items.items = std::move(next);
			}
		}
		if (d.has("has_min_item_height")) w->list_box.has_min_item_height = bool(d["has_min_item_height"]);
		if (d.has("min_item_height")) w->list_box.min_item_height = int(d["min_item_height"]);
		if (d.has("has_sb_edge_pad")) w->list_box.has_sb_edge_pad = bool(d["has_sb_edge_pad"]);
		if (d.has("sb_edge_pad")) w->list_box.sb_edge_pad = int(d["sb_edge_pad"]);
		if (d.has("scrollbar")) apply_scrollbar_patch(w->list_box.scrollbar, Dictionary(d["scrollbar"]));
	}
	if (p_patch.has("spinup")) apply_spin_button_patch(w->spinup, Dictionary(p_patch["spinup"]));
	if (p_patch.has("spindown")) apply_spin_button_patch(w->spindown, Dictionary(p_patch["spindown"]));
	if (p_patch.has("scroll_parts")) {
		const Dictionary d = p_patch["scroll_parts"];
		if (d.has("shuttle")) w->shuttle = appearances_from_array(TypedArray<Dictionary>(d["shuttle"]));
		if (d.has("scrollup")) w->scrollup = appearances_from_array(TypedArray<Dictionary>(d["scrollup"]));
		if (d.has("scrolldown")) w->scrolldown = appearances_from_array(TypedArray<Dictionary>(d["scrolldown"]));
	}
	if (p_patch.has("table")) {
		const Dictionary d = p_patch["table"];
		if (d.has("has_count")) w->table_data.column.has_count = bool(d["has_count"]);
		if (d.has("count")) w->table_data.column.count = int(d["count"]);
		if (d.has("has_spacing")) w->table_data.column.has_spacing = bool(d["has_spacing"]);
		if (d.has("spacing")) w->table_data.column.spacing = int(d["spacing"]);
		if (d.has("headers")) {
			const TypedArray<Dictionary> rows = d["headers"];
			w->table_data.column.headers.clear();
			for (int i = 0; i < rows.size(); ++i) w->table_data.column.headers.push_back(header_from_dict(rows[i]));
		}
		if (d.has("bodies")) {
			const TypedArray<Dictionary> rows = d["bodies"];
			w->table_data.column.bodies.clear();
			for (int i = 0; i < rows.size(); ++i) w->table_data.column.bodies.push_back(body_from_dict(rows[i]));
		}
		if (d.has("substitutions")) {
			const TypedArray<Dictionary> rows = d["substitutions"];
			w->table_data.column.substitutions.clear();
			for (int i = 0; i < rows.size(); ++i) w->table_data.column.substitutions.push_back(subst_from_dict(rows[i]));
		}
		if (d.has("has_min_item_height")) w->table_data.has_min_item_height = bool(d["has_min_item_height"]);
		if (d.has("min_item_height")) w->table_data.min_item_height = int(d["min_item_height"]);
		if (d.has("outline_color")) {
			w->table_data.outline_color = to_std(String(d["outline_color"]));
			set_table_item_alias(*w, "default", "outline",
					w->table_data.outline_color);
		}
		if (d.has("selection_color")) {
			w->table_data.selection_color = to_std(String(d["selection_color"]));
			set_table_item_alias(*w, "selected", "color",
					w->table_data.selection_color);
		}
		if (d.has("multiselect")) w->table_data.multiselect = bool(d["multiselect"]);
		if (d.has("scrollbar")) apply_scrollbar_patch(w->table_data.scrollbar, Dictionary(d["scrollbar"]));
	}
	// Table alias edits above may have authored ITEMS. An explicit presence
	// choice in the same patch is authoritative and wins last.
	if (p_patch.has("items")) {
		const Dictionary d = p_patch["items"];
		if (d.has("present")) w->items.present = bool(d["present"]);
	}
	if (p_patch.has("scrollbar")) {
		apply_scrollbar_patch(w->table_data.scrollbar, Dictionary(p_patch["scrollbar"]));
	}
	if (p_patch.has("appearances")) w->appearances = appearances_from_array(TypedArray<Dictionary>(p_patch["appearances"]));
	if (p_patch.has("frame")) {
		const Dictionary d = p_patch["frame"];
		if (d.has("stencil")) w->frame.stencil = to_std(String(d["stencil"]));
		if (d.has("has_stencil_size")) w->frame.has_stencil_size = bool(d["has_stencil_size"]);
		if (d.has("stencil_size")) w->frame.stencil_size = int(d["stencil_size"]);
		if (d.has("brush")) w->frame.brush = to_std(String(d["brush"]));
		if (d.has("monogram")) w->frame.monogram = to_std(String(d["monogram"]));
		if (d.has("has_insetx")) w->frame.has_insetx = bool(d["has_insetx"]);
		if (d.has("insetx")) w->frame.insetx = int(d["insetx"]);
		if (d.has("has_insety")) w->frame.has_insety = bool(d["has_insety"]);
		if (d.has("insety")) w->frame.insety = int(d["insety"]);
	}
	if (p_patch.has("sounds")) {
		const TypedArray<Dictionary> rows = p_patch["sounds"];
		std::vector<mnu::Sound> next;
		for (int i = 0; i < rows.size(); ++i) {
			const Dictionary d = rows[i];
			mnu::Sound sound;
			sound.state = to_std(String(d.get("state", "")));
			sound.trigger = to_std(String(d.get("trigger", "")));
			sound.file = to_std(String(d.get("file", "")));
			next.push_back(std::move(sound));
		}
		w->sounds = std::move(next);
	}
	if (p_patch.has("actions")) {
		const TypedArray<Dictionary> rows = p_patch["actions"];
		std::vector<mnu::Action> next;
		for (int i = 0; i < rows.size(); ++i) {
			const Dictionary d = rows[i];
			mnu::Action action;
			action.type = to_std(String(d.get("type", "")));
			action.state = to_std(String(d.get("state", "")));
			action.file = to_std(String(d.get("file", "")));
			action.source = to_std(String(d.get("source", "")));
			action.field = to_std(String(d.get("field", "")));
			action.test = to_std(String(d.get("test", "")));
			action.target = to_std(String(d.get("target", "")));
			action.has_target_form = bool(d.get("has_target_form", false));
			action.target_form = int(d.get("target_form", 0));
			action.external_browser = bool(d.get("external_browser", false));
			action.toggle = bool(d.get("toggle", false));
			next.push_back(std::move(action));
		}
		w->actions = std::move(next);
	}
	const Dictionary after_state = get_widget_authoring_state(p_id);
	if (before_state.recursive_equal(after_state, 0)) {
		return false;
	}
	touch();
	return true;
}

TypedArray<Dictionary> NovaMnuDocument::get_widget_sounds(int p_id) const {
	TypedArray<Dictionary> out;
	const mnu::Window *w = window_at(locate(p_id));
	if (!w) {
		return out;
	}
	for (const mnu::Sound &s : w->sounds) {
		Dictionary d;
		d["state"] = to_gd(s.state);
		d["trigger"] = to_gd(s.trigger);
		d["file"] = to_gd(s.file);
		out.push_back(d);
	}
	return out;
}

void NovaMnuDocument::set_widget_sounds(int p_id, const TypedArray<Dictionary> &p_sounds) {
	mnu::Window *w = window_at(locate(p_id));
	if (!w) {
		return;
	}
	std::vector<mnu::Sound> next;
	next.reserve(static_cast<size_t>(p_sounds.size()));
	for (int i = 0; i < p_sounds.size(); ++i) {
		const Dictionary d = p_sounds[i];
		mnu::Sound s;
		s.state = to_std(String(d.get("state", "")));
		s.trigger = to_std(String(d.get("trigger", "")));
		s.file = to_std(String(d.get("file", "")));
		next.push_back(s);
	}
	w->sounds = next;
	touch();
}

TypedArray<Dictionary> NovaMnuDocument::get_widget_appearances(int p_id) const {
	TypedArray<Dictionary> out;
	const mnu::Window *w = window_at(locate(p_id));
	if (!w) {
		return out;
	}
	for (const mnu::Appearance &a : w->appearances) {
		out.push_back(appearance_to_dict(a));
	}
	return out;
}

void NovaMnuDocument::set_widget_appearances(int p_id, const TypedArray<Dictionary> &p_rows) {
	mnu::Window *w = window_at(locate(p_id));
	if (!w) {
		return;
	}
	std::vector<mnu::Appearance> next;
	next.reserve(static_cast<size_t>(p_rows.size()));
	for (int i = 0; i < p_rows.size(); ++i) {
		const Dictionary d = p_rows[i];
		mnu::Appearance a;
		a.state = to_std(String(d.get("state", "")));
		a.type = to_std(String(d.get("type", "")));
		a.value = to_std(String(d.get("value", "")));
		a.has_map_state = static_cast<bool>(d.get("has_map_state",
				d.has("map_state") && int(d.get("map_state", -1)) >= 0));
		a.map_state = int(d.get("map_state", -1));
		a.has_height = static_cast<bool>(d.get("has_height",
				d.has("height") && int(d.get("height", 0)) != 0));
		a.height = int(d.get("height", 0));
		next.push_back(a);
	}
	w->appearances = next;
	touch();
}

Dictionary NovaMnuDocument::get_window_frame(int p_id) const {
	Dictionary out;
	const mnu::Window *w = window_at(locate(p_id));
	if (!w) {
		return out;
	}
	out["stencil"] = to_gd(w->frame.stencil);
	out["has_stencil_size"] = w->frame.has_stencil_size;
	out["stencil_size"] = w->frame.stencil_size;
	out["brush"] = to_gd(w->frame.brush);
	out["monogram"] = to_gd(w->frame.monogram);
	out["has_insetx"] = w->frame.has_insetx;
	out["insetx"] = w->frame.insetx;
	out["has_insety"] = w->frame.has_insety;
	out["insety"] = w->frame.insety;
	return out;
}

void NovaMnuDocument::set_window_frame(int p_id, const Dictionary &p_frame) {
	mnu::Window *w = window_at(locate(p_id));
	if (!w) {
		return;
	}
	w->frame.stencil = to_std(String(p_frame.get("stencil", "")));
	w->frame.has_stencil_size = bool(p_frame.get("has_stencil_size",
			p_frame.has("stencil_size") ? true : w->frame.has_stencil_size));
	w->frame.stencil_size = int(p_frame.get("stencil_size", 0));
	w->frame.brush = to_std(String(p_frame.get("brush", "")));
	w->frame.monogram = to_std(String(p_frame.get("monogram", "")));
	w->frame.has_insetx = bool(p_frame.get("has_insetx", w->frame.has_insetx));
	w->frame.insetx = int(p_frame.get("insetx", w->frame.insetx));
	w->frame.has_insety = bool(p_frame.get("has_insety", w->frame.has_insety));
	w->frame.insety = int(p_frame.get("insety", w->frame.insety));
	touch();
}

bool NovaMnuDocument::widget_has_scrollbar(int p_id) const {
	const mnu::Window *w = window_at(locate(p_id));
	return w != nullptr && w->table_data.scrollbar.present;
}

bool NovaMnuDocument::widget_has_spin_arrows(int p_id) const {
	const mnu::Window *w = window_at(locate(p_id));
	return w != nullptr && w->spinup.present && w->spindown.present;
}

TypedArray<Dictionary> NovaMnuDocument::get_widget_actions(int p_id) const {
	TypedArray<Dictionary> out;
	const mnu::Window *w = window_at(locate(p_id));
	if (!w) {
		return out;
	}
	for (const mnu::Action &a : w->actions) {
		Dictionary d;
		d["type"] = to_gd(a.type);
		d["target"] = to_gd(a.target);
		d["state"] = to_gd(a.state);
		d["file"] = to_gd(a.file);
		d["source"] = to_gd(a.source);
		d["field"] = to_gd(a.field);
		d["test"] = to_gd(a.test);
		d["has_target_form"] = a.has_target_form;
		d["target_form"] = a.target_form;
		d["toggle"] = a.toggle;
		d["external_browser"] = a.external_browser;
		out.push_back(d);
	}
	return out;
}

void NovaMnuDocument::set_widget_actions(int p_id, const TypedArray<Dictionary> &p_actions) {
	mnu::Window *w = window_at(locate(p_id));
	if (!w) {
		return;
	}
	std::vector<mnu::Action> next;
	next.reserve(static_cast<size_t>(p_actions.size()));
	for (int i = 0; i < p_actions.size(); ++i) {
		const Dictionary d = p_actions[i];
		mnu::Action a;
		a.type = to_std(String(d.get("type", "")));
		a.target = to_std(String(d.get("target", "")));
		a.state = to_std(String(d.get("state", "")));
		a.file = to_std(String(d.get("file", "")));
		a.source = to_std(String(d.get("source", "")));
		a.field = to_std(String(d.get("field", "")));
		a.test = to_std(String(d.get("test", "")));
		a.has_target_form = static_cast<bool>(d.get("has_target_form", false));
		a.target_form = static_cast<int>(d.get("target_form", 0));
		a.toggle = static_cast<bool>(d.get("toggle", false));
		a.external_browser = static_cast<bool>(d.get("external_browser", false));
		next.push_back(a);
	}
	w->actions = next;
	touch();
}

// --- M10: item rows (list / multi / spinlist / combo) ---

int NovaMnuDocument::get_item_count(int p_id) const {
	const mnu::Items *items = items_container(window_at(locate(p_id)));
	return items ? static_cast<int>(items->items.size()) : 0;
}

Dictionary NovaMnuDocument::get_item(int p_id, int p_index) const {
	const mnu::Items *items = items_container(window_at(locate(p_id)));
	if (items == nullptr || p_index < 0 || p_index >= static_cast<int>(items->items.size())) {
		return Dictionary();
	}
	return item_to_dict(items->items[p_index]);
}

TypedArray<Dictionary> NovaMnuDocument::get_items(int p_id) const {
	TypedArray<Dictionary> out;
	const mnu::Items *items = items_container(window_at(locate(p_id)));
	if (items == nullptr) {
		return out;
	}
	for (const auto &it : items->items) {
		out.push_back(item_to_dict(it));
	}
	return out;
}

void NovaMnuDocument::set_item(int p_id, int p_index, const Dictionary &p_row) {
	mnu::Window *w = window_at(locate(p_id));
	mnu::Items *items = items_container(w);
	if (items == nullptr || p_index < 0 || p_index >= static_cast<int>(items->items.size())) {
		return;
	}
	items->present = true;
	items->items[p_index] = item_from_dict(p_row);
	touch();
}

int NovaMnuDocument::add_item(int p_id, const Dictionary &p_row) {
	mnu::Window *w = window_at(locate(p_id));
	mnu::Items *items = items_container(w);
	if (items == nullptr) {
		return -1;
	}
	items->present = true;
	items->items.push_back(item_from_dict(p_row));
	const int index = static_cast<int>(items->items.size()) - 1;
	touch();
	return index;
}

void NovaMnuDocument::remove_item(int p_id, int p_index) {
	mnu::Window *w = window_at(locate(p_id));
	mnu::Items *items = items_container(w);
	if (items == nullptr || p_index < 0 || p_index >= static_cast<int>(items->items.size())) {
		return;
	}
	items->present = true;
	items->items.erase(items->items.begin() + p_index);
	touch();
}

void NovaMnuDocument::move_item(int p_id, int p_from, int p_to) {
	mnu::Window *w = window_at(locate(p_id));
	mnu::Items *items = items_container(w);
	if (items == nullptr) {
		return;
	}
	const int count = static_cast<int>(items->items.size());
	if (p_from < 0 || p_from >= count || p_to < 0 || p_to >= count || p_from == p_to) {
		return;
	}
	items->present = true;
	// Remove then re-insert so p_to names the destination slot in final indexing
	// (erase shifts the tail down by one when p_to > p_from). Clamp defensively.
	mnu::Item moved = items->items[p_from];
	items->items.erase(items->items.begin() + p_from);
	int dest = p_to;
	if (dest > static_cast<int>(items->items.size())) {
		dest = static_cast<int>(items->items.size());
	}
	items->items.insert(items->items.begin() + dest, std::move(moved));
	touch();
}

// --- M10: table column template (headers / bodies) ---

int NovaMnuDocument::get_table_column_count(int p_id) const {
	const mnu::TableData *td = table_of(window_at(locate(p_id)));
	return td ? td->column.count : 0;
}

void NovaMnuDocument::set_table_column_count(int p_id, int p_count) {
	mnu::TableData *td = table_of(window_at(locate(p_id)));
	if (td == nullptr || p_count < 0) {
		return;
	}
	td->column.has_count = true;
	td->column.count = p_count;
	touch();
}

int NovaMnuDocument::get_table_column_spacing(int p_id) const {
	const mnu::TableData *td = table_of(window_at(locate(p_id)));
	return td ? td->column.spacing : 0;
}

void NovaMnuDocument::set_table_column_spacing(int p_id, int p_spacing) {
	mnu::TableData *td = table_of(window_at(locate(p_id)));
	if (td == nullptr || p_spacing < 0) {
		return;
	}
	td->column.has_spacing = true;
	td->column.spacing = p_spacing;
	touch();
}

TypedArray<Dictionary> NovaMnuDocument::get_table_headers(int p_id) const {
	TypedArray<Dictionary> out;
	const mnu::TableData *td = table_of(window_at(locate(p_id)));
	if (td == nullptr) {
		return out;
	}
	for (const auto &h : td->column.headers) {
		out.push_back(header_to_dict(h));
	}
	return out;
}

void NovaMnuDocument::set_table_header(int p_id, int p_index, const Dictionary &p_row) {
	mnu::TableData *td = table_of(window_at(locate(p_id)));
	if (td == nullptr || p_index < 0 || p_index >= static_cast<int>(td->column.headers.size())) {
		return;
	}
	td->column.headers[p_index] = header_from_dict(p_row);
	touch();
}

int NovaMnuDocument::add_table_header(int p_id, const Dictionary &p_row) {
	mnu::TableData *td = table_of(window_at(locate(p_id)));
	if (td == nullptr) {
		return -1;
	}
	td->column.headers.push_back(header_from_dict(p_row));
	touch();
	return static_cast<int>(td->column.headers.size()) - 1;
}

void NovaMnuDocument::remove_table_header(int p_id, int p_index) {
	mnu::TableData *td = table_of(window_at(locate(p_id)));
	if (td == nullptr || p_index < 0 || p_index >= static_cast<int>(td->column.headers.size())) {
		return;
	}
	td->column.headers.erase(td->column.headers.begin() + p_index);
	touch();
}

TypedArray<Dictionary> NovaMnuDocument::get_table_bodies(int p_id) const {
	TypedArray<Dictionary> out;
	const mnu::TableData *td = table_of(window_at(locate(p_id)));
	if (td == nullptr) {
		return out;
	}
	for (const auto &b : td->column.bodies) {
		out.push_back(body_to_dict(b));
	}
	return out;
}

void NovaMnuDocument::set_table_body(int p_id, int p_index, const Dictionary &p_row) {
	mnu::TableData *td = table_of(window_at(locate(p_id)));
	if (td == nullptr || p_index < 0 || p_index >= static_cast<int>(td->column.bodies.size())) {
		return;
	}
	td->column.bodies[p_index] = body_from_dict(p_row);
	touch();
}

int NovaMnuDocument::add_table_body(int p_id, const Dictionary &p_row) {
	mnu::TableData *td = table_of(window_at(locate(p_id)));
	if (td == nullptr) {
		return -1;
	}
	td->column.bodies.push_back(body_from_dict(p_row));
	touch();
	return static_cast<int>(td->column.bodies.size()) - 1;
}

void NovaMnuDocument::remove_table_body(int p_id, int p_index) {
	mnu::TableData *td = table_of(window_at(locate(p_id)));
	if (td == nullptr || p_index < 0 || p_index >= static_cast<int>(td->column.bodies.size())) {
		return;
	}
	td->column.bodies.erase(td->column.bodies.begin() + p_index);
	touch();
}

TypedArray<Dictionary> NovaMnuDocument::get_table_substs(int p_id) const {
	TypedArray<Dictionary> out;
	const mnu::TableData *td = table_of(window_at(locate(p_id)));
	if (td == nullptr) {
		return out;
	}
	for (const auto &s : td->column.substitutions) {
		out.push_back(subst_to_dict(s));
	}
	return out;
}

void NovaMnuDocument::set_table_subst(int p_id, int p_index, const Dictionary &p_row) {
	mnu::TableData *td = table_of(window_at(locate(p_id)));
	if (td == nullptr || p_index < 0 || p_index >= static_cast<int>(td->column.substitutions.size())) {
		return;
	}
	td->column.substitutions[p_index] = subst_from_dict(p_row);
	touch();
}

int NovaMnuDocument::add_table_subst(int p_id, const Dictionary &p_row) {
	mnu::TableData *td = table_of(window_at(locate(p_id)));
	if (td == nullptr) {
		return -1;
	}
	td->column.substitutions.push_back(subst_from_dict(p_row));
	touch();
	return static_cast<int>(td->column.substitutions.size()) - 1;
}

void NovaMnuDocument::remove_table_subst(int p_id, int p_index) {
	mnu::TableData *td = table_of(window_at(locate(p_id)));
	if (td == nullptr || p_index < 0 || p_index >= static_cast<int>(td->column.substitutions.size())) {
		return;
	}
	td->column.substitutions.erase(td->column.substitutions.begin() + p_index);
	touch();
}

String NovaMnuDocument::get_widget_color(int p_id, int p_slot) const {
	const mnu::Window *w = window_at(locate(p_id));
	if (!w) {
		return String();
	}
	const mnu::Font &f = w->font;
	switch (p_slot) {
		case COLOR_DEFAULT_FG:
			return to_gd(f.default_fg);
		case COLOR_DEFAULT_BG:
			return to_gd(f.default_bg);
		case COLOR_MOUSEOVER_FG:
			return to_gd(f.mouseover_fg);
		case COLOR_MOUSEOVER_BG:
			return to_gd(f.mouseover_bg);
		case COLOR_SELECTED_FG:
			return to_gd(f.selected_fg);
		case COLOR_SELECTED_BG:
			return to_gd(f.selected_bg);
		case COLOR_DISABLED_FG:
			return to_gd(f.disabled_fg);
		case COLOR_DISABLED_BG:
			return to_gd(f.disabled_bg);
		default:
			return String();
	}
}

void NovaMnuDocument::set_widget_color(int p_id, int p_slot, const String &p_value) {
	mnu::Window *w = window_at(locate(p_id));
	if (!w) {
		return;
	}
	mnu::Font &f = w->font;
	const std::string v = to_std(p_value);
	switch (p_slot) {
		case COLOR_DEFAULT_FG:
			f.default_fg = v;
			break;
		case COLOR_DEFAULT_BG:
			f.default_bg = v;
			break;
		case COLOR_MOUSEOVER_FG:
			f.mouseover_fg = v;
			break;
		case COLOR_MOUSEOVER_BG:
			f.mouseover_bg = v;
			break;
		case COLOR_SELECTED_FG:
			f.selected_fg = v;
			break;
		case COLOR_SELECTED_BG:
			f.selected_bg = v;
			break;
		case COLOR_DISABLED_FG:
			f.disabled_fg = v;
			break;
		case COLOR_DISABLED_BG:
			f.disabled_bg = v;
			break;
		default:
			return;
	}
	touch();
}

String NovaMnuDocument::get_widget_texture(int p_id, int p_slot) const {
	const mnu::Window *w = window_at(locate(p_id));
	if (!w) {
		return String();
	}
	const char *state = state_for_slot(p_slot);
	for (const auto &app : w->appearances) {
		if (app.state == state) {
			return to_gd(app.value);
		}
	}
	return String();
}

void NovaMnuDocument::set_widget_texture(int p_id, int p_slot, const String &p_value) {
	mnu::Window *w = window_at(locate(p_id));
	if (!w) {
		return;
	}
	mnu::Appearance *app = find_appearance(*w, state_for_slot(p_slot), true);
	app->value = to_std(p_value);
	touch();
}

int NovaMnuDocument::get_widget_flags(int p_id) const {
	const mnu::Window *w = window_at(locate(p_id));
	if (!w) {
		return 0;
	}
	int flags = 0;
	if (w->hidden) {
		flags |= FLAG_HIDDEN;
	}
	if (w->disabled) {
		flags |= FLAG_DISABLED;
	}
	if (w->checked) {
		flags |= FLAG_CHECKED;
	}
	if (w->draw_frame) {
		flags |= FLAG_DRAW_FRAME;
	}
	if (w->modal) {
		flags |= FLAG_MODAL;
	}
	if (w->readonly) {
		flags |= FLAG_READONLY;
	}
	return flags;
}

void NovaMnuDocument::set_widget_flags(int p_id, int p_flags) {
	mnu::Window *w = window_at(locate(p_id));
	if (!w) {
		return;
	}
	w->hidden = (p_flags & FLAG_HIDDEN) != 0;
	w->disabled = (p_flags & FLAG_DISABLED) != 0;
	w->checked = (p_flags & FLAG_CHECKED) != 0;
	w->draw_frame = (p_flags & FLAG_DRAW_FRAME) != 0;
	w->modal = (p_flags & FLAG_MODAL) != 0;
	w->readonly = (p_flags & FLAG_READONLY) != 0;
	touch();
}

int NovaMnuDocument::get_widget_group(int p_id) const {
	const mnu::Window *w = window_at(locate(p_id));
	return w ? w->group : 0;
}

void NovaMnuDocument::set_widget_group(int p_id, int p_group) {
	mnu::Window *w = window_at(locate(p_id));
	if (!w) {
		return;
	}
	w->has_group = true;
	w->group = p_group;
	touch();
}

PackedStringArray NovaMnuDocument::get_flag_labels() const {
	PackedStringArray out;
	out.push_back("Hidden");
	out.push_back("Disabled");
	out.push_back("Checked");
	out.push_back("Draw Frame");
	out.push_back("Modal");
	out.push_back("Read Only");
	return out;
}

// --- Structural mutation ---

int NovaMnuDocument::add_widget(int p_parent_id, int p_type, const Rect2 &p_rect) {
	Locator loc = locate(p_parent_id);
	if (!loc.valid()) {
		return -1;
	}
	// A screen "contains" its root window; adding under a screen targets the root.
	mnu::Window *parent;
	IdWindow *id_parent;
	if (loc.is_screen) {
		parent = &doc_.screens[loc.screen_index].root_window;
		id_parent = &ids_[loc.screen_index].root;
	} else {
		parent = window_at(loc);
		id_parent = id_window_at(loc);
	}
	if (!parent || !id_parent) {
		return -1;
	}

	mnu::Window w;
	w.type = static_cast<mnu::WindowType>(p_type);
	w.name = mnu::window_type_name(w.type);
	// Same auto-size convention as set_window_rect: a negative extent leaves
	// has_right/has_bottom unset so the writer omits RIGHT/BOTTOM.
	const bool auto_width = p_rect.size.x < 0.0f;
	const bool auto_height = p_rect.size.y < 0.0f;
	w.position.left = static_cast<int>(p_rect.position.x);
	w.position.top = static_cast<int>(p_rect.position.y);
	w.position.right = auto_width ? 0 : static_cast<int>(p_rect.position.x + p_rect.size.x);
	w.position.bottom = auto_height ? 0 : static_cast<int>(p_rect.position.y + p_rect.size.y);
	w.position.has_left = w.position.has_top = true;
	w.position.has_right = !auto_width;
	w.position.has_bottom = !auto_height;
	parent->children.push_back(std::move(w));

	IdWindow id_node;
	id_node.id = next_id_++;
	id_parent->children.push_back(std::move(id_node));
	const int new_id = id_parent->children.back().id;

	touch();
	return new_id;
}

void NovaMnuDocument::delete_widget(int p_id) {
	const Locator loc = locate(p_id);
	if (!loc.valid() || loc.is_screen || loc.path.empty()) {
		// Cannot delete an unknown id, a screen, or a screen's root window here.
		return;
	}
	const int child_index = loc.path.back();

	mnu::Window *parent = &doc_.screens[loc.screen_index].root_window;
	IdWindow *id_parent = &ids_[loc.screen_index].root;
	for (size_t i = 0; i + 1 < loc.path.size(); ++i) {
		parent = &parent->children[loc.path[i]];
		id_parent = &id_parent->children[loc.path[i]];
	}
	parent->children.erase(parent->children.begin() + child_index);
	id_parent->children.erase(id_parent->children.begin() + child_index);
	touch();
}

int NovaMnuDocument::add_screen(const String &p_name) {
	mnu::Screen screen = make_default_screen(to_std(p_name));
	// Stock-shaped root: every shipped screen root is a MAIN window with a full
	// 4-corner POSITION and at least one APPEARANCE row — the original engine's
	// layout/render paths assume them (a bare root crashed it).
	doc_.screens.push_back(std::move(screen));

	IdScreen s;
	s.id = next_id_++;
	s.root.id = next_id_++;
	ids_.push_back(std::move(s));

	touch();
	return ids_.back().id;
}

void NovaMnuDocument::delete_screen(int p_screen_id) {
	const Locator loc = locate(p_screen_id);
	if (!loc.valid() || !loc.is_screen) {
		return;
	}
	doc_.screens.erase(doc_.screens.begin() + loc.screen_index);
	ids_.erase(ids_.begin() + loc.screen_index);
	touch();
}

PackedByteArray NovaMnuDocument::capture_widget_subtree(int p_id) const {
	PackedByteArray out;
	const mnu::Window *w = window_at(locate(p_id));
	if (w == nullptr) {
		return out;
	}
	mnu::Document clipboard;
	mnu::Screen screen;
	screen.name = "CLIPBOARD";
	screen.root_window = *w;
	clipboard.screens.push_back(std::move(screen));
	const std::string bytes = mnu::serialize(clipboard, true, 2);
	out.resize(static_cast<int64_t>(bytes.size()));
	if (!bytes.empty()) {
		std::memcpy(out.ptrw(), bytes.data(), bytes.size());
	}
	return out;
}

int NovaMnuDocument::insert_widget_subtree(int p_parent_id, const PackedByteArray &p_payload,
		int p_index, const Vector2i &p_offset) {
	if (p_payload.is_empty()) {
		return -1;
	}
	mnu::Document clipboard;
	std::string error;
	if (!mnu::parse(p_payload.ptr(), static_cast<size_t>(p_payload.size()), clipboard, error)
			|| clipboard.screens.empty()) {
		return -1;
	}
	const Locator parent_loc = locate(p_parent_id);
	if (!parent_loc.valid()) {
		return -1;
	}
	mnu::Window *parent = nullptr;
	IdWindow *id_parent = nullptr;
	if (parent_loc.is_screen) {
		parent = &doc_.screens[parent_loc.screen_index].root_window;
		id_parent = &ids_[parent_loc.screen_index].root;
	} else {
		parent = window_at(parent_loc);
		id_parent = id_window_at(parent_loc);
	}
	if (parent == nullptr || id_parent == nullptr) {
		return -1;
	}

	mnu::Window pasted = clipboard.screens.front().root_window;
	if (pasted.position.has_left) pasted.position.left += p_offset.x;
	if (pasted.position.has_right) pasted.position.right += p_offset.x;
	if (pasted.position.has_top) pasted.position.top += p_offset.y;
	if (pasted.position.has_bottom) pasted.position.bottom += p_offset.y;
	IdWindow pasted_ids = make_id_window(pasted);
	const int new_id = pasted_ids.id;
	const int index = p_index < 0
			? static_cast<int>(parent->children.size())
			: CLAMP(p_index, 0, static_cast<int>(parent->children.size()));
	parent->children.insert(parent->children.begin() + index, std::move(pasted));
	id_parent->children.insert(id_parent->children.begin() + index, std::move(pasted_ids));
	touch();
	return new_id;
}

bool NovaMnuDocument::move_widget_to_index(int p_id, int p_index) {
	const Locator loc = locate(p_id);
	if (!loc.valid() || loc.is_screen || loc.path.empty()) {
		return false;
	}
	mnu::Window *parent = &doc_.screens[loc.screen_index].root_window;
	IdWindow *id_parent = &ids_[loc.screen_index].root;
	for (size_t i = 0; i + 1 < loc.path.size(); ++i) {
		parent = &parent->children[loc.path[i]];
		id_parent = &id_parent->children[loc.path[i]];
	}
	const int from = loc.path.back();
	const int to = CLAMP(p_index, 0, static_cast<int>(parent->children.size()) - 1);
	if (from == to) {
		return false;
	}
	mnu::Window moved = std::move(parent->children[from]);
	IdWindow moved_ids = std::move(id_parent->children[from]);
	parent->children.erase(parent->children.begin() + from);
	id_parent->children.erase(id_parent->children.begin() + from);
	const int insert_at = CLAMP(to, 0, static_cast<int>(parent->children.size()));
	parent->children.insert(parent->children.begin() + insert_at, std::move(moved));
	id_parent->children.insert(id_parent->children.begin() + insert_at, std::move(moved_ids));
	touch();
	return true;
}

int NovaMnuDocument::duplicate_screen(int p_screen_id, const String &p_name) {
	const Locator loc = locate(p_screen_id);
	if (!loc.valid() || !loc.is_screen) {
		return -1;
	}
	mnu::Screen screen = doc_.screens[loc.screen_index];
	screen.name = to_std(p_name);
	IdScreen ids;
	ids.id = next_id_++;
	ids.root = make_id_window(screen.root_window);
	const int insert_at = loc.screen_index + 1;
	doc_.screens.insert(doc_.screens.begin() + insert_at, std::move(screen));
	ids_.insert(ids_.begin() + insert_at, std::move(ids));
	touch();
	return ids_[insert_at].id;
}

bool NovaMnuDocument::move_screen_to_index(int p_screen_id, int p_index) {
	const Locator loc = locate(p_screen_id);
	if (!loc.valid() || !loc.is_screen || doc_.screens.size() < 2) {
		return false;
	}
	const int from = loc.screen_index;
	const int to = CLAMP(p_index, 0, static_cast<int>(doc_.screens.size()) - 1);
	if (from == to) {
		return false;
	}
	mnu::Screen moved = std::move(doc_.screens[from]);
	IdScreen moved_ids = std::move(ids_[from]);
	doc_.screens.erase(doc_.screens.begin() + from);
	ids_.erase(ids_.begin() + from);
	const int insert_at = CLAMP(to, 0, static_cast<int>(doc_.screens.size()));
	doc_.screens.insert(doc_.screens.begin() + insert_at, std::move(moved));
	ids_.insert(ids_.begin() + insert_at, std::move(moved_ids));
	touch();
	return true;
}

void NovaMnuDocument::set_native(const mnu::Document &p_doc) {
	doc_ = p_doc;
	rebuild_ids();
}

// --- Bindings ---

void NovaMnuDocument::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load_from_bytes", "bytes"), &NovaMnuDocument::load_from_bytes);
	ClassDB::bind_method(D_METHOD("to_byte_array"), &NovaMnuDocument::to_byte_array);
	ClassDB::bind_method(D_METHOD("load_from_path", "path"), &NovaMnuDocument::load_from_path);
	ClassDB::bind_method(D_METHOD("save_to_path", "path"), &NovaMnuDocument::save_to_path);
	ClassDB::bind_method(D_METHOD("create_empty"), &NovaMnuDocument::create_empty);

	ClassDB::bind_method(D_METHOD("get_menu_size"), &NovaMnuDocument::get_menu_size);
	ClassDB::bind_method(D_METHOD("set_menu_size", "size"), &NovaMnuDocument::set_menu_size);

	ClassDB::bind_method(D_METHOD("get_screen_count"), &NovaMnuDocument::get_screen_count);
	ClassDB::bind_method(D_METHOD("get_screen_ids"), &NovaMnuDocument::get_screen_ids);
	ClassDB::bind_method(D_METHOD("get_screen_root_id", "screen_id"), &NovaMnuDocument::get_screen_root_id);
	ClassDB::bind_method(D_METHOD("is_screen", "id"), &NovaMnuDocument::is_screen);
	ClassDB::bind_method(D_METHOD("widget_exists", "id"), &NovaMnuDocument::widget_exists);
	ClassDB::bind_method(D_METHOD("get_parent_id", "id"), &NovaMnuDocument::get_parent_id);
	ClassDB::bind_method(D_METHOD("get_child_ids", "id"), &NovaMnuDocument::get_child_ids);
	ClassDB::bind_method(D_METHOD("get_widget_type", "id"), &NovaMnuDocument::get_widget_type);
	ClassDB::bind_method(D_METHOD("get_widget_type_name", "type"), &NovaMnuDocument::get_widget_type_name);
	ClassDB::bind_method(D_METHOD("get_widget_name", "id"), &NovaMnuDocument::get_widget_name);
	ClassDB::bind_method(D_METHOD("get_window_rect", "id"), &NovaMnuDocument::get_window_rect);

	ClassDB::bind_method(D_METHOD("get_screen_name", "screen_id"), &NovaMnuDocument::get_screen_name);
	ClassDB::bind_method(D_METHOD("get_screen_has_music_var", "screen_id"), &NovaMnuDocument::get_screen_has_music_var);
	ClassDB::bind_method(D_METHOD("get_screen_music_var", "screen_id"), &NovaMnuDocument::get_screen_music_var);
	ClassDB::bind_method(D_METHOD("get_screen_text_rsrc", "screen_id"), &NovaMnuDocument::get_screen_text_rsrc);
	ClassDB::bind_method(D_METHOD("get_screen_cursor_file", "screen_id"), &NovaMnuDocument::get_screen_cursor_file);
	ClassDB::bind_method(D_METHOD("get_screen_cursor_flags", "screen_id"), &NovaMnuDocument::get_screen_cursor_flags);
	ClassDB::bind_method(D_METHOD("set_screen_property", "screen_id", "key", "value"), &NovaMnuDocument::set_screen_property);

	ClassDB::bind_method(D_METHOD("set_widget_name", "id", "name"), &NovaMnuDocument::set_widget_name);
	ClassDB::bind_method(D_METHOD("set_window_rect", "id", "rect"), &NovaMnuDocument::set_window_rect);
	ClassDB::bind_method(D_METHOD("get_widget_text", "id"), &NovaMnuDocument::get_widget_text);
	ClassDB::bind_method(D_METHOD("set_widget_text", "id", "text"), &NovaMnuDocument::set_widget_text);
	ClassDB::bind_method(D_METHOD("get_widget_string_type", "id"), &NovaMnuDocument::get_widget_string_type);
	ClassDB::bind_method(D_METHOD("set_widget_string_type", "id", "type"), &NovaMnuDocument::set_widget_string_type);
	ClassDB::bind_method(D_METHOD("get_widget_font", "id"), &NovaMnuDocument::get_widget_font);
	ClassDB::bind_method(D_METHOD("set_widget_font", "id", "font"), &NovaMnuDocument::set_widget_font);
	ClassDB::bind_method(D_METHOD("get_widget_datasource", "id"), &NovaMnuDocument::get_widget_datasource);
	ClassDB::bind_method(D_METHOD("set_widget_datasource", "id", "value"), &NovaMnuDocument::set_widget_datasource);
	ClassDB::bind_method(D_METHOD("get_widget_orientation", "id"), &NovaMnuDocument::get_widget_orientation);
	ClassDB::bind_method(D_METHOD("set_widget_orientation", "id", "value"), &NovaMnuDocument::set_widget_orientation);
	ClassDB::bind_method(D_METHOD("get_widget_authoring_state", "id"), &NovaMnuDocument::get_widget_authoring_state);
	ClassDB::bind_method(D_METHOD("apply_widget_patch", "id", "patch"), &NovaMnuDocument::apply_widget_patch);

	ClassDB::bind_method(D_METHOD("get_item_count", "id"), &NovaMnuDocument::get_item_count);
	ClassDB::bind_method(D_METHOD("get_widget_sounds", "id"), &NovaMnuDocument::get_widget_sounds);
	ClassDB::bind_method(D_METHOD("set_widget_sounds", "id", "sounds"), &NovaMnuDocument::set_widget_sounds);
	ClassDB::bind_method(D_METHOD("get_widget_actions", "id"), &NovaMnuDocument::get_widget_actions);
	ClassDB::bind_method(D_METHOD("set_widget_actions", "id", "actions"), &NovaMnuDocument::set_widget_actions);
	ClassDB::bind_method(D_METHOD("get_item", "id", "index"), &NovaMnuDocument::get_item);
	ClassDB::bind_method(D_METHOD("get_items", "id"), &NovaMnuDocument::get_items);
	ClassDB::bind_method(D_METHOD("set_item", "id", "index", "row"), &NovaMnuDocument::set_item);
	ClassDB::bind_method(D_METHOD("add_item", "id", "row"), &NovaMnuDocument::add_item);
	ClassDB::bind_method(D_METHOD("remove_item", "id", "index"), &NovaMnuDocument::remove_item);
	ClassDB::bind_method(D_METHOD("move_item", "id", "from", "to"), &NovaMnuDocument::move_item);

	ClassDB::bind_method(D_METHOD("get_table_column_count", "id"), &NovaMnuDocument::get_table_column_count);
	ClassDB::bind_method(D_METHOD("set_table_column_count", "id", "count"), &NovaMnuDocument::set_table_column_count);
	ClassDB::bind_method(D_METHOD("get_table_column_spacing", "id"), &NovaMnuDocument::get_table_column_spacing);
	ClassDB::bind_method(D_METHOD("set_table_column_spacing", "id", "spacing"), &NovaMnuDocument::set_table_column_spacing);
	ClassDB::bind_method(D_METHOD("get_table_headers", "id"), &NovaMnuDocument::get_table_headers);
	ClassDB::bind_method(D_METHOD("set_table_header", "id", "index", "row"), &NovaMnuDocument::set_table_header);
	ClassDB::bind_method(D_METHOD("add_table_header", "id", "row"), &NovaMnuDocument::add_table_header);
	ClassDB::bind_method(D_METHOD("remove_table_header", "id", "index"), &NovaMnuDocument::remove_table_header);
	ClassDB::bind_method(D_METHOD("get_table_bodies", "id"), &NovaMnuDocument::get_table_bodies);
	ClassDB::bind_method(D_METHOD("set_table_body", "id", "index", "row"), &NovaMnuDocument::set_table_body);
	ClassDB::bind_method(D_METHOD("add_table_body", "id", "row"), &NovaMnuDocument::add_table_body);
	ClassDB::bind_method(D_METHOD("remove_table_body", "id", "index"), &NovaMnuDocument::remove_table_body);
	ClassDB::bind_method(D_METHOD("get_table_substs", "id"), &NovaMnuDocument::get_table_substs);
	ClassDB::bind_method(D_METHOD("set_table_subst", "id", "index", "row"), &NovaMnuDocument::set_table_subst);
	ClassDB::bind_method(D_METHOD("add_table_subst", "id", "row"), &NovaMnuDocument::add_table_subst);
	ClassDB::bind_method(D_METHOD("remove_table_subst", "id", "index"), &NovaMnuDocument::remove_table_subst);

	ClassDB::bind_method(D_METHOD("get_widget_color", "id", "slot"), &NovaMnuDocument::get_widget_color);
	ClassDB::bind_method(D_METHOD("set_widget_color", "id", "slot", "value"), &NovaMnuDocument::set_widget_color);
	ClassDB::bind_method(D_METHOD("get_widget_texture", "id", "slot"), &NovaMnuDocument::get_widget_texture);
	ClassDB::bind_method(D_METHOD("set_widget_texture", "id", "slot", "value"), &NovaMnuDocument::set_widget_texture);
	ClassDB::bind_method(D_METHOD("get_widget_appearances", "id"), &NovaMnuDocument::get_widget_appearances);
	ClassDB::bind_method(D_METHOD("set_widget_appearances", "id", "rows"), &NovaMnuDocument::set_widget_appearances);
	ClassDB::bind_method(D_METHOD("get_window_frame", "id"), &NovaMnuDocument::get_window_frame);
	ClassDB::bind_method(D_METHOD("set_window_frame", "id", "frame"), &NovaMnuDocument::set_window_frame);
	ClassDB::bind_method(D_METHOD("widget_has_scrollbar", "id"), &NovaMnuDocument::widget_has_scrollbar);
	ClassDB::bind_method(D_METHOD("widget_has_spin_arrows", "id"), &NovaMnuDocument::widget_has_spin_arrows);
	ClassDB::bind_method(D_METHOD("get_window_rect_flags", "id"), &NovaMnuDocument::get_window_rect_flags);
	BIND_CONSTANT(RECT_HAS_LEFT);
	BIND_CONSTANT(RECT_HAS_TOP);
	BIND_CONSTANT(RECT_HAS_RIGHT);
	BIND_CONSTANT(RECT_HAS_BOTTOM);
	ClassDB::bind_method(D_METHOD("get_widget_flags", "id"), &NovaMnuDocument::get_widget_flags);
	ClassDB::bind_method(D_METHOD("set_widget_flags", "id", "flags"), &NovaMnuDocument::set_widget_flags);
	ClassDB::bind_method(D_METHOD("get_widget_group", "id"), &NovaMnuDocument::get_widget_group);
	ClassDB::bind_method(D_METHOD("set_widget_group", "id", "group"), &NovaMnuDocument::set_widget_group);
	ClassDB::bind_method(D_METHOD("get_flag_labels"), &NovaMnuDocument::get_flag_labels);

	ClassDB::bind_method(D_METHOD("add_widget", "parent_id", "type", "rect"), &NovaMnuDocument::add_widget);
	ClassDB::bind_method(D_METHOD("delete_widget", "id"), &NovaMnuDocument::delete_widget);
	ClassDB::bind_method(D_METHOD("add_screen", "name"), &NovaMnuDocument::add_screen);
	ClassDB::bind_method(D_METHOD("delete_screen", "screen_id"), &NovaMnuDocument::delete_screen);
	ClassDB::bind_method(D_METHOD("capture_widget_subtree", "id"), &NovaMnuDocument::capture_widget_subtree);
	ClassDB::bind_method(D_METHOD("insert_widget_subtree", "parent_id", "payload", "index", "offset"),
			&NovaMnuDocument::insert_widget_subtree, DEFVAL(-1), DEFVAL(Vector2i()));
	ClassDB::bind_method(D_METHOD("move_widget_to_index", "id", "index"), &NovaMnuDocument::move_widget_to_index);
	ClassDB::bind_method(D_METHOD("duplicate_screen", "screen_id", "name"), &NovaMnuDocument::duplicate_screen);
	ClassDB::bind_method(D_METHOD("move_screen_to_index", "screen_id", "index"), &NovaMnuDocument::move_screen_to_index);

	ClassDB::bind_method(D_METHOD("capture_state"), &NovaMnuDocument::capture_state);
	ClassDB::bind_method(D_METHOD("apply_state", "state"), &NovaMnuDocument::apply_state);
	ClassDB::bind_method(D_METHOD("reparent_widget", "id", "new_parent_id", "index"), &NovaMnuDocument::reparent_widget);

	BIND_ENUM_CONSTANT(TYPE_WINDOW);
	BIND_ENUM_CONSTANT(TYPE_STATIC);
	BIND_ENUM_CONSTANT(TYPE_BUTTON);
	BIND_ENUM_CONSTANT(TYPE_EDIT);
	BIND_ENUM_CONSTANT(TYPE_MULTILINE_EDIT);
	BIND_ENUM_CONSTANT(TYPE_LIST);
	BIND_ENUM_CONSTANT(TYPE_CHECKBOX);
	BIND_ENUM_CONSTANT(TYPE_RADIO);
	BIND_ENUM_CONSTANT(TYPE_COMBO);
	BIND_ENUM_CONSTANT(TYPE_SCROLL);
	BIND_ENUM_CONSTANT(TYPE_TABLE);
	BIND_ENUM_CONSTANT(TYPE_SPINLIST);
	BIND_ENUM_CONSTANT(TYPE_MULTI);
	BIND_ENUM_CONSTANT(TYPE_MAP);
	BIND_ENUM_CONSTANT(TYPE_GLOBE);
	BIND_ENUM_CONSTANT(TYPE_LABEL);
	BIND_ENUM_CONSTANT(TYPE_GOTO);
	BIND_ENUM_CONSTANT(TYPE_MARQUEE);
	BIND_ENUM_CONSTANT(TYPE_GLB_TABLE);
	BIND_ENUM_CONSTANT(TYPE_RADIOEDIT);
	BIND_ENUM_CONSTANT(TYPE_LAN_LIST);
	BIND_ENUM_CONSTANT(TYPE_GOPHER);
	BIND_ENUM_CONSTANT(TYPE_UNKNOWN);

	BIND_ENUM_CONSTANT(COLOR_DEFAULT_FG);
	BIND_ENUM_CONSTANT(COLOR_DEFAULT_BG);
	BIND_ENUM_CONSTANT(COLOR_MOUSEOVER_FG);
	BIND_ENUM_CONSTANT(COLOR_MOUSEOVER_BG);
	BIND_ENUM_CONSTANT(COLOR_SELECTED_FG);
	BIND_ENUM_CONSTANT(COLOR_SELECTED_BG);
	BIND_ENUM_CONSTANT(COLOR_DISABLED_FG);
	BIND_ENUM_CONSTANT(COLOR_DISABLED_BG);

	BIND_ENUM_CONSTANT(TEX_DEFAULT);
	BIND_ENUM_CONSTANT(TEX_MOUSEOVER);
	BIND_ENUM_CONSTANT(TEX_SELECTED);
	BIND_ENUM_CONSTANT(TEX_DISABLED);

	BIND_ENUM_CONSTANT(FLAG_HIDDEN);
	BIND_ENUM_CONSTANT(FLAG_DISABLED);
	BIND_ENUM_CONSTANT(FLAG_CHECKED);
	BIND_ENUM_CONSTANT(FLAG_DRAW_FRAME);
	BIND_ENUM_CONSTANT(FLAG_MODAL);
	BIND_ENUM_CONSTANT(FLAG_READONLY);
}
