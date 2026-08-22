#include "nova_weapon_database.h"

#include "resource_index/nova_resource_root.h"

#include <godot_cpp/variant/packed_float32_array.hpp>

#include <def/def.h>
#include <world/player_loadout.h> // armory class policy (ADR 0016: one impl)

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace godot;

static_assert(WeaponDatabase::FLAG_EMPLACED == DEF_WEAPON_FLAG_EMPLACED,
              "FLAG_EMPLACED drifted from def.h");
static_assert(WeaponDatabase::FLAG2_NOAMMOTYPES == DEF_WEAPON_FLAG2_NOAMMOTYPES,
              "FLAG2_NOAMMOTYPES drifted from def.h");
static_assert(WeaponDatabase::ENCUMBRANCE_LIGHT == DEF_ENCUMBRANCE_LIGHT &&
              WeaponDatabase::ENCUMBRANCE_NORMAL == DEF_ENCUMBRANCE_NORMAL &&
              WeaponDatabase::ENCUMBRANCE_HEAVY == DEF_ENCUMBRANCE_HEAVY,
              "ENCUMBRANCE_* drifted from def.h");
static_assert(WeaponDatabase::CLASS_ALLOW_ALL ==
                      static_cast<int>(opennova::world::kClassAllowMaskAll),
              "CLASS_ALLOW_ALL drifted from world/player_loadout.h");
static_assert(WeaponDatabase::CLASS_MASK_ALL ==
                      opennova::world::kClassFilterMaskAll,
              "CLASS_MASK_ALL drifted from world/player_loadout.h");

void WeaponDatabase::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load", "path"), &WeaponDatabase::load);
	ClassDB::bind_method(D_METHOD("load_from_resource_root", "resource_root", "name"),
			&WeaponDatabase::load_from_resource_root);
	ClassDB::bind_method(D_METHOD("is_loaded"), &WeaponDatabase::is_loaded);
	ClassDB::bind_method(D_METHOD("get_source_path"), &WeaponDatabase::get_source_path);
	ClassDB::bind_method(D_METHOD("get_last_error"), &WeaponDatabase::get_last_error);
	ClassDB::bind_method(D_METHOD("get_count"), &WeaponDatabase::get_count);
	ClassDB::bind_method(D_METHOD("get_slot_weapons", "slot", "class_mask", "team_mask"),
			&WeaponDatabase::get_slot_weapons);
	ClassDB::bind_method(D_METHOD("get_weapons"), &WeaponDatabase::get_weapons);
	ClassDB::bind_method(D_METHOD("get_weapon", "index"), &WeaponDatabase::get_weapon);
	ClassDB::bind_method(D_METHOD("find_weapon", "name"), &WeaponDatabase::find_weapon);
	ClassDB::bind_method(D_METHOD("loadout_weight", "weapon_indices", "ammo_counts"),
			&WeaponDatabase::loadout_weight);
	ClassDB::bind_method(D_METHOD("extra_ammo_weight", "index", "count"),
			&WeaponDatabase::extra_ammo_weight);
	ClassDB::bind_method(D_METHOD("subclass_weapon_index", "parent_index"),
			&WeaponDatabase::subclass_weapon_index);
	ClassDB::bind_method(D_METHOD("encumbrance_class", "weight"),
			&WeaponDatabase::encumbrance_class);
	ClassDB::bind_static_method("WeaponDatabase",
			D_METHOD("player_info_team_mask", "team"),
			&WeaponDatabase::player_info_team_mask);
	ClassDB::bind_static_method("WeaponDatabase",
			D_METHOD("player_info_class_mask", "playerclass_value"),
			&WeaponDatabase::player_info_class_mask);
	ClassDB::bind_static_method("WeaponDatabase",
			D_METHOD("default_clip_row", "saved", "maxclips"),
			&WeaponDatabase::default_clip_row);
	ClassDB::bind_static_method("WeaponDatabase",
			D_METHOD("armory_resolve_selected_class", "player_class", "class_allow_mask"),
			&WeaponDatabase::armory_resolve_selected_class);
	ClassDB::bind_static_method("WeaponDatabase",
			D_METHOD("armory_class_filter_mask", "selected_class"),
			&WeaponDatabase::armory_class_filter_mask);
	ClassDB::bind_static_method("WeaponDatabase",
			D_METHOD("armory_class_catalog"),
			&WeaponDatabase::armory_class_catalog);

	BIND_CONSTANT(SLOT_ACCESSORY);
	BIND_CONSTANT(SLOT_PRIMARY);
	BIND_CONSTANT(SLOT_SECONDARY);
	BIND_CONSTANT(SLOT_GRENADE);
	BIND_CONSTANT(FLAG_EMPLACED);
	BIND_CONSTANT(FLAG2_NOAMMOTYPES);
	BIND_CONSTANT(ENCUMBRANCE_LIGHT);
	BIND_CONSTANT(ENCUMBRANCE_NORMAL);
	BIND_CONSTANT(ENCUMBRANCE_HEAVY);
	BIND_CONSTANT(CLIP_COUNT_DEF_DEFAULT);
	BIND_CONSTANT(CLASS_ALLOW_ALL);
	BIND_CONSTANT(CLASS_MASK_ALL);
}

