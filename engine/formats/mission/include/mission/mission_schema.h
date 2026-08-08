#pragma once

#include <array>
#include <string>
#include <vector>

// Editor-facing parameter SCHEMA for mission triggers + actions. Pure reflection
// metadata (labels, value kinds, enum option lists) the ONED inspector renders for
// each trigger/action; it carries no runtime state and ticks nothing. Mirrors the
// original dfx2med editor's per-widget param tables (Med_*Param* @0x449xxx /
// Med_AiSubTypeParams @0x44A920). Consumed via NovaMissionData.get_*_param_schema ->
// MissionParamSchema.gd -> mission_inspector.gd. Distinct from the runtime evaluators
// (event_runtime.h / wac), which mutate the shared world.

namespace opennova::mission {

enum class MissionParamKind : int {
	Raw = 0,
	Group = 1,
	Entity = 2,
	Zone = 3,
	Event = 4,
	Waypoint = 5,
	Bool = 6,
	Enum = 7,
	FixedSeconds = 8,  // raw int = seconds * 65536 (16.16); the editor shows a seconds spin
};

struct MissionParamEnumEntry {
	int value = 0;
	std::string label;
};

struct MissionParamSlot {
	std::string label;
	MissionParamKind kind = MissionParamKind::Raw;
	std::string tip;
	std::vector<MissionParamEnumEntry> enum_values;
	bool used = true;
};

struct MissionParamSpec {
	std::string description;
	std::array<MissionParamSlot, 4> params;
	bool known = false;
	bool variable = false;
};

MissionParamSpec trigger_param_schema(int main_type, int sub_type);
// action_sub_type selects the per-sub-type slot layout for the AI-change action family
// (CHANGE_GROUP_AI / AREA_AI_RED/BLUE / CHANGE_SINGLE_AI); ignored by other action types.
MissionParamSpec action_param_schema(int action_type, int action_sub_type = 0);
bool mission_param_kind_is_picker(MissionParamKind kind);

} // namespace opennova::mission
