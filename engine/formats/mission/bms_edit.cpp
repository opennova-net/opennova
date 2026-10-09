// The mission document's edit operations and typed views over bms::File
// (ADR 0043 slice E11). Every body is the former MissionDocument method with
// the document's file as an explicit argument and the error as an out
// parameter; the witnessed rules and their citations are unchanged.
#include <formats/mission/bms_edit.h>

#include <formats/mission/mission_chains.h>
#include <formats/mission/mission_field.h>

#include "mission_detail.h"
#include "mission_names.h"
#include "mission_records.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace opennova::mission {

using namespace detail; // the shared primitives, unqualified as before

namespace {

// A field of `record` the setters by name take: the field row `key` names, of `type` (null for none, a
// field of another type or one the format sets alone).
const MissionField *settable(MissionRecord record, const std::string &key, MissionFieldType type) {
	const MissionField *field = find_mission_field(record, key);
	return field && field->type == type && field->set ? field : nullptr;
}

// "G11.trn" -> "G11" (case-insensitive on the extension); a bare base passes through.
std::string strip_reference_extension(std::string ref, const char *ext) {
	const size_t ext_len = std::strlen(ext);
	if (ref.size() <= ext_len) return ref;
	const size_t at = ref.size() - ext_len;
	for (size_t i = 0; i < ext_len; ++i) {
		const unsigned char a = static_cast<unsigned char>(ref[at + i]);
		const unsigned char b = static_cast<unsigned char>(ext[i]);
		if (std::tolower(a) != std::tolower(b)) return ref;
	}
	ref.resize(at);
	return ref;
}

bms::Entity *entity_at(bms::File &file, EntityKind kind, size_t index, std::string &error) {
	std::vector<bms::Entity> *list = entities(file, kind);
	if (list == nullptr || index >= list->size()) {
		error = "Mission entity index out of range";
		return nullptr;
	}
	return &(*list)[index];
}

} // namespace

// --- the document as a whole ------------------------------------------------

void make_default(bms::File &file) {
	// Build a minimal, valid, empty mission in memory (no file backing). The only header
	// field the format requires is the magic + version (parse gates magic == "BMS" and the
	// version byte >= kMinVersion); every other field round-trips fine at zero, and write()
	// recomputes no positional offsets. sync_counts() then backfills the fixed
	// waypoint/group/layer tables (and waypoint padding) so bms::write produces a
	// buffer parse() accepts.
	file = {};
	bms::Header &header = file.header;
	header.magic[0] = 'B';
	header.magic[1] = 'M';
	header.magic[2] = 'S';
	header.magic[3] = static_cast<char>(bms::kMinVersion);
	sync_counts(file);
}

// What the original editor's new mission holds is not witnessed (dfx2med, D-MIS-3). The header values
// here are the ones the shipped missions hold [corpus, of the 115 missions the install ships:
// default_str "Default" and mission_type 1 in all 115; bonus_expiration 10 and max_saves 3 in 114;
// map_zoom 0.5 in 86; minutes_per_day 1440 in 47 and start_time 3840 in 23, each the most common
// value (none ships a day of no minutes); every win and lose condition 255, each slot's most common
// value (56 to 66 of the 115; the others hold 0 or a directive's number)]. Every other member is zero:
// no weather, water or fog override, no tile set (the terrain's own stands [orig:
// Terrain_LoadEnvironmentConfig @0x6109C8]).
bool make_blank(bms::File &file, const BlankMission &blank, std::string &error) {
	const auto fits = [&error](const std::string &text, size_t slot, const char *what) {
		if (text.size() <= slot) return true;
		error = std::string("The mission's ") + what + " is longer than its " + std::to_string(slot) + " bytes.";
		return false;
	};
	bms::Header header = {};
	if (!fits(blank.name, sizeof(header.mission_name), "name") || !fits(blank.designer, sizeof(header.designer), "designer") ||
	    !fits(blank.terrain, 16, "terrain") || !fits(blank.environment, sizeof(header.environment), "environment"))
		return false;
	make_default(file);
	bms::Header &made = file.header;
	copy_fixed_field(made.mission_name, sizeof(made.mission_name), blank.name);
	copy_fixed_field(made.designer, sizeof(made.designer), blank.designer);
	copy_fixed_field(made.terrain, 16, blank.terrain);
	copy_fixed_field(made.environment, sizeof(made.environment), blank.environment);
	copy_fixed_field(made.default_str, sizeof(made.default_str), "Default");
	made.mission_type = bms::MissionType::NormalMission;
	made.bonus_expiration = 10;
	made.max_saves = 3;
	made.map_zoom = 0.5f;
	made.minutes_per_day = 1440;
	made.start_time = 3840;
	std::memset(made.win_conditions, 0xFF, sizeof(made.win_conditions));
	std::memset(made.lose_conditions, 0xFF, sizeof(made.lose_conditions));
	return true;
}