Error WeaponDatabase::load(const String &path) {
	source_path = path;
	last_error = String();
	weapons.clear();

	DefWeaponsFile file = {};
	if (def_parse_weapons(path.utf8().get_data(), &file) != 0) {
		last_error = String("def_parse_weapons failed for ") + path;
		return ERR_CANT_OPEN;
	}
	for (size_t i = 0; i < file.count; ++i) {
		append_entry(file.entries[i]);
	}
	def_free_weapons(&file);
	return OK;
}

Error WeaponDatabase::load_from_resource_root(const Ref<ResourceRoot> &p_resource_root, const String &p_name) {
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
		append_entry(file.entries[i]);
	}
	def_free_weapons(&file);
	return OK;
}

void WeaponDatabase::append_entry(const DefWeaponDef &e) {
	Weapon w;
	w.name = String(e.weapon_name);
	w.display_textid = String(e.loadout_menu_textid);
	w.round_type = String(e.round_type);
	w.icon = String(e.loadout_menu_icon);
	w.selectable = e.loadout_selectable;
	w.loadout_subclasses = e.loadout_subclasses;
	w.slot = e.weapon_class_slot;
	w.team_mask = e.teamfilter_mask;
	w.class_mask = e.charfilter_mask;
	w.weight = e.weaponweight;
	w.clip_weight = e.clipweight;
	w.clipsize = e.clipsize;
	w.startrounds = e.startrounds;
	w.maxclips = e.maxclips;
	w.animadm = String(e.animadm);
	w.gfx1 = String(e.gfx1);
	w.gfx3 = String(e.gfx3);
	for (int k = 0; k < 6; ++k) {
		w.pos[k] = e.pos[k];
		w.tpos[k] = e.tpos[k];
	}
	w.renderfov = e.renderfov;
	w.flags = e.flags;
	w.flags2 = e.flags2;
	w.scope_max_mag = e.scope_max_mag;
	w.special_hold = e.special_hold;
	w.attack_anim = e.attack_anim;
	w.run_anim = e.run_anim;
	w.heat_per_shot = e.heat_per_shot;
	w.heat_decay_per_tick = e.heat_decay_per_tick;
	w.heat_glow_threshold = e.heat_glow_threshold;
	w.heat_effect = String(e.heat_effect);
	for (int k = 0; k < 6; ++k) {
		w.error[k] = e.error[k];
	}
	w.hudclipgfx_texture = String(e.hudclipgfx_texture);
	w.hudclipgfx_offset[0] = e.hudclipgfx_offset[0];
	w.hudclipgfx_offset[1] = e.hudclipgfx_offset[1];
	w.hudrndgfx_texture = String(e.hudrndgfx_texture);
	w.hudrndgfx_offset[0] = e.hudrndgfx_offset[0];
	w.hudrndgfx_offset[1] = e.hudrndgfx_offset[1];
	for (int k = 0; k < 3; ++k) {
		w.hudrndgfx_layout[k] = e.hudrndgfx_layout[k];
	}
	// The standard SIGHTS card rows, authored draw order preserved. The frame's
	// dynamic Scoped/Sighted/NoCardSwitch selector lives in the simulation; row
	// presence supplies card contents rather than selecting the card.
	// [orig: rows at the weapon record +0x1C8 (stride 36, count +0x258) drawn by
	// draw_weapon_sight_overlays @ 0x4dce00]
	w.sights.clear();
	for (size_t si = 0; si < e.sights_count; ++si) {
		const DefSightEntry &se = e.sights[si];
		Dictionary row;
		row["texture"] = String(se.texture);
		row["x1"] = se.x1;
		row["y1"] = se.y1;
		row["x2"] = se.x2;
		row["y2"] = se.y2;
		row["blend"] = se.blend; // 0=Blend 1=Add 2=BlendAt
		row["scale"] = se.scale != 0;
		row["slide"] = se.slide != 0;
		row["slide_frames"] = se.slide_frames;
		w.sights.push_back(row);
	}
	for (size_t a = 0; a < e.actions_count; ++a) {
		const DefWeaponAction &row = e.actions[a];
		Dictionary act;
		act["name"] = String(row.name);
		act["anim"] = String(row.anim);
		act["function"] = String(row.function);
		act["delaystart"] = row.delaystart;
		act["delayend"] = row.delayend;
		// Per-ACTION audio/effect hooks: the GF_* sound set played on action
		// start/end and the particle effect spawned at the model user point
		// [orig: ActionSlot fields consumed by ActionSlot_SpawnEffect
		// @ 0x401f20]. Dropping these severed weapon-fire sound and
		// muzzle-flash from every consumer.
		act["soundset"] = String(row.soundset);
		act["soundsetend"] = String(row.soundsetend);
		act["particle"] = String(row.particle);
		act["particleuserpoint"] = String(row.particleuserpoint);
		w.actions.push_back(act);
	}
	weapons.push_back(w);
}

