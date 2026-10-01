// The mission document's edit operations and typed views over bms::File
// (ADR 0043 slice E11). Every body is the former MissionDocument method with
// the document's file as an explicit argument and the error as an out
// parameter; the witnessed rules and their citations are unchanged.
#include <formats/mission/bms_edit.h>

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

// The chain-entry ceiling the insert guards enforce.
constexpr int kMaxEventChainEntries = 20;

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
		const std::string flags = entry.flags.empty() ? "-1" : entry.flags;
		loadout_chunk.insert(loadout_chunk.end(), flags.begin(), flags.end());
		loadout_chunk.push_back(0);
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

size_t add_entity(bms::File &file, EntityKind kind, int item_id, const EntityTransform &transform) {
	std::vector<bms::Entity> *list = entities(file, kind);
	list->push_back(make_default_entity(file, kind, item_id, transform));
	sync_counts(file);
	return list->size() - 1;
}

bms::Entity make_entity(const bms::File &file, EntityKind kind, int item_id, const EntityTransform &transform) {
	return make_default_entity(file, kind, item_id, transform);
}

int next_entity_id(const bms::File &file) {
	int max_id = 0;
	for (const std::vector<bms::Entity> *pool : {&file.items, &file.buildings, &file.markers, &file.organics})
		for (const bms::Entity &entity : *pool) max_id = std::max(max_id, entity.id);
	return max_id + 1;
}

