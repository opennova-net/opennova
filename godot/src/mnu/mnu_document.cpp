#include "mnu/mnu_document.h"
#include "util/data_format.h"

#include "util/string_convert.h"

#include <runtime/menu/options_policy.h>

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
opennova::mnu::Items *items_container(opennova::mnu::Window *w) {
	if (w == nullptr) {
		return nullptr;
	}
	switch (w->type) {
		case opennova::mnu::WindowType::List:
		case opennova::mnu::WindowType::Table:
		case opennova::mnu::WindowType::GlbTable:
		case opennova::mnu::WindowType::Multi:
		case opennova::mnu::WindowType::LanList:
		case opennova::mnu::WindowType::SpinList:
			return &w->items;
		case opennova::mnu::WindowType::Combo:
			// LIST_BOX owns combo items when it actually authors an ITEMS
			// block under an authored LIST_BOX. A latent/disabled LIST_BOX or a
			// styling-only one falls back to top-level ITEMS, matching runtime.
			return w->list_box.present && w->list_box.items.present ?
					&w->list_box.items : &w->items;
		default:
			return nullptr;
	}
}

const opennova::mnu::Items *items_container(const opennova::mnu::Window *w) {
	return items_container(const_cast<opennova::mnu::Window *>(w));
}

Dictionary item_to_dict(const opennova::mnu::Item &it) {
	Dictionary d;
	d["type"] = to_gd(it.type);
	d["value"] = to_gd(it.value);
	d["text"] = to_gd(it.text);
	return d;
}

Dictionary sound_to_dict(const opennova::mnu::Sound &s) {
	Dictionary d;
	d["state"] = to_gd(s.state);
	d["trigger"] = to_gd(s.trigger);
	d["file"] = to_gd(s.file);
	return d;
}

TypedArray<Dictionary> sounds_to_array(const std::vector<opennova::mnu::Sound> &sounds) {
	TypedArray<Dictionary> out;
	for (const opennova::mnu::Sound &s : sounds) {
		out.push_back(sound_to_dict(s));
	}
	return out;
}

} // namespace

MnuDocument::MnuDocument() {
	rebuild_ids();
}

// --- Id tree ---

MnuDocument::IdWindow MnuDocument::make_id_window(const opennova::mnu::Window &w) {
	IdWindow node;
	node.id = next_id_++;
	node.children.reserve(w.children.size());
	for (const auto &child : w.children) {
		node.children.push_back(make_id_window(child));
	}
	return node;
}

