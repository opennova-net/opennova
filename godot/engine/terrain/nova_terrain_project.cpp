#include "nova_terrain_project.h"

#include "nova_terrain_foliage_def.h"

#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <algorithm>

using namespace godot;

namespace {

static Ref<NovaTerrainFoliageDef> foliage_def_to_object(const opennova::FoliageDef &def) {
	Ref<NovaTerrainFoliageDef> object;
	object.instantiate();
	object->copy_from_native(def);
	return object;
}

static opennova::FoliageDef foliage_def_from_variant(const Variant &value) {
	opennova::FoliageDef def;
	if (value.get_type() == Variant::OBJECT) {
		Object *object = value;
		if (const NovaTerrainFoliageDef *foliage = Object::cast_to<NovaTerrainFoliageDef>(object)) {
			return foliage->to_native();
		}
	}

	if (value.get_type() == Variant::DICTIONARY) {
		const Dictionary dict = value;
		def.graphic = String(dict.get("graphic", "")).utf8().get_data();
		def.color_lower = static_cast<int>(dict.get("color_lower", static_cast<int>(opennova::FoliageColorMode::MatchGround)));
		def.color_upper = static_cast<int>(dict.get("color_upper", static_cast<int>(opennova::FoliageColorMode::MatchGround)));
		def.match = static_cast<int>(dict.get("match", -1));
		int attrib_flags = static_cast<int>(dict.get("attrib_flags", 0));
		if (static_cast<bool>(dict.get("shadow", false))) {
			attrib_flags |= opennova::FOLIAGE_ATTRIB_SHADOW;
		}
		if (static_cast<bool>(dict.get("force_on", false))) {
			attrib_flags |= opennova::FOLIAGE_ATTRIB_FORCE_ON;
		}
		def.attrib_flags = static_cast<uint8_t>(std::clamp(attrib_flags, 0, 255));
	}
	return opennova::foliage_normalize_def(def);
}

static Vector2i to_vector2i(const opennova::TpjLockCoord &coord) {
	return Vector2i(coord.x, coord.y);
}

static opennova::TpjLockCoord to_native_lock(const Vector2i &coord) {
	return {coord.x, coord.y};
}

} // namespace

