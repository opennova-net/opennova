#include "object/weapon_database.h"

#include "resource_index/resource_root.h"

#include <godot_cpp/variant/packed_float32_array.hpp>

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>

#include "rtxt/rtxt_string_file.h" // the string-table document + the game_text_lookup factory
#include "util/string_convert.h"

#include <formats/def/def.h>
#include <runtime/menu/loadout_labels.h> // the loadout screens' labels, row order and weight line
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
	ClassDB::bind_method(D_METHOD("encumbrance_class", "weight"),
			&WeaponDatabase::encumbrance_class);
	ClassDB::bind_static_method("WeaponDatabase",
			D_METHOD("player_info_team_mask", "team"),
			&WeaponDatabase::player_info_team_mask);
	ClassDB::bind_static_method("WeaponDatabase",
			D_METHOD("player_info_class_mask", "playerclass_value"),
			&WeaponDatabase::player_info_class_mask);
	ClassDB::bind_method(D_METHOD("player_info_kit_entries", "team", "player_class", "slot_indices",
								 "slot_ammo_primary", "slot_ammo_secondary", "slot_flags",
								 "grenade_indices", "grenade_ammo_primary", "grenade_ammo_secondary"),
			&WeaponDatabase::player_info_kit_entries);
	ClassDB::bind_method(D_METHOD("weapon_label", "index", "gametext"),
			&WeaponDatabase::weapon_label);
	ClassDB::bind_static_method("WeaponDatabase", D_METHOD("armory_slot_order", "labels"),
			&WeaponDatabase::armory_slot_order);
	ClassDB::bind_static_method("WeaponDatabase",
			D_METHOD("loadout_weight_line", "total", "menutxt", "gameui"),
			&WeaponDatabase::loadout_weight_line);
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

	// A loose file read with no mount: the parser takes every named file as
	// present (DefFileProbe), so a SIGHTS, crosshair or HUD-graphic row whose
	// texture the game lacks is kept here; the game's own load is
	// load_from_resource_root, which probes the mount.
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
		if (!p_resource_root->has_file(file_name)) {
			last_error = String("Weapon database not found in resource root: ") + file_name;
			return ERR_FILE_NOT_FOUND;
		}
		// A present, zero-byte weapon.def: the walk finds no line, so the
		// table loads empty.
		source_path = file_name;
		weapons_file_loaded_ = true;
		return OK;
	}
	source_path = file_name;

	// A SIGHTS row whose texture the resource root lacks is no row (the def
	// parser's sights arm, def_weapons.cpp, cites the original).
	const DefFileProbe probe = {
			[](const void *ctx, const char *name) {
				return static_cast<const ResourceRoot *>(ctx)->has_file(String::utf8(name));
			},
			p_resource_root.ptr()};
	if (def_parse_weapons_memory(bytes.ptr(), static_cast<size_t>(bytes.size()), &weapons_file_,
				nullptr, &probe) != 0) {
		last_error = String("def_parse_weapons_memory failed for ") + file_name;
		return ERR_CANT_OPEN;
	}
	weapons_file_loaded_ = true;
	return OK;
}

bool WeaponDatabase::is_loaded() const {
	return weapons_file_loaded_;
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

int WeaponDatabase::player_info_team_mask(int p_team) {
	return opennova::world::player_info_team_mask(p_team);
}

int WeaponDatabase::player_info_class_mask(int p_playerclass_value) {
	return opennova::world::player_info_class_mask(p_playerclass_value);
}

String WeaponDatabase::weapon_label(int p_index, const Ref<RtxtStringFile> &p_gametext) const {
	const opennova::def::DefWeaponDef *w = row(p_index);
	if (w == nullptr) return String();
	return opennova::to_gd(opennova::menu::weapon_label(*w, game_text_lookup(p_gametext)));
}

PackedInt32Array WeaponDatabase::armory_slot_order(const PackedStringArray &p_labels) {
	std::vector<std::string> labels;
	labels.reserve(static_cast<size_t>(p_labels.size()));
	for (int i = 0; i < p_labels.size(); ++i) labels.push_back(opennova::to_std(p_labels[i]));
	PackedInt32Array out;
	for (int index : opennova::menu::armory_slot_order(labels)) out.push_back(index);
	return out;
}

String WeaponDatabase::loadout_weight_line(double p_total, const Ref<RtxtStringFile> &p_menutxt,
		const Ref<RtxtStringFile> &p_gameui) {
	return opennova::to_gd(opennova::menu::loadout_weight_line(p_total,
			game_text_lookup(p_menutxt), game_text_lookup(p_gameui)));
}

static_assert(godot::WeaponDatabase::DEFAULT_VOICE_VALUE == opennova::menu::kDefaultVoiceValue,
		"DEFAULT_VOICE_VALUE mirrors the engine's PLAYERVOICE default");

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
		d["name"] = opennova::to_gd(e.name);
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