void MnuDocument::rebuild_ids() {
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

bool MnuDocument::find_in_id_window(const IdWindow &node, int id, std::vector<int> &path) {
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

MnuDocument::Locator MnuDocument::locate(int id) const {
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

opennova::mnu::Window *MnuDocument::window_at(const Locator &loc) {
	if (!loc.valid() || loc.is_screen) {
		return nullptr;
	}
	opennova::mnu::Window *w = &doc_.screens[loc.screen_index].root_window;
	for (int idx : loc.path) {
		w = &w->children[idx];
	}
	return w;
}

const opennova::mnu::Window *MnuDocument::window_at(const Locator &loc) const {
	if (!loc.valid() || loc.is_screen) {
		return nullptr;
	}
	const opennova::mnu::Window *w = &doc_.screens[loc.screen_index].root_window;
	for (int idx : loc.path) {
		w = &w->children[idx];
	}
	return w;
}

MnuDocument::IdWindow *MnuDocument::id_window_at(const Locator &loc) {
	if (!loc.valid() || loc.is_screen) {
		return nullptr;
	}
	IdWindow *n = &ids_[loc.screen_index].root;
	for (int idx : loc.path) {
		n = &n->children[idx];
	}
	return n;
}

const char *MnuDocument::state_for_slot(int slot) {
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

// --- I/O ---

Error MnuDocument::load_from_bytes(const PackedByteArray &p_bytes) {
	std::vector<uint8_t> bytes(static_cast<size_t>(p_bytes.size()));
	if (!bytes.empty()) {
		std::memcpy(bytes.data(), p_bytes.ptr(), bytes.size());
	}
	opennova::mnu::Document parsed;
	std::string error;
	if (!opennova::mnu::parse(bytes.data(), bytes.size(), parsed, error)) {
		UtilityFunctions::push_warning("MnuDocument: parse failed: ", error.c_str());
		return ERR_FILE_CORRUPT;
	}
	doc_ = std::move(parsed);
	rebuild_ids();
	return OK;
}

PackedByteArray MnuDocument::to_byte_array() const {
	std::vector<uint8_t> bytes;
	std::string error;
	if (!opennova::mnu::serialize_bytes(doc_, bytes, error, true, 2)) {
		UtilityFunctions::push_warning(String("MnuDocument::to_byte_array: ") + to_gd(error));
		return PackedByteArray();
	}
	return to_packed_bytes(bytes);
}

Error MnuDocument::load_from_path(const String &p_path) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null()) {
		return ERR_FILE_CANT_OPEN;
	}
	const PackedByteArray packed = file->get_buffer(file->get_length());
	file->close();
	return load_from_bytes(packed);
}

Error MnuDocument::save_to_path(const String &p_path) const {
	const PackedByteArray packed = to_byte_array();
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		return ERR_FILE_CANT_WRITE;
	}
	file->store_buffer(packed);
	file->close();
	return OK;
}

// --- Tree read ---

int MnuDocument::get_screen_count() const {
	return static_cast<int>(doc_.screens.size());
}

PackedInt32Array MnuDocument::get_screen_ids() const {
	PackedInt32Array out;
	out.resize(static_cast<int64_t>(ids_.size()));
	for (size_t i = 0; i < ids_.size(); ++i) {
		out.set(static_cast<int64_t>(i), ids_[i].id);
	}
	return out;
}

int MnuDocument::get_screen_root_id(int p_screen_id) const {
	for (const auto &s : ids_) {
		if (s.id == p_screen_id) {
			return s.root.id;
		}
	}
	return -1;
}

bool MnuDocument::is_screen(int p_id) const {
	const Locator loc = locate(p_id);
	return loc.valid() && loc.is_screen;
}

bool MnuDocument::widget_exists(int p_id) const {
	return locate(p_id).valid();
}

int MnuDocument::get_parent_id(int p_id) const {
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

PackedInt32Array MnuDocument::get_child_ids(int p_id) const {
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

int MnuDocument::get_widget_type(int p_id) const {
	const Locator loc = locate(p_id);
	if (!loc.valid() || loc.is_screen) {
		return -1;
	}
	return static_cast<int>(window_at(loc)->type);
}

String MnuDocument::get_widget_name(int p_id) const {
	const Locator loc = locate(p_id);
	if (loc.is_screen) {
		return to_gd(doc_.screens[loc.screen_index].name);
	}
	const opennova::mnu::Window *w = window_at(loc);
	return w ? to_gd(w->name) : String();
}

Rect2 MnuDocument::get_window_rect(int p_id) const {
	const Locator loc = locate(p_id);
	const opennova::mnu::Window *w = window_at(loc);
	if (!w) {
		return Rect2();
	}
	const opennova::mnu::Position &p = w->position;
	const float x = p.has_left ? static_cast<float>(p.left) : 0.0f;
	const float y = p.has_top ? static_cast<float>(p.top) : 0.0f;
	const float wd = (p.has_left && p.has_right) ? static_cast<float>(p.right - p.left) : 0.0f;
	const float ht = (p.has_top && p.has_bottom) ? static_cast<float>(p.bottom - p.top) : 0.0f;
	return Rect2(x, y, wd, ht);
}

// --- Screen properties ---

String MnuDocument::get_screen_name(int p_screen_id) const {
	const Locator loc = locate(p_screen_id);
	if (!loc.valid() || !loc.is_screen) {
		return String();
	}
	return to_gd(doc_.screens[loc.screen_index].name);
}

bool MnuDocument::get_screen_has_music_var(int p_screen_id) const {
	const Locator loc = locate(p_screen_id);
	return loc.valid() && loc.is_screen &&
			doc_.screens[loc.screen_index].has_music_var;
}

int MnuDocument::get_screen_music_var(int p_screen_id) const {
	const Locator loc = locate(p_screen_id);
	if (!loc.valid() || !loc.is_screen) {
		return 0;
	}
	return doc_.screens[loc.screen_index].music_var;
}

String MnuDocument::get_screen_text_rsrc(int p_screen_id) const {
	const Locator loc = locate(p_screen_id);
	if (!loc.valid() || !loc.is_screen) {
		return String();
	}
	return to_gd(doc_.screens[loc.screen_index].text_rsrc);
}

// --- Widget property read/write ---

String MnuDocument::get_widget_text(int p_id) const {
	const opennova::mnu::Window *w = window_at(locate(p_id));
	return w ? to_gd(w->string_data.value) : String();
}

String MnuDocument::get_widget_string_type(int p_id) const {
	const opennova::mnu::Window *w = window_at(locate(p_id));
	return w ? to_gd(w->string_data.type) : String();
}

String MnuDocument::get_widget_font(int p_id) const {
	const opennova::mnu::Window *w = window_at(locate(p_id));
	return w ? to_gd(w->font.name) : String();
}

String MnuDocument::get_widget_datasource(int p_id) const {
	const opennova::mnu::Window *w = window_at(locate(p_id));
	return w ? to_gd(w->datasource) : String();
}

String MnuDocument::get_widget_orientation(int p_id) const {
	const opennova::mnu::Window *w = window_at(locate(p_id));
	return w ? to_gd(w->orientation) : String();
}

TypedArray<Dictionary> MnuDocument::get_widget_sounds(int p_id) const {
	TypedArray<Dictionary> out;
	const opennova::mnu::Window *w = window_at(locate(p_id));
	if (!w) {
		return out;
	}
	for (const opennova::mnu::Sound &s : w->sounds) {
		Dictionary d;
		d["state"] = to_gd(s.state);
		d["trigger"] = to_gd(s.trigger);
		d["file"] = to_gd(s.file);
		out.push_back(d);
	}
	return out;
}

TypedArray<Dictionary> MnuDocument::get_widget_actions(int p_id) const {
	TypedArray<Dictionary> out;
	const opennova::mnu::Window *w = window_at(locate(p_id));
	if (!w) {
		return out;
	}
	for (const opennova::mnu::Action &a : w->actions) {
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

// --- M10: item rows (list / multi / spinlist / combo) ---

bool MnuDocument::is_widget_multiselect(int p_id) const {
	const opennova::mnu::Window *w = window_at(locate(p_id));
	return w != nullptr && w->items.multiselect;
}

int MnuDocument::get_item_count(int p_id) const {
	const opennova::mnu::Items *items = items_container(window_at(locate(p_id)));
	return items ? static_cast<int>(items->items.size()) : 0;
}

int MnuDocument::find_item_row_by_value(int p_id, const String &p_value) const {
	const opennova::mnu::Items *items = items_container(window_at(locate(p_id)));
	if (items == nullptr) return -1;
	return opennova::menu::spinlist_row_for_value(
			*items, std::string(p_value.utf8().get_data()));
}

Dictionary MnuDocument::get_item(int p_id, int p_index) const {
	const opennova::mnu::Items *items = items_container(window_at(locate(p_id)));
	if (items == nullptr || p_index < 0 || p_index >= static_cast<int>(items->items.size())) {
		return Dictionary();
	}
	return item_to_dict(items->items[p_index]);
}

// --- M10: table column template (headers / bodies) ---

String MnuDocument::get_widget_color(int p_id, int p_slot) const {
	const opennova::mnu::Window *w = window_at(locate(p_id));
	if (!w) {
		return String();
	}
	const opennova::mnu::Font &f = w->font;
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

String MnuDocument::get_widget_texture(int p_id, int p_slot) const {
	const opennova::mnu::Window *w = window_at(locate(p_id));
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

int MnuDocument::get_widget_flags(int p_id) const {
	const opennova::mnu::Window *w = window_at(locate(p_id));
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

int MnuDocument::get_widget_group(int p_id) const {
	const opennova::mnu::Window *w = window_at(locate(p_id));
	return w ? w->group : 0;
}

// --- Structural mutation ---

void MnuDocument::set_native(const opennova::mnu::Document &p_doc) {
	doc_ = p_doc;
	rebuild_ids();
}

// --- Bindings ---

void MnuDocument::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load_from_bytes", "bytes"), &MnuDocument::load_from_bytes);
	ClassDB::bind_method(D_METHOD("to_byte_array"), &MnuDocument::to_byte_array);
	ClassDB::bind_method(D_METHOD("load_from_path", "path"), &MnuDocument::load_from_path);
	ClassDB::bind_method(D_METHOD("save_to_path", "path"), &MnuDocument::save_to_path);


	ClassDB::bind_method(D_METHOD("get_screen_count"), &MnuDocument::get_screen_count);
	ClassDB::bind_method(D_METHOD("get_screen_ids"), &MnuDocument::get_screen_ids);
	ClassDB::bind_method(D_METHOD("get_screen_root_id", "screen_id"), &MnuDocument::get_screen_root_id);
	ClassDB::bind_method(D_METHOD("is_screen", "id"), &MnuDocument::is_screen);
	ClassDB::bind_method(D_METHOD("widget_exists", "id"), &MnuDocument::widget_exists);
	ClassDB::bind_method(D_METHOD("get_parent_id", "id"), &MnuDocument::get_parent_id);
	ClassDB::bind_method(D_METHOD("get_child_ids", "id"), &MnuDocument::get_child_ids);
	ClassDB::bind_method(D_METHOD("get_widget_type", "id"), &MnuDocument::get_widget_type);
	ClassDB::bind_method(D_METHOD("get_widget_name", "id"), &MnuDocument::get_widget_name);
	ClassDB::bind_method(D_METHOD("get_window_rect", "id"), &MnuDocument::get_window_rect);

	ClassDB::bind_method(D_METHOD("get_screen_name", "screen_id"), &MnuDocument::get_screen_name);
	ClassDB::bind_method(D_METHOD("get_screen_has_music_var", "screen_id"), &MnuDocument::get_screen_has_music_var);
	ClassDB::bind_method(D_METHOD("get_screen_music_var", "screen_id"), &MnuDocument::get_screen_music_var);
	ClassDB::bind_method(D_METHOD("get_screen_text_rsrc", "screen_id"), &MnuDocument::get_screen_text_rsrc);

	ClassDB::bind_method(D_METHOD("get_widget_text", "id"), &MnuDocument::get_widget_text);
	ClassDB::bind_method(D_METHOD("get_widget_string_type", "id"), &MnuDocument::get_widget_string_type);
	ClassDB::bind_method(D_METHOD("get_widget_font", "id"), &MnuDocument::get_widget_font);
	ClassDB::bind_method(D_METHOD("get_widget_datasource", "id"), &MnuDocument::get_widget_datasource);
	ClassDB::bind_method(D_METHOD("get_widget_orientation", "id"), &MnuDocument::get_widget_orientation);

	ClassDB::bind_method(D_METHOD("is_widget_multiselect", "id"), &MnuDocument::is_widget_multiselect);
	ClassDB::bind_method(D_METHOD("get_item_count", "id"), &MnuDocument::get_item_count);
	ClassDB::bind_method(D_METHOD("find_item_row_by_value", "id", "value"),
			&MnuDocument::find_item_row_by_value);
	ClassDB::bind_method(D_METHOD("get_widget_sounds", "id"), &MnuDocument::get_widget_sounds);
	ClassDB::bind_method(D_METHOD("get_widget_actions", "id"), &MnuDocument::get_widget_actions);
	ClassDB::bind_method(D_METHOD("get_item", "id", "index"), &MnuDocument::get_item);


	ClassDB::bind_method(D_METHOD("get_widget_color", "id", "slot"), &MnuDocument::get_widget_color);
	ClassDB::bind_method(D_METHOD("get_widget_texture", "id", "slot"), &MnuDocument::get_widget_texture);
	ClassDB::bind_method(D_METHOD("get_widget_flags", "id"), &MnuDocument::get_widget_flags);
	ClassDB::bind_method(D_METHOD("get_widget_group", "id"), &MnuDocument::get_widget_group);



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
	BIND_ENUM_CONSTANT(TYPE_LABEL);
	BIND_ENUM_CONSTANT(TYPE_GOTO);
	BIND_ENUM_CONSTANT(TYPE_MARQUEE);
	BIND_ENUM_CONSTANT(TYPE_GLB_TABLE);
	BIND_ENUM_CONSTANT(TYPE_LAN_LIST);

	BIND_ENUM_CONSTANT(COLOR_DEFAULT_FG);

	BIND_ENUM_CONSTANT(TEX_DEFAULT);
	BIND_ENUM_CONSTANT(TEX_MOUSEOVER);

	BIND_ENUM_CONSTANT(FLAG_HIDDEN);
	BIND_ENUM_CONSTANT(FLAG_DISABLED);
	BIND_ENUM_CONSTANT(FLAG_CHECKED);
	BIND_ENUM_CONSTANT(FLAG_READONLY);
}