void sync_counts(bms::File &file) {
	file.header.num_items = static_cast<uint32_t>(file.items.size());
	file.header.num_buildings = static_cast<uint32_t>(file.buildings.size());
	file.header.num_markers = static_cast<uint32_t>(file.markers.size());
	file.header.num_people = static_cast<uint32_t>(file.organics.size());
	file.header.num_events = static_cast<uint32_t>(file.events.size());
	file.header.area_trigger_count = static_cast<int16_t>(file.area_triggers.size());
	std::vector<uint8_t> loadout_chunk;
	for (const bms::WeaponLoadoutRecord &entry : file.loadout.entries) {
		loadout_chunk.insert(loadout_chunk.end(), entry.name.begin(), entry.name.end());
		loadout_chunk.push_back(0);
		loadout_chunk.insert(loadout_chunk.end(), entry.ammo_primary.begin(), entry.ammo_primary.end());
		loadout_chunk.push_back(0);
		loadout_chunk.insert(loadout_chunk.end(), entry.ammo_secondary.begin(), entry.ammo_secondary.end());
		loadout_chunk.push_back(0);
		if (entry.has_flags) { // a record that wrote three strings writes three (bms.h)
			const std::string flags = entry.flags.empty() ? "-1" : entry.flags;
			loadout_chunk.insert(loadout_chunk.end(), flags.begin(), flags.end());
			loadout_chunk.push_back(0);
		}
	}
	if (!loadout_chunk.empty()) {
		loadout_chunk.push_back(0);
	}
	std::vector<uint8_t> availability_chunk;
	for (const bms::ItemAvailabilityEntry &entry : file.item_availability) {
		availability_chunk.insert(availability_chunk.end(), entry.name.begin(), entry.name.end());
		availability_chunk.push_back(0);
		availability_chunk.push_back(entry.status);
	}
	if (!availability_chunk.empty()) {
		availability_chunk.push_back(0);
	}
	file.header.weapon_loadout_chunk_len = static_cast<uint16_t>(loadout_chunk.size());
	file.header.secondary_chunk_len = static_cast<uint16_t>(availability_chunk.size());
	file.events_count = static_cast<int32_t>(file.events.size());
	file.trigger_count = static_cast<int32_t>(file.triggers.size());
	file.action_count = static_cast<int32_t>(file.actions.size());
	file.bounding_box_count = static_cast<int32_t>(file.bounding_boxes.size());

	if (file.waypoint_records.empty()) {
		file.waypoint_records.resize(bms::kWaypointRecordCount);
	}
	if (file.group_records.empty()) {
		file.group_records.resize(bms::kGroupRecordCount);
	}
	if (file.layer_records.empty()) {
		file.layer_records.resize(bms::kLayerRecordCount);
	}
	// A freshly-resized WaypointRecord has empty padding, so the writer emits a short
	// (8-byte) record that no longer reparses (parse expects the fixed 136-byte record).
	// Normalize every record's padding to the 128-byte payload (128 - markers*4), matching
	// parse_waypoint_record. Idempotent for already-loaded records, so byte-exact
	// round-trips are preserved; it is the from-scratch (make_default) path that needs it.
	for (bms::WaypointRecord &record : file.waypoint_records) {
		resize_waypoint_padding(record, /*preserve_over_count=*/true); // pure round-trip: keep a shipped over-count
	}
}

MissionInfo mission_info(const bms::File &file) {
	MissionInfo out;
	const bms::Header &header = file.header;
	out.mission_name = fixed_string(header.mission_name, sizeof(header.mission_name));
	out.designer = fixed_string(header.designer, sizeof(header.designer));
	out.briefing = fixed_string(header.mission_briefing, sizeof(header.mission_briefing));
	// terrain[48] packs three 16-byte slots (terrain / cnv_file / tt_file); bound the read to the
	// first slot so a full 16-char terrain name does not bleed into cnv_file.
	// MissionInfo carries the references as basenames: a file .bms authors the bare
	// base ("G11") while the wire S2C 0x0B header (retail's g_BmsHeaderBlock) carries
	// the extension ("G11.trn", "FULL_07.env"), so the extension is dropped here and
	// every consumer appends its own.
	out.terrain = strip_reference_extension(fixed_string(header.terrain, 16), ".trn");
	out.environment = strip_reference_extension(
			fixed_string(header.environment, sizeof(header.environment)), ".env");
	out.tile_set = fixed_string(header.terrain_tile, sizeof(header.terrain_tile));
	out.climate = static_cast<int>(header.climate);
	out.weather = static_cast<int>(header.weather_type);
	out.mission_type = static_cast<int>(header.mission_type);
	out.attrib_flags = static_cast<int>(header.attrib_flags);
	out.start_time = header.start_time;
	out.minutes_per_day = header.minutes_per_day;
	out.player_health = static_cast<int>(header.health);
	out.max_saves = header.max_saves;
	out.music = static_cast<int>(header.music);
	out.reverb = static_cast<int>(header.reverb);
	out.wind_speed = static_cast<int>(header.wind_speed);
	out.wind_direction = static_cast<int>(header.wind_direction);
	out.map_zoom = header.map_zoom;
	out.water_override = static_cast<int16_t>(header.water_override);
	out.fog_override = static_cast<int>(header.fog_override);
	out.fog_color[0] = header.fog_color[0];
	out.fog_color[1] = header.fog_color[1];
	out.fog_color[2] = header.fog_color[2];
	out.water_color[0] = header.water_color[0];
	out.water_color[1] = header.water_color[1];
	out.water_color[2] = header.water_color[2];
	out.water_murk = header.murk;
	return out;
}

