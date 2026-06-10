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

// The canonical item-row container for a list-like widget: win.items. The parser
// mirrors a combo's LIST_BOX rows into win.items on load (mnu.cpp), and the
// runtime builder reads list_box.items only when it is non-empty (otherwise
// win.items) -- so win.items is always the correct view to read and edit. A
// combo that stores its rows in a top-level <ITEMS> with a present-but-empty
// LIST_BOX (styling only) is therefore handled correctly too. Non-list widgets
// have no item list.
mnu::Items *items_container(mnu::Window *w) {
	if (w == nullptr) {
		return nullptr;
	}
	switch (w->type) {
		case mnu::WindowType::List:
		case mnu::WindowType::Multi:
		case mnu::WindowType::SpinList:
		case mnu::WindowType::Combo:
			return &w->items;
		default:
			return nullptr;
	}
}

const mnu::Items *items_container(const mnu::Window *w) {
	return items_container(const_cast<mnu::Window *>(w));
}

// After editing a combo's items, push win.items (the canonical container) into
// its LIST_BOX so the two stay equal: the serializer writes both win.items (as a
// top-level <ITEMS>) and list_box.items (inside <LIST_BOX>), and on re-parse a
// non-empty LIST_BOX wins (win.items = list_box.items). Keeping them equal makes
// that round-trip a no-op instead of letting a stale copy win.
void sync_item_mirror(mnu::Window *w) {
	if (w != nullptr && w->type == mnu::WindowType::Combo && w->list_box.present) {
		w->list_box.items = w->items;
	}
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
	return (w != nullptr && w->type == mnu::WindowType::Table) ? &w->table_data : nullptr;
}

const mnu::TableData *table_of(const mnu::Window *w) {
	return table_of(const_cast<mnu::Window *>(w));
}

Dictionary header_to_dict(const mnu::TableHeader &h) {
	Dictionary d;
	d["column"] = h.column;
	d["width"] = h.width;
	d["justify"] = to_gd(h.justify);
	d["vjustify"] = to_gd(h.vjustify);
	d["sort"] = to_gd(h.sort);
	d["text"] = to_gd(h.text);
	return d;
}

mnu::TableHeader header_from_dict(const Dictionary &d) {
	mnu::TableHeader h;
	h.column = static_cast<int>(d.get("column", 0));
	h.width = static_cast<int>(d.get("width", 0));
	h.justify = to_std(String(d.get("justify", "")));
	h.vjustify = to_std(String(d.get("vjustify", "")));
	h.sort = to_std(String(d.get("sort", "")));
	h.text = to_std(String(d.get("text", "")));
	return h;
}

Dictionary body_to_dict(const mnu::TableBody &b) {
	Dictionary d;
	d["column"] = b.column;
	d["justify"] = to_gd(b.justify);
	d["vjustify"] = to_gd(b.vjustify);
	d["bitmap_draw"] = b.bitmap_draw;
	d["scale_bitmap"] = b.scale_bitmap;
	d["bitmap_flags"] = to_gd(b.bitmap_flags);
	return d;
}

mnu::TableBody body_from_dict(const Dictionary &d) {
	mnu::TableBody b;
	b.column = static_cast<int>(d.get("column", 0));
	b.justify = to_std(String(d.get("justify", "")));
	b.vjustify = to_std(String(d.get("vjustify", "")));
	b.bitmap_draw = static_cast<bool>(d.get("bitmap_draw", false));
	b.scale_bitmap = static_cast<bool>(d.get("scale_bitmap", false));
	b.bitmap_flags = to_std(String(d.get("bitmap_flags", "")));
	return b;
}

Dictionary subst_to_dict(const mnu::TableSubst &s) {
	Dictionary d;
	d["column"] = s.column;
	d["value"] = to_gd(s.value);
	d["is_file"] = s.is_file;
	d["file"] = to_gd(s.file);
	return d;
}

