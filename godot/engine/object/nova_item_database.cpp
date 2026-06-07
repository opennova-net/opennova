#include "nova_item_database.h"

#include "resource_index/nova_resource_root.h"

#include <def/def.h>

#include <algorithm>
#include <vector>

using namespace godot;

void NovaItemDatabase::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load", "path"), &NovaItemDatabase::load);
	ClassDB::bind_method(D_METHOD("load_from_resource_root", "resource_root", "name"), &NovaItemDatabase::load_from_resource_root);
	ClassDB::bind_method(D_METHOD("is_loaded"), &NovaItemDatabase::is_loaded);
	ClassDB::bind_method(D_METHOD("get_source_path"), &NovaItemDatabase::get_source_path);
	ClassDB::bind_method(D_METHOD("get_last_error"), &NovaItemDatabase::get_last_error);
	ClassDB::bind_method(D_METHOD("get_count"), &NovaItemDatabase::get_count);
	ClassDB::bind_method(D_METHOD("has_item", "id"), &NovaItemDatabase::has_item);
	ClassDB::bind_method(D_METHOD("get_graphic", "id"), &NovaItemDatabase::get_graphic);
	ClassDB::bind_method(D_METHOD("get_anim_def", "id"), &NovaItemDatabase::get_anim_def);
	ClassDB::bind_method(D_METHOD("get_item_type", "id"), &NovaItemDatabase::get_item_type);
	ClassDB::bind_method(D_METHOD("get_display_name", "id"), &NovaItemDatabase::get_display_name);
	ClassDB::bind_method(D_METHOD("get_sound_profile", "id"), &NovaItemDatabase::get_sound_profile);
	ClassDB::bind_method(D_METHOD("get_sound_loops", "id"), &NovaItemDatabase::get_sound_loops);
	ClassDB::bind_method(D_METHOD("get_item", "id"), &NovaItemDatabase::get_item);
	ClassDB::bind_method(D_METHOD("get_item_ids"), &NovaItemDatabase::get_item_ids);
	ClassDB::bind_method(D_METHOD("get_items"), &NovaItemDatabase::get_items);

	BIND_CONSTANT(TYPE_UNKNOWN);
	BIND_CONSTANT(TYPE_MARKER);
	BIND_CONSTANT(TYPE_VEHICLE);
	BIND_CONSTANT(TYPE_PERSON);
	BIND_CONSTANT(TYPE_BUILDING);
	BIND_CONSTANT(TYPE_DECORATION);
	BIND_CONSTANT(TYPE_FOLIAGE);
	BIND_CONSTANT(TYPE_OBJECT);
	BIND_CONSTANT(TYPE_POWERUP);
}

Error NovaItemDatabase::load(const String &path) {
	source_path = path;
	last_error = String();
	items.clear();

	DefItemsFile file = {};
	if (def_parse_items(path.utf8().get_data(), &file) != 0) {
		last_error = String("def_parse_items failed for ") + path;
		return ERR_CANT_OPEN;
	}

	for (size_t i = 0; i < file.count; ++i) {
		const DefItemDef &entry = file.entries[i];
		Item item;
		item.id = entry.id;
		item.type = entry.type;
		item.display_name = String(entry.display_name);
		item.graphic = String(entry.graphic);
		item.anim_def = String(entry.anim_def);
		item.sound_profile = String(entry.sound_profile);
		for (int s = 0; s < 7; ++s) {
			item.soundloops[s] = String(entry.soundloops[s]);
		}
		items[entry.id] = item;
	}

	def_free_items(&file);
	return OK;
}

