#include "nova_mission_runtime.h"

using namespace godot;

namespace {

int command_kind_to_int(opennova::mission::MissionRuntimeCommandKind kind) {
	return static_cast<int>(kind);
}

} // namespace

void NovaMissionRuntime::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load_from_mission", "mission"), &NovaMissionRuntime::load_from_mission);
	ClassDB::bind_method(D_METHOD("reset"), &NovaMissionRuntime::reset);
	ClassDB::bind_method(D_METHOD("tick"), &NovaMissionRuntime::tick);
	ClassDB::bind_method(D_METHOD("get_load_diagnostics"), &NovaMissionRuntime::get_load_diagnostics);
	ClassDB::bind_method(D_METHOD("get_event_count"), &NovaMissionRuntime::get_event_count);
	ClassDB::bind_method(D_METHOD("set_mission_variable", "index", "value"), &NovaMissionRuntime::set_mission_variable);
	ClassDB::bind_method(D_METHOD("get_mission_variable", "index"), &NovaMissionRuntime::get_mission_variable);
	ClassDB::bind_method(D_METHOD("has_event_fired", "index"), &NovaMissionRuntime::has_event_fired);
	ClassDB::bind_method(D_METHOD("set_host_trigger", "main_type", "sub_type", "param1", "param2", "param3", "param4", "value"), &NovaMissionRuntime::set_host_trigger);
	ClassDB::bind_method(D_METHOD("clear_host_triggers"), &NovaMissionRuntime::clear_host_triggers);

	BIND_CONSTANT(COMMAND_HOST_ACTION);
	BIND_CONSTANT(COMMAND_MISSION_VARIABLE_CHANGED);
	BIND_CONSTANT(COMMAND_RESET_EVENT);
	BIND_CONSTANT(COMMAND_OUTPUT_TEXT);
	BIND_CONSTANT(COMMAND_PLAY_DIALOG);
	BIND_CONSTANT(COMMAND_TEAM_WIN);
	BIND_CONSTANT(COMMAND_SUBGOAL);
	BIND_CONSTANT(COMMAND_SHOW_WAYPOINTS);
	BIND_CONSTANT(COMMAND_SET_LIGHT_STATE);
	BIND_CONSTANT(COMMAND_UNSUPPORTED_ACTION);
}

Dictionary NovaMissionRuntime::command_to_dictionary(const opennova::mission::MissionRuntimeCommand &command) {
	Dictionary out;
	out["kind"] = command_kind_to_int(command.kind);
	out["event_index"] = static_cast<int64_t>(command.event_index);
	out["action_index"] = static_cast<int64_t>(command.action_index);
	out["action_type"] = command.action_type;
	out["action_sub_type"] = command.action_sub_type;
	out["param1"] = command.param1;
	out["param2"] = command.param2;
	out["param3"] = command.param3;
	out["param4"] = command.param4;
	out["requires_host"] = command.requires_host;
	out["supported"] = command.supported;
	out["label"] = String(command.label.c_str());
	return out;
}

Dictionary NovaMissionRuntime::diagnostic_to_dictionary(const opennova::mission::MissionRuntimeDiagnostic &diagnostic) {
	Dictionary out;
	out["severity"] = String(diagnostic.severity.c_str());
	out["code"] = String(diagnostic.code.c_str());
	out["message"] = String(diagnostic.message.c_str());
	out["event_index"] = static_cast<int64_t>(diagnostic.event_index);
	out["subject_index"] = diagnostic.subject_index;
	return out;
}

bool NovaMissionRuntime::load_from_mission(const Ref<NovaMissionData> &p_mission) {
	if (!p_mission.is_valid() || !p_mission->is_loaded()) {
		mission.unref();
		runtime = opennova::mission::MissionRuntime();
		return false;
	}
	mission = p_mission;
	return runtime.load(mission->native_document());
}

void NovaMissionRuntime::reset() {
	runtime.reset();
}

Array NovaMissionRuntime::tick() {
	const opennova::mission::MissionRuntimeTickResult result = runtime.tick();
	Array out;
	for (const opennova::mission::MissionRuntimeCommand &command : result.commands) {
		out.push_back(command_to_dictionary(command));
	}
	return out;
}

Array NovaMissionRuntime::get_load_diagnostics() const {
	Array out;
	for (const opennova::mission::MissionRuntimeDiagnostic &diagnostic : runtime.load_diagnostics()) {
		out.push_back(diagnostic_to_dictionary(diagnostic));
	}
	return out;
}

int NovaMissionRuntime::get_event_count() const {
	return static_cast<int>(runtime.event_count());
}

void NovaMissionRuntime::set_mission_variable(int index, int value) {
	runtime.set_mission_variable(index, value);
}

int NovaMissionRuntime::get_mission_variable(int index) const {
	return runtime.get_mission_variable(index);
}

bool NovaMissionRuntime::has_event_fired(int index) const {
	return index >= 0 && runtime.has_event_fired(static_cast<size_t>(index));
}

void NovaMissionRuntime::set_host_trigger(int main_type, int sub_type, int param1, int param2, int param3, int param4, bool value) {
	opennova::mission::MissionRuntimeTriggerKey key;
	key.main_type = main_type;
	key.sub_type = sub_type;
	key.param1 = param1;
	key.param2 = param2;
	key.param3 = param3;
	key.param4 = param4;
	runtime.set_host_trigger(key, value);
}

void NovaMissionRuntime::clear_host_triggers() {
	runtime.clear_host_triggers();
}