// --- the header ---------------------------------------------------------------

// The header's setters by name look the key up in the header's field rows (mission_field.cpp), which
// keep each rule: the fixed slots copied at full width, the terrain's first 16-byte slot alone, each
// number cast to its width.
bool set_header_string(bms::File &file, const std::string &field, const std::string &value, std::string &error) {
	const MissionField *row = settable(MissionRecord::Header, field, MissionFieldType::Text);
	if (!row) {
		error = "Unknown header string field: " + field;
		return false;
	}
	return row->set(&file.header, value, error);
}

bool set_header_int(bms::File &file, const std::string &field, int value, std::string &error) {
	const MissionField *row = settable(MissionRecord::Header, field, MissionFieldType::Integer);
	if (!row) {
		error = "Unknown header int field: " + field;
		return false;
	}
	return row->set(&file.header, int64_t(value), error);
}

void set_header_flag(bms::File &file, int bit, bool on) {
	uint32_t flags = static_cast<uint32_t>(file.header.attrib_flags);
	if (on) {
		flags |= static_cast<uint32_t>(bit);
	} else {
		flags &= ~static_cast<uint32_t>(bit);
	}
	file.header.attrib_flags = static_cast<bms::AttribFlags>(flags);
}

bool set_header_float(bms::File &file, const std::string &field, float value, std::string &error) {
	const MissionField *row = settable(MissionRecord::Header, field, MissionFieldType::Real);
	if (!row) {
		error = "Unknown header float field: " + field;
		return false;
	}
	return row->set(&file.header, double(value), error);
}

// --- entities -----------------------------------------------------------------

std::vector<bms::Entity> *entities(bms::File &file, EntityKind kind) {
	switch (kind) {
		case EntityKind::Marker: return &file.markers;
		case EntityKind::Item: return &file.items;
		case EntityKind::Building: return &file.buildings;
		case EntityKind::Organic: return &file.organics;
	}
	return nullptr;
}

const std::vector<bms::Entity> *entities(const bms::File &file, EntityKind kind) {
	switch (kind) {
		case EntityKind::Marker: return &file.markers;
		case EntityKind::Item: return &file.items;
		case EntityKind::Building: return &file.buildings;
		case EntityKind::Organic: return &file.organics;
	}
	return nullptr;
}

size_t entity_count(const bms::File &file, EntityKind kind) {
	const std::vector<bms::Entity> *list = entities(file, kind);
	return list ? list->size() : 0;
}

int entity_item_id(const bms::Entity &entity) {
	return bms_type_id_to_item_id(entity.type_id);
}

EntityTransform entity_transform(const bms::Entity &entity) {
	EntityTransform out;
	out.x = entity.get_x();
	out.y = entity.get_y();
	out.z = entity.get_z();
	out.pitch = entity.pitch;
	out.yaw = entity.yaw;
	out.roll = entity.roll;
	return out;
}

std::string entity_name1(const bms::Entity &entity) {
	return fixed_string(entity.name1, sizeof(entity.name1));
}

std::string entity_name2(const bms::Entity &entity) {
	return fixed_string(entity.name2, sizeof(entity.name2));
}

// The entity's setters by name look the key up in the entity's field rows (mission_field.cpp): the
// uint8-backed fields clamp, ai_flags refuses a bit past the known attributes, name1 / name2 are
// copied at full width.
bool set_entity_property_int(bms::File &file, EntityKind kind, size_t index,
		const std::string &name, int value, std::string &error) {
	bms::Entity *entity = entity_at(file, kind, index, error);
	if (entity == nullptr) return false;
	const MissionField *row = settable(MissionRecord::Entity, name, MissionFieldType::Integer);
	if (!row) {
		error = "Unknown entity int property: " + name;
		return false;
	}
	return row->set(entity, int64_t(value), error);
}

bool set_entity_property_string(bms::File &file, EntityKind kind, size_t index,
		const std::string &name, const std::string &value, std::string &error) {
	bms::Entity *entity = entity_at(file, kind, index, error);
	if (entity == nullptr) return false;
	const MissionField *row = settable(MissionRecord::Entity, name, MissionFieldType::Text);
	if (!row) {
		error = "Unknown entity string property: " + name;
		return false;
	}
	return row->set(entity, value, error);
}

bool set_entity_transform(bms::File &file, EntityKind kind, size_t index,
		const EntityTransform &transform, std::string &error) {
	bms::Entity *entity = entity_at(file, kind, index, error);
	if (entity == nullptr) return false;
	apply_transform(*entity, transform);
	return true;
}