Error NovaItemDatabase::load_from_resource_root(const Ref<NovaResourceRoot> &p_resource_root, const String &p_name) {
	last_error = String();
	items.clear();
	if (p_resource_root.is_null() || p_resource_root->get_root_dir().is_empty()) {
		last_error = "Resource root is not configured";
		return ERR_INVALID_PARAMETER;
	}
	const String file_name = p_name.get_file();
	if (file_name.is_empty()) {
		last_error = "Item database filename is empty";
		return ERR_INVALID_PARAMETER;
	}
	const PackedByteArray bytes = p_resource_root->read_file(file_name);
	if (bytes.is_empty()) {
		last_error = String("Item database not found in resource root: ") + file_name;
		return ERR_FILE_NOT_FOUND;
	}

	DefItemsFile file = {};
	if (def_parse_items_memory(bytes.ptr(), static_cast<size_t>(bytes.size()), &file) != 0) {
		last_error = String("def_parse_items_memory failed for ") + file_name;
		return ERR_CANT_OPEN;
	}

	for (size_t i = 0; i < file.count; ++i) {
		const DefItemDef &entry = file.entries[i];
		Item item;
		item.id = entry.id;
		item.type = entry.type;
		item.display_name = String(entry.display_name);
		item.graphic = String(entry.graphic);
		item.anim_def = String(entry.anim_def);
		item.sound_profile = String(entry.sound_profile);
		for (int s = 0; s < 7; ++s) {
			item.soundloops[s] = String(entry.soundloops[s]);
		}
		items[entry.id] = item;
	}

	def_free_items(&file);
	source_path = file_name;
	return OK;
}

bool NovaItemDatabase::is_loaded() const {
	return !items.empty();
}

String NovaItemDatabase::get_source_path() const {
	return source_path;
}

String NovaItemDatabase::get_last_error() const {
	return last_error;
}

int NovaItemDatabase::get_count() const {
	return static_cast<int>(items.size());
}

bool NovaItemDatabase::has_item(int id) const {
	return items.find(id) != items.end();
}

String NovaItemDatabase::get_graphic(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? String() : it->second.graphic;
}

String NovaItemDatabase::get_anim_def(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? String() : it->second.anim_def;
}

int NovaItemDatabase::get_item_type(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? static_cast<int>(TYPE_UNKNOWN) : it->second.type;
}

String NovaItemDatabase::get_display_name(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? String() : it->second.display_name;
}

String NovaItemDatabase::get_sound_profile(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? String() : it->second.sound_profile;
}

PackedStringArray NovaItemDatabase::get_sound_loops(int id) const {
	PackedStringArray out;
	out.resize(7);
	const auto it = items.find(id);
	if (it != items.end()) {
		for (int s = 0; s < 7; ++s) {
			out.set(s, it->second.soundloops[s]);
		}
	}
	return out;
}

Dictionary NovaItemDatabase::get_item(int id) const {
	Dictionary out;
	const auto it = items.find(id);
	if (it == items.end()) {
		return out;
	}
	out["id"] = it->second.id;
	out["type"] = it->second.type;
	out["display_name"] = it->second.display_name;
	out["graphic"] = it->second.graphic;
	out["anim_def"] = it->second.anim_def;
	out["sound_profile"] = it->second.sound_profile;
	out["soundloops"] = get_sound_loops(id);
	return out;
}

// The backing store is an unordered_map, so callers that enumerate get a stable
// order only if we impose one. Sort by display name (case-insensitive, the order a
// user scans a palette), breaking ties by id so the order is total and reproducible.
std::vector<const NovaItemDatabase::Item *> NovaItemDatabase::sorted_items() const {
	std::vector<const Item *> out;
	out.reserve(items.size());
	for (const auto &pair : items) {
		out.push_back(&pair.second);
	}
	std::sort(out.begin(), out.end(), [](const Item *a, const Item *b) {
		const int name_cmp = a->display_name.naturalnocasecmp_to(b->display_name);
		if (name_cmp != 0) {
			return name_cmp < 0;
		}
		return a->id < b->id;
	});
	return out;
}

PackedInt32Array NovaItemDatabase::get_item_ids() const {
	PackedInt32Array out;
	const std::vector<const Item *> sorted = sorted_items();
	out.resize(static_cast<int>(sorted.size()));
	for (size_t i = 0; i < sorted.size(); ++i) {
		out.set(static_cast<int>(i), sorted[i]->id);
	}
	return out;
}

Array NovaItemDatabase::get_items() const {
	Array out;
	for (const Item *item : sorted_items()) {
		Dictionary entry;
		entry["id"] = item->id;
		entry["type"] = item->type;
		entry["display_name"] = item->display_name;
		entry["graphic"] = item->graphic;
		entry["anim_def"] = item->anim_def;
		entry["sound_profile"] = item->sound_profile;
		entry["soundloops"] = get_sound_loops(item->id);
		out.push_back(entry);
	}
	return out;
}
