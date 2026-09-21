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

// The canonical item-row container of a list-like widget is the engine's
// (runtime/menu menu_items_container: a combo's authored LIST_BOX rows, else
// the window-level ITEMS block).
using opennova::menu::menu_items_container;

} // namespace

MnuDocument::MnuDocument() {
	rebuild_ids();
}

// --- Id tree ---

void MnuDocument::rebuild_ids() {
	index_.build(doc_);
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
	for (int id : index_.screen_ids()) out.push_back(id);
	return out;
}

int MnuDocument::get_screen_root_id(int p_screen_id) const {
	return index_.screen_root_id(p_screen_id);
}

bool MnuDocument::is_screen(int p_id) const {
	return index_.screen(p_id) != nullptr;
}

int MnuDocument::get_parent_id(int p_id) const {
	// Screens (and unknown ids) have no parent; a root window's parent is its screen.
	const opennova::menu::MenuDocIndex::Node *node = index_.node(p_id);
	return node != nullptr && node->window != nullptr ? node->parent_id : -1;
}

PackedInt32Array MnuDocument::get_child_ids(int p_id) const {
	PackedInt32Array out;
	// A screen's one child is its root window.
	if (const opennova::menu::MenuDocIndex::Node *node = index_.node(p_id))
		for (int child : node->child_ids) out.push_back(child);
	return out;
}

int MnuDocument::get_widget_type(int p_id) const {
	const opennova::mnu::Window *w = index_.window(p_id);
	return w != nullptr ? static_cast<int>(w->type) : -1;
}

String MnuDocument::get_widget_name(int p_id) const {
	if (const opennova::mnu::Screen *screen = index_.screen(p_id)) return to_gd(screen->name);
	const opennova::mnu::Window *w = index_.window(p_id);
	return w ? to_gd(w->name) : String();
}

Rect2 MnuDocument::get_window_rect(int p_id) const {
	const opennova::mnu::Window *w = index_.window(p_id);
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
	const opennova::mnu::Screen *screen = index_.screen(p_screen_id);
	return screen != nullptr ? to_gd(screen->name) : String();
}

bool MnuDocument::get_screen_has_music_var(int p_screen_id) const {
	const opennova::mnu::Screen *screen = index_.screen(p_screen_id);
	return screen != nullptr && screen->has_music_var;
}

int MnuDocument::get_screen_music_var(int p_screen_id) const {
	const opennova::mnu::Screen *screen = index_.screen(p_screen_id);
	return screen != nullptr ? screen->music_var : 0;
}

String MnuDocument::get_screen_text_rsrc(int p_screen_id) const {
	const opennova::mnu::Screen *screen = index_.screen(p_screen_id);
	return screen != nullptr ? to_gd(screen->text_rsrc) : String();
}

// --- Widget property read/write ---

String MnuDocument::get_widget_text(int p_id) const {
	const opennova::mnu::Window *w = index_.window(p_id);
	return w ? to_gd(w->string_data.value) : String();
}

String MnuDocument::get_widget_string_type(int p_id) const {
	const opennova::mnu::Window *w = index_.window(p_id);
	return w ? to_gd(w->string_data.type) : String();
}

String MnuDocument::get_widget_font(int p_id) const {
	const opennova::mnu::Window *w = index_.window(p_id);
	return w ? to_gd(w->font.name) : String();
}

String MnuDocument::get_widget_datasource(int p_id) const {
	const opennova::mnu::Window *w = index_.window(p_id);
	return w ? to_gd(w->datasource) : String();
}

String MnuDocument::get_widget_orientation(int p_id) const {
	const opennova::mnu::Window *w = index_.window(p_id);
	return w ? to_gd(w->orientation) : String();
}

TypedArray<MnuSoundRow> MnuDocument::get_widget_sounds(int p_id) const {
	TypedArray<MnuSoundRow> out;
	const opennova::mnu::Window *w = index_.window(p_id);
	if (!w) {
		return out;
	}
	for (const opennova::mnu::Sound &s : w->sounds) {
		Ref<MnuSoundRow> row;
		row.instantiate();
		row->assign(s);
		out.push_back(row);
	}
	return out;
}

TypedArray<MnuActionRow> MnuDocument::get_widget_actions(int p_id) const {
	TypedArray<MnuActionRow> out;
	const opennova::mnu::Window *w = index_.window(p_id);
	if (!w) {
		return out;
	}
	for (const opennova::mnu::Action &a : w->actions) {
		Ref<MnuActionRow> row;
		row.instantiate();
		row->assign(a);
		out.push_back(row);
	}
	return out;
}

// --- M10: item rows (list / multi / spinlist / combo) ---

bool MnuDocument::is_widget_multiselect(int p_id) const {
	const opennova::mnu::Window *w = index_.window(p_id);
	return w != nullptr && w->items.multiselect;
}

int MnuDocument::get_item_count(int p_id) const {
	const opennova::mnu::Items *items = menu_items_container(index_.window(p_id));
	return items ? static_cast<int>(items->items.size()) : 0;
}

int MnuDocument::find_item_row_by_value(int p_id, const String &p_value) const {
	const opennova::mnu::Items *items = menu_items_container(index_.window(p_id));
	if (items == nullptr) return -1;
	return opennova::menu::spinlist_row_for_value(
			*items, to_std(p_value));
}

String MnuDocument::get_item_text(int p_id, int p_index) const {
	const opennova::mnu::Items *items = menu_items_container(index_.window(p_id));
	if (items == nullptr || p_index < 0 || p_index >= static_cast<int>(items->items.size())) {
		return String();
	}
	return to_gd(items->items[p_index].text);
}

String MnuDocument::get_item_value(int p_id, int p_index) const {
	const opennova::mnu::Items *items = menu_items_container(index_.window(p_id));
	if (items == nullptr || p_index < 0 || p_index >= static_cast<int>(items->items.size())) {
		return String();
	}
	return to_gd(items->items[p_index].value);
}

// --- M10: table column template (headers / bodies) ---

String MnuDocument::get_widget_color(int p_id, int p_slot) const {
	const opennova::mnu::Window *w = index_.window(p_id);
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
	const opennova::mnu::Window *w = index_.window(p_id);
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
	const opennova::mnu::Window *w = index_.window(p_id);
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
	const opennova::mnu::Window *w = index_.window(p_id);
	return w ? w->group : 0;
}

// --- Structural mutation ---

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
	ClassDB::bind_method(D_METHOD("get_item_text", "id", "index"), &MnuDocument::get_item_text);
	ClassDB::bind_method(D_METHOD("get_item_value", "id", "index"), &MnuDocument::get_item_value);


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
