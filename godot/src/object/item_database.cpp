#include "object/item_database.h"
#include "object/item_records.h"
#include "resource_index/resource_root.h"

#include <formats/mission/mission.h> // kItemIdOffset
#include <runtime/simassets/seat_spec_extract.h>
#include <runtime/simassets/sim_model_cache.h>

#include "mission/mission_data.h"
#include "resource_index/resource_root.h"
#include "util/data_format.h"

#include <runtime/audio/envs_markers.h>
#include <formats/def/def.h>

#include <algorithm>
#include <vector>

using namespace godot;

// Pin the GDScript-facing TYPE_* mirror to the engine/formats/def source of truth so the
// two mappings can never drift again (docs/world/itemdef-re.md D-ITEMDEF-1).
static_assert(ItemDatabase::TYPE_UNKNOWN == DEF_ITEM_TYPE_UNSET, "TYPE_UNKNOWN drifted from DefItemType");
static_assert(ItemDatabase::TYPE_VEHICLE == DEF_ITEM_TYPE_VEHICLE, "TYPE_VEHICLE drifted from DefItemType");
static_assert(ItemDatabase::TYPE_DECORATION == DEF_ITEM_TYPE_DECORATION, "TYPE_DECORATION drifted from DefItemType");
static_assert(ItemDatabase::TYPE_FOLIAGE == DEF_ITEM_TYPE_FOLIAGE, "TYPE_FOLIAGE drifted from DefItemType");
static_assert(ItemDatabase::TYPE_PERSON == DEF_ITEM_TYPE_PERSON, "TYPE_PERSON drifted from DefItemType");
static_assert(ItemDatabase::TYPE_MARKER == DEF_ITEM_TYPE_MARKER, "TYPE_MARKER drifted from DefItemType");
static_assert(ItemDatabase::TYPE_BUILDING == DEF_ITEM_TYPE_BUILDING, "TYPE_BUILDING drifted from DefItemType");
static_assert(ItemDatabase::TYPE_POWERUP == DEF_ITEM_TYPE_POWERUP, "TYPE_POWERUP drifted from DefItemType");
static_assert(ItemDatabase::ATTRIB_POWERUP == DEF_ITEM_ATTRIB_POWERUP, "ATTRIB_POWERUP drifted from def.h");
static_assert(ItemDatabase::ATTRIB_PLAYER_CONTROL == DEF_ITEM_ATTRIB_PLAYERCONTROL, "ATTRIB_PLAYER_CONTROL drifted from def.h");
static_assert(ItemDatabase::ATTRIB_ARMORY == DEF_ITEM_ATTRIB_ARMORY, "ATTRIB_ARMORY drifted from def.h");
static_assert(ItemDatabase::TYPE_OBJECT == DEF_ITEM_TYPE_OBJECT, "TYPE_OBJECT drifted from DefItemType");
static_assert(ItemDatabase::TYPE_EFFECT == DEF_ITEM_TYPE_EFFECT, "TYPE_EFFECT drifted from DefItemType");
static_assert(ItemDatabase::EMPLACEMENT_ADDEWEAP == DEF_ITEM_EMPLACEMENT_ADDEWEAP,
		"EMPLACEMENT_ADDEWEAP drifted from DefItemEmplacementAttachmentKind");
static_assert(ItemDatabase::EMPLACEMENT_ADDEWEAP_G == DEF_ITEM_EMPLACEMENT_ADDEWEAP_G,
		"EMPLACEMENT_ADDEWEAP_G drifted from DefItemEmplacementAttachmentKind");
static_assert(ItemDatabase::EMPLACEMENT_ADDEWEAP_C == DEF_ITEM_EMPLACEMENT_ADDEWEAP_C,
		"EMPLACEMENT_ADDEWEAP_C drifted from DefItemEmplacementAttachmentKind");

