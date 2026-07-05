#include "nova_weapon_database.h"

#include "resource_index/nova_resource_root.h"

#include <def/def.h>

using namespace godot;

void NovaWeaponDatabase::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load", "path"), &NovaWeaponDatabase::load);
	ClassDB::bind_method(D_METHOD("load_from_resource_root", "resource_root", "name"),
			&NovaWeaponDatabase::load_from_resource_root);
	ClassDB::bind_method(D_METHOD("is_loaded"), &NovaWeaponDatabase::is_loaded);
	ClassDB::bind_method(D_METHOD("get_source_path"), &NovaWeaponDatabase::get_source_path);
	ClassDB::bind_method(D_METHOD("get_last_error"), &NovaWeaponDatabase::get_last_error);
	ClassDB::bind_method(D_METHOD("get_count"), &NovaWeaponDatabase::get_count);
	ClassDB::bind_method(D_METHOD("get_slot_weapons", "slot", "class_mask", "team_mask"),
			&NovaWeaponDatabase::get_slot_weapons);
	ClassDB::bind_method(D_METHOD("get_weapons"), &NovaWeaponDatabase::get_weapons);
	ClassDB::bind_method(D_METHOD("get_weapon", "index"), &NovaWeaponDatabase::get_weapon);

	BIND_CONSTANT(SLOT_ACCESSORY);
	BIND_CONSTANT(SLOT_PRIMARY);
	BIND_CONSTANT(SLOT_SECONDARY);
	BIND_CONSTANT(SLOT_GRENADE);
}

Error NovaWeaponDatabase::load(const String &path) {
	source_path = path;
	last_error = String();
	weapons.clear();

	DefWeaponsFile file = {};
	if (def_parse_weapons(path.utf8().get_data(), &file) != 0) {
		last_error = String("def_parse_weapons failed for ") + path;
		return ERR_CANT_OPEN;
	}
	for (size_t i = 0; i < file.count; ++i) {
		const DefWeaponDef &e = file.entries[i];
		Weapon w;
		w.name = String(e.weapon_name);
		w.display_textid = String(e.loadout_menu_textid);
		w.round_type = String(e.round_type);
		w.icon = String(e.loadout_menu_icon);
		w.selectable = e.loadout_selectable;
		w.slot = e.weapon_class_slot;
		w.team_mask = e.teamfilter_mask;
		w.class_mask = e.charfilter_mask;
		w.weight = e.weaponweight;
		w.clip_weight = e.clipweight;
		w.clipsize = e.clipsize;
		w.startrounds = e.startrounds;
		w.maxclips = e.maxclips;
		weapons.push_back(w);
	}
	def_free_weapons(&file);
	return OK;
}

Error NovaWeaponDatabase::load_from_resource_root(const Ref<NovaResourceRoot> &p_resource_root, const String &p_name) {
	last_error = String();
	weapons.clear();
	if (p_resource_root.is_null() || p_resource_root->get_root_dir().is_empty()) {
		last_error = "Resource root is not configured";
		return ERR_INVALID_PARAMETER;
	}
	const String file_name = p_name.get_file();
	if (file_name.is_empty()) {
		last_error = "Weapon database filename is empty";
		return ERR_INVALID_PARAMETER;
	}
	const PackedByteArray bytes = p_resource_root->read_file(file_name);
	if (bytes.is_empty()) {
		last_error = String("Weapon database not found in resource root: ") + file_name;
		return ERR_FILE_NOT_FOUND;
	}
	source_path = file_name;

	DefWeaponsFile file = {};
	if (def_parse_weapons_memory(bytes.ptr(), static_cast<size_t>(bytes.size()), &file) != 0) {
		last_error = String("def_parse_weapons_memory failed for ") + file_name;
		return ERR_CANT_OPEN;
	}
	for (size_t i = 0; i < file.count; ++i) {
		const DefWeaponDef &e = file.entries[i];
		Weapon w;
		w.name = String(e.weapon_name);
		w.display_textid = String(e.loadout_menu_textid);
		w.round_type = String(e.round_type);
		w.icon = String(e.loadout_menu_icon);
		w.selectable = e.loadout_selectable;
		w.slot = e.weapon_class_slot;
		w.team_mask = e.teamfilter_mask;
		w.class_mask = e.charfilter_mask;
		w.weight = e.weaponweight;
		w.clip_weight = e.clipweight;
		w.clipsize = e.clipsize;
		w.startrounds = e.startrounds;
		w.maxclips = e.maxclips;
		weapons.push_back(w);
	}
	def_free_weapons(&file);
	return OK;
}

bool NovaWeaponDatabase::is_loaded() const {
	return !weapons.empty();
}

String NovaWeaponDatabase::get_source_path() const {
	return source_path;
}

String NovaWeaponDatabase::get_last_error() const {
	return last_error;
}

int NovaWeaponDatabase::get_count() const {
	return static_cast<int>(weapons.size());
}

Dictionary NovaWeaponDatabase::weapon_dict(int index) const {
	Dictionary d;
	if (index < 0 || index >= static_cast<int>(weapons.size())) {
		return d;
	}
	const Weapon &w = weapons[index];
	d["index"] = index;
	d["name"] = w.name;
	d["display_textid"] = w.display_textid;
	d["round_type"] = w.round_type;
	d["icon"] = w.icon;
	d["selectable"] = w.selectable;
	d["slot"] = w.slot;
	d["team_mask"] = w.team_mask;
	d["class_mask"] = w.class_mask;
	d["weight"] = w.weight;
	d["clip_weight"] = w.clip_weight;
	d["clipsize"] = w.clipsize;
	d["startrounds"] = w.startrounds;
	d["maxclips"] = w.maxclips;
	return d;
}

Array NovaWeaponDatabase::get_slot_weapons(int slot, int class_mask, int team_mask) const {
	Array out;
	for (int i = 0; i < static_cast<int>(weapons.size()); ++i) {
		const Weapon &w = weapons[i];
		if (w.slot != slot) {
			continue;
		}
		// [orig: populate_weapon_slot_lists @ 0x560430] gate.
		if (w.selectable == 0) {
			continue;
		}
		if ((w.class_mask & class_mask) == 0) {
			continue;
		}
		if ((w.team_mask & team_mask) == 0) {
			continue;
		}
		out.push_back(weapon_dict(i));
	}
	return out;
}

Array NovaWeaponDatabase::get_weapons() const {
	Array out;
	for (int i = 0; i < static_cast<int>(weapons.size()); ++i) {
		out.push_back(weapon_dict(i));
	}
	return out;
}

Dictionary NovaWeaponDatabase::get_weapon(int index) const {
	return weapon_dict(index);
}