mnu::TableSubst subst_from_dict(const Dictionary &d) {
	mnu::TableSubst s;
	s.column = static_cast<int>(d.get("column", 0));
	s.value = to_std(String(d.get("value", "")));
	s.is_file = static_cast<bool>(d.get("is_file", false));
	s.file = to_std(String(d.get("file", "")));
	return s;
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
		UtilityFunctions::printerr("NovaMnuDocument: parse failed: ", error.c_str());
		return ERR_FILE_CORRUPT;
	}
	doc_ = std::move(parsed);
	rebuild_ids();
	// Derive the design canvas from the authored content so the editor preview
	// letterboxes real menus correctly: JO menus are 800x600, while the 640x480
	// default only fit the older / hand-authored ones (the cause of the preview
	// not filling its pane). The runtime ignores menu_size_; it only drives the
	// editor fit. Leaves the default untouched if no window carries a position.
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
	const std::string text = mnu::serialize(doc_, true, 2);
	PackedByteArray out;
	out.resize(static_cast<int64_t>(text.size()));
	if (!text.empty()) {
		std::memcpy(out.ptrw(), text.data(), text.size());
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
	mnu::Screen screen;
	screen.name = "SCREEN";
	screen.root_window.name = "ROOT";
	screen.root_window.type = mnu::WindowType::Window;
	doc_.screens.push_back(std::move(screen));
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
	mnu::Position &p = w->position;
	p.left = static_cast<int>(p_rect.position.x);
	p.top = static_cast<int>(p_rect.position.y);
	p.right = static_cast<int>(p_rect.position.x + p_rect.size.x);
	p.bottom = static_cast<int>(p_rect.position.y + p_rect.size.y);
	p.has_left = p.has_top = p.has_right = p.has_bottom = true;
	touch();
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
	items->items[p_index] = item_from_dict(p_row);
	sync_item_mirror(w);
	touch();
}

int NovaMnuDocument::add_item(int p_id, const Dictionary &p_row) {
	mnu::Window *w = window_at(locate(p_id));
	mnu::Items *items = items_container(w);
	if (items == nullptr) {
		return -1;
	}
	items->items.push_back(item_from_dict(p_row));
	const int index = static_cast<int>(items->items.size()) - 1;
	sync_item_mirror(w);
	touch();
	return index;
}

