#include "nova_mnu_document.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstring>
#include <string>

using namespace godot;

namespace {

String std_to_gd(const std::string &s) {
	return String::utf8(s.c_str(), static_cast<int>(s.length()));
}

std::string gd_to_std(const String &s) {
	const CharString utf8 = s.utf8();
	return std::string(utf8.get_data(), static_cast<size_t>(utf8.length()));
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
	return std_to_gd(mnu::window_type_name(static_cast<mnu::WindowType>(p_type)));
}

String NovaMnuDocument::get_widget_name(int p_id) const {
	const Locator loc = locate(p_id);
	if (loc.is_screen) {
		return std_to_gd(doc_.screens[loc.screen_index].name);
	}
	const mnu::Window *w = window_at(loc);
	return w ? std_to_gd(w->name) : String();
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
	return std_to_gd(doc_.screens[loc.screen_index].name);
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
	return std_to_gd(doc_.screens[loc.screen_index].text_rsrc);
}

String NovaMnuDocument::get_screen_cursor_file(int p_screen_id) const {
	const Locator loc = locate(p_screen_id);
	if (!loc.valid() || !loc.is_screen) {
		return String();
	}
	return std_to_gd(doc_.screens[loc.screen_index].cursor_file);
}

void NovaMnuDocument::set_screen_property(int p_screen_id, const String &p_key, const Variant &p_value) {
	const Locator loc = locate(p_screen_id);
	if (!loc.valid() || !loc.is_screen) {
		return;
	}
	mnu::Screen &s = doc_.screens[loc.screen_index];
	const String key = p_key.to_lower();
	if (key == "name") {
		s.name = gd_to_std(p_value);
	} else if (key == "music_var") {
		s.music_var = static_cast<int>(p_value);
	} else if (key == "text_rsrc") {
		s.text_rsrc = gd_to_std(p_value);
	} else if (key == "cursor_file") {
		s.cursor_file = gd_to_std(p_value);
	} else if (key == "cursor_flags") {
		s.cursor_flags = gd_to_std(p_value);
	} else {
		return;
	}
	touch();
}

// --- Widget property read/write ---

void NovaMnuDocument::set_widget_name(int p_id, const String &p_name) {
	const Locator loc = locate(p_id);
	if (loc.is_screen) {
		doc_.screens[loc.screen_index].name = gd_to_std(p_name);
		touch();
		return;
	}
	mnu::Window *w = window_at(loc);
	if (!w) {
		return;
	}
	w->name = gd_to_std(p_name);
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
	return w ? std_to_gd(w->string_data.value) : String();
}

void NovaMnuDocument::set_widget_text(int p_id, const String &p_text) {
	mnu::Window *w = window_at(locate(p_id));
	if (!w) {
		return;
	}
	w->string_data.value = gd_to_std(p_text);
	touch();
}

String NovaMnuDocument::get_widget_string_type(int p_id) const {
	const mnu::Window *w = window_at(locate(p_id));
	return w ? std_to_gd(w->string_data.type) : String();
}

void NovaMnuDocument::set_widget_string_type(int p_id, const String &p_type) {
	mnu::Window *w = window_at(locate(p_id));
	if (!w) {
		return;
	}
	w->string_data.type = gd_to_std(p_type);
	touch();
}

String NovaMnuDocument::get_widget_font(int p_id) const {
	const mnu::Window *w = window_at(locate(p_id));
	return w ? std_to_gd(w->font.name) : String();
}

void NovaMnuDocument::set_widget_font(int p_id, const String &p_font) {
	mnu::Window *w = window_at(locate(p_id));
	if (!w) {
		return;
	}
	w->font.name = gd_to_std(p_font);
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
			return std_to_gd(f.default_fg);
		case COLOR_DEFAULT_BG:
			return std_to_gd(f.default_bg);
		case COLOR_MOUSEOVER_FG:
			return std_to_gd(f.mouseover_fg);
		case COLOR_MOUSEOVER_BG:
			return std_to_gd(f.mouseover_bg);
		case COLOR_SELECTED_FG:
			return std_to_gd(f.selected_fg);
		case COLOR_SELECTED_BG:
			return std_to_gd(f.selected_bg);
		case COLOR_DISABLED_FG:
			return std_to_gd(f.disabled_fg);
		case COLOR_DISABLED_BG:
			return std_to_gd(f.disabled_bg);
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
	const std::string v = gd_to_std(p_value);
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
			return std_to_gd(app.value);
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
	app->value = gd_to_std(p_value);
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
	screen.name = gd_to_std(p_name);
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
