#include "nova_item_database.h"

#include "resource_index/nova_resource_root.h"
#include "util/nova_data_format.h"

#include <def/def.h>

#include <algorithm>
#include <vector>

using namespace godot;

// Pin the GDScript-facing TYPE_* mirror to the libs/def source of truth so the
// two mappings can never drift again (docs/world/itemdef-re.md D-ITEMDEF-1).
static_assert(NovaItemDatabase::TYPE_UNKNOWN == DEF_ITEM_TYPE_UNSET, "TYPE_UNKNOWN drifted from DefItemType");
static_assert(NovaItemDatabase::TYPE_VEHICLE == DEF_ITEM_TYPE_VEHICLE, "TYPE_VEHICLE drifted from DefItemType");
static_assert(NovaItemDatabase::TYPE_DECORATION == DEF_ITEM_TYPE_DECORATION, "TYPE_DECORATION drifted from DefItemType");
static_assert(NovaItemDatabase::TYPE_FOLIAGE == DEF_ITEM_TYPE_FOLIAGE, "TYPE_FOLIAGE drifted from DefItemType");
static_assert(NovaItemDatabase::TYPE_PERSON == DEF_ITEM_TYPE_PERSON, "TYPE_PERSON drifted from DefItemType");
static_assert(NovaItemDatabase::TYPE_MARKER == DEF_ITEM_TYPE_MARKER, "TYPE_MARKER drifted from DefItemType");
static_assert(NovaItemDatabase::TYPE_BUILDING == DEF_ITEM_TYPE_BUILDING, "TYPE_BUILDING drifted from DefItemType");
static_assert(NovaItemDatabase::TYPE_POWERUP == DEF_ITEM_TYPE_POWERUP, "TYPE_POWERUP drifted from DefItemType");
static_assert(NovaItemDatabase::ATTRIB_POWERUP == DEF_ITEM_ATTRIB_POWERUP, "ATTRIB_POWERUP drifted from def.h");
static_assert(NovaItemDatabase::ATTRIB_PLAYER_CONTROL == DEF_ITEM_ATTRIB_PLAYERCONTROL, "ATTRIB_PLAYER_CONTROL drifted from def.h");
static_assert(NovaItemDatabase::ATTRIB_ARMORY == DEF_ITEM_ATTRIB_ARMORY, "ATTRIB_ARMORY drifted from def.h");
static_assert(NovaItemDatabase::TYPE_OBJECT == DEF_ITEM_TYPE_OBJECT, "TYPE_OBJECT drifted from DefItemType");
static_assert(NovaItemDatabase::TYPE_EFFECT == DEF_ITEM_TYPE_EFFECT, "TYPE_EFFECT drifted from DefItemType");
static_assert(NovaItemDatabase::EMPLACEMENT_ADDEWEAP == DEF_ITEM_EMPLACEMENT_ADDEWEAP,
		"EMPLACEMENT_ADDEWEAP drifted from DefItemEmplacementAttachmentKind");
static_assert(NovaItemDatabase::EMPLACEMENT_ADDEWEAP_G == DEF_ITEM_EMPLACEMENT_ADDEWEAP_G,
		"EMPLACEMENT_ADDEWEAP_G drifted from DefItemEmplacementAttachmentKind");
static_assert(NovaItemDatabase::EMPLACEMENT_ADDEWEAP_C == DEF_ITEM_EMPLACEMENT_ADDEWEAP_C,
		"EMPLACEMENT_ADDEWEAP_C drifted from DefItemEmplacementAttachmentKind");

