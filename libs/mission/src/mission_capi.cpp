#include "mission/mission_capi.h"

#include "mission/mission.h"

// Split out of mission.cpp (quality campaign W3-1). Motion only — every body is
// unchanged. Nothing here cites an original: the flat C ABI is our own interop
// layer over MissionDocument, not a port of witnessed engine code.
//
// The flat C ABI and the struct copies that feed it. Consumers are the Python FFI
// (ctypes, no header) and tests/mission/mission_c_abi_test.cpp; see libs/CLAUDE.md
// for which functions are Model A (MISSION_EXPORT) and which are C++-link only.

#include "mission_detail.h"
#include "mission_mis.h"

#include <cstring>
#include <new>
#include <string>
#include <vector>

struct OpenNovaMissionDocument {
	opennova::mission::MissionDocument document;
};

namespace opennova::mission {

using namespace detail; // the shared primitives, unqualified as before

namespace {

OpenNovaMissionEntityTransform to_c_transform(const EntityTransform &transform) {
	return {
		transform.x,
		transform.y,
		transform.z,
		transform.pitch,
		transform.yaw,
		transform.roll,
	};
}

EntityTransform from_c_transform(const OpenNovaMissionEntityTransform &transform) {
	return {
		transform.x,
		transform.y,
		transform.z,
		transform.pitch,
		transform.yaw,
		transform.roll,
	};
}

EntityProperties from_c_properties(const OpenNovaMissionEntityProperties &properties) {
	// Assign by name rather than a positional aggregate initializer: a future reorder or insert among
	// EntityProperties' members would otherwise keep compiling while silently copying C-ABI fields into
	// the wrong members. EntityProperties' default member initializers zero the trailing
	// no_less_than / map_symbol / name1 / name2, which are not part of the C-ABI struct.
	EntityProperties out;
	out.group_id = properties.group_id;
	out.waypoint_id = properties.waypoint_id;
	out.wp_number = properties.wp_number;
	out.team = properties.team;
	out.ai_flags = properties.ai_flags;
	out.perception = properties.perception;
	out.accuracy = properties.accuracy;
	out.alert_state = properties.alert_state;
	out.min_engagement_distance = properties.min_engagement_distance;
	out.max_engagement_distance = properties.max_engagement_distance;
	out.max_attack_distance = properties.max_attack_distance;
	out.spawn_count = properties.spawn_count;
	out.max_simultaneous = properties.max_simultaneous;
	return out;
}

void copy_record(OpenNovaMissionEntityRecord &out, const EntityRecord &record) {
	out.kind = static_cast<int>(record.kind);
	out.index = record.index;
	out.item_id = record.item_id;
	out.bms_type_id = record.bms_type_id;
	out.bms_id = record.bms_id;
	out.transform = to_c_transform(record.transform);
	out.group_id = record.group_id;
	out.waypoint_id = record.waypoint_id;
	out.wp_number = record.wp_number;
	out.team = record.team;
	out.ai_flags = record.ai_flags;
	out.perception = record.perception;
	out.accuracy = record.accuracy;
	out.alert_state = record.alert_state;
	out.min_engagement_distance = record.min_engagement_distance;
	out.max_engagement_distance = record.max_engagement_distance;
	out.max_attack_distance = record.max_attack_distance;
	out.spawn_count = record.spawn_count;
	out.max_simultaneous = record.max_simultaneous;
}

void copy_waypoint_summary(OpenNovaMissionWaypointSummary &out, const WaypointSummary &summary) {
	out.index = summary.index;
	out.flags = summary.flags;
	out.marker_count = summary.marker_count;
}

void copy_waypoint_path(OpenNovaMissionWaypointPath &out, const WaypointPath &path) {
	std::memset(&out, 0, sizeof(out));
	out.index = path.index;
	out.flags = path.flags;
	out.marker_count = std::min<size_t>(path.marker_indices.size(), kMaxWaypointPathMarkers);
	for (size_t i = 0; i < out.marker_count; ++i) {
		out.marker_indices[i] = static_cast<uint32_t>(path.marker_indices[i]);
	}
}

void copy_area_trigger(OpenNovaMissionAreaTriggerRecord &out, const AreaTriggerRecord &record) {
	out.index = record.index;
	out.wp_number = record.wp_number;
	out.min_x = record.min_x;
	out.min_y = record.min_y;
	out.min_z = record.min_z;
	out.max_x = record.max_x;
	out.max_y = record.max_y;
	out.max_z = record.max_z;
	out.reserved = record.reserved;
	out.active = record.active ? 1 : 0;
	out.constrain_z = record.constrain_z ? 1 : 0;
}

AreaTriggerRecord from_c_area_trigger(const OpenNovaMissionAreaTriggerRecord &in) {
	AreaTriggerRecord out;
	out.index = in.index;
	out.wp_number = in.wp_number;
	out.min_x = in.min_x;
	out.min_y = in.min_y;
	out.min_z = in.min_z;
	out.max_x = in.max_x;
	out.max_y = in.max_y;
	out.max_z = in.max_z;
	out.reserved = in.reserved;
	out.active = in.active != 0;
	out.constrain_z = in.constrain_z != 0;
	return out;
}

void copy_event(OpenNovaMissionEventRecord &out, const MissionEventRecord &record) {
	out.index = record.index;
	out.flags = record.flags;
	out.trigger_index = record.trigger_index;
	out.action_index = record.action_index;
	out.trigger_count = record.trigger_count;
	out.action_count = record.action_count;
	out.reset_after = record.reset_after;
	out.delay = record.delay;
	out.unknown5 = record.unknown5;
	out.unknown6 = record.unknown6;
}

void copy_trigger(OpenNovaMissionTriggerRecord &out, const MissionTriggerRecord &record) {
	std::memset(&out, 0, sizeof(out));
	out.index = record.index;
	out.condition_flags = record.condition_flags;
	out.main_type = record.main_type;
	copy_cstr(out.main_type_name, sizeof(out.main_type_name), record.main_type_name);
	out.sub_type = record.sub_type;
	copy_cstr(out.sub_type_name, sizeof(out.sub_type_name), record.sub_type_name);
	out.param1 = record.param1;
	out.param2 = record.param2;
	out.param3 = record.param3;
	out.param4 = record.param4;
	out.unknown7 = record.unknown7;
	out.negated = record.negated ? 1 : 0;
	out.logic_or = record.logic_or ? 1 : 0;
	out.logic_xor = record.logic_xor ? 1 : 0;
	copy_cstr(out.logic_operator, sizeof(out.logic_operator), record.logic_operator);
}

void copy_action(OpenNovaMissionActionRecord &out, const MissionActionRecord &record) {
	std::memset(&out, 0, sizeof(out));
	out.index = record.index;
	out.action_type = record.action_type;
	copy_cstr(out.action_type_name, sizeof(out.action_type_name), record.action_type_name);
	out.action_sub_type = record.action_sub_type;
	copy_cstr(out.action_sub_type_name, sizeof(out.action_sub_type_name), record.action_sub_type_name);
	out.param1 = record.param1;
	out.param2 = record.param2;
	out.param3 = record.param3;
	out.param4 = record.param4;
	out.reserved0 = record.reserved0;
	out.reserved1 = record.reserved1;
}

void copy_logic_summary(OpenNovaMissionLogicSummary &out, const MissionLogicSummary &summary) {
	out.event_count = summary.event_count;
	out.trigger_count = summary.trigger_count;
	out.action_count = summary.action_count;
	out.area_trigger_count = summary.area_trigger_count;
	out.diagnostic_count = summary.diagnostic_count;
}
} // namespace

extern "C" {

MISSION_EXPORT OpenNovaMissionDocument *opennova_mission_create(void) {
	return new (std::nothrow) OpenNovaMissionDocument();
}

MISSION_EXPORT void opennova_mission_destroy(OpenNovaMissionDocument *document) {
	delete document;
}

void opennova_mission_clear(OpenNovaMissionDocument *document) {
	if (document != nullptr) {
		document->document.clear();
	}
}

MISSION_EXPORT void opennova_mission_create_default(OpenNovaMissionDocument *document) {
	if (document != nullptr) {
		document->document.create_default();
	}
}

MISSION_EXPORT int opennova_mission_load_path(OpenNovaMissionDocument *document, const char *path) {
	if (document == nullptr || path == nullptr) {
		return 0;
	}
	const std::string path_string(path);
	if (extension_lower(path_string) == "mis") {
		return document->document.load_mis_file(path_string) ? 1 : 0;
	}
	return document->document.load_bms_file(path_string) ? 1 : 0;
}

int opennova_mission_load_bytes(OpenNovaMissionDocument *document, const uint8_t *data, size_t size) {
	if (document == nullptr) {
		return 0;
	}
	return document->document.load_bms_bytes(data, size) ? 1 : 0;
}

int opennova_mission_save_path(OpenNovaMissionDocument *document, const char *path) {
	if (document == nullptr || path == nullptr) {
		return 0;
	}
	const std::string path_string(path);
	if (extension_lower(path_string) == "mis") {
		return document->document.save_mis_file(path_string) ? 1 : 0;
	}
	return document->document.save_bms_file(path_string) ? 1 : 0;
}

int opennova_mission_write_bytes(OpenNovaMissionDocument *document, OpenNovaMissionBytes *out_bytes) {
	if (document == nullptr || out_bytes == nullptr) {
		return 0;
	}
	out_bytes->data = nullptr;
	out_bytes->size = 0;
	std::vector<uint8_t> bytes;
	if (!document->document.write_bms_bytes(bytes)) {
		return 0;
	}
	if (!bytes.empty()) {
		out_bytes->data = new (std::nothrow) uint8_t[bytes.size()];
		if (out_bytes->data == nullptr) {
			return 0;
		}
		std::memcpy(out_bytes->data, bytes.data(), bytes.size());
	}
	out_bytes->size = bytes.size();
	return 1;
}

MISSION_EXPORT int opennova_mission_save_mis_path(OpenNovaMissionDocument *document, const char *path) {
	if (document == nullptr || path == nullptr) {
		return 0;
	}
	return document->document.save_mis_file(path) ? 1 : 0;
}

MISSION_EXPORT int opennova_mission_write_mis_text(OpenNovaMissionDocument *document, OpenNovaMissionBytes *out_bytes) {
	if (document == nullptr || out_bytes == nullptr) {
		return 0;
	}
	out_bytes->data = nullptr;
	out_bytes->size = 0;
	std::string text;
	if (!document->document.write_mis_text(text)) {
		return 0;
	}
	uint8_t *data = new (std::nothrow) uint8_t[text.size()];
	if (data == nullptr && !text.empty()) {
		return 0;
	}
	if (!text.empty()) {
		std::memcpy(data, text.data(), text.size());
	}
	out_bytes->data = data;
	out_bytes->size = text.size();
	return 1;
}

MISSION_EXPORT void opennova_mission_free_bytes(OpenNovaMissionBytes *bytes) {
	if (bytes == nullptr) {
		return;
	}
	delete[] bytes->data;
	bytes->data = nullptr;
	bytes->size = 0;
}

int opennova_mission_is_loaded(const OpenNovaMissionDocument *document) {
	return document != nullptr && document->document.is_loaded() ? 1 : 0;
}

const char *opennova_mission_source_path(const OpenNovaMissionDocument *document) {
	if (document == nullptr) {
		return "";
	}
	return document->document.source_path().c_str();
}

MISSION_EXPORT const char *opennova_mission_last_error(const OpenNovaMissionDocument *document) {
	if (document == nullptr) {
		return "Invalid mission document";
	}
	return document->document.last_error().c_str();
}

int opennova_mission_get_info(const OpenNovaMissionDocument *document, OpenNovaMissionInfo *out_info) {
	if (document == nullptr || out_info == nullptr || !document->document.is_loaded()) {
		return 0;
	}
	std::memset(out_info, 0, sizeof(*out_info));
	const opennova::mission::MissionInfo info = document->document.info();
	copy_cstr(out_info->mission_name, sizeof(out_info->mission_name), info.mission_name);
	copy_cstr(out_info->designer, sizeof(out_info->designer), info.designer);
	copy_cstr(out_info->briefing, sizeof(out_info->briefing), info.briefing);
	copy_cstr(out_info->terrain, sizeof(out_info->terrain), info.terrain);
	copy_cstr(out_info->environment, sizeof(out_info->environment), info.environment);
	out_info->climate = info.climate;
	out_info->weather = info.weather;
	out_info->attrib_flags = info.attrib_flags;
	out_info->start_time = info.start_time;
	out_info->minutes_per_day = info.minutes_per_day;
	out_info->player_health = info.player_health;
	out_info->max_saves = info.max_saves;
	out_info->music = info.music;
	out_info->reverb = info.reverb;
	return 1;
}

size_t opennova_mission_entity_count(const OpenNovaMissionDocument *document, int kind) {
	if (document == nullptr) {
		return 0;
	}
	opennova::mission::EntityKind entity_kind;
	if (!from_int_kind(kind, entity_kind)) {
		return 0;
	}
	return document->document.entity_count(entity_kind);
}

int opennova_mission_get_entity(const OpenNovaMissionDocument *document,
                                int kind,
                                size_t index,
                                OpenNovaMissionEntityRecord *out_record) {
	if (document == nullptr || out_record == nullptr) {
		return 0;
	}
	opennova::mission::EntityKind entity_kind;
	if (!from_int_kind(kind, entity_kind)) {
		return 0;
	}
	opennova::mission::EntityRecord record;
	if (!document->document.get_entity(entity_kind, index, record)) {
		return 0;
	}
	copy_record(*out_record, record);
	return 1;
}

int opennova_mission_set_entity_transform(OpenNovaMissionDocument *document,
                                          int kind,
                                          size_t index,
                                          const OpenNovaMissionEntityTransform *transform) {
	if (document == nullptr || transform == nullptr) {
		return 0;
	}
	opennova::mission::EntityKind entity_kind;
	if (!from_int_kind(kind, entity_kind)) {
		return 0;
	}
	return document->document.set_entity_transform(entity_kind, index, from_c_transform(*transform)) ? 1 : 0;
}

int opennova_mission_set_entity_properties(OpenNovaMissionDocument *document,
                                           int kind,
                                           size_t index,
                                           const OpenNovaMissionEntityProperties *properties,
                                           OpenNovaMissionEntityRecord *out_record) {
	if (document == nullptr || properties == nullptr) {
		return 0;
	}
	opennova::mission::EntityKind entity_kind;
	if (!from_int_kind(kind, entity_kind)) {
		return 0;
	}
	// set_entity_properties overwrites the whole record. The C-ABI struct can't carry name1/name2
	// (std::string) or the no_less_than/map_symbol ints, so from_c_properties leaves them default;
	// seed those four from the current record first so this bulk setter preserves them instead of
	// zeroing/clearing them. (The GDScript editor path is unaffected: it sets fields individually via
	// set_entity_property_int/_string, which seed the full set from properties_from_record.)
	opennova::mission::EntityProperties props = from_c_properties(*properties);
	opennova::mission::EntityRecord current;
	if (document->document.get_entity(entity_kind, index, current)) {
		props.no_less_than = current.no_less_than;
		props.map_symbol = current.map_symbol;
		props.name1 = current.name1;
		props.name2 = current.name2;
	}
	opennova::mission::EntityRecord record;
	if (!document->document.set_entity_properties(entity_kind, index, props, &record)) {
		return 0;
	}
	if (out_record != nullptr) {
		copy_record(*out_record, record);
	}
	return 1;
}

int opennova_mission_add_entity(OpenNovaMissionDocument *document,
                                int kind,
                                int item_id,
                                const OpenNovaMissionEntityTransform *transform,
                                OpenNovaMissionEntityRecord *out_record) {
	if (document == nullptr || transform == nullptr) {
		return 0;
	}
	opennova::mission::EntityKind entity_kind;
	if (!from_int_kind(kind, entity_kind)) {
		return 0;
	}
	opennova::mission::EntityRecord record;
	if (!document->document.add_entity(entity_kind, item_id, from_c_transform(*transform), &record)) {
		return 0;
	}
	if (out_record != nullptr) {
		copy_record(*out_record, record);
	}
	return 1;
}

int opennova_mission_remove_entity(OpenNovaMissionDocument *document, int kind, size_t index) {
	if (document == nullptr) {
		return 0;
	}
	opennova::mission::EntityKind entity_kind;
	if (!from_int_kind(kind, entity_kind)) {
		return 0;
	}
	return document->document.remove_entity(entity_kind, index) ? 1 : 0;
}

size_t opennova_mission_waypoint_summary_count(const OpenNovaMissionDocument *document) {
	if (document == nullptr || !document->document.is_loaded()) {
		return 0;
	}
	return document->document.waypoint_summaries().size();
}

int opennova_mission_get_waypoint_summary(const OpenNovaMissionDocument *document,
                                          size_t index,
                                          OpenNovaMissionWaypointSummary *out_summary) {
	if (document == nullptr || out_summary == nullptr) {
		return 0;
	}
	const std::vector<opennova::mission::WaypointSummary> summaries = document->document.waypoint_summaries();
	if (index >= summaries.size()) {
		return 0;
	}
	copy_waypoint_summary(*out_summary, summaries[index]);
	return 1;
}

size_t opennova_mission_waypoint_path_count(const OpenNovaMissionDocument *document) {
	if (document == nullptr || !document->document.is_loaded()) {
		return 0;
	}
	return document->document.waypoint_path_count();
}

int opennova_mission_get_waypoint_path(const OpenNovaMissionDocument *document,
                                       size_t index,
                                       OpenNovaMissionWaypointPath *out_path) {
	if (document == nullptr || out_path == nullptr) {
		return 0;
	}
	opennova::mission::WaypointPath path;
	if (!document->document.get_waypoint_path(index, path)) {
		return 0;
	}
	copy_waypoint_path(*out_path, path);
	return 1;
}

int opennova_mission_set_waypoint_path(OpenNovaMissionDocument *document,
                                       size_t index,
                                       const uint32_t *marker_indices,
                                       size_t marker_count,
                                       int flags,
                                       OpenNovaMissionWaypointPath *out_path) {
	if (document == nullptr || (marker_indices == nullptr && marker_count > 0)) {
		return 0;
	}
	std::vector<int> indices;
	indices.reserve(marker_count);
	for (size_t i = 0; i < marker_count; ++i) {
		if (marker_indices[i] > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
			return 0;
		}
		indices.push_back(static_cast<int>(marker_indices[i]));
	}
	opennova::mission::WaypointPath path;
	if (!document->document.set_waypoint_path(index, indices, flags, &path)) {
		return 0;
	}
	if (out_path != nullptr) {
		copy_waypoint_path(*out_path, path);
	}
	return 1;
}

int opennova_mission_clear_waypoint_path(OpenNovaMissionDocument *document,
                                         size_t index,
                                         OpenNovaMissionWaypointPath *out_path) {
	if (document == nullptr) {
		return 0;
	}
	opennova::mission::WaypointPath path;
	if (!document->document.clear_waypoint_path(index, &path)) {
		return 0;
	}
	if (out_path != nullptr) {
		copy_waypoint_path(*out_path, path);
	}
	return 1;
}

int opennova_mission_add_waypoint_marker(OpenNovaMissionDocument *document,
                                         size_t path_index,
                                         int marker_item_id,
                                         const OpenNovaMissionEntityTransform *transform,
                                         int insert_index,
                                         OpenNovaMissionEntityRecord *out_marker,
                                         OpenNovaMissionWaypointPath *out_path) {
	if (document == nullptr || transform == nullptr) {
		return 0;
	}
	opennova::mission::EntityRecord marker;
	opennova::mission::WaypointPath path;
	if (!document->document.add_waypoint_marker(
				path_index,
				marker_item_id,
				from_c_transform(*transform),
				insert_index,
				&marker,
				&path)) {
		return 0;
	}
	if (out_marker != nullptr) {
		copy_record(*out_marker, marker);
	}
	if (out_path != nullptr) {
		copy_waypoint_path(*out_path, path);
	}
	return 1;
}

size_t opennova_mission_area_trigger_count(const OpenNovaMissionDocument *document) {
	if (document == nullptr || !document->document.is_loaded()) {
		return 0;
	}
	return document->document.area_trigger_count();
}

int opennova_mission_get_area_trigger(const OpenNovaMissionDocument *document,
                                      size_t index,
                                      OpenNovaMissionAreaTriggerRecord *out_record) {
	if (document == nullptr || out_record == nullptr) {
		return 0;
	}
	opennova::mission::AreaTriggerRecord record;
	if (!document->document.get_area_trigger(index, record)) {
		return 0;
	}
	copy_area_trigger(*out_record, record);
	return 1;
}

int opennova_mission_add_area_trigger(OpenNovaMissionDocument *document,
                                      const OpenNovaMissionAreaTriggerRecord *record,
                                      OpenNovaMissionAreaTriggerRecord *out_record) {
	if (document == nullptr || record == nullptr) {
		return 0;
	}
	opennova::mission::AreaTriggerRecord out;
	if (!document->document.add_area_trigger(from_c_area_trigger(*record), &out)) {
		return 0;
	}
	if (out_record != nullptr) {
		copy_area_trigger(*out_record, out);
	}
	return 1;
}

int opennova_mission_set_area_trigger(OpenNovaMissionDocument *document,
                                      size_t index,
                                      const OpenNovaMissionAreaTriggerRecord *record,
                                      OpenNovaMissionAreaTriggerRecord *out_record) {
	if (document == nullptr || record == nullptr) {
		return 0;
	}
	opennova::mission::AreaTriggerRecord out;
	if (!document->document.set_area_trigger(index, from_c_area_trigger(*record), &out)) {
		return 0;
	}
	if (out_record != nullptr) {
		copy_area_trigger(*out_record, out);
	}
	return 1;
}

int opennova_mission_remove_area_trigger(OpenNovaMissionDocument *document, size_t index) {
	if (document == nullptr) {
		return 0;
	}
	return document->document.remove_area_trigger(index) ? 1 : 0;
}

size_t opennova_mission_weapon_loadout_count(const OpenNovaMissionDocument *document) {
	if (document == nullptr || !document->document.is_loaded()) {
		return 0;
	}
	return document->document.weapon_loadout().size();
}

int opennova_mission_get_weapon_loadout_entry(const OpenNovaMissionDocument *document,
                                              size_t index,
                                              OpenNovaMissionWeaponLoadoutEntry *out_entry) {
	if (document == nullptr || out_entry == nullptr) {
		return 0;
	}
	const std::vector<WeaponLoadoutEntry> entries = document->document.weapon_loadout();
	if (index >= entries.size()) {
		return 0;
	}
	const WeaponLoadoutEntry &entry = entries[index];
	copy_cstr(out_entry->name, sizeof(out_entry->name), entry.name);
	copy_cstr(out_entry->ammo_primary, sizeof(out_entry->ammo_primary), entry.ammo_primary);
	copy_cstr(out_entry->ammo_secondary, sizeof(out_entry->ammo_secondary), entry.ammo_secondary);
	copy_cstr(out_entry->flags, sizeof(out_entry->flags), entry.flags);
	return 1;
}

int opennova_mission_set_weapon_loadout(OpenNovaMissionDocument *document,
                                        const OpenNovaMissionWeaponLoadoutEntry *entries,
                                        size_t count) {
	if (document == nullptr || (entries == nullptr && count > 0)) {
		return 0;
	}
	std::vector<WeaponLoadoutEntry> records;
	records.reserve(count);
	for (size_t i = 0; i < count; ++i) {
		WeaponLoadoutEntry record;
		record.name = fixed_string(entries[i].name, sizeof(entries[i].name));
		record.ammo_primary = fixed_string(entries[i].ammo_primary, sizeof(entries[i].ammo_primary));
		record.ammo_secondary = fixed_string(entries[i].ammo_secondary, sizeof(entries[i].ammo_secondary));
		record.flags = fixed_string(entries[i].flags, sizeof(entries[i].flags));
		if (record.flags.empty()) record.flags = "-1";
		records.push_back(std::move(record));
	}
	return document->document.set_weapon_loadout(records) ? 1 : 0;
}

size_t opennova_mission_group_count(const OpenNovaMissionDocument *document) {
	if (document == nullptr || !document->document.is_loaded()) {
		return 0;
	}
	return document->document.group_count();
}

int opennova_mission_get_group(const OpenNovaMissionDocument *document,
                               size_t index,
                               OpenNovaMissionGroupRecord *out_record) {
	if (document == nullptr || out_record == nullptr) {
		return 0;
	}
	GroupFields fields;
	if (!document->document.get_group(index, fields)) {
		return 0;
	}
	out_record->index = fields.index;
	out_record->field0 = fields.field0;
	out_record->field8 = fields.field8;
	out_record->field12 = fields.field12;
	return 1;
}

int opennova_mission_set_group(OpenNovaMissionDocument *document,
                               size_t index,
                               int field0, int field8, int field12) {
	if (document == nullptr) {
		return 0;
	}
	return document->document.set_group(index, field0, field8, field12) ? 1 : 0;
}

size_t opennova_mission_event_count(const OpenNovaMissionDocument *document) {
	if (document == nullptr || !document->document.is_loaded()) {
		return 0;
	}
	return document->document.event_count();
}

int opennova_mission_get_event(const OpenNovaMissionDocument *document,
                               size_t index,
                               OpenNovaMissionEventRecord *out_record) {
	if (document == nullptr || out_record == nullptr) {
		return 0;
	}
	opennova::mission::MissionEventRecord record;
	if (!document->document.get_event(index, record)) {
		return 0;
	}
	copy_event(*out_record, record);
	return 1;
}

size_t opennova_mission_trigger_count(const OpenNovaMissionDocument *document) {
	if (document == nullptr || !document->document.is_loaded()) {
		return 0;
	}
	return document->document.trigger_count();
}

int opennova_mission_get_trigger(const OpenNovaMissionDocument *document,
                                 size_t index,
                                 OpenNovaMissionTriggerRecord *out_record) {
	if (document == nullptr || out_record == nullptr) {
		return 0;
	}
	opennova::mission::MissionTriggerRecord record;
	if (!document->document.get_trigger(index, record)) {
		return 0;
	}
	copy_trigger(*out_record, record);
	return 1;
}

size_t opennova_mission_action_count(const OpenNovaMissionDocument *document) {
	if (document == nullptr || !document->document.is_loaded()) {
		return 0;
	}
	return document->document.action_count();
}

int opennova_mission_get_action(const OpenNovaMissionDocument *document,
                                size_t index,
                                OpenNovaMissionActionRecord *out_record) {
	if (document == nullptr || out_record == nullptr) {
		return 0;
	}
	opennova::mission::MissionActionRecord record;
	if (!document->document.get_action(index, record)) {
		return 0;
	}
	copy_action(*out_record, record);
	return 1;
}

int opennova_mission_get_logic_summary(const OpenNovaMissionDocument *document,
                                       OpenNovaMissionLogicSummary *out_summary) {
	if (document == nullptr || out_summary == nullptr || !document->document.is_loaded()) {
		return 0;
	}
	copy_logic_summary(*out_summary, document->document.logic_summary());
	return 1;
}

} // extern "C"

} // namespace opennova::mission