void ItemDatabase::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load", "path"), &ItemDatabase::load);
	ClassDB::bind_method(D_METHOD("load_from_resource_root", "resource_root", "name"), &ItemDatabase::load_from_resource_root);
	ClassDB::bind_method(D_METHOD("is_loaded"), &ItemDatabase::is_loaded);
	ClassDB::bind_method(D_METHOD("get_source_path"), &ItemDatabase::get_source_path);
	ClassDB::bind_method(D_METHOD("get_last_error"), &ItemDatabase::get_last_error);
	ClassDB::bind_method(D_METHOD("get_count"), &ItemDatabase::get_count);
	ClassDB::bind_method(D_METHOD("has_item", "id"), &ItemDatabase::has_item);
	ClassDB::bind_method(D_METHOD("get_vehicle_physics", "id"), &ItemDatabase::get_vehicle_physics);
	ClassDB::bind_method(D_METHOD("get_graphic", "id"), &ItemDatabase::get_graphic);
	ClassDB::bind_method(D_METHOD("get_sid", "id"), &ItemDatabase::get_sid);
	ClassDB::bind_method(D_METHOD("get_display_name", "id"), &ItemDatabase::get_display_name);
	ClassDB::bind_method(D_METHOD("get_husk", "id"), &ItemDatabase::get_husk);
	ClassDB::bind_method(D_METHOD("get_huskfinal", "id"), &ItemDatabase::get_huskfinal);
	ClassDB::bind_method(D_METHOD("get_anim_def", "id"), &ItemDatabase::get_anim_def);
	ClassDB::bind_method(D_METHOD("get_ai_function", "id"), &ItemDatabase::get_ai_function);
	ClassDB::bind_method(D_METHOD("get_move_function", "id"), &ItemDatabase::get_move_function);
	ClassDB::bind_method(D_METHOD("get_item_type", "id"), &ItemDatabase::get_item_type);
	ClassDB::bind_method(D_METHOD("get_light_transfer", "id"), &ItemDatabase::get_light_transfer);
	ClassDB::bind_method(D_METHOD("is_ai_capable", "id"), &ItemDatabase::is_ai_capable);
	ClassDB::bind_method(
			D_METHOD("extract_seat_specs_for_item", "resource_root", "item_id"),
			&ItemDatabase::extract_seat_specs_for_item);
	ClassDB::bind_method(D_METHOD("get_emplacement_attachments", "id"), &ItemDatabase::get_emplacement_attachments);
	ClassDB::bind_method(D_METHOD("get_emplacement_g_slot", "id"), &ItemDatabase::get_emplacement_g_slot);
	ClassDB::bind_method(D_METHOD("get_emplacement_c_slot", "id"), &ItemDatabase::get_emplacement_c_slot);
	ClassDB::bind_method(D_METHOD("has_mount_config", "id"), &ItemDatabase::has_mount_config);
	ClassDB::bind_method(D_METHOD("get_mount_config", "id"), &ItemDatabase::get_mount_config);
	ClassDB::bind_method(D_METHOD("resolve_envs_markers", "mission"),
			&ItemDatabase::resolve_envs_markers);
	ClassDB::bind_method(D_METHOD("get_particle_fx", "id"), &ItemDatabase::get_particle_fx);
	ClassDB::bind_method(D_METHOD("get_attrib", "id"), &ItemDatabase::get_attrib);
	ClassDB::bind_method(D_METHOD("get_attrib2", "id"), &ItemDatabase::get_attrib2);
	ClassDB::bind_method(D_METHOD("get_item_ids"), &ItemDatabase::get_item_ids);

	BIND_CONSTANT(TYPE_VEHICLE);
	BIND_CONSTANT(TYPE_PERSON);
	BIND_CONSTANT(TYPE_BUILDING);
	BIND_CONSTANT(TYPE_POWERUP);
	BIND_CONSTANT(TYPE_OBJECT);
	BIND_CONSTANT(EMPLACEMENT_ADDEWEAP);
	BIND_CONSTANT(EMPLACEMENT_ADDEWEAP_G);
	BIND_CONSTANT(EMPLACEMENT_ADDEWEAP_C);
	BIND_CONSTANT(ATTRIB_POWERUP);
	BIND_CONSTANT(ATTRIB_PLAYER_CONTROL);
}