bool WeaponDatabase::is_loaded() const {
	return !weapons.empty();
}

String WeaponDatabase::get_source_path() const {
	return source_path;
}

String WeaponDatabase::get_last_error() const {
	return last_error;
}

int WeaponDatabase::get_count() const {
	return static_cast<int>(weapons.size());
}

Dictionary WeaponDatabase::weapon_dict(int index) const {
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
	d["loadout_subclasses"] = w.loadout_subclasses; // (+36) the *_AMMO2 walk bound
	d["slot"] = w.slot;
	d["team_mask"] = w.team_mask;
	d["class_mask"] = w.class_mask;
	d["weight"] = w.weight;
	d["clip_weight"] = w.clip_weight;
	d["clipsize"] = w.clipsize;
	d["startrounds"] = w.startrounds;
	d["maxclips"] = w.maxclips;
	d["animadm"] = w.animadm;
	d["gfx1"] = w.gfx1;
	d["gfx3"] = w.gfx3;
	PackedFloat32Array pos;
	PackedFloat32Array tpos;
	for (int k = 0; k < 6; ++k) {
		pos.push_back(w.pos[k]);
		tpos.push_back(w.tpos[k]);
	}
	d["pos"] = pos;   // xyz raw file units (/256 = world), then yaw/pitch/roll degrees
	d["tpos"] = tpos; // the ADS variant [orig: WeaponDef.CamOffsetTpos @ 0x124]
	d["renderfov"] = w.renderfov;
	d["flags"] = w.flags;
	d["flags2"] = w.flags2;
	d["scope_max_mag"] = w.scope_max_mag;
	d["sights"] = w.sights.duplicate(true);
	// The 3P body-channel kinds [orig: weapon.def special_hold/attack_anim ->
	// AdmDefs +0xA4/+0xA8; world-wac-ai-re.md section 14.8].
	d["special_hold"] = w.special_hold;
	d["attack_anim"] = w.attack_anim;
	// The run-gait class [orig: 'run_anim' -> AdmDefs +0xAC; promotion @ 0x4b729d].
	d["run_anim"] = w.run_anim;
	// The weapon heat model [orig: weapon.def 'heat_values'/'heat_effect' ->
	// WeaponDef +0x36C/+0x370/+0x374/+0x358; docs/net/novaworld-net-re.md §5.62].
	d["heat_per_shot"] = w.heat_per_shot;
	d["heat_decay_per_tick"] = w.heat_decay_per_tick;
	d["heat_glow_threshold"] = w.heat_glow_threshold;
	d["heat_effect"] = w.heat_effect;
	PackedFloat32Array error;
	for (int k = 0; k < 6; ++k) {
		error.push_back(w.error[k]);
	}
	d["error"] = error; // degrees; rows hip P/C/S then scoped P/C/S [orig: weapon+0xB0]
	d["hudclipgfx_texture"] = w.hudclipgfx_texture;
	d["hudclipgfx_offset"] = Vector2i(w.hudclipgfx_offset[0], w.hudclipgfx_offset[1]);
	d["hudrndgfx_texture"] = w.hudrndgfx_texture;
	d["hudrndgfx_offset"] = Vector2i(w.hudrndgfx_offset[0], w.hudrndgfx_offset[1]);
	d["hudrndgfx_layout"] = Vector3i(w.hudrndgfx_layout[0], w.hudrndgfx_layout[1], w.hudrndgfx_layout[2]);
	d["actions"] = w.actions.duplicate(true);
	return d;
}

int WeaponDatabase::find_weapon(const String &p_name) const {
	for (int i = 0; i < static_cast<int>(weapons.size()); ++i) {
		if (weapons[i].name.nocasecmp_to(p_name) == 0) {
			return i;
		}
	}
	return -1;
}