// A new record holds what the original editor's holds when it places an item: the item zeroed, then its
// initializer's values, as its BMS writer lays them into the record [orig: JOTACmed.exe
// MissionItem_AppendByTypeId @ 0x455900 (the memset, then MissionItem_InitFromDefinition @ 0x44dbf0,
// whose bytes jomed.exe and dfx2med.exe carry too); the record's layout sub_44C8E0 @ 0x44c8e0]: the
// waypoint distance 10 (+28), perception and perfectionist 100 (+32, +36), both accuracies 100 (+52, +54),
// the obliqueness 15, the crouch timer 3, the shoot timer 5 and the attention 30, the waypoint advance
// trigger -1 (+68) and the generic string "null" (+120) [orig: JOTACmed.exe @ 0x44dc29..0x44dca6,
// @ 0x44dd30..0x44dd7a]. The engagement and attack distances and the advance timer are the item's
// items.def min_engagement_dist, max_engagement_dist, max_attack_dist and fire_timer [orig: JOTACmed.exe
// @ 0x44dc46..0x44dcc2], which its parse seeds with 16, 320, 16 and 10 at an item's begin [orig:
// JOTACmed.exe ItemsDef_ParseToken @ 0x4310d0] and no shipped item sets but one (to 0): those seeds. The
// map symbol (+81) stays 0; the properties dialog writes its combo's selection there on its OK, -1 with
// none, which the shipped records most often hold [orig: JOTACmed.exe sub_4096D0 @ 0x4096d0]. The team, the
// AI class (name1) and the AI profile (name2) are the item's: its items.def good or evil attribute, its
// sid, its default_aip where that profile exists [orig: JOTACmed.exe @ 0x44dd51..0x44dd86, @ 0x44dcdc..
// 0x44dd2e, sub_44C8E0's name1 copy]; zero and empty here, as an item without them leaves them (D-MIS-10,
// docs/mission/mis-format-re.md).
bms::Entity new_entity(EntityKind kind, int item_id, int id) {
	bms::Entity entity = {};
	entity.type = to_bms_type(kind);
	entity.type_id = item_id_to_bms_type_id(item_id);
	entity.id = id;
	entity.wp_distance = 10;
	entity.perception2 = 100;
	entity.perfectionist2 = 100;
	entity.min_engagement_distance = 16;
	entity.max_engagement_distance = 320;
	entity.w_accuracy1 = 100;
	entity.w_accuracy2 = 100;
	entity.crouch_timer = 3;
	entity.shoot_timer = 5;
	entity.wp_adv_trigger = -1;
	entity.attention = 30;
	entity.obliqueness = 15;
	entity.advancetimer = 10;
	entity.max_attack_distance = 16;
	copy_fixed_field(entity.gen_string, sizeof(entity.gen_string), "null");
	// Editor-authored entities are placed at absolute z (BMS semantics), so a .mis export must
	// declare the height locked, same as the .bms parse path (see bms.cpp parse_entity)
	// [orig: MisLdr_WriteNileProjectXml @ 0x10004930, misldr.dll].
	entity.mis_height_lock = 1;
	return entity;
}

int next_entity_ssn(const bms::File &file) { return next_entity_id(file); }

size_t add_entity(bms::File &file, EntityKind kind, int item_id, const EntityTransform &transform) {
	std::vector<bms::Entity> *list = entities(file, kind);
	list->push_back(make_default_entity(file, kind, item_id, transform));
	sync_counts(file);
	return list->size() - 1;
}

bool remove_entity(bms::File &file, EntityKind kind, size_t index, std::string &error) {
	std::vector<bms::Entity> *list = entities(file, kind);
	if (list == nullptr || index >= list->size()) {
		error = "Mission entity index out of range";
		return false;
	}
	list->erase(list->begin() + static_cast<std::ptrdiff_t>(index));
	if (kind == EntityKind::Marker) {
		repair_waypoint_marker_references(file, index);
	}
	sync_counts(file);
	return true;
}

// --- waypoints ------------------------------------------------------------------

std::vector<WaypointSummary> waypoint_summaries(const bms::File &file) {
	std::vector<WaypointSummary> out;
	out.reserve(file.waypoint_records.size());
	for (size_t i = 0; i < file.waypoint_records.size(); ++i) {
		const bms::WaypointRecord &record = file.waypoint_records[i];
		WaypointSummary summary;
		summary.index = i;
		summary.flags = static_cast<int>(record.flags);
		// Report the count the editor can actually act on. A shipped record may carry a raw
		// marker_count above the 32-slot region (CP19.bms has 39); the parser preserves that on
		// disk for byte-exact round-trip, but the editor only ever has min(count, 32) marker slots,
		// so clamp here to keep the path-list label honest and the ">0 = populated" test correct.
		summary.marker_count = static_cast<int>(
				std::min<uint32_t>(record.marker_count, static_cast<uint32_t>(kMaxWaypointPathMarkers)));
		out.push_back(summary);
	}
	return out;
}

bool waypoint_path(const bms::File &file, size_t index, WaypointPath &out) {
	if (index >= file.waypoint_records.size()) {
		return false;
	}
	out = to_path(file.waypoint_records[index], index);
	return true;
}

bool set_waypoint_path(bms::File &file, size_t index, const std::vector<int> &marker_indices,
		int flags, std::string &error) {
	if (!validate_waypoint_path(file, index, marker_indices, error)) {
		return false;
	}
	apply_waypoint_path_to_record(file.waypoint_records[index], marker_indices, flags);
	return true;
}