ItemDatabase::~ItemDatabase() {
	release_native_items();
}

void ItemDatabase::release_native_items() {
	if (items_file_loaded_) {
		def_free_items(&items_file_);
		items_file_loaded_ = false;
	}
	items_file_ = {};
}

Error ItemDatabase::load(const String &path) {
	++revision;
	source_path = path;
	last_error = String();
	items.clear();
	replication_definition_records.clear();
	release_native_items();

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

	replication_definition_records.reserve(file.count);
	for (size_t i = 0; i < file.count; ++i) {
		replication_definition_records.push_back(
				replication_definition_from_entry(file.entries[i]));
		items[file.entries[i].id] = item_from_entry(file.entries[i]);
	}

	// Retain the parse (ADR 0028): the engine-side trait fold reads these
	// rows directly through native_items().
	items_file_ = file;
	items_file_loaded_ = true;
	return OK;
}

ItemDatabase::Item ItemDatabase::item_from_entry(const ::DefItemDef &entry) {
	Item item;
	item.id = entry.id;
	item.type = entry.type;
	item.sid = String(entry.sid);
	item.attrib = static_cast<uint32_t>(entry.attrib);
	item.attrib2 = static_cast<uint32_t>(entry.attrib2);
	item.display_name = String(entry.display_name);
	item.graphic = String(entry.graphic);
	item.anim_def = String(entry.anim_def);
	item.sound_profile = String(entry.sound_profile);
	item.ai_function = String(entry.ai_function);
	item.move_function = String(entry.move_function);
	item.render_function = String(entry.render_function);
	item.disk_function = String(entry.disk_function);
	item.default_aip = String(entry.default_aip);
	item.hp = entry.hp;
	item.shadow_texture = String(entry.shadow_texture);
	item.shadow_width = entry.shadow_width;
	item.shadow_length = entry.shadow_length;
	item.shadow_offset_x = entry.shadow_offset_x;
	item.shadow_offset_y = entry.shadow_offset_y;
	item.light_transfer = entry.light_transfer;
	item.damage_reduc_pp = entry.damage_reduc_pp;
	item.damage_reduc_max = entry.damage_reduc_max;
	item.physics = entry.physics;
	item.acceleration = entry.acceleration;
	item.deceleration = entry.deceleration;
	item.player_speed = entry.player_speed;
	item.water_speed = entry.water_speed;
	item.climb_speed = entry.climb_speed;
	item.turn_roll = entry.turn_roll;
	item.speed_pitch = entry.speed_pitch;
	item.max_slope = entry.max_slope;
	item.slip_slope = entry.slip_slope;
	item.mass = entry.mass;
	item.lean = entry.lean;
	item.lean_velocity = entry.lean_velocity;
	item.pitch = entry.pitch;
	item.pitch_velocity = entry.pitch_velocity;
	item.bob = entry.bob;
	item.flip = entry.flip;
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
	item.launchups_closeattack = String(entry.launchups_closeattack);
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
	item.model_scale_q16 = entry.scale_q16;
	item.debris_scale = entry.debris_scale;
	item.husk_sub_parts = entry.husk_sub_parts;
	for (int s = 0; s < 16; ++s) {
		item.husk_sub_part_types[s] = entry.husk_sub_part_types[s];
	}
	return item;
}

ItemDatabase::ReplicationDefinitionRecord
ItemDatabase::replication_definition_from_entry(
		const ::DefItemDef &entry) {
	ReplicationDefinitionRecord record;
	record.definition_id = entry.id;
	record.item_type = entry.type;
	record.attrib = static_cast<uint32_t>(entry.attrib);
	record.attrib2 = static_cast<uint32_t>(entry.attrib2);
	record.physics = entry.physics;
	record.ai_function = String(entry.ai_function);
	record.move_function = String(entry.move_function);
	record.render_function = String(entry.render_function);
	record.disk_function = String(entry.disk_function);
	return record;
}

