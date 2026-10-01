#pragma once

// The fields of a mission's records as rows (ADR 0046 S13 D10: the mission's setters by name were
// if-chains over a key, one per record; they are this one table, which bms_edit's setters by name
// look a key up in and the editor's mission table projects into the one table shape): each record's
// fields by key (the names the mission editor binding and the .mis writer use), the value each takes,
// the width and the range its record stores it in, the values it takes by name, and how it reads and
// writes the record's own struct. The rules are the witnessed ones the setters kept (the fixed slots
// copied at full width, the byte fields clamped, the AI flags refused past the known attributes, an
// event's delays clamped to their ten packed bits), each cited where it was.

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>

#include <formats/mission/bms.h>

namespace opennova::mission {

// The records a mission's fields are fields of, each its own struct: the header (bms::Header), an
// entity of any of the four pools (bms::Entity), a waypoint path (bms::WaypointRecord), a group
// (bms::GroupRecord), a layer (bms::LayerRecord), an area trigger (bms::AreaTrigger), an event
// (bms::Event), a trigger (bms::Trigger), an action (bms::Action) and a weapon loadout entry
// (bms::WeaponLoadoutRecord). A path's stops name markers of the file, so their field is the file's
// (bms_edit's waypoint stops), not a row here.
enum class MissionRecord { Header, Entity, WaypointPath, Group, Layer, Area, Event, Trigger, Action, Loadout, kCount };
inline constexpr size_t kMissionRecordCount = size_t(MissionRecord::kCount);

// A field's value: an integer, a real (a 16.16 position or bound in mission units, the map zoom) or a
// text.
using MissionValue = std::variant<int64_t, double, std::string>;
enum class MissionFieldType { Integer, Real, Text };

// One value of an enumeration, or one bit of a flag word, by the name its enum in bms.h gives it.
struct MissionChoice {
	const char *name;
	int64_t value;
};

// One field of a record. `width` is a text's slot in bytes (a fixed slot may be full, with no
// terminator), 0 for a text with no slot (a loadout entry's strings, which the chunk ends with a NUL
// each). `min` and `max` are the range a number keeps to: what the record's width holds, or the
// narrower range a rule keeps; a value past it is clamped or refused by `set`, as the rule says, and
// the editor refuses it before. `choices` are the values it takes by name (`flags`: bits of one word),
// none for a number with no names. `set` null: shown only (derived from the record, or a value the
// format sets alone).
struct MissionField {
	MissionRecord record;
	const char *key;
	MissionFieldType type;
	size_t width;
	int64_t min, max;
	const MissionChoice *choices;
	size_t choice_count;
	bool flags;
	bool (*get)(const void *record, MissionValue &out);
	bool (*set)(void *record, const MissionValue &value, std::string &error);
};

// The fields of one record, in the order the editor shows them.
struct MissionFields {
	const MissionField *rows = nullptr;
	size_t count = 0;
	const MissionField *begin() const { return rows; }
	const MissionField *end() const { return rows + count; }
};
MissionFields mission_fields(MissionRecord record);
// The field `key` names among a record's (null: none).
const MissionField *find_mission_field(MissionRecord record, const std::string &key);

// The name a value of an enumeration's choices goes by (null: none of them).
const char *mission_choice_name(const MissionChoice *choices, size_t count, int64_t value);
// A trigger's main types and an action's types by name [orig: EventTrigger_EvaluateCondition @
// 0x453620 dispatches on the main type, EventAction_Dispatch @ 0x4542e0 on the action type]: what
// the typed views name them by (mission_names) and the editor offers.
struct MissionChoices {
	const MissionChoice *rows = nullptr;
	size_t count = 0;
};
MissionChoices trigger_main_types();
MissionChoices action_types();

} // namespace opennova::mission