bool clear_waypoint_path(bms::File &file, size_t index, std::string &error) {
	return set_waypoint_path(file, index, {}, 0, error);
}

bool add_waypoint_marker(bms::File &file, size_t path_index, int marker_item_id,
		const EntityTransform &transform, int insert_index, std::string &error,
		size_t *out_marker_index) {
	if (path_index >= file.waypoint_records.size()) {
		error = "Waypoint path index out of range";
		return false;
	}
	WaypointPath current = to_path(file.waypoint_records[path_index], path_index);
	if (current.marker_indices.size() >= kMaxWaypointPathMarkers) {
		error = "Waypoint path marker count exceeds 32";
		return false;
	}
	if (!validate_waypoint_path(file, path_index, current.marker_indices, error)) {
		return false;
	}

	bms::Entity marker = make_default_entity(file, EntityKind::Marker, marker_item_id, transform);
	marker.bmsi_attributes |= static_cast<uint32_t>(bms::BmsiAttributeFlags::NavigationWaypoint);
	file.markers.push_back(marker);
	const int new_marker_index = static_cast<int>(file.markers.size() - 1);
	size_t insertion = current.marker_indices.size();
	if (insert_index >= 0) {
		insertion = std::min<size_t>(static_cast<size_t>(insert_index), current.marker_indices.size());
	}
	current.marker_indices.insert(current.marker_indices.begin() + static_cast<std::ptrdiff_t>(insertion), new_marker_index);
	apply_waypoint_path_to_record(file.waypoint_records[path_index], current.marker_indices, current.flags);
	sync_counts(file);
	if (out_marker_index != nullptr) {
		*out_marker_index = file.markers.size() - 1;
	}
	return true;
}

bool insert_waypoint_stop(bms::WaypointRecord &path, size_t index, uint32_t marker, std::string &error) {
	std::vector<uint32_t> &stops = path.waypoint_numbers;
	if (stops.size() >= kMaxWaypointPathMarkers) {
		error = "A path holds 32 stops: a stop past them is in no .bms, and the game reads the next path's words for it.";
		return false;
	}
	stops.insert(stops.begin() + static_cast<std::ptrdiff_t>(std::min(index, stops.size())), marker);
	resize_waypoint_padding(path, /*preserve_over_count=*/false); // the stops changed: the count is theirs
	return true;
}

bool erase_waypoint_stop(bms::WaypointRecord &path, size_t index) {
	std::vector<uint32_t> &stops = path.waypoint_numbers;
	if (index >= stops.size()) return false;
	stops.erase(stops.begin() + static_cast<std::ptrdiff_t>(index));
	resize_waypoint_padding(path, /*preserve_over_count=*/false);
	return true;
}

// --- area triggers ----------------------------------------------------------------

bool area_trigger(const bms::File &file, size_t index, AreaTriggerRecord &out) {
	if (index >= file.area_triggers.size()) {
		return false;
	}
	out = to_area_trigger_record(file.area_triggers[index], index);
	return true;
}

std::vector<AreaTriggerRecord> area_triggers(const bms::File &file) {
	std::vector<AreaTriggerRecord> out;
	out.reserve(file.area_triggers.size());
	for (size_t i = 0; i < file.area_triggers.size(); ++i) {
		out.push_back(to_area_trigger_record(file.area_triggers[i], i));
	}
	return out;
}

size_t add_area_trigger(bms::File &file, const AreaTriggerRecord &record) {
	file.area_triggers.push_back(from_area_trigger_record(record));
	sync_counts(file);
	return file.area_triggers.size() - 1;
}

bool set_area_trigger(bms::File &file, size_t index, const AreaTriggerRecord &record, std::string &error) {
	if (index >= file.area_triggers.size()) {
		error = "Area trigger index out of range";
		return false;
	}
	file.area_triggers[index] = from_area_trigger_record(record);
	return true;
}

bool remove_area_trigger(bms::File &file, size_t index, std::string &error) {
	if (index >= file.area_triggers.size()) {
		error = "Area trigger index out of range";
		return false;
	}
	// A trigger's or an action's zone parameter is the area trigger's ID in the file, which the game
	// remaps to its array index at mission start [orig: EventTrigger_ResolveZoneTriggerRefs @0x453000,
	// EventTrigger_ResolveZoneActionRefs @0x453100; docs/mission/bms-event-runtime-re.md section 7.3]:
	// removing a zone moves no id, so nothing that names another zone is rewritten. What named the
	// removed zone names none from then on: the load neuters such a trigger (it reads false, a negated
	// one true) and zeroes such an action, which event_chain flags.
	file.area_triggers.erase(file.area_triggers.begin() + static_cast<std::ptrdiff_t>(index));
	sync_counts(file);
	return true;
}

// --- the weapon loadout, the item availability rules, the groups -----------------

std::vector<WeaponLoadoutEntry> weapon_loadout(const bms::File &file) {
	std::vector<WeaponLoadoutEntry> out;
	out.reserve(file.loadout.entries.size());
	for (const bms::WeaponLoadoutRecord &entry : file.loadout.entries) {
		out.push_back({entry.name, entry.ammo_primary, entry.ammo_secondary, entry.flags, entry.has_flags});
	}
	return out;
}

