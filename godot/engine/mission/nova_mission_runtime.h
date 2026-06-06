#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <mission/mission_runtime.h>

#include "mission/nova_mission_data.h"

namespace godot {

class NovaMissionRuntime : public RefCounted {
	GDCLASS(NovaMissionRuntime, RefCounted)

private:
	opennova::mission::MissionRuntime runtime;
	Ref<NovaMissionData> mission;

	static Dictionary command_to_dictionary(const opennova::mission::MissionRuntimeCommand &command);
	static Dictionary diagnostic_to_dictionary(const opennova::mission::MissionRuntimeDiagnostic &diagnostic);

protected:
	static void _bind_methods();

public:
	enum : int {
		COMMAND_HOST_ACTION = 0,
		COMMAND_MISSION_VARIABLE_CHANGED = 1,
		COMMAND_RESET_EVENT = 2,
		COMMAND_OUTPUT_TEXT = 3,
		COMMAND_PLAY_DIALOG = 4,
		COMMAND_TEAM_WIN = 5,
		COMMAND_SUBGOAL = 6,
		COMMAND_SHOW_WAYPOINTS = 7,
		COMMAND_SET_LIGHT_STATE = 8,
		COMMAND_UNSUPPORTED_ACTION = 9,
	};

	bool load_from_mission(const Ref<NovaMissionData> &p_mission);
	void reset();
	Array tick();
	Array get_load_diagnostics() const;
	int get_event_count() const;
	void set_mission_variable(int index, int value);
	int get_mission_variable(int index) const;
	bool has_event_fired(int index) const;
	void set_host_trigger(int main_type, int sub_type, int param1, int param2, int param3, int param4, bool value);
	void clear_host_triggers();
};

} // namespace godot
