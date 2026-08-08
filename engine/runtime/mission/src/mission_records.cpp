#include "mission_records.h"

// Split out of mission.cpp (quality campaign W3-1). Motion only — every body is
// unchanged, and each original-code citation moved with the code it annotates.
//
// bms:: structs <-> the typed records the editor edits (ADR 0017), plus the entity
// vector plumbing they share.

#include "mission_detail.h"
#include "mission_names.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace opennova::mission::detail {

AreaTriggerRecord to_area_trigger_record(const bms::AreaTrigger &area, size_t index) {
	AreaTriggerRecord out;
	out.index = index;
	// Corrected mapping: file layout is interleaved per axis with a flags dword at off 28.
	// (id at off 0 carried in wp_number for ABI stability; raw flags in reserved.)
	out.wp_number = area.id;
	out.min_x = area.get_x_min();
	out.min_y = area.get_y_min();
	out.min_z = area.get_z_min();
	out.max_x = area.get_x_max();
	out.max_y = area.get_y_max();
	out.max_z = area.get_z_max();
	out.reserved = static_cast<int>(area.flags);
	out.active = area.is_active();
	out.constrain_z = area.constrains_z();
	return out;
}

// Inverse of to_area_trigger_record: build a byte-faithful bms::AreaTrigger from the typed record.
// Bounds go to interleaved fixed-point 16.16 (no swap). The flags dword keeps every bit of `reserved`
// except the two low bits, which are recomposed from active/constrain_z so the UI toggles stay
// consistent with the raw value the engine reads (Entity_IsTeamInTriggerBounds @0x43c75c).
bms::AreaTrigger from_area_trigger_record(const AreaTriggerRecord &rec) {
	bms::AreaTrigger area;
	area.id = rec.wp_number;
	area.x_min = bms::to_fixed_16_16(rec.min_x);
	area.x_max = bms::to_fixed_16_16(rec.max_x);
	area.y_min = bms::to_fixed_16_16(rec.min_y);
	area.y_max = bms::to_fixed_16_16(rec.max_y);
	area.z_min = bms::to_fixed_16_16(rec.min_z);
	area.z_max = bms::to_fixed_16_16(rec.max_z);
	uint32_t flags = static_cast<uint32_t>(rec.reserved);
	flags = (flags & ~(bms::AreaTrigger::kFlagMissionArea | bms::AreaTrigger::kFlagConstrainZ)) |
	        (rec.active ? bms::AreaTrigger::kFlagMissionArea : 0u) |
	        (rec.constrain_z ? bms::AreaTrigger::kFlagConstrainZ : 0u);
	area.flags = flags;
	return area;
}

MissionEventRecord to_event_record(const bms::Event &event, size_t index) {
	MissionEventRecord out;
	out.index = index;
	out.flags = static_cast<int>(event.flags);
	out.trigger_index = event.trigger_index;
	out.action_index = event.action_index;
	out.trigger_count = event.trigger_count;
	out.action_count = event.action_count;
	out.reset_after = event.reset_after;
	out.delay = event.delay;
	out.unknown5 = 0;
	out.unknown6 = 0;
	return out;
}

MissionTriggerRecord to_trigger_record(const bms::Trigger &trigger, size_t index) {
	MissionTriggerRecord out;
	out.index = index;
	out.condition_flags = trigger.condition_flags;
	out.main_type = static_cast<int>(trigger.main_type);
	out.main_type_name = trigger_main_type_name(out.main_type);
	out.sub_type = trigger.sub_type;
	out.sub_type_name = trigger_sub_type_name(out.main_type, out.sub_type);
	out.param1 = trigger.param1;
	out.param2 = trigger.param2;
	out.param3 = trigger.param3;
	out.param4 = trigger.param4;
	out.unknown7 = 0;
	out.negated = trigger.is_negated();
	out.logic_or = trigger.is_or();
	out.logic_xor = trigger.is_xor();
	out.logic_operator = trigger.get_logic_operator();
	return out;
}