bool set_weapon_loadout(bms::File &file, const std::vector<WeaponLoadoutEntry> &entries, std::string &error) {
	// The .bms loadout chunk serializes an empty name as a leading NUL, which the loader reads as the
	// chunk terminator: a nameless entry cannot be stored and would silently drop that weapon (and every
	// entry after it). Reject the whole edit and leave the loadout untouched rather than lose data.
	// Removing a weapon goes through the dedicated remove path, never a blanked name; the editor also
	// guards this up front in _commit_loadout_editors. An empty entry list (delete all) is still valid.
	for (const WeaponLoadoutEntry &entry : entries) {
		if (entry.name.empty()) {
			error = "Weapon loadout entries require a name";
			return false;
		}
	}
	std::vector<bms::WeaponLoadoutRecord> records;
	records.reserve(entries.size());
	for (const WeaponLoadoutEntry &entry : entries) {
		// Names are guaranteed non-empty by the validation above.
		records.push_back({entry.name, entry.ammo_primary, entry.ammo_secondary,
		                   entry.flags.empty() ? "-1" : entry.flags, entry.has_flags});
	}
	file.loadout.entries = std::move(records);
	sync_counts(file);
	return true;
}

bool group(const bms::File &file, size_t index, GroupFields &out) {
	if (index >= file.group_records.size()) {
		return false;
	}
	out.index = index;
	out.field0 = file.group_records[index].flags;
	out.field8 = file.group_records[index].value;
	out.field12 = 10;
	return true;
}

std::vector<GroupFields> groups(const bms::File &file) {
	std::vector<GroupFields> out;
	out.reserve(file.group_records.size());
	for (size_t i = 0; i < file.group_records.size(); ++i) {
		GroupFields g;
		group(file, i, g);
		out.push_back(g);
	}
	return out;
}

bool set_group(bms::File &file, size_t index, int field0, int field8, int field12, std::string &error) {
	if (index >= file.group_records.size()) {
		error = "Group index out of range";
		return false;
	}
	if ((field0 & ~0x3) != 0) {
		error = "Group flags may only use bits 0 and 1";
		return false;
	}
	if (field12 != 10) {
		error = "Group constant must be 10";
		return false;
	}
	file.group_records[index].flags = static_cast<int32_t>(field0);
	file.group_records[index].value = static_cast<int32_t>(field8);
	return true;
}

// --- the event logic ------------------------------------------------------------------

bool event(const bms::File &file, size_t index, MissionEventRecord &out) {
	if (index >= file.events.size()) {
		return false;
	}
	out = to_event_record(file.events[index], index);
	return true;
}

std::vector<MissionEventRecord> events(const bms::File &file) {
	std::vector<MissionEventRecord> out;
	out.reserve(file.events.size());
	for (size_t i = 0; i < file.events.size(); ++i) {
		out.push_back(to_event_record(file.events[i], i));
	}
	return out;
}

bool set_event(bms::File &file, size_t index, const MissionEventRecord &record, std::string &error) {
	if (index >= file.events.size()) {
		error = "Mission event index out of range";
		return false;
	}
	apply_event_record(file.events[index], record);
	return true;
}

bool trigger(const bms::File &file, size_t index, MissionTriggerRecord &out) {
	if (index >= file.triggers.size()) {
		return false;
	}
	out = to_trigger_record(file.triggers[index], index);
	return true;
}

std::vector<MissionTriggerRecord> triggers(const bms::File &file) {
	std::vector<MissionTriggerRecord> out;
	out.reserve(file.triggers.size());
	for (size_t i = 0; i < file.triggers.size(); ++i) {
		out.push_back(to_trigger_record(file.triggers[i], i));
	}
	return out;
}

bool set_trigger(bms::File &file, size_t index, const MissionTriggerRecord &record, std::string &error) {
	if (index >= file.triggers.size()) {
		error = "Mission trigger index out of range";
		return false;
	}
	file.triggers[index] = trigger_from_record(record);
	return true;
}

bool action(const bms::File &file, size_t index, MissionActionRecord &out) {
	if (index >= file.actions.size()) {
		return false;
	}
	out = to_action_record(file.actions[index], index);
	return true;
}

std::vector<MissionActionRecord> actions(const bms::File &file) {
	std::vector<MissionActionRecord> out;
	out.reserve(file.actions.size());
	for (size_t i = 0; i < file.actions.size(); ++i) {
		out.push_back(to_action_record(file.actions[i], i));
	}
	return out;
}

bool set_action(bms::File &file, size_t index, const MissionActionRecord &record, std::string &error) {
	if (index >= file.actions.size()) {
		error = "Mission action index out of range";
		return false;
	}
	file.actions[index] = action_from_record(record);
	return true;
}

