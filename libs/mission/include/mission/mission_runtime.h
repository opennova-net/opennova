#pragma once

#include <array>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "mission/mission.h"

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

enum class MissionRuntimeCommandKind : int {
	HostAction = 0,
	MissionVariableChanged = 1,
	ResetEvent = 2,
	OutputText = 3,
	PlayDialog = 4,
	TeamWin = 5,
	Subgoal = 6,
	ShowWaypoints = 7,
	SetLightState = 8,
	UnsupportedAction = 9,
};

struct MissionRuntimeCommand {
	MissionRuntimeCommandKind kind = MissionRuntimeCommandKind::HostAction;
	size_t event_index = 0;
	size_t action_index = 0;
	int action_type = 0;
	int action_sub_type = 0;
	int param1 = 0;
	int param2 = 0;
	int param3 = 0;
	int param4 = 0;
	bool requires_host = false;
	bool supported = true;
	std::string label;
};

struct MissionRuntimeDiagnostic {
	std::string severity;
	std::string code;
	std::string message;
	size_t event_index = 0;
	int subject_index = -1;
};

struct MissionRuntimeTickResult {
	std::vector<MissionRuntimeCommand> commands;
	std::vector<MissionRuntimeDiagnostic> diagnostics;
};

struct MissionRuntimeTriggerKey {
	int main_type = 0;
	int sub_type = 0;
	int param1 = 0;
	int param2 = 0;
	int param3 = 0;
	int param4 = 0;

	bool operator<(const MissionRuntimeTriggerKey &other) const;
};

class MissionRuntime {
public:
	bool load(const MissionDocument &document);
	void reset();

	MissionRuntimeTickResult tick();

	void set_mission_variable(int index, int value);
	int get_mission_variable(int index) const;

	void set_host_trigger(const MissionRuntimeTriggerKey &key, bool value);
	void clear_host_triggers();

	bool has_event_fired(size_t index) const;
	const std::vector<MissionRuntimeDiagnostic> &load_diagnostics() const;
	size_t event_count() const;

private:
	struct EventState {
		bool fired = false;
		int delay_remaining = 0;
		int reset_remaining = 0;
	};

	std::vector<MissionEventChain> chains_;
	std::vector<EventState> states_;
	std::vector<int> mission_variables_;
	std::map<MissionRuntimeTriggerKey, bool> host_triggers_;
	std::vector<MissionRuntimeDiagnostic> load_diagnostics_;

	bool evaluate_chain(const MissionEventChain &chain, MissionRuntimeTickResult &result) const;
	bool evaluate_trigger(const MissionTriggerRecord &trigger, size_t event_index, MissionRuntimeTickResult &result) const;
	void dispatch_action(const MissionActionRecord &action, size_t event_index, MissionRuntimeTickResult &result);
	void add_runtime_diagnostic(MissionRuntimeTickResult &result,
	                            const std::string &code,
	                            const std::string &message,
	                            size_t event_index,
	                            int subject_index = -1) const;
};

} // namespace opennova::mission