MissionActionRecord to_action_record(const bms::Action &action, size_t index) {
	MissionActionRecord out;
	out.index = index;
	out.action_type = static_cast<int>(action.action_type);
	out.action_type_name = action_type_name(out.action_type);
	out.action_sub_type = action.action_sub_type;
	out.action_sub_type_name = action_sub_type_name(out.action_type, out.action_sub_type);
	out.param1 = action.param1;
	out.param2 = action.param2;
	out.param3 = action.param3;
	out.param4 = action.param4;
	out.reserved0 = 0;
	out.reserved1 = 0;
	return out;
}


// reset_after / delay are stored in the upper 10 bits of their u32 slot (see write_event), so the
// representable value range is 0..1023.
constexpr int kMaxEventDelayTicks = 1023;


void apply_event_record(bms::Event &event, const MissionEventRecord &record) {
	const uint32_t preserved_internal = static_cast<uint32_t>(event.flags) & bms::kEventInternalFlagMask;
	const uint32_t requested_known = static_cast<uint32_t>(record.flags) & bms::kEventKnownFlagMask;
	event.flags = static_cast<bms::EventFlags>(preserved_internal | requested_known);
	// reset_after / delay occupy only the upper 10 bits on disk (write_event packs them << 22, parse
	// reads >> 22), so the value range is 0..1023. Clamp here at the library boundary the way the other
	// apply_* setters bound their fields: an out-of-range value would otherwise wrap on serialize
	// (e.g. 2000 -> (uint32)2000 << 22 truncates, reparses as 976) with no error. The editor SpinBox
	// already caps at 1023, but a direct C/C-ABI caller of set_event/add_event does not.
	event.reset_after = std::clamp(record.reset_after, 0, kMaxEventDelayTicks);
	event.delay = std::clamp(record.delay, 0, kMaxEventDelayTicks);
	event.unknown5 = 0;
	event.unknown6 = 0;
}

bms::Trigger trigger_from_record(const MissionTriggerRecord &record) {
	bms::Trigger trigger = {};
	trigger.condition_flags = record.condition_flags;
	trigger.main_type = static_cast<bms::TriggerMainType>(record.main_type);
	trigger.sub_type = record.sub_type;
	trigger.param1 = record.param1;
	trigger.param2 = record.param2;
	trigger.param3 = record.param3;
	trigger.param4 = record.param4;
	trigger.unknown7 = 0;
	return trigger;
}

bms::Action action_from_record(const MissionActionRecord &record) {
	bms::Action action = {};
	action.reserved0 = 0;
	action.action_type = static_cast<bms::ActionType>(record.action_type);
	action.action_sub_type = record.action_sub_type;
	action.param1 = record.param1;
	action.param2 = record.param2;
	action.param3 = record.param3;
	action.param4 = record.param4;
	action.reserved1 = 0;
	return action;
}

MissionLogicDiagnostic logic_diagnostic(const std::string &code,
                                        const std::string &message,
                                        const std::string &subject_kind,
                                        int subject_index) {
	MissionLogicDiagnostic diagnostic;
	diagnostic.severity = "warning";
	diagnostic.code = code;
	diagnostic.message = message;
	diagnostic.subject_kind = subject_kind;
	diagnostic.subject_index = subject_index;
	return diagnostic;
}

bool valid_range(int start, int count, size_t total) {
	if (count == 0) {
		return true;
	}
	if (start < 0 || count < 0) {
		return false;
	}
	const size_t range_start = static_cast<size_t>(start);
	const size_t range_count = static_cast<size_t>(count);
	return range_start <= total && range_count <= total - range_start;
}

MissionLogicReference logic_reference(const std::string &source_kind,
                                      int source_index,
                                      const std::string &target_kind,
                                      int target_index,
                                      int param_slot,
                                      int raw_value,
                                      const std::string &label,
                                      bool valid) {
	MissionLogicReference reference;
	reference.source_kind = source_kind;
	reference.source_index = source_index;
	reference.target_kind = target_kind;
	reference.target_index = target_index;
	reference.param_slot = param_slot;
	reference.raw_value = raw_value;
	reference.label = label;
	reference.valid = valid;
	return reference;
}

