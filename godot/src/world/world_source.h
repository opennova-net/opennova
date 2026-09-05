#pragma once

#include <godot_cpp/classes/resource.hpp>

namespace godot {

// Persistent selection only: no parsed documents, mounts or generated meshes.
class WorldSource : public Resource {
	GDCLASS(WorldSource, Resource)
public:
	enum SourceKind { LOOSE_SOURCE, RETAIL_INSTALL };
private:
	SourceKind source_kind_ = LOOSE_SOURCE;
	String data_directory_;
	String install_key_;
	String mission_name_;
	String game_code_ = "jo";
	String expansion_;
protected:
	static void _bind_methods();
public:
	void set_source_kind(SourceKind p_value);
	SourceKind get_source_kind() const { return source_kind_; }
	void set_data_directory(const String &p_value);
	String get_data_directory() const { return data_directory_; }
	void set_install_key(const String &p_value);
	String get_install_key() const { return install_key_; }
	void set_mission_name(const String &p_value);
	String get_mission_name() const { return mission_name_; }
	void set_game_code(const String &p_value);
	String get_game_code() const { return game_code_; }
	void set_expansion(const String &p_value);
	String get_expansion() const { return expansion_; }
};

} // namespace godot

VARIANT_ENUM_CAST(godot::WorldSource::SourceKind)
