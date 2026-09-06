#include "world/world_source.h"

#include <godot_cpp/classes/project_settings.hpp>

using namespace godot;

void WorldSource::_bind_methods() {
	BIND_ENUM_CONSTANT(LOOSE_SOURCE);
	BIND_ENUM_CONSTANT(RETAIL_INSTALL);
	BIND_ENUM_CONSTANT(EDITABLE_GAME_DATA);
	ClassDB::bind_method(D_METHOD("open_root", "local_directory"), &WorldSource::open_root, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("open_mission", "root"), &WorldSource::open_mission);
	ClassDB::bind_method(D_METHOD("get_last_error_code"), &WorldSource::get_last_error_code);
	ClassDB::bind_method(D_METHOD("get_last_error"), &WorldSource::get_last_error);
	ClassDB::bind_method(D_METHOD("set_source_kind", "value"), &WorldSource::set_source_kind);
	ClassDB::bind_method(D_METHOD("get_source_kind"), &WorldSource::get_source_kind);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "source_kind", PROPERTY_HINT_ENUM,
			"Loose source,Retail install,Editable game data"), "set_source_kind", "get_source_kind");
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
	if (p_value < LOOSE_SOURCE || p_value > EDITABLE_GAME_DATA || source_kind_ == p_value) return;
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

Ref<ResourceRoot> WorldSource::open_root(const String &p_local_directory) {
	last_error_code_ = OK;
	last_error_ = String();
	String directory = install_key_.is_empty() ? data_directory_.strip_edges() : p_local_directory.strip_edges();
	if (directory.is_empty()) {
		last_error_code_ = ERR_UNCONFIGURED;
		last_error_ = install_key_.is_empty() ? String("Set a data directory or local install key.") :
				String("Set the local folder for install '") + install_key_ + String("'.");
		return Ref<ResourceRoot>();
	}
	if (!directory.is_absolute_path()) directory = String("res://").path_join(directory);
	directory = ProjectSettings::get_singleton()->globalize_path(directory);
	Ref<ResourceRoot> root;
	root.instantiate();
	last_error_code_ = source_kind_ == LOOSE_SOURCE ? root->set_root_dir(directory) :
			root->mount_runtime(directory, expansion_, source_kind_ == EDITABLE_GAME_DATA, game_code_);
	if (last_error_code_ != OK) {
		last_error_ = root->get_last_error();
		return Ref<ResourceRoot>();
	}
	return root;
}

Ref<MissionData> WorldSource::open_mission(const Ref<ResourceRoot> &p_root) {
	last_error_code_ = OK;
	last_error_ = String();
	const String name = mission_name_.strip_edges();
	if (p_root.is_null() || name.is_empty() || name != name.get_file() || name.get_extension().to_lower() != "bms") {
		last_error_code_ = ERR_INVALID_PARAMETER;
		last_error_ = "Mission must name a top-level .bms file on an open root.";
		return Ref<MissionData>();
	}
	Ref<MissionData> mission;
	mission.instantiate();
	if (source_kind_ == EDITABLE_GAME_DATA) {
		// The editable mission itself must exist loose, exactly like Play's
		// load_loose_mission entry. An archive cannot hide a missing edit.
		const String path = p_root->resolve_file(name);
		if (path.is_empty()) {
			last_error_code_ = ERR_FILE_NOT_FOUND;
			last_error_ = name + String(": editable mission is missing from the data folder.");
			return Ref<MissionData>();
		}
		last_error_code_ = mission->open_file(path);
	} else {
		last_error_code_ = mission->open_from_resource_root(p_root, name,
				source_kind_ == RETAIL_INSTALL ? ResourceRoot::LOOKUP_FORCE_ARCHIVE_ONLY : ResourceRoot::LOOKUP_SESSION_DEFAULT);
	}
	if (last_error_code_ != OK) {
		last_error_ = name + String(": ") + mission->get_last_error();
		return Ref<MissionData>();
	}
	return mission;
}