void NovaTerrainProject::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_terrain_name", "value"), &NovaTerrainProject::set_terrain_name);
	ClassDB::bind_method(D_METHOD("get_terrain_name"), &NovaTerrainProject::get_terrain_name);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "terrain_name"), "set_terrain_name", "get_terrain_name");

	ClassDB::bind_method(D_METHOD("set_creator", "value"), &NovaTerrainProject::set_creator);
	ClassDB::bind_method(D_METHOD("get_creator"), &NovaTerrainProject::get_creator);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "creator"), "set_creator", "get_creator");

	ClassDB::bind_method(D_METHOD("set_project_path", "value"), &NovaTerrainProject::set_project_path);
	ClassDB::bind_method(D_METHOD("get_project_path"), &NovaTerrainProject::get_project_path);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "project_path"), "set_project_path", "get_project_path");

	ClassDB::bind_method(D_METHOD("set_depthmap", "value"), &NovaTerrainProject::set_depthmap);
	ClassDB::bind_method(D_METHOD("get_depthmap"), &NovaTerrainProject::get_depthmap);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "depthmap"), "set_depthmap", "get_depthmap");

	ClassDB::bind_method(D_METHOD("set_output", "value"), &NovaTerrainProject::set_output);
	ClassDB::bind_method(D_METHOD("get_output"), &NovaTerrainProject::get_output);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "output"), "set_output", "get_output");

	ClassDB::bind_method(D_METHOD("set_lock_topleft", "value"), &NovaTerrainProject::set_lock_topleft);
	ClassDB::bind_method(D_METHOD("get_lock_topleft"), &NovaTerrainProject::get_lock_topleft);
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2I, "lock_topleft"), "set_lock_topleft", "get_lock_topleft");

	ClassDB::bind_method(D_METHOD("set_lock_topright", "value"), &NovaTerrainProject::set_lock_topright);
	ClassDB::bind_method(D_METHOD("get_lock_topright"), &NovaTerrainProject::get_lock_topright);
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2I, "lock_topright"), "set_lock_topright", "get_lock_topright");

	ClassDB::bind_method(D_METHOD("set_lock_bottomleft", "value"), &NovaTerrainProject::set_lock_bottomleft);
	ClassDB::bind_method(D_METHOD("get_lock_bottomleft"), &NovaTerrainProject::get_lock_bottomleft);
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2I, "lock_bottomleft"), "set_lock_bottomleft", "get_lock_bottomleft");

	ClassDB::bind_method(D_METHOD("set_lock_bottomright", "value"), &NovaTerrainProject::set_lock_bottomright);
	ClassDB::bind_method(D_METHOD("get_lock_bottomright"), &NovaTerrainProject::get_lock_bottomright);
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2I, "lock_bottomright"), "set_lock_bottomright", "get_lock_bottomright");

	ClassDB::bind_method(D_METHOD("set_charmap", "value"), &NovaTerrainProject::set_charmap);
	ClassDB::bind_method(D_METHOD("get_charmap"), &NovaTerrainProject::get_charmap);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "charmap"), "set_charmap", "get_charmap");

	ClassDB::bind_method(D_METHOD("set_foliagemap", "value"), &NovaTerrainProject::set_foliagemap);
	ClassDB::bind_method(D_METHOD("get_foliagemap"), &NovaTerrainProject::get_foliagemap);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "foliagemap"), "set_foliagemap", "get_foliagemap");

	ClassDB::bind_method(D_METHOD("set_tilestrip", "value"), &NovaTerrainProject::set_tilestrip);
	ClassDB::bind_method(D_METHOD("get_tilestrip"), &NovaTerrainProject::get_tilestrip);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "tilestrip"), "set_tilestrip", "get_tilestrip");

	ClassDB::bind_method(D_METHOD("set_tileinfo", "value"), &NovaTerrainProject::set_tileinfo);
	ClassDB::bind_method(D_METHOD("get_tileinfo"), &NovaTerrainProject::get_tileinfo);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "tileinfo"), "set_tileinfo", "get_tileinfo");

	ClassDB::bind_method(D_METHOD("set_foliage_defs", "value"), &NovaTerrainProject::set_foliage_defs);
	ClassDB::bind_method(D_METHOD("get_foliage_defs"), &NovaTerrainProject::get_foliage_defs);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "foliage_defs", PROPERTY_HINT_ARRAY_TYPE, "NovaTerrainFoliageDef"),
	             "set_foliage_defs",
	             "get_foliage_defs");

	ClassDB::bind_method(D_METHOD("set_has_metadata", "value"), &NovaTerrainProject::set_has_metadata);
	ClassDB::bind_method(D_METHOD("get_has_metadata"), &NovaTerrainProject::get_has_metadata);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "has_metadata"), "set_has_metadata", "get_has_metadata");
}

void NovaTerrainProject::set_terrain_name(const String &p_value) { terrain_name = p_value; }
String NovaTerrainProject::get_terrain_name() const { return terrain_name; }

void NovaTerrainProject::set_creator(const String &p_value) { creator = p_value; }
String NovaTerrainProject::get_creator() const { return creator; }

void NovaTerrainProject::set_project_path(const String &p_value) { project_path = p_value; }
String NovaTerrainProject::get_project_path() const { return project_path; }

void NovaTerrainProject::set_depthmap(const String &p_value) { depthmap = p_value; }
String NovaTerrainProject::get_depthmap() const { return depthmap; }

void NovaTerrainProject::set_output(const String &p_value) { output = p_value; }
String NovaTerrainProject::get_output() const { return output; }

void NovaTerrainProject::set_lock_topleft(const Vector2i &p_value) { lock_topleft = p_value; }
Vector2i NovaTerrainProject::get_lock_topleft() const { return lock_topleft; }

void NovaTerrainProject::set_lock_topright(const Vector2i &p_value) { lock_topright = p_value; }
Vector2i NovaTerrainProject::get_lock_topright() const { return lock_topright; }