bool entity_id_taken(const bms::File &file, int id) {
	for (const std::vector<bms::Entity> *pool : {&file.items, &file.buildings, &file.markers, &file.organics})
		for (const bms::Entity &entity : *pool)
			if (entity.id == id) return true;
	return false;
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

bool insert_waypoint_stop(bms::File &file, size_t path_index, size_t index, int marker, std::string &error) {
	if (path_index >= file.waypoint_records.size()) {
		error = "Waypoint path index out of range";
		return false;
	}
	const bms::WaypointRecord &record = file.waypoint_records[path_index];
	WaypointPath path = to_path(record, path_index);
	path.marker_indices.insert(path.marker_indices.begin() +
	                                   static_cast<std::ptrdiff_t>(std::min(index, path.marker_indices.size())),
	                           marker);
	return set_waypoint_path(file, path_index, path.marker_indices, path.flags, error);
}

bool erase_waypoint_stop(bms::File &file, size_t path_index, size_t index, std::string &error) {
	if (path_index >= file.waypoint_records.size() || index >= file.waypoint_records[path_index].waypoint_numbers.size()) {
		error = "Waypoint path stop out of range";
		return false;
	}
	WaypointPath path = to_path(file.waypoint_records[path_index], path_index);
	path.marker_indices.erase(path.marker_indices.begin() + static_cast<std::ptrdiff_t>(index));
	return set_waypoint_path(file, path_index, path.marker_indices, path.flags, error);
}

bool set_waypoint_stop(bms::File &file, size_t path_index, size_t index, int marker, std::string &error) {
	if (path_index >= file.waypoint_records.size() || index >= file.waypoint_records[path_index].waypoint_numbers.size()) {
		error = "Waypoint path stop out of range";
		return false;
	}
	WaypointPath path = to_path(file.waypoint_records[path_index], path_index);
	path.marker_indices[index] = marker;
	return set_waypoint_path(file, path_index, path.marker_indices, path.flags, error);
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
	// Repair *IsWithinArea references. Phase-5 RE confirmed param2 is the area-trigger ARRAY INDEX
	// (Entity_IsTeamInTriggerBounds @0x43c730: &unk_A32D10 + 32*param2), so removing a zone shifts every
	// higher index down by one. A reference to the removed zone becomes -1 (dangling), which
	// event_chain then flags. [orig: zone bounds consumers @0x43c730 / @0x43e510]
	const int removed = static_cast<int>(index);
	for (bms::Trigger &t : file.triggers) {
		const bool is_area_trigger =
				(t.main_type == bms::TriggerMainType::Group &&
						t.sub_type == static_cast<int>(bms::GroupTriggerType::GroupIsWithinArea)) ||
				(t.main_type == bms::TriggerMainType::Single &&
						t.sub_type == static_cast<int>(bms::SingleTriggerType::SingleIsWithinArea));
		if (!is_area_trigger) {
			continue;
		}
		if (t.param2 == removed) {
			t.param2 = -1;  // the referenced zone is gone
		} else if (t.param2 > removed) {
			--t.param2;     // zones above the hole shifted down
		}
	}
	file.area_triggers.erase(file.area_triggers.begin() + static_cast<std::ptrdiff_t>(index));
	sync_counts(file);
	return true;
}

// --- the weapon loadout, the item availability rules, the groups -----------------

std::vector<WeaponLoadoutEntry> weapon_loadout(const bms::File &file) {
	std::vector<WeaponLoadoutEntry> out;
	out.reserve(file.loadout.entries.size());
	for (const bms::WeaponLoadoutRecord &entry : file.loadout.entries) {
		out.push_back({entry.name, entry.ammo_primary, entry.ammo_secondary, entry.flags});
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
		                   entry.flags.empty() ? "-1" : entry.flags});
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

bool insert_event_trigger(bms::File &file, size_t event_index, size_t local_index,
		const MissionTriggerRecord &record, std::string &error) {
	return insert_event_trigger(file, event_index, local_index, trigger_from_record(record), error);
}

bool insert_event_trigger(bms::File &file, size_t event_index, size_t local_index,
		const bms::Trigger &trigger, std::string &error) {
	if (event_index >= file.events.size()) {
		error = "Mission event index out of range";
		return false;
	}
	bms::Event &ev = file.events[event_index];
	if (ev.trigger_count >= kMaxEventChainEntries) {
		error = "Mission event trigger count exceeds 20";
		return false;
	}
	if (local_index > ev.trigger_count) {
		error = "Mission event trigger insert index out of range";
		return false;
	}
	if (ev.trigger_count > 0 && !valid_range(ev.trigger_index, ev.trigger_count, file.triggers.size())) {
		error = "Mission event trigger range is invalid";
		return false;
	}
	const size_t global_index = ev.trigger_count == 0 ? file.triggers.size() : static_cast<size_t>(ev.trigger_index) + local_index;
	file.triggers.insert(file.triggers.begin() + static_cast<std::ptrdiff_t>(global_index), trigger);
	for (size_t i = 0; i < file.events.size(); ++i) {
		if (i == event_index) {
			continue;
		}
		bms::Event &other = file.events[i];
		if (other.trigger_count > 0 && other.trigger_index >= static_cast<int32_t>(global_index)) {
			other.trigger_index += 1;
		}
	}
	if (ev.trigger_count == 0) {
		ev.trigger_index = static_cast<int32_t>(global_index);
	}
	ev.trigger_count = static_cast<uint8_t>(ev.trigger_count + 1);
	sync_counts(file);
	return true;
}

bool remove_event_trigger(bms::File &file, size_t event_index, size_t local_index, std::string &error) {
	if (event_index >= file.events.size()) {
		error = "Mission event index out of range";
		return false;
	}
	bms::Event &ev = file.events[event_index];
	if (local_index >= ev.trigger_count) {
		error = "Mission event trigger index out of range";
		return false;
	}
	if (!valid_range(ev.trigger_index, ev.trigger_count, file.triggers.size())) {
		error = "Mission event trigger range is invalid";
		return false;
	}
	const size_t global_index = static_cast<size_t>(ev.trigger_index) + local_index;
	file.triggers.erase(file.triggers.begin() + static_cast<std::ptrdiff_t>(global_index));
	ev.trigger_count = static_cast<uint8_t>(ev.trigger_count - 1);
	if (ev.trigger_count == 0) {
		ev.trigger_index = 0;
	}
	for (size_t i = 0; i < file.events.size(); ++i) {
		if (i == event_index) {
			continue;
		}
		bms::Event &other = file.events[i];
		if (other.trigger_count > 0 && other.trigger_index > static_cast<int32_t>(global_index)) {
			other.trigger_index -= 1;
		}
	}
	sync_counts(file);
	return true;
}

bool move_event_trigger(bms::File &file, size_t event_index, size_t local_index, int delta, std::string &error) {
	if (event_index >= file.events.size()) {
		error = "Mission event index out of range";
		return false;
	}
	bms::Event &ev = file.events[event_index];
	const int next_local = static_cast<int>(local_index) + delta;
	if (delta == 0 || local_index >= ev.trigger_count || next_local < 0 || next_local >= ev.trigger_count) {
		error = "Mission event trigger move index out of range";
		return false;
	}
	if (!valid_range(ev.trigger_index, ev.trigger_count, file.triggers.size())) {
		error = "Mission event trigger range is invalid";
		return false;
	}
	const size_t first = static_cast<size_t>(ev.trigger_index);
	std::swap(file.triggers[first + local_index], file.triggers[first + static_cast<size_t>(next_local)]);
	return true;
}

bool insert_event_action(bms::File &file, size_t event_index, size_t local_index,
		const MissionActionRecord &record, std::string &error) {
	return insert_event_action(file, event_index, local_index, action_from_record(record), error);
}

bool insert_event_action(bms::File &file, size_t event_index, size_t local_index,
		const bms::Action &action, std::string &error) {
	if (event_index >= file.events.size()) {
		error = "Mission event index out of range";
		return false;
	}
	bms::Event &ev = file.events[event_index];
	if (ev.action_count >= kMaxEventChainEntries) {
		error = "Mission event action count exceeds 20";
		return false;
	}
	if (local_index > ev.action_count) {
		error = "Mission event action insert index out of range";
		return false;
	}
	if (ev.action_count > 0 && !valid_range(ev.action_index, ev.action_count, file.actions.size())) {
		error = "Mission event action range is invalid";
		return false;
	}
	const size_t global_index = ev.action_count == 0 ? file.actions.size() : static_cast<size_t>(ev.action_index) + local_index;
	file.actions.insert(file.actions.begin() + static_cast<std::ptrdiff_t>(global_index), action);
	for (size_t i = 0; i < file.events.size(); ++i) {
		if (i == event_index) {
			continue;
		}
		bms::Event &other = file.events[i];
		if (other.action_count > 0 && other.action_index >= static_cast<int32_t>(global_index)) {
			other.action_index += 1;
		}
	}
	if (ev.action_count == 0) {
		ev.action_index = static_cast<int32_t>(global_index);
	}
	ev.action_count = static_cast<uint8_t>(ev.action_count + 1);
	sync_counts(file);
	return true;
}

bool remove_event_action(bms::File &file, size_t event_index, size_t local_index, std::string &error) {
	if (event_index >= file.events.size()) {
		error = "Mission event index out of range";
		return false;
	}
	bms::Event &ev = file.events[event_index];
	if (local_index >= ev.action_count) {
		error = "Mission event action index out of range";
		return false;
	}
	if (!valid_range(ev.action_index, ev.action_count, file.actions.size())) {
		error = "Mission event action range is invalid";
		return false;
	}
	const size_t global_index = static_cast<size_t>(ev.action_index) + local_index;
	file.actions.erase(file.actions.begin() + static_cast<std::ptrdiff_t>(global_index));
	ev.action_count = static_cast<uint8_t>(ev.action_count - 1);
	if (ev.action_count == 0) {
		ev.action_index = 0;
	}
	for (size_t i = 0; i < file.events.size(); ++i) {
		if (i == event_index) {
			continue;
		}
		bms::Event &other = file.events[i];
		if (other.action_count > 0 && other.action_index > static_cast<int32_t>(global_index)) {
			other.action_index -= 1;
		}
	}
	sync_counts(file);
	return true;
}

bool move_event_action(bms::File &file, size_t event_index, size_t local_index, int delta, std::string &error) {
	if (event_index >= file.events.size()) {
		error = "Mission event index out of range";
		return false;
	}
	bms::Event &ev = file.events[event_index];
	const int next_local = static_cast<int>(local_index) + delta;
	if (delta == 0 || local_index >= ev.action_count || next_local < 0 || next_local >= ev.action_count) {
		error = "Mission event action move index out of range";
		return false;
	}
	if (!valid_range(ev.action_index, ev.action_count, file.actions.size())) {
		error = "Mission event action range is invalid";
		return false;
	}
	const size_t first = static_cast<size_t>(ev.action_index);
	std::swap(file.actions[first + local_index], file.actions[first + static_cast<size_t>(next_local)]);
	return true;
}

size_t add_event(bms::File &file, const MissionEventRecord &record) {
	bms::Event ev = {};
	apply_event_record(ev, record);
	// A fresh event owns no triggers/actions; the index/count fields stay zero until the caller adds
	// entries via insert_event_trigger/insert_event_action (each assigns the global index on first add).
	ev.trigger_index = 0;
	ev.action_index = 0;
	ev.trigger_count = 0;
	ev.action_count = 0;
	file.events.push_back(ev);
	sync_counts(file);
	return file.events.size() - 1;
}

bool remove_event(bms::File &file, size_t index, std::string &error) {
	if (!erase_event(file, index, error)) return false;
	// Repair ResetEvent action references (param1 = event index, the one proven cross-reference): events
	// after the hole shift down by one; a reference to the removed event becomes dangling (-1), which
	// event_chain then flags as out-of-range. (Area-trigger refs are left alone because their index
	// semantics are still under RE; here the semantics are proven, so the repair is safe.)
	for (bms::Action &act : file.actions) {
		if (act.action_type != bms::ActionType::ResetEvent) {
			continue;
		}
		if (act.param1 > static_cast<int32_t>(index)) {
			act.param1 -= 1;
		} else if (act.param1 == static_cast<int32_t>(index)) {
			act.param1 = -1;
		}
	}
	return true;
}

bool erase_event(bms::File &file, size_t index, std::string &error) {
	if (index >= file.events.size()) {
		error = "Mission event index out of range";
		return false;
	}
	const bms::Event &ev = file.events[index];
	if (!valid_range(ev.trigger_index, ev.trigger_count, file.triggers.size())) {
		error = "Mission event trigger range is invalid";
		return false;
	}
	if (!valid_range(ev.action_index, ev.action_count, file.actions.size())) {
		error = "Mission event action range is invalid";
		return false;
	}
	// Drain the event's triggers and actions through the single-element removers, which fix up every
	// other event's trigger_index / action_index exactly as a normal trigger/action delete would. The
	// ranges above are validated before the first mutation so malformed documents fail transactionally.
	std::string drain_error;
	while (file.events[index].trigger_count > 0) {
		remove_event_trigger(file, index, 0, drain_error);
	}
	while (file.events[index].action_count > 0) {
		remove_event_action(file, index, 0, drain_error);
	}
	file.events.erase(file.events.begin() + static_cast<std::ptrdiff_t>(index));
	sync_counts(file);
	return true;
}

bool insert_event(bms::File &file, size_t index, const bms::Event &event,
		const std::vector<bms::Trigger> &triggers, const std::vector<bms::Action> &actions,
		std::string &error) {
	if (triggers.size() > size_t(kMaxEventChainEntries) || actions.size() > size_t(kMaxEventChainEntries)) {
		error = "Mission event chain exceeds 20 entries";
		return false;
	}
	// The event goes in owning no chain (its range fields are the file's to assign), then its chain
	// through the single-element inserts, each appended to the file's tables in order.
	bms::Event fresh = event;
	fresh.trigger_index = 0;
	fresh.action_index = 0;
	fresh.trigger_count = 0;
	fresh.action_count = 0;
	index = std::min(index, file.events.size());
	file.events.insert(file.events.begin() + static_cast<std::ptrdiff_t>(index), fresh);
	for (size_t i = 0; i < triggers.size(); ++i)
		if (!insert_event_trigger(file, index, i, triggers[i], error)) return false;
	for (size_t i = 0; i < actions.size(); ++i)
		if (!insert_event_action(file, index, i, actions[i], error)) return false;
	sync_counts(file);
	return true;
}

bool event_trigger_range(const bms::File &file, size_t event_index, size_t &first, size_t &count) {
	if (event_index >= file.events.size()) return false;
	const bms::Event &ev = file.events[event_index];
	if (!valid_range(ev.trigger_index, ev.trigger_count, file.triggers.size())) return false;
	first = ev.trigger_count ? static_cast<size_t>(ev.trigger_index) : 0;
	count = ev.trigger_count;
	return true;
}

bool event_action_range(const bms::File &file, size_t event_index, size_t &first, size_t &count) {
	if (event_index >= file.events.size()) return false;
	const bms::Event &ev = file.events[event_index];
	if (!valid_range(ev.action_index, ev.action_count, file.actions.size())) return false;
	first = ev.action_count ? static_cast<size_t>(ev.action_index) : 0;
	count = ev.action_count;
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
			add_trigger_area_reference(trig, file.area_triggers.size(), out);
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
