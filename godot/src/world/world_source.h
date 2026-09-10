#pragma once

#include <godot_cpp/classes/resource.hpp>
#include "resource_index/resource_root.h"
#include "mission/mission_data.h"

namespace godot {

// Persistent selection only: no parsed documents, mounts or generated meshes.
class WorldSource : public Resource {
	GDCLASS(WorldSource, Resource)
public:
	enum SourceKind { LOOSE_SOURCE, RETAIL_INSTALL, EDITABLE_GAME_DATA };
private:
	SourceKind source_kind_ = LOOSE_SOURCE;
	String data_directory_;
	String local_install_name_;
	String mission_name_;
	String game_code_ = "jo";
	String expansion_;
	Error last_error_code_ = OK;
	String last_error_;
protected:
	static void _bind_methods();
public:
	Ref<ResourceRoot> open_root(const String &p_local_directory = String());
	Ref<MissionData> open_mission(const Ref<ResourceRoot> &p_root);
	Error get_last_error_code() const { return last_error_code_; }
	String get_last_error() const { return last_error_; }
	void set_source_kind(SourceKind p_value);
	SourceKind get_source_kind() const { return source_kind_; }
	void set_data_directory(const String &p_value);
	String get_data_directory() const { return data_directory_; }
	// A name for a machine-local data folder ("jo", "dfx2"): the folder itself
	// is chosen per machine through the toolbar's Folder button and kept in
	// the editor's project metadata, never in the scene. Overrides
	// data_directory while set.
	void set_local_install_name(const String &p_value);
	String get_local_install_name() const { return local_install_name_; }
	void set_mission_name(const String &p_value);
	String get_mission_name() const { return mission_name_; }
	void set_game_code(const String &p_value);
	String get_game_code() const { return game_code_; }
	void set_expansion(const String &p_value);
	String get_expansion() const { return expansion_; }
};

} // namespace godot

VARIANT_ENUM_CAST(godot::WorldSource::SourceKind)