void NovaItemDatabase::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load", "path"), &NovaItemDatabase::load);
	ClassDB::bind_method(D_METHOD("load_from_resource_root", "resource_root", "name"), &NovaItemDatabase::load_from_resource_root);
	ClassDB::bind_method(D_METHOD("is_loaded"), &NovaItemDatabase::is_loaded);
	ClassDB::bind_method(D_METHOD("get_source_path"), &NovaItemDatabase::get_source_path);
	ClassDB::bind_method(D_METHOD("get_last_error"), &NovaItemDatabase::get_last_error);
	ClassDB::bind_method(D_METHOD("get_count"), &NovaItemDatabase::get_count);
	ClassDB::bind_method(D_METHOD("has_item", "id"), &NovaItemDatabase::has_item);
	ClassDB::bind_method(D_METHOD("get_vehicle_physics", "id"), &NovaItemDatabase::get_vehicle_physics);
	ClassDB::bind_method(D_METHOD("get_graphic", "id"), &NovaItemDatabase::get_graphic);
	ClassDB::bind_method(D_METHOD("get_husk", "id"), &NovaItemDatabase::get_husk);
	ClassDB::bind_method(D_METHOD("get_huskfinal", "id"), &NovaItemDatabase::get_huskfinal);
	ClassDB::bind_method(D_METHOD("get_death_traits", "id"), &NovaItemDatabase::get_death_traits);
	ClassDB::bind_method(D_METHOD("get_anim_def", "id"), &NovaItemDatabase::get_anim_def);
	ClassDB::bind_method(D_METHOD("get_ai_function", "id"), &NovaItemDatabase::get_ai_function);
	ClassDB::bind_method(D_METHOD("get_hp", "id"), &NovaItemDatabase::get_hp);
	ClassDB::bind_method(D_METHOD("get_move_function", "id"), &NovaItemDatabase::get_move_function);
	ClassDB::bind_method(D_METHOD("get_item_type", "id"), &NovaItemDatabase::get_item_type);
	ClassDB::bind_method(D_METHOD("get_light_transfer", "id"), &NovaItemDatabase::get_light_transfer);
	ClassDB::bind_method(D_METHOD("is_ai_capable", "id"), &NovaItemDatabase::is_ai_capable);
	ClassDB::bind_method(D_METHOD("get_display_name", "id"), &NovaItemDatabase::get_display_name);
	ClassDB::bind_method(D_METHOD("get_ammo_closeattack", "id"), &NovaItemDatabase::get_ammo_closeattack);
	ClassDB::bind_method(D_METHOD("get_clipsize", "id"), &NovaItemDatabase::get_clipsize);
	ClassDB::bind_method(D_METHOD("get_deathtime_ticks", "id"), &NovaItemDatabase::get_deathtime_ticks);
	ClassDB::bind_method(D_METHOD("get_primary_weapon", "id"), &NovaItemDatabase::get_primary_weapon);
	ClassDB::bind_method(D_METHOD("get_emplacement_attachments", "id"), &NovaItemDatabase::get_emplacement_attachments);
	ClassDB::bind_method(D_METHOD("get_emplacement_attachment_markers", "id"), &NovaItemDatabase::get_emplacement_attachment_markers);
	ClassDB::bind_method(D_METHOD("get_mount_config", "id"), &NovaItemDatabase::get_mount_config);
	ClassDB::bind_method(D_METHOD("get_sound_profile", "id"), &NovaItemDatabase::get_sound_profile);
	ClassDB::bind_method(D_METHOD("get_sound_loops", "id"), &NovaItemDatabase::get_sound_loops);
	ClassDB::bind_method(D_METHOD("get_particle_effects", "id"), &NovaItemDatabase::get_particle_effects);
	ClassDB::bind_method(D_METHOD("get_attrib", "id"), &NovaItemDatabase::get_attrib);
	ClassDB::bind_method(D_METHOD("get_attrib2", "id"), &NovaItemDatabase::get_attrib2);
	ClassDB::bind_method(D_METHOD("get_item", "id"), &NovaItemDatabase::get_item);
	ClassDB::bind_method(D_METHOD("get_item_ids"), &NovaItemDatabase::get_item_ids);
	ClassDB::bind_method(D_METHOD("get_items"), &NovaItemDatabase::get_items);

	BIND_CONSTANT(TYPE_UNKNOWN);
	BIND_CONSTANT(TYPE_VEHICLE);
	BIND_CONSTANT(TYPE_DECORATION);
	BIND_CONSTANT(TYPE_FOLIAGE);
	BIND_CONSTANT(TYPE_PERSON);
	BIND_CONSTANT(TYPE_MARKER);
	BIND_CONSTANT(TYPE_BUILDING);
	BIND_CONSTANT(TYPE_POWERUP);
	BIND_CONSTANT(TYPE_OBJECT);
	BIND_CONSTANT(TYPE_EFFECT);
	BIND_CONSTANT(EMPLACEMENT_ADDEWEAP);
	BIND_CONSTANT(EMPLACEMENT_ADDEWEAP_G);
	BIND_CONSTANT(EMPLACEMENT_ADDEWEAP_C);
	BIND_CONSTANT(ATTRIB_POWERUP);
	BIND_CONSTANT(ATTRIB_PLAYER_CONTROL);
	BIND_CONSTANT(ATTRIB_ARMORY);
}