Error ItemDatabase::load_from_resource_root(const Ref<ResourceRoot> &p_resource_root, const String &p_name) {
	++revision;
	last_error = String();
	items.clear();
	replication_definition_records.clear();
	release_native_items();
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

	replication_definition_records.reserve(file.count);
	for (size_t i = 0; i < file.count; ++i) {
		replication_definition_records.push_back(
				replication_definition_from_entry(file.entries[i]));
		items[file.entries[i].id] = item_from_entry(file.entries[i]);
	}

	// Retain the parse (ADR 0028): the engine-side trait fold reads these
	// rows directly through native_items().
	items_file_ = file;
	items_file_loaded_ = true;
	source_path = file_name;
	return OK;
}

bool ItemDatabase::is_loaded() const {
	return !items.empty();
}

String ItemDatabase::get_source_path() const {
	return source_path;
}

String ItemDatabase::get_last_error() const {
	return last_error;
}

int ItemDatabase::get_count() const {
	return static_cast<int>(items.size());
}

bool ItemDatabase::has_item(int id) const {
	return items.find(id) != items.end();
}

String ItemDatabase::get_graphic(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? String() : it->second.graphic;
}

String ItemDatabase::get_sid(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? String() : it->second.sid;
}

String ItemDatabase::get_anim_def(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? String() : it->second.anim_def;
}

String ItemDatabase::get_ai_function(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? String() : it->second.ai_function;
}

String ItemDatabase::get_move_function(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? String() : it->second.move_function;
}

int ItemDatabase::get_item_type(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? static_cast<int>(TYPE_UNKNOWN) : it->second.type;
}

int32_t ItemDatabase::get_model_scale_q16(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? 0 : it->second.model_scale_q16;
}

float ItemDatabase::get_light_transfer(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? 0.0f : it->second.light_transfer;
}

// items.def ItemDefAttrib & 0x100000 (AIData). Mirrors the stock 0x0D decoder's own gate
// (itemDef.attrib & 0x100000 @0x433327) so the host emits the AI-trailer iff the item is
// AI-capable. [docs/world/itemdef-re.md; docs/net/novaworld-net-re.md D-NET-97]
bool ItemDatabase::is_ai_capable(int id) const {
	const auto it = items.find(id);
	return it != items.end() && (it->second.attrib & 0x100000u) != 0;
}

// The raw items.def ItemDefAttrib dword (itemDef+0x54); 0 for unknown ids. The AS zone
// traits read bits 0x20000 "ChangeTeam" (capture trigger) and 0x40000 "SpawnPoint"
// (deploy-selectable). [docs/world/itemdef-re.md; net-re §5.61]
uint32_t ItemDatabase::get_attrib(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? 0u : it->second.attrib;
}

uint32_t ItemDatabase::get_attrib2(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? 0u : it->second.attrib2;
}

bool ItemDatabase::get_shadow_decal(int id, String &r_texture,
		Vector4 &r_dims) const {
	const auto it = items.find(id);
	if (it == items.end() || it->second.shadow_texture.is_empty()) {
		return false;
	}
	r_texture = it->second.shadow_texture;
	r_dims = Vector4(it->second.shadow_width, it->second.shadow_length,
			it->second.shadow_offset_x, it->second.shadow_offset_y);
	return true;
}

PackedInt32Array ItemDatabase::get_vehicle_physics(int id) const {
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
	out.push_back(item.water_speed);
	out.push_back(item.climb_speed);
	out.push_back(item.turn_roll);
	out.push_back(item.speed_pitch);
	out.push_back(item.max_slope);
	out.push_back(item.slip_slope);
	out.push_back(item.mass);
	out.push_back(item.lean);
	out.push_back(item.lean_velocity);
	out.push_back(item.pitch);
	out.push_back(item.pitch_velocity);
	out.push_back(item.bob);
	out.push_back(item.flip);
	return out;
}

String ItemDatabase::get_display_name(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? String() : it->second.display_name;
}

// The def-authored closeattack launch userpoint name — the AI muzzle point the
// placer pushes onto the placed model (world-wac-ai-re §21.2). [orig:
// ItemDef_ParseProperty launchups_* -> def+0x5EB/+0x5FB]
String ItemDatabase::get_launchups_closeattack(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? String() : it->second.launchups_closeattack;
}

