#include "object/weapon_database.h"

#include "resource_index/resource_root.h"

#include <godot_cpp/variant/packed_float32_array.hpp>

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>

#include <formats/def/def.h>
#include <runtime/menu/player_info_kit.h> // the PLAYER_INFO voice list + kit page order
#include <runtime/world/player_loadout.h> // armory class policy (ADR 0016: one impl)

#include <base/io/strutil.h>

#include <algorithm>
#include <vector>

using namespace godot;
using namespace opennova::def;

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
			D_METHOD("player_info_voice_values", "sex"),
			&WeaponDatabase::player_info_voice_values);
	ClassDB::bind_static_method("WeaponDatabase",
			D_METHOD("player_info_voice_selection", "saved", "values"),
			&WeaponDatabase::player_info_voice_selection);
	ClassDB::bind_method(D_METHOD("player_info_kit_entries", "team", "player_class", "slot_indices",
								 "slot_ammo_primary", "slot_ammo_secondary", "slot_flags",
								 "grenade_indices", "grenade_ammo_primary", "grenade_ammo_secondary"),
			&WeaponDatabase::player_info_kit_entries);
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
	BIND_CONSTANT(FLAG2_NOAMMOTYPES);
	BIND_CONSTANT(ENCUMBRANCE_LIGHT);
	BIND_CONSTANT(ENCUMBRANCE_NORMAL);
	BIND_CONSTANT(ENCUMBRANCE_HEAVY);
	BIND_CONSTANT(CLIP_COUNT_DEF_DEFAULT);
	BIND_CONSTANT(DEFAULT_VOICE_VALUE);
	BIND_CONSTANT(CLASS_ALLOW_ALL);
	BIND_CONSTANT(CLASS_MASK_ALL);
}

WeaponDatabase::~WeaponDatabase() {
	release_native_weapons();
}

void WeaponDatabase::release_native_weapons() {
	if (weapons_file_loaded_) {
		def_free_weapons(&weapons_file_);
	}
	weapons_file_ = {};
	weapons_file_loaded_ = false;
}

Error WeaponDatabase::load(const String &path) {
	source_path = path;
	last_error = String();
	release_native_weapons();

	if (def_parse_weapons(path.utf8().get_data(), &weapons_file_) != 0) {
		last_error = String("def_parse_weapons failed for ") + path;
		return ERR_CANT_OPEN;
	}
	weapons_file_loaded_ = true;
	return OK;
}

Error WeaponDatabase::load_from_resource_root(const Ref<ResourceRoot> &p_resource_root, const String &p_name) {
	last_error = String();
	release_native_weapons();
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

	if (def_parse_weapons_memory(bytes.ptr(), static_cast<size_t>(bytes.size()), &weapons_file_) != 0) {
		last_error = String("def_parse_weapons_memory failed for ") + file_name;
		return ERR_CANT_OPEN;
	}
	weapons_file_loaded_ = true;
	return OK;
}

bool WeaponDatabase::is_loaded() const {
	return weapons_file_loaded_ && weapons_file_.count > 0;
}

String WeaponDatabase::get_source_path() const {
	return source_path;
}

String WeaponDatabase::get_last_error() const {
	return last_error;
}

int WeaponDatabase::get_count() const {
	return static_cast<int>(weapons_file_.count);
}

