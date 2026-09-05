#include "world/world_source.h"

using namespace godot;

void WorldSource::_bind_methods() {
	BIND_ENUM_CONSTANT(LOOSE_SOURCE);
	BIND_ENUM_CONSTANT(RETAIL_INSTALL);
	ClassDB::bind_method(D_METHOD("set_source_kind", "value"), &WorldSource::set_source_kind);
	ClassDB::bind_method(D_METHOD("get_source_kind"), &WorldSource::get_source_kind);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "source_kind", PROPERTY_HINT_ENUM,
			"Loose source,Retail install"), "set_source_kind", "get_source_kind");
	ClassDB::bind_method(D_METHOD("set_data_directory", "value"), &WorldSource::set_data_directory);
	ClassDB::bind_method(D_METHOD("get_data_directory"), &WorldSource::get_data_directory);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "data_directory"), "set_data_directory", "get_data_directory");
	ClassDB::bind_method(D_METHOD("set_install_key", "value"), &WorldSource::set_install_key);
	ClassDB::bind_method(D_METHOD("get_install_key"), &WorldSource::get_install_key);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "install_key"), "set_install_key", "get_install_key");
	ClassDB::bind_method(D_METHOD("set_mission_name", "value"), &WorldSource::set_mission_name);
	ClassDB::bind_method(D_METHOD("get_mission_name"), &WorldSource::get_mission_name);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "mission_name"), "set_mission_name", "get_mission_name");
	ClassDB::bind_method(D_METHOD("set_game_code", "value"), &WorldSource::set_game_code);
	ClassDB::bind_method(D_METHOD("get_game_code"), &WorldSource::get_game_code);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "game_code"), "set_game_code", "get_game_code");
	ClassDB::bind_method(D_METHOD("set_expansion", "value"), &WorldSource::set_expansion);
	ClassDB::bind_method(D_METHOD("get_expansion"), &WorldSource::get_expansion);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "expansion"), "set_expansion", "get_expansion");
}

void WorldSource::set_source_kind(SourceKind p_value) {
	if (p_value < LOOSE_SOURCE || p_value > RETAIL_INSTALL || source_kind_ == p_value) return;
	source_kind_ = p_value;
	emit_changed();
}

void WorldSource::set_data_directory(const String &p_value) {
	if (data_directory_ == p_value) return;
	data_directory_ = p_value;
	emit_changed();
}

void WorldSource::set_install_key(const String &p_value) {
	if (install_key_ == p_value) return;
	install_key_ = p_value;
	emit_changed();
}

void WorldSource::set_mission_name(const String &p_value) {
	if (mission_name_ == p_value) return;
	mission_name_ = p_value;
	emit_changed();
}

void WorldSource::set_game_code(const String &p_value) {
	if (game_code_ == p_value) return;
	game_code_ = p_value;
	emit_changed();
}

void WorldSource::set_expansion(const String &p_value) {
	if (expansion_ == p_value) return;
	expansion_ = p_value;
	emit_changed();
}