TypedArray<ItemEmplacementAttachment> ItemDatabase::get_emplacement_attachments(int id) const {
	TypedArray<ItemEmplacementAttachment> out;
	const auto it = items.find(id);
	if (it == items.end()) {
		return out;
	}
	for (size_t i = 0; i < it->second.emplacement_attachments.size(); ++i) {
		const Item::EmplacementAttachment &attachment =
				it->second.emplacement_attachments[i];
		const int stored_slot = static_cast<int>(i + 1);
		Ref<ItemEmplacementAttachment> row;
		row.instantiate();
		row->assign(attachment.kind, attachment.userpoint, attachment.item_id, stored_slot,
				attachment.angle_count, attachment.down_angle, attachment.up_angle,
				attachment.right_angle, attachment.left_angle,
				stored_slot == it->second.emplacement_g_slot,
				stored_slot == it->second.emplacement_c_slot);
		out.push_back(row);
	}
	return out;
}

int ItemDatabase::get_emplacement_g_slot(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? 0 : it->second.emplacement_g_slot;
}

int ItemDatabase::get_emplacement_c_slot(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? 0 : it->second.emplacement_c_slot;
}

bool ItemDatabase::has_mount_config(int id) const {
	const auto it = items.find(id);
	return it != items.end() && it->second.mount_config_valid;
}

int ItemDatabase::get_mount_config(int id) const {
	const auto it = items.find(id);
	return it != items.end() && it->second.mount_config_valid ? it->second.mount_config : 0;
}

String ItemDatabase::get_husk(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? String() : it->second.husk;
}

String ItemDatabase::get_huskfinal(int id) const {
	const auto it = items.find(id);
	return it == items.end() ? String() : it->second.huskfinal;
}


// items.def soundloop_1..7 looping ambient set names for "snd:" marker items
// [orig: ItemDef_ParseProperty @ 0x49eb00, "soundloop_" prefix @ 0x49fec4; the
// 7-slot count matches the engine's Soundloop_1..7 type table @ 0x7d0788].
// S13 (ADR 0028): the envs-class dispatch + soundloop slot resolution runs in
// engine/runtime/audio over the retained items.def parse and the mission's
// native bms document. The shell applies its own bank-presence filtering.
TypedArray<EnvsMarkerRow> ItemDatabase::resolve_envs_markers(
		const Ref<MissionData> &p_mission) const {
	TypedArray<EnvsMarkerRow> out;
	if (p_mission.is_null()) return out;
	const std::vector<opennova::audio::EnvsMarker> markers =
			opennova::audio::resolve_envs_markers(
					p_mission->native_document().bms_file(), native_items());
	for (const opennova::audio::EnvsMarker &marker : markers) {
		Ref<EnvsMarkerRow> row;
		row.instantiate();
		row->assign(marker);
		out.push_back(row);
	}
	return out;
}

