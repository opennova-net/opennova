#include "world/loading_screen_info.h"

#include "resource_index/resource_root.h"
#include "util/string_convert.h"

#include <runtime/inmatch/host_boot.h>
#include <runtime/mission/runtime_boot.h>

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

Ref<LoadingScreenInfo> LoadingScreenInfo::for_host(const Ref<ResourceRoot> &p_root,
		const String &p_mission_file, const String &p_server_name, int p_game_type,
		bool p_game_type_auto, const String &p_custom_text) {
	opennova::mission::BootFileSource files;
	std::vector<uint8_t> override_table;
	if (p_root.is_valid()) {
		files = opennova::mission::boot_files_from_index(p_root->native_index());
		const PackedByteArray table = p_root->get_expansion_override_table();
		override_table.assign(table.ptr(), table.ptr() + table.size());
	}
	const opennova::inmatch::HostLoadingBand band = opennova::inmatch::host_loading_band(files,
			opennova::to_std(p_mission_file), static_cast<uint32_t>(p_game_type),
			p_game_type_auto, override_table);
	return make(p_mission_file, true, p_server_name, opennova::to_gd(band.mission_name),
			static_cast<int>(band.game_type), p_custom_text);
}

void LoadingScreenInfo::_bind_methods() {
	ClassDB::bind_static_method("LoadingScreenInfo",
			D_METHOD("make", "mission_file", "in_session", "server_name", "mission_name",
					"game_type", "custom_text"),
			&LoadingScreenInfo::make);
	ClassDB::bind_static_method("LoadingScreenInfo", D_METHOD("for_mission", "mission_file"),
			&LoadingScreenInfo::for_mission);
	ClassDB::bind_static_method("LoadingScreenInfo",
			D_METHOD("for_host", "root", "mission_file", "server_name", "game_type",
					"game_type_auto", "custom_text"),
			&LoadingScreenInfo::for_host);
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