Error NovaItemDatabase::load(const String &path) {
	source_path = path;
	last_error = String();
	items.clear();

	PackedByteArray bytes;
	if (!read_nova_payload_file(path, bytes)) {
		last_error = String("Cannot open item database: ") + path;
		return ERR_CANT_OPEN;
	}

	DefItemsFile file = {};
	if (def_parse_items_memory(bytes.ptr(), static_cast<size_t>(bytes.size()), &file) != 0) {
		last_error = String("def_parse_items_memory failed for ") + path;
		return ERR_CANT_OPEN;
	}

	for (size_t i = 0; i < file.count; ++i) {
		items[file.entries[i].id] = item_from_entry(file.entries[i]);
	}

	def_free_items(&file);
	return OK;
}

NovaItemDatabase::Item NovaItemDatabase::item_from_entry(const ::DefItemDef &entry) {
	Item item;
	item.id = entry.id;
	item.type = entry.type;
	item.attrib = static_cast<uint32_t>(entry.attrib);
	item.attrib2 = static_cast<uint32_t>(entry.attrib2);
	item.display_name = String(entry.display_name);
	item.graphic = String(entry.graphic);
	item.anim_def = String(entry.anim_def);
	item.sound_profile = String(entry.sound_profile);
	item.ai_function = String(entry.ai_function);
	item.move_function = String(entry.move_function);
	item.hp = entry.hp;
	item.light_transfer = entry.light_transfer;
	item.damage_reduc_pp = entry.damage_reduc_pp;
	item.damage_reduc_max = entry.damage_reduc_max;
	item.physics = entry.physics;
	item.acceleration = entry.acceleration;
	item.deceleration = entry.deceleration;
	item.player_speed = entry.player_speed;
	item.turn_rate = entry.turn_rate;
	item.turn_rate2 = entry.turn_rate2;
	item.torque = entry.torque;
	item.unit_type = entry.unit_type;
	for (int s = 0; s < 7; ++s) {
		item.soundloops[s] = String(entry.soundloops[s]);
	}
	// The per-item particle-effect keys [orig: ItemDef_ParseProperty @ 0x49eb00].
	const auto copy_fx = [](Item::ParticleFx &dst, const DefItemParticleFx &src) {
		dst.effect = String(src.effect);
		dst.userpoint = String(src.userpoint);
		dst.secondary_effect = String(src.secondary_effect);
	};
	copy_fx(item.particlefx, entry.particlefx);
	copy_fx(item.particlefxs, entry.particlefxs);
	copy_fx(item.particlefxw[0], entry.particlefxw1);
	copy_fx(item.particlefxw[1], entry.particlefxw2);
	copy_fx(item.particlefxw[2], entry.particlefxw3);
	copy_fx(item.particlefxw[3], entry.particlefxw4);
	item.particledeath = String(entry.particledeath);
	item.particleh2odeath = String(entry.particleh2odeath);
	item.particlefire = String(entry.particlefire);
	item.particleother = String(entry.particleother);
	item.particlespawn = String(entry.particlespawn);
	item.particlefinale = String(entry.particlefinale);
	// The person-item anim-fire weapon family (world-wac-ai-re §17.4, D-AI-5).
	item.ammo_closeattack = String(entry.ammo_closeattack);
	item.clipsize = entry.clipsize;
	item.deathtime_ticks = entry.deathtime_ticks;
	item.primary_weapon = String(entry.primary_weapon);
	item.emplacement_attachments.reserve(entry.emplacement_attachments_count);
	for (size_t i = 0; i < entry.emplacement_attachments_count; ++i) {
		const DefItemEmplacementAttachment &src = entry.emplacement_attachments[i];
		Item::EmplacementAttachment dst;
		dst.userpoint = String(src.userpoint);
		dst.item_id = src.item_id;
		dst.down_angle = src.down_angle;
		dst.up_angle = src.up_angle;
		dst.right_angle = src.right_angle;
		dst.left_angle = src.left_angle;
		dst.angle_count = src.angle_count;
		dst.kind = src.kind;
		item.emplacement_attachments.push_back(dst);
	}
	item.emplacement_g_slot = entry.emplacement_g_slot;
	item.emplacement_c_slot = entry.emplacement_c_slot;
	item.mount_config_valid = entry.phrase_set_valid != 0;
	item.mount_config = entry.phrase_set;
	// The destruction/husk block (world-wac-ai-re §24).
	item.husk = String(entry.husk);
	item.huskfinal = String(entry.huskfinal);
	item.sounddeath = String(entry.sounddeath);
	item.armor_impact = entry.armor_impact;
	item.armor_blast = entry.armor_blast;
	item.armor_kz = entry.armor_kz;
	item.kz = entry.kz;
	item.debris_scale = entry.debris_scale;
	item.husk_sub_parts = entry.husk_sub_parts;
	for (int s = 0; s < 16; ++s) {
		item.husk_sub_part_types[s] = entry.husk_sub_part_types[s];
	}
	return item;
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
		items[file.entries[i].id] = item_from_entry(file.entries[i]);
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

String NovaItemDatabase::get_ai_function(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? String() : it->second.ai_function;
}

String NovaItemDatabase::get_move_function(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? String() : it->second.move_function;
}

int NovaItemDatabase::get_hp(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? 0 : it->second.hp;
}

int NovaItemDatabase::get_item_type(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? static_cast<int>(TYPE_UNKNOWN) : it->second.type;
}

float NovaItemDatabase::get_light_transfer(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? 0.0f : it->second.light_transfer;
}

float NovaItemDatabase::get_damage_reduc_pp(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? 0.0f : it->second.damage_reduc_pp;
}

float NovaItemDatabase::get_damage_reduc_max(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? 0.0f : it->second.damage_reduc_max;
}

int NovaItemDatabase::get_armor_impact(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? 0 : it->second.armor_impact;
}

int NovaItemDatabase::get_armor_kz(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? 0 : it->second.armor_kz;
}

// items.def ItemDefAttrib & 0x100000 (AIData). Mirrors the stock 0x0D decoder's own gate
// (itemDef.attrib & 0x100000 @0x433327) so the host emits the AI-trailer iff the item is
// AI-capable. [docs/world/itemdef-re.md; docs/net/novaworld-net-re.md D-NET-97]
bool NovaItemDatabase::is_ai_capable(int id) const {
	const auto it = items.find(id);
	return it != items.end() && (it->second.attrib & 0x100000u) != 0;
}

// The raw items.def ItemDefAttrib dword (itemDef+0x54); 0 for unknown ids. The AS zone
// traits read bits 0x20000 "ChangeTeam" (capture trigger) and 0x40000 "SpawnPoint"
// (deploy-selectable). [docs/world/itemdef-re.md; net-re §5.61]
uint32_t NovaItemDatabase::get_attrib(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? 0u : it->second.attrib;
}

uint32_t NovaItemDatabase::get_attrib2(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? 0u : it->second.attrib2;
}

PackedInt32Array NovaItemDatabase::get_vehicle_physics(int id) const {
	PackedInt32Array out;
	const auto it = items.find(id);
	if (it == items.end()) return out;
	const Item &item = it->second;
	out.push_back(item.physics);
	out.push_back(item.player_speed);
	out.push_back(item.acceleration);
	out.push_back(item.deceleration);
	out.push_back(item.turn_rate);
	out.push_back(item.turn_rate2);
	out.push_back(item.unit_type);
	out.push_back(item.torque);
	return out;
}

String NovaItemDatabase::get_display_name(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? String() : it->second.display_name;
}

// Person-item anim-fire round name (world-wac-ai-re §17.4). The sim's AI weapon
// seed resolves it against the mission ammo table (D-AI-5). [orig:
// ItemDef_ParseProperty @ 0x4a1823 -> def+0x56B]
String NovaItemDatabase::get_ammo_closeattack(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? String() : it->second.ammo_closeattack;
}

// items.def clipsize — the respawn magazine reseed (word entity+0x35C = def+0x894).
// [orig: ItemDef_ParseProperty @ 0x49fa1c; Entity_ResetToSpawnState @ 0x4b97a9]
int NovaItemDatabase::get_clipsize(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? 0 : it->second.clipsize;
}

// items.def deathtime in ticks (parse-scaled (62*s or 496) + 62) — the corpse
// timer's seed. [orig: ItemDef_ParseProperty @ 0x49fa6c -> def+0x890; consumer
// Entity_UpdateInfantryAI @ 0x4b9c97 -> entity+0x148]
int NovaItemDatabase::get_deathtime_ticks(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? 0 : it->second.deathtime_ticks;
}

String NovaItemDatabase::get_primary_weapon(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? String() : it->second.primary_weapon;
}

Array NovaItemDatabase::get_emplacement_attachments(int id) const {
	Array out;
	const auto it = items.find(id);
	if (it == items.end()) {
		return out;
	}
	for (size_t i = 0; i < it->second.emplacement_attachments.size(); ++i) {
		const Item::EmplacementAttachment &attachment =
				it->second.emplacement_attachments[i];
		Dictionary row;
		row["kind"] = attachment.kind;
		switch (attachment.kind) {
			case EMPLACEMENT_ADDEWEAP_G:
				row["key"] = "addeweapG";
				break;
			case EMPLACEMENT_ADDEWEAP_C:
				row["key"] = "addeweapC";
				break;
			default:
				row["key"] = "addeweap";
				break;
		}
		row["userpoint"] = attachment.userpoint;
		row["item_id"] = attachment.item_id;
		row["stored_slot"] = static_cast<int>(i + 1);
		row["angle_count"] = attachment.angle_count;
		row["has_explicit_limits"] = attachment.angle_count == 4;
		row["down_limit_bam"] = attachment.down_angle;
		row["up_limit_bam"] = attachment.up_angle;
		row["right_limit_bam"] = attachment.right_angle;
		row["left_limit_bam"] = attachment.left_angle;
		row["designated_g"] =
				static_cast<int>(i + 1) == it->second.emplacement_g_slot;
		row["designated_c"] =
				static_cast<int>(i + 1) == it->second.emplacement_c_slot;
		out.push_back(row);
	}
	return out;
}

Dictionary NovaItemDatabase::get_emplacement_attachment_markers(int id) const {
	Dictionary out;
	const auto it = items.find(id);
	out["g_slot"] =
			it == items.end() ? 0 : it->second.emplacement_g_slot;
	out["c_slot"] =
			it == items.end() ? 0 : it->second.emplacement_c_slot;
	return out;
}

Dictionary NovaItemDatabase::get_mount_config(int id) const {
	Dictionary out;
	const auto it = items.find(id);
	const bool valid = it != items.end() && it->second.mount_config_valid;
	out["valid"] = valid;
	out["value"] = valid ? it->second.mount_config : 0;
	return out;
}

// Entity-attached profile name; the engine composes "<EntityDefName>_<SoundType>"
// lookups from it [orig: SoundProfile_FindByEntityAndType @ 0x528180].
String NovaItemDatabase::get_sound_profile(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? String() : it->second.sound_profile;
}

String NovaItemDatabase::get_husk(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? String() : it->second.husk;
}

String NovaItemDatabase::get_huskfinal(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? String() : it->second.huskfinal;
}

// The destruction traits bundle (world-wac-ai-re §24), consumed by the sim's
// item-traits sweep into world::ItemDeathTraits.
Dictionary NovaItemDatabase::get_death_traits(int id) const {
	Dictionary out;
	const auto it = items.find(id);
	if (it == items.end()) {
		return out;
	}
	const Item &item = it->second;
	out["unit_type"] = item.unit_type;
	out["kz"] = item.kz;
	out["armor_impact"] = item.armor_impact;
	out["armor_blast"] = item.armor_blast;
	out["sounddeath"] = item.sounddeath;
	out["debris_scale"] = item.debris_scale;
	out["husk_sub_parts"] = item.husk_sub_parts;
	PackedInt32Array types;
	types.resize(16);
	for (int s = 0; s < 16; ++s) {
		types.set(s, item.husk_sub_part_types[s]);
	}
	out["husk_sub_part_types"] = types;
	out["has_husk"] = !item.husk.is_empty() || !item.huskfinal.is_empty();
	return out;
}

// items.def soundloop_1..7 looping ambient set names for "snd:" marker items
// [orig: ItemDef_ParseProperty @ 0x49eb00, "soundloop_" prefix @ 0x49fec4; the
// 7-slot count matches the engine's Soundloop_1..7 type table @ 0x7d0788].
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

// The particle-effect keys as authored, keyed by the ITEMS.DEF key names — the runtime
// effect-attach pass consumes slot A ("particlefx"); the rest ride along for future
// consumers. [orig: ItemDef_ParseProperty @ 0x49eb00; runtime witness
// resolve_item_materials_and_spawn_bone_trails @ 0x522ee0]
Dictionary NovaItemDatabase::get_particle_effects(int id) const {
	Dictionary out;
	const auto it = items.find(id);
	if (it == items.end()) {
		return out;
	}
	const Item &item = it->second;
	const auto fx_dict = [](const Item::ParticleFx &fx) {
		Dictionary d;
		d["effect"] = fx.effect;
		d["userpoint"] = fx.userpoint;
		d["secondary_effect"] = fx.secondary_effect;
		return d;
	};
	out["particlefx"] = fx_dict(item.particlefx);
	out["particlefxs"] = fx_dict(item.particlefxs);
	out["particlefxw1"] = fx_dict(item.particlefxw[0]);
	out["particlefxw2"] = fx_dict(item.particlefxw[1]);
	out["particlefxw3"] = fx_dict(item.particlefxw[2]);
	out["particlefxw4"] = fx_dict(item.particlefxw[3]);
	out["particledeath"] = item.particledeath;
	out["particleh2odeath"] = item.particleh2odeath;
	out["particlefire"] = item.particlefire;
	out["particleother"] = item.particleother;
	out["particlespawn"] = item.particlespawn;
	out["particlefinale"] = item.particlefinale;
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
	out["light_transfer"] = it->second.light_transfer;
	out["sound_profile"] = it->second.sound_profile;
	out["soundloops"] = get_sound_loops(id);
	out["mount_config_valid"] = it->second.mount_config_valid;
	out["mount_config"] = it->second.mount_config_valid ? it->second.mount_config : 0;
	out["emplacement_attachments"] = get_emplacement_attachments(id);
	out["emplacement_attachment_markers"] =
			get_emplacement_attachment_markers(id);
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
		entry["light_transfer"] = item->light_transfer;
		entry["sound_profile"] = item->sound_profile;
		entry["soundloops"] = get_sound_loops(item->id);
		entry["mount_config_valid"] = item->mount_config_valid;
		entry["mount_config"] = item->mount_config_valid ? item->mount_config : 0;
		entry["emplacement_attachments"] = get_emplacement_attachments(item->id);
		entry["emplacement_attachment_markers"] =
				get_emplacement_attachment_markers(item->id);
		out.push_back(entry);
	}
	return out;
}