int WeaponDatabase::find_weapon(const String &p_name) const {
	const CharString wanted = p_name.utf8();
	for (size_t i = 0; i < weapons_file_.count; ++i) {
		if (opennova::strutil::iequals(weapons_file_.entries[i].weapon_name, wanted.get_data())) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

TypedArray<WeaponDef> WeaponDatabase::get_slot_weapons(int slot, int class_mask, int team_mask) const {
	std::vector<int32_t> indices;
	opennova::world::weapon_slot_indices(weapons_file_.entries, weapons_file_.count,
			slot, class_mask, team_mask, indices);
	TypedArray<WeaponDef> out;
	for (const int32_t i : indices) {
		out.push_back(get_weapon(i));
	}
	return out;
}

Ref<WeaponDef> WeaponDatabase::get_weapon(int index) const {
	const DefWeaponDef *w = row(index);
	if (w == nullptr) {
		return Ref<WeaponDef>();
	}
	Ref<WeaponDef> def;
	def.instantiate();
	def->assign(index, *w);
	return def;
}

double WeaponDatabase::loadout_weight(const PackedInt32Array &weapon_indices,
		const PackedInt32Array &ammo_counts) const {
	// def_loadout_weight walks one contiguous row array; the selected rows are
	// gathered by value (a struct copy, the parse stays the owner of its arrays).
	std::vector<DefWeaponDef> defs;
	std::vector<int> counts;
	defs.reserve(weapon_indices.size());
	counts.reserve(weapon_indices.size());
	for (int i = 0; i < weapon_indices.size(); ++i) {
		const DefWeaponDef *w = row(weapon_indices[i]);
		if (w == nullptr) {
			continue;
		}
		defs.push_back(*w);
		counts.push_back(i < ammo_counts.size() ? ammo_counts[i] : -1);
	}
	return def_loadout_weight(defs.data(), counts.data(), defs.size());
}

double WeaponDatabase::extra_ammo_weight(int p_index, int p_count) const {
	const DefWeaponDef *w = row(p_index);
	if (w == nullptr) {
		return 0.0;
	}
	return def_extra_ammo_weight(w, p_count);
}

int WeaponDatabase::subclass_weapon_index(int p_parent_index) const {
	// The parent's loadout_subclasses window is contiguous in the retained
	// table, so the engine walks the rows in place.
	const DefWeaponDef *parent = row(p_parent_index);
	if (parent == nullptr) {
		return -1;
	}
	const size_t start = static_cast<size_t>(p_parent_index);
	const int subclasses = std::max(parent->loadout_subclasses, 0);
	const size_t end = std::min(weapons_file_.count, start + static_cast<size_t>(subclasses) + 1);
	const int found = def_subclass_weapon_index(weapons_file_.entries + start, end - start, 0);
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

static_assert(godot::WeaponDatabase::DEFAULT_VOICE_VALUE == opennova::menu::kDefaultVoiceValue,
		"DEFAULT_VOICE_VALUE mirrors the engine's PLAYERVOICE default");

PackedInt32Array WeaponDatabase::player_info_voice_values(int p_sex) {
	PackedInt32Array out;
	for (int32_t value : opennova::menu::player_info_voice_values(p_sex)) out.push_back(value);
	return out;
}

int WeaponDatabase::player_info_voice_selection(int p_saved, const PackedInt32Array &p_values) {
	std::vector<int32_t> values;
	values.reserve(static_cast<size_t>(p_values.size()));
	for (int i = 0; i < p_values.size(); ++i) values.push_back(p_values[i]);
	return opennova::menu::player_info_voice_selection(p_saved, values);
}

Array WeaponDatabase::player_info_kit_entries(int p_team, int p_player_class,
		const PackedInt32Array &p_slot_indices, const PackedInt32Array &p_slot_ammo_primary,
		const PackedInt32Array &p_slot_ammo_secondary, const PackedInt32Array &p_slot_flags,
		const PackedInt32Array &p_grenade_indices, const PackedInt32Array &p_grenade_ammo_primary,
		const PackedInt32Array &p_grenade_ammo_secondary) const {
	const auto pick = [](const PackedInt32Array &idx, const PackedInt32Array &pri,
							  const PackedInt32Array &sec, const PackedInt32Array *flags,
							  int i) {
		opennova::menu::KitSlotPick p;
		if (i < idx.size()) p.weapon_index = idx[i];
		if (i < pri.size()) p.ammo_primary = pri[i];
		if (i < sec.size()) p.ammo_secondary = sec[i];
		if (flags != nullptr && i < flags->size()) p.flags = (*flags)[i];
		return p;
	};
	opennova::menu::PlayerInfoKitSelection sel;
	sel.team_mask = opennova::world::player_info_team_mask(p_team);
	sel.player_class = p_player_class;
	sel.primary = pick(p_slot_indices, p_slot_ammo_primary, p_slot_ammo_secondary, &p_slot_flags, 0);
	sel.secondary = pick(p_slot_indices, p_slot_ammo_primary, p_slot_ammo_secondary, &p_slot_flags, 1);
	sel.accessory = pick(p_slot_indices, p_slot_ammo_primary, p_slot_ammo_secondary, &p_slot_flags, 2);
	for (int i = 0; i < 3; ++i) {
		sel.grenades[i] = pick(p_grenade_indices, p_grenade_ammo_primary, p_grenade_ammo_secondary,
				nullptr, i);
	}
	const opennova::menu::WeaponNameLookup name = [this](int32_t index) -> std::string {
		const opennova::def::DefWeaponDef *w = row(index);
		return w != nullptr ? std::string(w->weapon_name) : std::string();
	};
	Array out;
	for (const opennova::playersav::KitEntry &e : opennova::menu::player_info_kit_entries(sel, name)) {
		Dictionary d;
		d["name"] = String::utf8(e.name.c_str());
		d["ammo_primary"] = e.ammo_primary;
		d["ammo_secondary"] = e.ammo_secondary;
		d["flags"] = e.flags;
		out.push_back(d);
	}
	return out;
}

int WeaponDatabase::armory_resolve_selected_class(int p_player_class,
		int p_class_allow_mask) {
	return opennova::world::armory_resolve_selected_class(p_player_class,
			static_cast<uint32_t>(p_class_allow_mask) & 0xFFFFu);
}

TypedArray<ArmoryClassRow> WeaponDatabase::armory_class_catalog() {
	// The engine table (world/player_loadout.h kArmoryClassCatalog) is the
	// one authored spin order; this only marshals rows.
	TypedArray<ArmoryClassRow> rows;
	for (int i = 0; i < opennova::world::kArmoryClassCount; ++i) {
		const opennova::world::ArmoryClassCatalogEntry &entry =
				opennova::world::kArmoryClassCatalog[i];
		Ref<ArmoryClassRow> row;
		row.instantiate();
		row->assign(entry.class_value, entry.text_key);
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
