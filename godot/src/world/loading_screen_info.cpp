#include "world/loading_screen_info.h"

using namespace godot;

Ref<LoadingScreenInfo> LoadingScreenInfo::make(const String &p_mission_file, bool p_in_session,
		const String &p_server_name, const String &p_mission_name, int p_game_type,
		const String &p_custom_text) {
	Ref<LoadingScreenInfo> info;
	info.instantiate();
	info->mission_file_ = p_mission_file;
	info->in_session_ = p_in_session;
	info->server_name_ = p_server_name;
	info->mission_name_ = p_mission_name;
	info->game_type_ = p_game_type;
	info->custom_text_ = p_custom_text;
	return info;
}

Ref<LoadingScreenInfo> LoadingScreenInfo::for_mission(const String &p_mission_file) {
	return make(p_mission_file, false, String(), String(), -1, String());
}

void LoadingScreenInfo::_bind_methods() {
	ClassDB::bind_static_method("LoadingScreenInfo",
			D_METHOD("make", "mission_file", "in_session", "server_name", "mission_name",
					"game_type", "custom_text"),
			&LoadingScreenInfo::make);
	ClassDB::bind_static_method("LoadingScreenInfo", D_METHOD("for_mission", "mission_file"),
			&LoadingScreenInfo::for_mission);
	ClassDB::bind_method(D_METHOD("get_mission_file"), &LoadingScreenInfo::get_mission_file);
	ClassDB::bind_method(D_METHOD("set_mission_file", "value"), &LoadingScreenInfo::set_mission_file);
	ClassDB::bind_method(D_METHOD("get_in_session"), &LoadingScreenInfo::get_in_session);
	ClassDB::bind_method(D_METHOD("set_in_session", "value"), &LoadingScreenInfo::set_in_session);
	ClassDB::bind_method(D_METHOD("get_server_name"), &LoadingScreenInfo::get_server_name);
	ClassDB::bind_method(D_METHOD("set_server_name", "value"), &LoadingScreenInfo::set_server_name);
	ClassDB::bind_method(D_METHOD("get_mission_name"), &LoadingScreenInfo::get_mission_name);
	ClassDB::bind_method(D_METHOD("set_mission_name", "value"), &LoadingScreenInfo::set_mission_name);
	ClassDB::bind_method(D_METHOD("get_custom_text"), &LoadingScreenInfo::get_custom_text);
	ClassDB::bind_method(D_METHOD("set_custom_text", "value"), &LoadingScreenInfo::set_custom_text);
	ClassDB::bind_method(D_METHOD("get_game_type"), &LoadingScreenInfo::get_game_type);
	ClassDB::bind_method(D_METHOD("set_game_type", "value"), &LoadingScreenInfo::set_game_type);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "mission_file"), "set_mission_file", "get_mission_file");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "in_session"), "set_in_session", "get_in_session");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "server_name"), "set_server_name", "get_server_name");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "mission_name"), "set_mission_name", "get_mission_name");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "custom_text"), "set_custom_text", "get_custom_text");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "game_type"), "set_game_type", "get_game_type");
}
