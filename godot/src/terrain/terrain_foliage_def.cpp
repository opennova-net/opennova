#include "terrain/terrain_foliage_def.h"

#include <algorithm>

using namespace godot;

namespace {

template <typename T>
static T clamp_int(int value, int min_value, int max_value) {
	return static_cast<T>(std::clamp(value, min_value, max_value));
}

} // namespace

void TerrainFoliageDef::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_graphic", "value"), &TerrainFoliageDef::set_graphic);
	ClassDB::bind_method(D_METHOD("get_graphic"), &TerrainFoliageDef::get_graphic);
	ClassDB::bind_method(D_METHOD("set_match", "value"), &TerrainFoliageDef::set_match);
	ClassDB::bind_method(D_METHOD("get_match"), &TerrainFoliageDef::get_match);
	ClassDB::bind_method(D_METHOD("set_attrib_flags", "value"), &TerrainFoliageDef::set_attrib_flags);
	ClassDB::bind_method(D_METHOD("get_attrib_flags"), &TerrainFoliageDef::get_attrib_flags);
	ClassDB::bind_method(D_METHOD("set_shadow", "enabled"), &TerrainFoliageDef::set_shadow);
	ClassDB::bind_method(D_METHOD("get_shadow"), &TerrainFoliageDef::get_shadow);
	ClassDB::bind_method(D_METHOD("to_dictionary"), &TerrainFoliageDef::to_dictionary);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "graphic"), "set_graphic", "get_graphic");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "match", PROPERTY_HINT_RANGE, "-1,255,1"), "set_match", "get_match");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "attrib_flags"), "set_attrib_flags", "get_attrib_flags");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "shadow"), "set_shadow", "get_shadow");

}

void TerrainFoliageDef::set_graphic(const String &value) {
	graphic = value;
}

String TerrainFoliageDef::get_graphic() const {
	return graphic;
}

void TerrainFoliageDef::set_match(int value) {
	match = std::clamp(value, -1, 255);
}

int TerrainFoliageDef::get_match() const {
	return match;
}

void TerrainFoliageDef::set_attrib_flags(int value) {
	attrib_flags = clamp_int<int>(opennova::foliage_normalize_attrib_flags(static_cast<uint8_t>(std::clamp(value, 0, 255))), 0, 255);
}

int TerrainFoliageDef::get_attrib_flags() const {
	return attrib_flags;
}

void TerrainFoliageDef::set_shadow(bool enabled) {
	if (enabled) {
		attrib_flags |= ATTRIB_SHADOW;
	} else {
		attrib_flags &= ~ATTRIB_SHADOW;
	}
	attrib_flags = clamp_int<int>(opennova::foliage_normalize_attrib_flags(static_cast<uint8_t>(attrib_flags)), 0, 255);
}

bool TerrainFoliageDef::get_shadow() const {
	return (attrib_flags & ATTRIB_SHADOW) != 0;
}

bool TerrainFoliageDef::get_force_on() const {
	return (attrib_flags & ATTRIB_FORCE_ON) != 0;
}

Dictionary TerrainFoliageDef::to_dictionary() const {
	Dictionary out;
	out["graphic"] = graphic;
	out["color_lower"] = color_lower;
	out["color_upper"] = color_upper;
	out["match"] = match;
	out["attrib_flags"] = attrib_flags;
	out["shadow"] = get_shadow();
	out["force_on"] = get_force_on();
	return out;
}

void TerrainFoliageDef::copy_from_native(const opennova::FoliageDef &def) {
	const opennova::FoliageDef normalized = opennova::foliage_normalize_def(def);
	graphic = String(normalized.graphic.c_str());
	color_lower = normalized.color_lower;
	color_upper = normalized.color_upper;
	match = normalized.match;
	attrib_flags = clamp_int<int>(normalized.attrib_flags, 0, 255);
}

opennova::FoliageDef TerrainFoliageDef::to_native() const {
	opennova::FoliageDef def;
	def.graphic = graphic.utf8().get_data();
	def.color_lower = color_lower;
	def.color_upper = color_upper;
	def.match = match;
	def.attrib_flags = clamp_int<uint8_t>(attrib_flags, 0, 255);
	return opennova::foliage_normalize_def(def);
}