// An event's triggers and actions are records the event owns (mission_chains.h): each chain edit
// splits the file's tables into the events' chains, edits one, and joins them back, so every run
// stands where its event does and its first index is the running offset, an empty run's too [corpus:
// 115 of 115 shipped missions]. A file whose tables the chains cannot hold (a record in two runs or in
// none, a run past its table) is refused, nothing changed.
namespace {

std::string run_words(RunLayout layout, const char *table) {
	switch (layout) {
	case RunLayout::OutOfRange: return std::string("Mission event ") + table + " range is invalid";
	case RunLayout::Shared: return std::string("Mission event ") + table + " runs share a record";
	case RunLayout::Unowned: return std::string("A mission ") + table + " lies in no event's run";
	default: return std::string();
	}
}

bool split_for_edit(const bms::File &file, size_t event_index, std::vector<EventChain> &chains, std::string &error) {
	if (event_index >= file.events.size()) {
		error = "Mission event index out of range";
		return false;
	}
	RunReport report;
	if (split_event_chains(file, chains, report)) return true;
	error = run_words(report.triggers, "trigger");
	if (error.empty()) error = run_words(report.actions, "action");
	return false;
}

// One list of a chain edited: a record put in at `local_index`, the one there taken out, or moved by
// `delta` within its chain.
template <class Record>
bool insert_into(std::vector<Record> &list, size_t local_index, const Record &record, const char *what, std::string &error) {
	if (list.size() >= kMaxEventChainEntries) {
		error = std::string("Mission event ") + what + " count exceeds 20";
		return false;
	}
	if (local_index > list.size()) {
		error = std::string("Mission event ") + what + " insert index out of range";
		return false;
	}
	list.insert(list.begin() + static_cast<std::ptrdiff_t>(local_index), record);
	return true;
}

template <class Record>
bool erase_from(std::vector<Record> &list, size_t local_index, const char *what, std::string &error) {
	if (local_index >= list.size()) {
		error = std::string("Mission event ") + what + " index out of range";
		return false;
	}
	list.erase(list.begin() + static_cast<std::ptrdiff_t>(local_index));
	return true;
}

template <class Record>
bool move_within(std::vector<Record> &list, size_t local_index, int delta, const char *what, std::string &error) {
	const int next_local = static_cast<int>(local_index) + delta;
	if (delta == 0 || local_index >= list.size() || next_local < 0 || next_local >= static_cast<int>(list.size())) {
		error = std::string("Mission event ") + what + " move index out of range";
		return false;
	}
	std::swap(list[local_index], list[static_cast<size_t>(next_local)]);
	return true;
}

} // namespace

bool insert_event_trigger(bms::File &file, size_t event_index, size_t local_index,
		const MissionTriggerRecord &record, std::string &error) {
	std::vector<EventChain> chains;
	if (!split_for_edit(file, event_index, chains, error)) return false;
	if (!insert_into(chains[event_index].triggers, local_index, trigger_from_record(record), "trigger", error)) return false;
	join_event_chains(chains, file);
	sync_counts(file);
	return true;
}

bool remove_event_trigger(bms::File &file, size_t event_index, size_t local_index, std::string &error) {
	std::vector<EventChain> chains;
	if (!split_for_edit(file, event_index, chains, error)) return false;
	if (!erase_from(chains[event_index].triggers, local_index, "trigger", error)) return false;
	join_event_chains(chains, file);
	sync_counts(file);
	return true;
}

bool move_event_trigger(bms::File &file, size_t event_index, size_t local_index, int delta, std::string &error) {
	std::vector<EventChain> chains;
	if (!split_for_edit(file, event_index, chains, error)) return false;
	if (!move_within(chains[event_index].triggers, local_index, delta, "trigger", error)) return false;
	join_event_chains(chains, file);
	sync_counts(file);
	return true;
}

bool insert_event_action(bms::File &file, size_t event_index, size_t local_index,
		const MissionActionRecord &record, std::string &error) {
	std::vector<EventChain> chains;
	if (!split_for_edit(file, event_index, chains, error)) return false;
	if (!insert_into(chains[event_index].actions, local_index, action_from_record(record), "action", error)) return false;
	join_event_chains(chains, file);
	sync_counts(file);
	return true;
}

bool remove_event_action(bms::File &file, size_t event_index, size_t local_index, std::string &error) {
	std::vector<EventChain> chains;
	if (!split_for_edit(file, event_index, chains, error)) return false;
	if (!erase_from(chains[event_index].actions, local_index, "action", error)) return false;
	join_event_chains(chains, file);
	sync_counts(file);
	return true;
}

bool move_event_action(bms::File &file, size_t event_index, size_t local_index, int delta, std::string &error) {
	std::vector<EventChain> chains;
	if (!split_for_edit(file, event_index, chains, error)) return false;
	if (!move_within(chains[event_index].actions, local_index, delta, "action", error)) return false;
	join_event_chains(chains, file);
	sync_counts(file);
	return true;
}

size_t add_event(bms::File &file, const MissionEventRecord &record) {
	bms::Event ev = {};
	apply_event_record(ev, record);
	// A fresh event owns no triggers or actions. Its empty runs carry the running offset, which past
	// the last event is each table's size [corpus: every shipped empty run, mission_chains.h]; the
	// inserts above fill it.
	ev.trigger_index = static_cast<int32_t>(file.triggers.size());
	ev.action_index = static_cast<int32_t>(file.actions.size());
	ev.trigger_count = 0;
	ev.action_count = 0;
	file.events.push_back(ev);
	sync_counts(file);
	return file.events.size() - 1;
}

