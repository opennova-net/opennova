#pragma once

// Internal to libs/mission — not part of the public interface. Split out of
// mission.cpp (quality campaign W3-1); the bodies are unchanged.
//
// Conversions between the on-disk bms:: structs and the typed records the editor
// edits (ADR 0017), plus the entity-vector plumbing they share. The facade owns the
// document; this owns the shape of what comes out of it.

#include "mission/mission.h"

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::mission::detail {

// Shared with the facade (mission.cpp): the chain-entry ceiling its insert guards
// enforce, the AI attribute bits it validates ai_flags against, and the int-property
// table set_entity_property_int walks. constexpr at namespace scope, so each TU gets
// its own copy and there is no ODR question.
constexpr int kMaxEventChainEntries = 20;

constexpr uint32_t kKnownAiAttributeMask =
	static_cast<uint32_t>(bms::BmsiAttributeFlags::Blind) |
	static_cast<uint32_t>(bms::BmsiAttributeFlags::Guarding) |
	static_cast<uint32_t>(bms::BmsiAttributeFlags::RemoveIfLessThan) |
	static_cast<uint32_t>(bms::BmsiAttributeFlags::RemoveIfMoreThan) |
	static_cast<uint32_t>(bms::BmsiAttributeFlags::Multiplayer) |
	static_cast<uint32_t>(bms::BmsiAttributeFlags::Berserk) |
	static_cast<uint32_t>(bms::BmsiAttributeFlags::FlyingOrganic) |
	static_cast<uint32_t>(bms::BmsiAttributeFlags::Coward) |
	static_cast<uint32_t>(bms::BmsiAttributeFlags::Attribute17) |
	static_cast<uint32_t>(bms::BmsiAttributeFlags::AdvancedAmmo) |
	static_cast<uint32_t>(bms::BmsiAttributeFlags::Indestructible) |
	static_cast<uint32_t>(bms::BmsiAttributeFlags::NavigationWaypoint) |
	static_cast<uint32_t>(bms::BmsiAttributeFlags::Reflective) |
	static_cast<uint32_t>(bms::BmsiAttributeFlags::NoShadow);

// The editable int properties, mapping each editor/dictionary key to its EntityProperties member.
// Single source of truth for set_entity_property_int: adding an int field is one row here. Only
// `group` differs from its member name (group_id); every other key equals its member.
struct EntityIntField {
	const char *name;
	int EntityProperties::*member;
};
constexpr EntityIntField kEntityIntFields[] = {
	{"group", &EntityProperties::group_id},
	{"waypoint_id", &EntityProperties::waypoint_id},
	{"wp_number", &EntityProperties::wp_number},
	{"team", &EntityProperties::team},
	{"ai_flags", &EntityProperties::ai_flags},
	{"perception", &EntityProperties::perception},
	{"accuracy", &EntityProperties::accuracy},
	{"alert_state", &EntityProperties::alert_state},
	{"min_engagement_distance", &EntityProperties::min_engagement_distance},
	{"max_engagement_distance", &EntityProperties::max_engagement_distance},
	{"max_attack_distance", &EntityProperties::max_attack_distance},
	{"spawn_count", &EntityProperties::spawn_count},
	{"max_simultaneous", &EntityProperties::max_simultaneous},
	{"no_less_than", &EntityProperties::no_less_than},
	{"map_symbol", &EntityProperties::map_symbol},
};

AreaTriggerRecord to_area_trigger_record(const bms::AreaTrigger &area, size_t index);

bms::AreaTrigger from_area_trigger_record(const AreaTriggerRecord &rec);

MissionEventRecord to_event_record(const bms::Event &event, size_t index);

MissionTriggerRecord to_trigger_record(const bms::Trigger &trigger, size_t index);

MissionActionRecord to_action_record(const bms::Action &action, size_t index);

void apply_event_record(bms::Event &event, const MissionEventRecord &record);

bms::Trigger trigger_from_record(const MissionTriggerRecord &record);

bms::Action action_from_record(const MissionActionRecord &record);

MissionLogicDiagnostic logic_diagnostic(const std::string &code,
                                        const std::string &message,
                                        const std::string &subject_kind,
                                        int subject_index);

bool valid_range(int start, int count, size_t total);

MissionLogicReference logic_reference(const std::string &source_kind,
                                      int source_index,
                                      const std::string &target_kind,
                                      int target_index,
                                      int param_slot,
                                      int raw_value,
                                      const std::string &label,
                                      bool valid);

void add_trigger_area_reference(const MissionTriggerRecord &trigger,
                                size_t area_count,
                                MissionEventChain &chain);

int next_entity_id(const bms::File &file);

EntityRecord to_record(const bms::Entity &entity, EntityKind kind, size_t index);

void apply_transform(bms::Entity &entity, const EntityTransform &transform);

void apply_properties(bms::Entity &entity, const EntityProperties &properties);

EntityProperties properties_from_record(const EntityRecord &record);

bms::Entity make_default_entity(const bms::File &file,
                                EntityKind kind,
                                int item_id,
                                const EntityTransform &transform);

std::vector<bms::Entity> *entities_for(bms::File &file, EntityKind kind);
const std::vector<bms::Entity> *entities_for(const bms::File &file, EntityKind kind);

} // namespace opennova::mission::detail