PackedStringArray ItemDatabase::get_sound_loops(int id) const {
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

// Slot A ("particlefx") as authored — the one the runtime effect-attach pass
// consumes (item_records.h carries the witness).
Ref<ItemParticleFx> ItemDatabase::get_particle_fx(int id) const {
	const auto it = items.find(id);
	if (it == items.end()) {
		return Ref<ItemParticleFx>();
	}
	Ref<ItemParticleFx> out;
	out.instantiate();
	out->assign(it->second.particlefx.effect, it->second.particlefx.userpoint,
			it->second.particlefx.secondary_effect);
	return out;
}

// The backing store is an unordered_map, so callers that enumerate get a stable
// order only if we impose one. Sort by display name (case-insensitive, the order a
// user scans a palette), breaking ties by id so the order is total and reproducible.
std::vector<const ItemDatabase::Item *> ItemDatabase::sorted_items() const {
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

PackedInt32Array ItemDatabase::get_item_ids() const {
	PackedInt32Array out;
	const std::vector<const Item *> sorted = sorted_items();
	out.resize(static_cast<int>(sorted.size()));
	for (size_t i = 0; i < sorted.size(); ++i) {
		out.set(static_cast<int>(i), sorted[i]->id);
	}
	return out;
}

Dictionary ItemDatabase::extract_seat_specs_for_item(
		const Ref<ResourceRoot> &p_root, int p_item_id) {
	Dictionary out;
	out["item_id"] = p_item_id;
	out["type_id"] =
			p_item_id - static_cast<int>(opennova::mission::kItemIdOffset);
	out["display_name"] = String();
	out["graphic"] = String();
	out["model"] = String();
	out["seats"] = Array();
	out["armory_points"] = Array();
	out["emplacement_attachments"] = Array();
	out["primary_weapon"] = String();
	out["mount_config_valid"] = false;
	out["mount_config"] = 0;
	out["error"] = String();
	if (p_root.is_null()) {
		out["error"] = "missing_resource_root_or_item_db";
		return out;
	}
	if (!has_item(p_item_id)) {
		out["error"] = "item_not_found";
		return out;
	}
	const String graphic = get_graphic(p_item_id);
	out["display_name"] = get_display_name(p_item_id);
	out["graphic"] = graphic;
	if (!graphic.is_empty())
		out["model"] = graphic.get_file().get_basename() + ".3di";

	opennova::simassets::SimModelCache models;
	models.set_index(&p_root->native_index());
	opennova::simassets::SeatSpecExtraction native;
	opennova::simassets::extract_item_seat_specs(
			native_items(),
			[&models](const std::string &key) { return models.model_for(key); },
			{p_item_id}, native);
	const int32_t type_id =
			p_item_id - static_cast<int>(opennova::mission::kItemIdOffset);
	const opennova::mission::ItemSeatSpec *spec = nullptr;
	for (const opennova::mission::ItemSeatSpec &candidate : native.specs) {
		if (candidate.type_id == type_id) {
			spec = &candidate;
			break;
		}
	}
	if (spec == nullptr) return out; // no runtime metadata — an empty card

	static const char *kSeatTypeLabels[] = {
			"none", "passenger", "controller", "gunner", "armory", "driver"};
	Array seats;
	for (const opennova::world::Seat &seat : spec->seats) {
		Dictionary row;
		const int seat_type = static_cast<int>(seat.type);
		row["type"] = seat_type;
		constexpr int kSeatTypeLabelCount =
				static_cast<int>(sizeof(kSeatTypeLabels) / sizeof(kSeatTypeLabels[0]));
		row["type_label"] = seat_type >= 0 && seat_type < kSeatTypeLabelCount
				? String(kSeatTypeLabels[seat_type])
				: String("none");
		row["retail_slot"] = seat.retail_slot;
		row["bone_index"] = seat.bone_index;
		row["pose_index"] = seat.pose_index;
		row["yaw_offset"] = seat.yaw_offset;
		row["local"] = Vector3(seat.seat_local.x, seat.seat_local.y,
				seat.seat_local.z);
		row["source_name"] = String(seat.source_name.c_str());
		row["occupied"] = false;
		seats.push_back(row);
	}
	out["seats"] = seats;
	Array armory;
	for (const opennova::world::Vec3 &p : spec->armory_points)
		armory.push_back(Vector3(p.x, p.y, p.z));
	out["armory_points"] = armory;
	Array attachments;
	for (const opennova::mission::ItemEmplacementAttachmentSpec &attachment :
			spec->emplacement_attachments) {
		Dictionary row;
		row["child_type_id"] = attachment.child_type_id;
		row["item_id"] = attachment.child_type_id +
				static_cast<int>(opennova::mission::kItemIdOffset);
		row["anchor_found"] = attachment.anchor_found;
		attachments.push_back(row);
	}
	out["emplacement_attachments"] = attachments;
	out["primary_weapon"] = String(spec->primary_weapon.c_str());
	out["mount_config_valid"] = spec->mount_config_valid;
	out["mount_config"] = spec->mount_config;
	return out;
}