bool remove_event(bms::File &file, size_t index, std::string &error) {
	std::vector<EventChain> chains;
	if (!split_for_edit(file, index, chains, error)) return false;
	chains.erase(chains.begin() + static_cast<std::ptrdiff_t>(index));
	// What names an event by its index (EVENT_REF, docs/mission/bms-event-runtime-re.md section 7.2): an
	// Event trigger's param1 [orig: EventTrigger_EvaluateCondition @0x453620 cat 3 reads events[p1]] and
	// a ResetEvent action's [orig: EventAction_Dispatch @0x4542e0 case 34 clears events[p1]'s latch].
	// Events after the hole shift down by one; a reference to the removed event becomes -1 (dangling),
	// which event_chain then flags as out of range. A zone parameter is an area trigger's id (section
	// 7.3), no event's: removing an event moves none.
	const auto repair = [index](int32_t &param) {
		if (param > static_cast<int32_t>(index)) param -= 1;
		else if (param == static_cast<int32_t>(index)) param = -1;
	};
	for (EventChain &chain : chains) {
		for (bms::Trigger &trig : chain.triggers)
			if (trig.main_type == bms::TriggerMainType::Event) repair(trig.param1);
		for (bms::Action &act : chain.actions)
			if (act.action_type == bms::ActionType::ResetEvent) repair(act.param1);
	}
	join_event_chains(chains, file);
	sync_counts(file);
	return true;
}

bool event_chain(const bms::File &file, size_t index, MissionEventChain &out) {
	out = {};
	if (index >= file.events.size()) {
		return false;
	}
	out.event = to_event_record(file.events[index], index);
	if (!valid_range(out.event.trigger_index, out.event.trigger_count, file.triggers.size())) {
		out.diagnostics.push_back(logic_diagnostic(
				"logic.trigger_range_out_of_range",
				"Event trigger range is outside the mission trigger table.",
				"event",
				static_cast<int>(index)));
	} else {
		for (int i = 0; i < out.event.trigger_count; ++i) {
			const size_t trigger_index = static_cast<size_t>(out.event.trigger_index + i);
			MissionTriggerRecord trig = to_trigger_record(file.triggers[trigger_index], trigger_index);
			out.triggers.push_back(trig);
			out.references.push_back(logic_reference("event", static_cast<int>(index), "trigger", static_cast<int>(trigger_index), 0, static_cast<int>(trigger_index), "trigger", true));
			add_trigger_area_reference(trig, file.area_triggers, out);
			// An Event trigger names an event by its index, as a ResetEvent action does
			// [orig: EventTrigger_EvaluateCondition @0x453620, main type 3 reads events[p1]].
			if (trig.main_type == static_cast<int>(bms::TriggerMainType::Event)) {
				const bool valid = trig.param1 >= 0 && static_cast<size_t>(trig.param1) < file.events.size();
				out.references.push_back(logic_reference("trigger", static_cast<int>(trigger_index), "event", trig.param1, 1, trig.param1, "fired event", valid));
				if (!valid) {
					out.diagnostics.push_back(logic_diagnostic(
							"logic.event_reference_out_of_range",
							"Trigger references an event index outside the mission event table.",
							"trigger",
							static_cast<int>(trigger_index)));
				}
			}
		}
	}
	if (!valid_range(out.event.action_index, out.event.action_count, file.actions.size())) {
		out.diagnostics.push_back(logic_diagnostic(
				"logic.action_range_out_of_range",
				"Event action range is outside the mission action table.",
				"event",
				static_cast<int>(index)));
	} else {
		for (int i = 0; i < out.event.action_count; ++i) {
			const size_t action_index = static_cast<size_t>(out.event.action_index + i);
			MissionActionRecord act = to_action_record(file.actions[action_index], action_index);
			out.actions.push_back(act);
			out.references.push_back(logic_reference("event", static_cast<int>(index), "action", static_cast<int>(action_index), 0, static_cast<int>(action_index), "action", true));
			if (act.action_type == static_cast<int>(bms::ActionType::ResetEvent)) {
				const bool valid = act.param1 >= 0 && static_cast<size_t>(act.param1) < file.events.size();
				out.references.push_back(logic_reference("action", static_cast<int>(action_index), "event", act.param1, 1, act.param1, "reset event", valid));
				if (!valid) {
					out.diagnostics.push_back(logic_diagnostic(
							"logic.event_reference_out_of_range",
							"Action references an event index outside the mission event table.",
							"action",
							static_cast<int>(action_index)));
				}
			}
		}
	}
	return true;
}

MissionLogicSummary logic_summary(const bms::File &file) {
	MissionLogicSummary out;
	out.event_count = file.events.size();
	out.trigger_count = file.triggers.size();
	out.action_count = file.actions.size();
	out.area_trigger_count = file.area_triggers.size();
	for (size_t i = 0; i < file.events.size(); ++i) {
		MissionEventChain chain;
		if (event_chain(file, i, chain)) {
			out.diagnostic_count += chain.diagnostics.size();
		}
	}
	return out;
}

} // namespace opennova::mission