void add_trigger_area_reference(const MissionTriggerRecord &trigger,
                                size_t area_count,
                                MissionEventChain &chain) {
	const bool area_trigger =
			(trigger.main_type == static_cast<int>(bms::TriggerMainType::Group) &&
					trigger.sub_type == static_cast<int>(bms::GroupTriggerType::GroupIsWithinArea)) ||
			(trigger.main_type == static_cast<int>(bms::TriggerMainType::Single) &&
					trigger.sub_type == static_cast<int>(bms::SingleTriggerType::SingleIsWithinArea));
	if (!area_trigger) {
		return;
	}
	const int area_index = trigger.param2;
	const bool valid = area_index >= 0 && static_cast<size_t>(area_index) < area_count;
	chain.references.push_back(logic_reference("trigger", static_cast<int>(trigger.index), "area_trigger", area_index, 2, trigger.param2, "area", valid));
	if (!valid) {
		chain.diagnostics.push_back(logic_diagnostic(
				"logic.area_reference_out_of_range",
				"Trigger references an area trigger index outside the mission area table.",
				"trigger",
				static_cast<int>(trigger.index)));
	}
}

std::vector<bms::Entity> *entities_for(bms::File &file, EntityKind kind) {
	switch (kind) {
		case EntityKind::Marker: return &file.markers;
		case EntityKind::Item: return &file.items;
		case EntityKind::Building: return &file.buildings;
		case EntityKind::Organic: return &file.organics;
	}
	return nullptr;
}

const std::vector<bms::Entity> *entities_for(const bms::File &file, EntityKind kind) {
	switch (kind) {
		case EntityKind::Marker: return &file.markers;
		case EntityKind::Item: return &file.items;
		case EntityKind::Building: return &file.buildings;
		case EntityKind::Organic: return &file.organics;
	}
	return nullptr;
}

int next_entity_id(const bms::File &file) {
	int max_id = 0;
	auto scan = [&max_id](const std::vector<bms::Entity> &entities) {
		for (const bms::Entity &entity : entities) {
			max_id = std::max(max_id, entity.id);
		}
	};
	scan(file.items);
	scan(file.buildings);
	scan(file.markers);
	scan(file.organics);
	return max_id + 1;
}

EntityRecord to_record(const bms::Entity &entity, EntityKind kind, size_t index) {
	EntityRecord out;
	out.kind = kind;
	out.index = index;
	out.item_id = bms_type_id_to_item_id(entity.type_id);
	out.bms_type_id = entity.type_id;
	out.bms_id = entity.id;
	out.transform.x = entity.get_x();
	out.transform.y = entity.get_y();
	out.transform.z = entity.get_z();
	out.transform.pitch = entity.pitch;
	out.transform.yaw = entity.yaw;
	out.transform.roll = entity.roll;
	out.group_id = entity.group_id;
	out.waypoint_id = entity.waypoint_id;
	out.wp_number = entity.wp_number;
	out.team = entity.team;
	out.ai_flags = static_cast<int>(entity.bmsi_attributes);
	out.perception = entity.perception2;
	out.accuracy = entity.w_accuracy1;
	out.alert_state = entity.alert_state;
	out.min_engagement_distance = entity.min_engagement_distance;
	out.max_engagement_distance = entity.max_engagement_distance;
	out.max_attack_distance = entity.max_attack_distance;
	out.spawn_count = entity.spawns;
	out.max_simultaneous = entity.no_more_than;
	out.no_less_than = entity.no_less_than;
	out.map_symbol = entity.map_symbol;
	out.name1 = fixed_string(entity.name1, sizeof(entity.name1));
	out.name2 = fixed_string(entity.name2, sizeof(entity.name2));
	return out;
}

void apply_transform(bms::Entity &entity, const EntityTransform &transform) {
	entity.set_x(transform.x);
	entity.set_y(transform.y);
	entity.set_z(transform.z);
	entity.pitch = static_cast<int16_t>(transform.pitch);
	entity.yaw = static_cast<int16_t>(transform.yaw);
	entity.roll = static_cast<int16_t>(transform.roll);
}

