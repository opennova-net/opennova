#include "object/weapon_database.h"

#include "resource_index/resource_root.h"

#include <godot_cpp/variant/packed_float32_array.hpp>

#include <formats/def/def.h>
#include <runtime/world/player_loadout.h> // armory class policy (ADR 0016: one impl)

#include <base/io/strutil.h>

#include <algorithm>
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
	BIND_CONSTANT(FLAG2_NOAMMOTYPES);
	BIND_CONSTANT(ENCUMBRANCE_LIGHT);
	BIND_CONSTANT(ENCUMBRANCE_NORMAL);
	BIND_CONSTANT(ENCUMBRANCE_HEAVY);
	BIND_CONSTANT(CLIP_COUNT_DEF_DEFAULT);
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

Dictionary WeaponDatabase::weapon_dict(int index) const {
	Dictionary d;
	const DefWeaponDef *w = row(index);
	if (w == nullptr) {
		return d;
	}
	d["index"] = index;
	d["name"] = String(w->weapon_name);                 // weapon "<id>", the display fallback
	d["display_textid"] = String(w->loadout_menu_textid); // GameText "WepDes" key
	d["round_type"] = String(w->round_type);            // ammo key (GameText "WepDes")
	d["icon"] = String(w->loadout_menu_icon);
	d["selectable"] = w->loadout_selectable;
	d["loadout_subclasses"] = w->loadout_subclasses;    // (+36) the *_AMMO2 walk bound
	d["slot"] = w->weapon_class_slot;                   // 0 accessory 1 primary 2 secondary 3 grenade
	d["team_mask"] = w->teamfilter_mask;                // blue/yellow=2, red/violet=1
	d["class_mask"] = w->charfilter_mask;               // medic1 sniper2 gunner4 rifleman8 engineer16
	d["weight"] = w->weaponweight;
	d["clip_weight"] = w->clipweight;
	d["clipsize"] = w->clipsize;
	d["startrounds"] = w->startrounds;
	d["maxclips"] = w->maxclips;
	// First-person viewmodel slice [orig: WeaponDef_ParseProperty @0x54d730
	// rows; consumer Player_RenderFirstPersonViewModel @0x4ded60]: the FP gun
	// model (gfx1), the 3P model (gfx3), the shared animation set (animadm),
	// the hip/ADS view biases (pos/tpos: xyz raw file units + yaw/pitch/roll
	// degrees), and renderfov (horizontal degrees, record default 80.0 — no
	// shipped JO def sets it). gfx1a/gfx1b are recognized-and-DISCARDED
	// tokens [orig: WeaponDefs_ParseLineCallback @0x5448d0 -> xor eax; the
	// FP arms are the character combo's, see docs/playerinfo/avatars-re.md].
	d["animadm"] = String(w->animadm);
	d["gfx1"] = String(w->gfx1);
	d["gfx3"] = String(w->gfx3);
	PackedFloat32Array pos;
	PackedFloat32Array tpos;
	for (int k = 0; k < 6; ++k) {
		pos.push_back(w->pos[k]);
		tpos.push_back(w->tpos[k]);
	}
	d["pos"] = pos;   // xyz raw file units (/256 = world), then yaw/pitch/roll degrees
	d["tpos"] = tpos; // the ADS variant [orig: WeaponDef.CamOffsetTpos @ 0x124]
	d["renderfov"] = w->renderfov;
	// The witnessed WeaponDef+8 flag mask (engine/formats/def flag_table maps the file
	// tokens: scoped 1, sighted 2, burst 0x20, auto 0x100, ...) and the ADS zoom
	// magnification [orig: Player_ToggleWeaponScope @ 0x4df0c0 gates Flags & 3;
	// scoped FOV = 80 / zoom @ 0x4df401]; flags2 is the second FLAGS dword
	// (Inset 0x200 = the 7-step ADS ease).
	d["flags"] = w->flags;
	d["flags2"] = w->flags2;
	d["scope_max_mag"] = w->scope_max_mag;
	// The standard SIGHTS card rows, authored draw order preserved. The frame's
	// dynamic Scoped/Sighted/NoCardSwitch selector lives in the simulation; row
	// presence supplies card contents rather than selecting the card.
	// [orig: rows at the weapon record +0x1C8 (stride 36, count +0x258) drawn by
	// draw_weapon_sight_overlays @ 0x4dce00]
	Array sights;
	for (size_t si = 0; si < w->sights_count; ++si) {
		const DefSightEntry &se = w->sights[si];
		Dictionary row;
		row["texture"] = String(se.texture);
		row["x1"] = se.x1;
		row["y1"] = se.y1;
		row["x2"] = se.x2;
		row["y2"] = se.y2;
		row["blend"] = se.blend; // DefSightBlendMode transport value.
		row["scale"] = se.scale != 0;
		row["slide"] = se.slide != 0;
		row["slide_frames"] = se.slide_frames;
		sights.push_back(row);
	}
	d["sights"] = sights;
	// The 3P body-channel kinds (0 = absent, rifle): special_hold 1..8 picks the
	// body hold-pose ladder 50-61 (2 also selects reload2), attack_anim 1/2 stamps
	// 62/63 on fire [orig: AdmDefs +0xA4/+0xA8, read @ 0x4b5dba / @ 0x542bbc;
	// world-wac-ai-re.md section 14.8].
	d["special_hold"] = w->special_hold;
	d["attack_anim"] = w->attack_anim;
	// The run-gait class: the forward-walk promotion adds this to the constant
	// pitch tier 2 to pick run_2/run_3 [orig: 'run_anim' -> AdmDefs +0xAC; @ 0x4b729d].
	d["run_anim"] = w->run_anim;
	// The heat model, in the def's pre-divided 16.16 units (0 = the weapon
	// authors no heat) [orig: weapon.def 'heat_values'/'heat_effect' ->
	// WeaponDef +0x36C/+0x370/+0x374/+0x358; docs/net/novaworld-net-re.md §5.62].
	d["heat_per_shot"] = w->heat_per_shot;
	d["heat_decay_per_tick"] = w->heat_decay_per_tick;
	d["heat_glow_threshold"] = w->heat_glow_threshold;
	d["heat_effect"] = String(w->heat_effect);
	// HUD weapon-coupled slice (docs/interface/hud-re.md): the 6-row dispersion
	// table in DEGREES, rows = hip prone/crouch/stand then scoped prone/crouch/
	// stand — the crosshair spread reads ERROR[stance + 3*scoped] [orig: weapon
	// +0xB0 parse @0x543b21 (16.16); HUD_DrawCrosshair @0x592b84]. The clip
	// graphic (HUDCLIPGFX: offset + texture [orig: parse @0x54427f]) and the
	// per-round row (HUDRNDGFX: start x/y, step x/y, rounds-per-icon divisor,
	// texture [orig: parse @0x5442fc -> weapon +644/+648/+652/+656/+727]).
	PackedFloat32Array error;
	for (int k = 0; k < 6; ++k) {
		error.push_back(w->error[k]);
	}
	d["error"] = error;
	d["hudclipgfx_texture"] = String(w->hudclipgfx_texture);
	d["hudclipgfx_offset"] = Vector2i(w->hudclipgfx_offset[0], w->hudclipgfx_offset[1]);
	d["hudrndgfx_texture"] = String(w->hudrndgfx_texture);
	d["hudrndgfx_offset"] = Vector2i(w->hudrndgfx_offset[0], w->hudrndgfx_offset[1]);
	d["hudrndgfx_layout"] = Vector3i(w->hudrndgfx_layout[0], w->hudrndgfx_layout[1], w->hudrndgfx_layout[2]);
	// The weapon's ACTION blocks, verbatim rows for the weapon-FSM bake — Dicts
	// {name, anim, function, delaystart, delayend} [orig: ActionDef_ParseScriptLine
	// @ 0x4023c0; bound by Anim_InitActions @ 0x541fa0; net-re §5.62], plus the
	// per-ACTION audio/effect hooks: the GF_* sound set played on action
	// start/end and the particle effect spawned at the model user point
	// [orig: ActionSlot fields consumed by ActionSlot_SpawnEffect @ 0x401f20].
	Array actions;
	for (size_t a = 0; a < w->actions_count; ++a) {
		const DefWeaponAction &act_row = w->actions[a];
		Dictionary act;
		act["name"] = String(act_row.name);
		act["anim"] = String(act_row.anim);
		act["function"] = String(act_row.function);
		act["delaystart"] = act_row.delaystart;
		act["delayend"] = act_row.delayend;
		act["soundset"] = String(act_row.soundset);
		act["soundsetend"] = String(act_row.soundsetend);
		act["particle"] = String(act_row.particle);
		act["particleuserpoint"] = String(act_row.particleuserpoint);
		actions.push_back(act);
	}
	d["actions"] = actions;
	return d;
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

Array WeaponDatabase::get_slot_weapons(int slot, int class_mask, int team_mask) const {
	std::vector<int32_t> indices;
	opennova::world::weapon_slot_indices(weapons_file_.entries, weapons_file_.count,
			slot, class_mask, team_mask, indices);
	Array out;
	for (const int32_t i : indices) {
		out.push_back(weapon_dict(i));
	}
	return out;
}

Dictionary WeaponDatabase::get_weapon(int index) const {
	return weapon_dict(index);
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