void NovaTerrainProject::set_lock_bottomleft(const Vector2i &p_value) { lock_bottomleft = p_value; }
Vector2i NovaTerrainProject::get_lock_bottomleft() const { return lock_bottomleft; }

void NovaTerrainProject::set_lock_bottomright(const Vector2i &p_value) { lock_bottomright = p_value; }
Vector2i NovaTerrainProject::get_lock_bottomright() const { return lock_bottomright; }

void NovaTerrainProject::set_charmap(const String &p_value) { charmap = p_value; }
String NovaTerrainProject::get_charmap() const { return charmap; }

void NovaTerrainProject::set_foliagemap(const String &p_value) { foliagemap = p_value; }
String NovaTerrainProject::get_foliagemap() const { return foliagemap; }

void NovaTerrainProject::set_tilestrip(const String &p_value) { tilestrip = p_value; }
String NovaTerrainProject::get_tilestrip() const { return tilestrip; }

void NovaTerrainProject::set_tileinfo(const String &p_value) { tileinfo = p_value; }
String NovaTerrainProject::get_tileinfo() const { return tileinfo; }

void NovaTerrainProject::set_foliage_defs(const Array &p_value) {
	foliage_defs = p_value.duplicate(true);
}

Array NovaTerrainProject::get_foliage_defs() const {
	return foliage_defs.duplicate(true);
}

void NovaTerrainProject::set_has_metadata(bool p_value) { has_metadata = p_value; }
bool NovaTerrainProject::get_has_metadata() const { return has_metadata; }

void NovaTerrainProject::copy_from_native(const opennova::TpjProject &project) {
	terrain_name = String(project.terrain_name.c_str());
	creator = String(project.creator.c_str());
	project_path = String(project.path.c_str());
	depthmap = String(project.depthmap.c_str());
	output = String(project.output.c_str());
	lock_topleft = to_vector2i(project.lock_topleft);
	lock_topright = to_vector2i(project.lock_topright);
	lock_bottomleft = to_vector2i(project.lock_bottomleft);
	lock_bottomright = to_vector2i(project.lock_bottomright);
	charmap = String(project.charmap.c_str());
	foliagemap = String(project.foliagemap.c_str());
	tilestrip = String(project.tilestrip.c_str());
	tileinfo = String(project.tileinfo.c_str());
	has_metadata = project.has_metadata;

	foliage_defs.clear();
	for (const opennova::FoliageDef &def : project.foliage_defs) {
		foliage_defs.push_back(foliage_def_to_object(def));
	}
}

opennova::TpjProject NovaTerrainProject::to_native() const {
	opennova::TpjProject project;
	project.terrain_name = terrain_name.utf8().get_data();
	project.creator = creator.utf8().get_data();
	project.path = project_path.utf8().get_data();
	project.depthmap = depthmap.utf8().get_data();
	project.output = output.utf8().get_data();
	project.lock_topleft = to_native_lock(lock_topleft);
	project.lock_topright = to_native_lock(lock_topright);
	project.lock_bottomleft = to_native_lock(lock_bottomleft);
	project.lock_bottomright = to_native_lock(lock_bottomright);
	project.charmap = charmap.utf8().get_data();
	project.foliagemap = foliagemap.utf8().get_data();
	project.tilestrip = tilestrip.utf8().get_data();
	project.tileinfo = tileinfo.utf8().get_data();
	project.has_metadata = has_metadata;

	const int count = foliage_defs.size() > 4 ? 4 : foliage_defs.size();
	project.foliage_defs.reserve(static_cast<size_t>(count));
	for (int i = 0; i < count; ++i) {
		project.foliage_defs.push_back(foliage_def_from_variant(foliage_defs[i]));
	}

	if (!project.charmap.empty() || !project.foliagemap.empty() || !project.tilestrip.empty() ||
	    !project.tileinfo.empty() || !project.foliage_defs.empty()) {
		project.has_metadata = true;
	}

	return project;
}
