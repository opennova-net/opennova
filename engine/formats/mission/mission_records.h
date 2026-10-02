#pragma once

// Internal to engine/formats/mission, not part of the public interface.
//
// Conversions between the on-disk bms:: structs and the typed event-logic views
// bms_edit.cpp hands out (ADR 0017, ADR 0043 slice E11).

#include <formats/mission/mission.h>

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::mission::detail {

// (The chain-entry ceiling, the known AI attribute mask and the editable
// int-property table live beside their one user in bms_edit.cpp.)

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

// The area trigger a zone trigger names by its id (a Group or Single IsWithinArea's second parameter,
// a PlayerSatchel's first), as a reference of the chain: its target the index of the first area
// trigger of that id, -1 where none has it; a diagnostic where none has it or its box is flat.
void add_trigger_area_reference(const MissionTriggerRecord &trigger,
                                const std::vector<bms::AreaTrigger> &areas,
                                MissionEventChain &chain);

int next_entity_id(const bms::File &file);

void apply_transform(bms::Entity &entity, const EntityTransform &transform);

bms::Entity make_default_entity(const bms::File &file,
                                EntityKind kind,
                                int item_id,
                                const EntityTransform &transform);

} // namespace opennova::mission::detail