void apply_properties(bms::Entity &entity, const EntityProperties &properties) {
	// The uint8-backed fields clamp (rather than a bare static_cast) so an out-of-range value from a
	// programmatic caller saturates instead of silently wrapping (e.g. map_symbol 300 -> 44). The
	// inspector SpinBoxes already cap these, but set_entity_properties is a public API boundary.
	entity.group_id = static_cast<uint8_t>(std::clamp(properties.group_id, 0, 255));
	entity.waypoint_id = static_cast<uint8_t>(std::clamp(properties.waypoint_id, 0, 255));
	entity.wp_number = properties.wp_number;
	entity.team = static_cast<uint8_t>(std::clamp(properties.team, 0, 255));
	entity.bmsi_attributes = static_cast<uint32_t>(properties.ai_flags);
	entity.perception2 = properties.perception;
	entity.w_accuracy1 = static_cast<int16_t>(properties.accuracy);
	entity.alert_state = static_cast<uint8_t>(std::clamp(properties.alert_state, 0, 255));
	entity.min_engagement_distance = properties.min_engagement_distance;
	entity.max_engagement_distance = properties.max_engagement_distance;
	entity.max_attack_distance = properties.max_attack_distance;
	entity.spawns = static_cast<int16_t>(properties.spawn_count);
	entity.no_more_than = static_cast<uint8_t>(std::clamp(properties.max_simultaneous, 0, 255));
	entity.no_less_than = static_cast<uint8_t>(std::clamp(properties.no_less_than, 0, 255));
	entity.map_symbol = static_cast<uint8_t>(std::clamp(properties.map_symbol, 0, 255));
	// name1/name2 are fixed 8-byte slots a mission can fill completely; copy_fixed_field keeps all
	// 8 bytes (copy_cstr would force a NUL into byte 7 and truncate an 8-char name on every edit).
	copy_fixed_field(entity.name1, sizeof(entity.name1), properties.name1);
	copy_fixed_field(entity.name2, sizeof(entity.name2), properties.name2);
}

// Build the editable property set from a record. set_entity_properties overwrites every field, so a
// single-property edit must seed the full set from the current record first. One copy helper shared
// by set_entity_property_int / _string keeps the field list in one place.
EntityProperties properties_from_record(const EntityRecord &record) {
	EntityProperties properties;
	properties.group_id = record.group_id;
	properties.waypoint_id = record.waypoint_id;
	properties.wp_number = record.wp_number;
	properties.team = record.team;
	properties.ai_flags = record.ai_flags;
	properties.perception = record.perception;
	properties.accuracy = record.accuracy;
	properties.alert_state = record.alert_state;
	properties.min_engagement_distance = record.min_engagement_distance;
	properties.max_engagement_distance = record.max_engagement_distance;
	properties.max_attack_distance = record.max_attack_distance;
	properties.spawn_count = record.spawn_count;
	properties.max_simultaneous = record.max_simultaneous;
	properties.no_less_than = record.no_less_than;
	properties.map_symbol = record.map_symbol;
	properties.name1 = record.name1;
	properties.name2 = record.name2;
	return properties;
}



bms::Entity make_default_entity(const bms::File &file,
                                EntityKind kind,
                                int item_id,
                                const EntityTransform &transform) {
	bms::Entity entity = {};
	entity.type = to_bms_type(kind);
	entity.type_id = item_id_to_bms_type_id(item_id);
	entity.id = next_entity_id(file);
	entity.perception2 = 100;
	entity.perfectionist2 = 100;
	entity.min_engagement_distance = 20;
	entity.max_engagement_distance = 200;
	entity.w_accuracy1 = 50;
	entity.w_accuracy2 = 50;
	entity.spawns = 1;
	entity.no_more_than = 1;
	entity.max_attack_distance = 100;
	// Editor-authored entities are placed at absolute z (BMS semantics), so a .mis export must
	// declare the height locked, same as the .bms parse path (see bms.cpp parse_entity)
	// [orig: MisLdr_WriteNileProjectXml @ 0x10004930, misldr.dll].
	entity.mis_height_lock = 1;
	apply_transform(entity, transform);
	return entity;
}

} // namespace opennova::mission::detail
