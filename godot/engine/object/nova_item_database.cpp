#include "nova_item_database.h"

#include <def/def.h>

using namespace godot;

void NovaItemDatabase::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load", "path"), &NovaItemDatabase::load);
	ClassDB::bind_method(D_METHOD("is_loaded"), &NovaItemDatabase::is_loaded);
	ClassDB::bind_method(D_METHOD("get_source_path"), &NovaItemDatabase::get_source_path);
	ClassDB::bind_method(D_METHOD("get_last_error"), &NovaItemDatabase::get_last_error);
	ClassDB::bind_method(D_METHOD("get_count"), &NovaItemDatabase::get_count);
	ClassDB::bind_method(D_METHOD("has_item", "id"), &NovaItemDatabase::has_item);
	ClassDB::bind_method(D_METHOD("get_graphic", "id"), &NovaItemDatabase::get_graphic);
	ClassDB::bind_method(D_METHOD("get_anim_def", "id"), &NovaItemDatabase::get_anim_def);
	ClassDB::bind_method(D_METHOD("get_item_type", "id"), &NovaItemDatabase::get_item_type);
	ClassDB::bind_method(D_METHOD("get_display_name", "id"), &NovaItemDatabase::get_display_name);
	ClassDB::bind_method(D_METHOD("get_item", "id"), &NovaItemDatabase::get_item);

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
		items[entry.id] = item;
	}

	def_free_items(&file);
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
	return out;
}