Array WeaponDatabase::get_slot_weapons(int slot, int class_mask, int team_mask) const {
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

Array WeaponDatabase::get_weapons() const {
	Array out;
	for (int i = 0; i < static_cast<int>(weapons.size()); ++i) {
		out.push_back(weapon_dict(i));
	}
	return out;
}

Dictionary WeaponDatabase::get_weapon(int index) const {
	return weapon_dict(index);
}

double WeaponDatabase::loadout_weight(const PackedInt32Array &weapon_indices,
		const PackedInt32Array &ammo_counts) const {
	// def_loadout_weight reads only weaponweight/maxclips/clipweight, so temp
	// records carrying just those fields forward the stored rows faithfully.
	std::vector<DefWeaponDef> defs;
	std::vector<int> counts;
	defs.reserve(weapon_indices.size());
	counts.reserve(weapon_indices.size());
	for (int i = 0; i < weapon_indices.size(); ++i) {
		const int index = weapon_indices[i];
		if (index < 0 || index >= static_cast<int>(weapons.size())) {
			continue;
		}
		const Weapon &w = weapons[index];
		DefWeaponDef d = {};
		d.weaponweight = w.weight;
		d.maxclips = w.maxclips;
		d.clipweight = w.clip_weight;
		defs.push_back(d);
		counts.push_back(i < ammo_counts.size() ? ammo_counts[i] : -1);
	}
	return def_loadout_weight(defs.data(), counts.data(), defs.size());
}

double WeaponDatabase::extra_ammo_weight(int p_index, int p_count) const {
	if (p_index < 0 || p_index >= static_cast<int>(weapons.size())) {
		return 0.0;
	}
	const Weapon &w = weapons[static_cast<size_t>(p_index)];
	DefWeaponDef d = {};
	d.maxclips = w.maxclips;
	d.clipweight = w.clip_weight;
	return def_extra_ammo_weight(&d, p_count);
}

int WeaponDatabase::subclass_weapon_index(int p_parent_index) const {
	// def_subclass_weapon_index reads only round_type/loadout_subclasses, so a
	// temp slice of the parent's window forwards the stored rows faithfully.
	if (p_parent_index < 0 || p_parent_index >= static_cast<int>(weapons.size())) {
		return -1;
	}
	const size_t parent = static_cast<size_t>(p_parent_index);
	const int subclasses = std::max(weapons[parent].loadout_subclasses, 0);
	const size_t end = std::min(weapons.size(), parent + static_cast<size_t>(subclasses) + 1);
	std::vector<DefWeaponDef> defs(end - parent);
	for (size_t i = 0; i < defs.size(); ++i) {
		const Weapon &w = weapons[parent + i];
		defs[i].loadout_subclasses = w.loadout_subclasses;
		const CharString round_type = w.round_type.utf8();
		std::snprintf(defs[i].round_type, sizeof defs[i].round_type, "%s",
				round_type.get_data());
	}
	const int found = def_subclass_weapon_index(defs.data(), defs.size(), 0);
	return found < 0 ? -1 : p_parent_index + found;
}

int WeaponDatabase::player_info_team_mask(int p_team) {
	return opennova::world::player_info_team_mask(p_team);
}

int WeaponDatabase::player_info_class_mask(int p_playerclass_value) {
	return opennova::world::player_info_class_mask(p_playerclass_value);
}

int WeaponDatabase::default_clip_row(int p_saved, int p_maxclips) {
	return opennova::world::player_info_default_clip_row(p_saved, p_maxclips);
}

int WeaponDatabase::armory_resolve_selected_class(int p_player_class,
		int p_class_allow_mask) {
	return opennova::world::armory_resolve_selected_class(p_player_class,
			static_cast<uint32_t>(p_class_allow_mask) & 0xFFFFu);
}

Array WeaponDatabase::armory_class_catalog() {
	// The engine table (world/player_loadout.h kArmoryClassCatalog) is the
	// one authored spin order; this only marshals rows.
	Array rows;
	for (int i = 0; i < opennova::world::kArmoryClassCount; ++i) {
		const opennova::world::ArmoryClassCatalogEntry &entry =
				opennova::world::kArmoryClassCatalog[i];
		Dictionary row;
		row["value"] = entry.class_value;
		row["text_key"] = String(entry.text_key);
		rows.push_back(row);
	}
	return rows;
}

int WeaponDatabase::armory_class_filter_mask(int p_selected_class) {
	return opennova::world::armory_class_filter_mask(p_selected_class);
}

int WeaponDatabase::encumbrance_class(double weight) const {
	return static_cast<int>(def_encumbrance_class(weight));
}
