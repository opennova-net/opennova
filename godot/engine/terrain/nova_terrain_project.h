#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <tpj/tpj.h>

namespace godot {

class NovaTerrainFoliageDef;

class NovaTerrainProject : public Resource {
	GDCLASS(NovaTerrainProject, Resource)

	String terrain_name;
	String creator;
	String project_path;
	String depthmap;
	String output;
	Vector2i lock_topleft;
	Vector2i lock_topright;
	Vector2i lock_bottomleft;
	Vector2i lock_bottomright;
	String charmap;
	String foliagemap;
	String tilestrip;
	String tileinfo;
	Array foliage_defs;
	bool has_metadata = false;

protected:
	static void _bind_methods();

public:
	void set_terrain_name(const String &p_value);
	String get_terrain_name() const;

	void set_creator(const String &p_value);
	String get_creator() const;

	void set_project_path(const String &p_value);
	String get_project_path() const;

	void set_depthmap(const String &p_value);
	String get_depthmap() const;

	void set_output(const String &p_value);
	String get_output() const;

	void set_lock_topleft(const Vector2i &p_value);
	Vector2i get_lock_topleft() const;

	void set_lock_topright(const Vector2i &p_value);
	Vector2i get_lock_topright() const;

	void set_lock_bottomleft(const Vector2i &p_value);
	Vector2i get_lock_bottomleft() const;

	void set_lock_bottomright(const Vector2i &p_value);
	Vector2i get_lock_bottomright() const;

	void set_charmap(const String &p_value);
	String get_charmap() const;

	void set_foliagemap(const String &p_value);
	String get_foliagemap() const;

	void set_tilestrip(const String &p_value);
	String get_tilestrip() const;

	void set_tileinfo(const String &p_value);
	String get_tileinfo() const;

	void set_foliage_defs(const Array &p_value);
	Array get_foliage_defs() const;

	void set_has_metadata(bool p_value);
	bool get_has_metadata() const;

	void copy_from_native(const opennova::TpjProject &project);
	opennova::TpjProject to_native() const;
};

} // namespace godot