void NovaMnuDocument::remove_item(int p_id, int p_index) {
	mnu::Window *w = window_at(locate(p_id));
	mnu::Items *items = items_container(w);
	if (items == nullptr || p_index < 0 || p_index >= static_cast<int>(items->items.size())) {
		return;
	}
	items->items.erase(items->items.begin() + p_index);
	sync_item_mirror(w);
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
	// Remove then re-insert so p_to names the destination slot in final indexing
	// (erase shifts the tail down by one when p_to > p_from). Clamp defensively.
	mnu::Item moved = items->items[p_from];
	items->items.erase(items->items.begin() + p_from);
	int dest = p_to;
	if (dest > static_cast<int>(items->items.size())) {
		dest = static_cast<int>(items->items.size());
	}
	items->items.insert(items->items.begin() + dest, std::move(moved));
	sync_item_mirror(w);
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
	w.position.left = static_cast<int>(p_rect.position.x);
	w.position.top = static_cast<int>(p_rect.position.y);
	w.position.right = static_cast<int>(p_rect.position.x + p_rect.size.x);
	w.position.bottom = static_cast<int>(p_rect.position.y + p_rect.size.y);
	w.position.has_left = w.position.has_top = w.position.has_right = w.position.has_bottom = true;
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
	mnu::Screen screen;
	screen.name = to_std(p_name);
	screen.root_window.name = "ROOT";
	screen.root_window.type = mnu::WindowType::Window;
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

// --- Snapshot + reparent (M8) ---

void NovaMnuDocument::collect_id_window(const IdWindow &node, PackedInt32Array &out) const {
	out.push_back(node.id);
	for (const auto &child : node.children) {
		collect_id_window(child, out);
	}
}

PackedInt32Array NovaMnuDocument::collect_ids() const {
	PackedInt32Array out;
	for (const auto &s : ids_) {
		out.push_back(s.id);
		collect_id_window(s.root, out);
	}
	return out;
}

bool NovaMnuDocument::build_id_window_from_list(const mnu::Window &w, const PackedInt32Array &ids, int &k, IdWindow &out) const {
	if (k >= ids.size()) {
		return false;
	}
	out.id = ids[k++];
	out.children.clear();
	out.children.reserve(w.children.size());
	for (const auto &child : w.children) {
		IdWindow child_node;
		if (!build_id_window_from_list(child, ids, k, child_node)) {
			return false;
		}
		out.children.push_back(std::move(child_node));
	}
	return true;
}

Dictionary NovaMnuDocument::capture_state() const {
	Dictionary state;
	state["mnu"] = to_byte_array();
	state["ids"] = collect_ids();
	state["next_id"] = next_id_;
	state["menu_size"] = menu_size_;
	return state;
}

void NovaMnuDocument::apply_state(const Dictionary &p_state) {
	if (!p_state.has("mnu")) {
		return;
	}
	const PackedByteArray packed = p_state.get("mnu", PackedByteArray());
	std::vector<uint8_t> bytes(static_cast<size_t>(packed.size()));
	if (!bytes.empty()) {
		std::memcpy(bytes.data(), packed.ptr(), bytes.size());
	}
	mnu::Document parsed;
	std::string error;
	if (!mnu::parse(bytes.data(), bytes.size(), parsed, error)) {
		UtilityFunctions::printerr("NovaMnuDocument::apply_state: parse failed: ", error.c_str());
		return;
	}
	doc_ = std::move(parsed);

	// Rebuild the id tree from the stored pre-order list (byte-identical restore),
	// walking doc_ in the same order rebuild_ids would. On any under-/over-run the
	// list is out of step with the parsed tree, so fall back to positional ids.
	const PackedInt32Array ids = p_state.get("ids", PackedInt32Array());
	bool ok = ids.size() > 0;
	std::vector<IdScreen> rebuilt;
	int k = 0;
	if (ok) {
		rebuilt.reserve(doc_.screens.size());
		for (const auto &screen : doc_.screens) {
			if (k >= ids.size()) {
				ok = false;
				break;
			}
			IdScreen s;
			s.id = ids[k++];
			if (!build_id_window_from_list(screen.root_window, ids, k, s.root)) {
				ok = false;
				break;
			}
			rebuilt.push_back(std::move(s));
		}
		if (ok && k != ids.size()) {
			ok = false; // extra ids: tree had fewer nodes than the list
		}
	}

	if (ok) {
		ids_ = std::move(rebuilt);
		int max_id = 0;
		for (int64_t i = 0; i < ids.size(); ++i) {
			if (ids[i] > max_id) {
				max_id = ids[i];
			}
		}
		const int stored_next = static_cast<int>(p_state.get("next_id", max_id + 1));
		next_id_ = stored_next > max_id + 1 ? stored_next : max_id + 1;
	} else {
		UtilityFunctions::printerr("NovaMnuDocument::apply_state: id list desync, falling back to positional ids");
		rebuild_ids();
	}

	const Vector2i restored_size = p_state.get("menu_size", menu_size_);
	menu_size_ = restored_size;
	touch();
}

bool NovaMnuDocument::reparent_widget(int p_id, int p_new_parent_id, int p_index) {
	const Locator src = locate(p_id);
	// Refuse an unknown id, a screen, or a screen's root window (path empty).
	if (!src.valid() || src.is_screen || src.path.empty()) {
		return false;
	}
	if (p_new_parent_id == p_id) {
		return false;
	}
	if (!locate(p_new_parent_id).valid()) {
		return false;
	}
	// Cycle check: the new parent must not be p_id or one of its descendants.
	for (int cur = p_new_parent_id; cur > 0 && widget_exists(cur);) {
		if (cur == p_id) {
			return false;
		}
		if (is_screen(cur)) {
			break;
		}
		cur = get_parent_id(cur);
	}

	// Resolve the source parent vectors + the child's index within them.
	const int src_index = src.path.back();
	mnu::Window *src_parent = &doc_.screens[src.screen_index].root_window;
	IdWindow *src_id_parent = &ids_[src.screen_index].root;
	for (size_t i = 0; i + 1 < src.path.size(); ++i) {
		src_parent = &src_parent->children[src.path[i]];
		src_id_parent = &src_id_parent->children[src.path[i]];
	}

	// Move the subtree (and its mirrored id subtree) out, then erase the slot.
	mnu::Window moved = std::move(src_parent->children[src_index]);
	IdWindow moved_id = std::move(src_id_parent->children[src_index]);
	src_parent->children.erase(src_parent->children.begin() + src_index);
	src_id_parent->children.erase(src_id_parent->children.begin() + src_index);

	// Re-locate the destination AFTER the erase (its path may have shifted when it
	// was a later sibling in the same parent). It cannot have vanished: it is not
	// inside the moved subtree (cycle check) and still exists in the tree.
	const Locator dst = locate(p_new_parent_id);
	mnu::Window *dst_parent = nullptr;
	IdWindow *dst_id_parent = nullptr;
	if (dst.valid()) {
		if (dst.is_screen) {
			dst_parent = &doc_.screens[dst.screen_index].root_window;
			dst_id_parent = &ids_[dst.screen_index].root;
		} else {
			dst_parent = window_at(dst);
			dst_id_parent = id_window_at(dst);
		}
	}
	if (dst_parent == nullptr || dst_id_parent == nullptr) {
		// Unreachable given the upfront guards; restore the node to keep the tree
		// consistent rather than dropping it.
		src_parent->children.insert(src_parent->children.begin() + src_index, std::move(moved));
		src_id_parent->children.insert(src_id_parent->children.begin() + src_index, std::move(moved_id));
		return false;
	}

	// When src and dst share the same parent vector and the node sat before the
	// requested slot, the erase shifted everything down by one (the requested
	// index was computed against the pre-erase tree).
	const bool same_parent = (dst_parent == src_parent);
	int insert_index = p_index;
	if (same_parent && src_index < insert_index) {
		insert_index -= 1;
	}
	if (insert_index < 0) {
		insert_index = 0;
	}
	if (insert_index > static_cast<int>(dst_parent->children.size())) {
		insert_index = static_cast<int>(dst_parent->children.size());
	}
	dst_parent->children.insert(dst_parent->children.begin() + insert_index, std::move(moved));
	dst_id_parent->children.insert(dst_id_parent->children.begin() + insert_index, std::move(moved_id));
	// A drop that lands the node back in its original slot is a no-op: the insert
	// above already restored the tree byte-for-byte, so report "not moved" without
	// a change event, so the caller records no undo entry and does not dirty the
	// document.
	if (same_parent && insert_index == src_index) {
		return false;
	}
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
	ClassDB::bind_method(D_METHOD("get_screen_music_var", "screen_id"), &NovaMnuDocument::get_screen_music_var);
	ClassDB::bind_method(D_METHOD("get_screen_text_rsrc", "screen_id"), &NovaMnuDocument::get_screen_text_rsrc);
	ClassDB::bind_method(D_METHOD("get_screen_cursor_file", "screen_id"), &NovaMnuDocument::get_screen_cursor_file);
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
	ClassDB::bind_method(D_METHOD("get_widget_flags", "id"), &NovaMnuDocument::get_widget_flags);
	ClassDB::bind_method(D_METHOD("set_widget_flags", "id", "flags"), &NovaMnuDocument::set_widget_flags);
	ClassDB::bind_method(D_METHOD("get_widget_group", "id"), &NovaMnuDocument::get_widget_group);
	ClassDB::bind_method(D_METHOD("set_widget_group", "id", "group"), &NovaMnuDocument::set_widget_group);
	ClassDB::bind_method(D_METHOD("get_flag_labels"), &NovaMnuDocument::get_flag_labels);

	ClassDB::bind_method(D_METHOD("add_widget", "parent_id", "type", "rect"), &NovaMnuDocument::add_widget);
	ClassDB::bind_method(D_METHOD("delete_widget", "id"), &NovaMnuDocument::delete_widget);
	ClassDB::bind_method(D_METHOD("add_screen", "name"), &NovaMnuDocument::add_screen);
	ClassDB::bind_method(D_METHOD("delete_screen", "screen_id"), &NovaMnuDocument::delete_screen);

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
