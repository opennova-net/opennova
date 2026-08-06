#include "nova_terrain_foliage_def.h"

#include <algorithm>

using namespace godot;

namespace {

template <typename T>
static T clamp_int(int value, int min_value, int max_value) {
	return static_cast<T>(std::clamp(value, min_value, max_value));
}

} // namespace

void NovaTerrainFoliageDef::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_graphic", "value"), &NovaTerrainFoliageDef::set_graphic);
	ClassDB::bind_method(D_METHOD("get_graphic"), &NovaTerrainFoliageDef::get_graphic);
	ClassDB::bind_method(D_METHOD("set_color_lower", "value"), &NovaTerrainFoliageDef::set_color_lower);
	ClassDB::bind_method(D_METHOD("get_color_lower"), &NovaTerrainFoliageDef::get_color_lower);
	ClassDB::bind_method(D_METHOD("set_color_upper", "value"), &NovaTerrainFoliageDef::set_color_upper);
	ClassDB::bind_method(D_METHOD("get_color_upper"), &NovaTerrainFoliageDef::get_color_upper);
	ClassDB::bind_method(D_METHOD("set_match", "value"), &NovaTerrainFoliageDef::set_match);
	ClassDB::bind_method(D_METHOD("get_match"), &NovaTerrainFoliageDef::get_match);
	ClassDB::bind_method(D_METHOD("set_attrib_flags", "value"), &NovaTerrainFoliageDef::set_attrib_flags);
	ClassDB::bind_method(D_METHOD("get_attrib_flags"), &NovaTerrainFoliageDef::get_attrib_flags);
	ClassDB::bind_method(D_METHOD("set_shadow", "enabled"), &NovaTerrainFoliageDef::set_shadow);
	ClassDB::bind_method(D_METHOD("get_shadow"), &NovaTerrainFoliageDef::get_shadow);
	ClassDB::bind_method(D_METHOD("set_force_on", "enabled"), &NovaTerrainFoliageDef::set_force_on);
	ClassDB::bind_method(D_METHOD("get_force_on"), &NovaTerrainFoliageDef::get_force_on);
	ClassDB::bind_method(D_METHOD("to_dictionary"), &NovaTerrainFoliageDef::to_dictionary);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "graphic"), "set_graphic", "get_graphic");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "color_lower", PROPERTY_HINT_ENUM, "Match Ground,Blend 50,Retain Full Color"),
	             "set_color_lower",
	             "get_color_lower");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "color_upper", PROPERTY_HINT_ENUM, "Match Ground,Blend 50,Retain Full Color"),
	             "set_color_upper",
	             "get_color_upper");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "match", PROPERTY_HINT_RANGE, "-1,255,1"), "set_match", "get_match");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "attrib_flags"), "set_attrib_flags", "get_attrib_flags");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "shadow"), "set_shadow", "get_shadow");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "force_on"), "set_force_on", "get_force_on");

	BIND_CONSTANT(COLOR_MATCH_GROUND);
	BIND_CONSTANT(COLOR_BLEND_50);
	BIND_CONSTANT(COLOR_RETAIN_FULL);
	BIND_CONSTANT(ATTRIB_FORCE_ON);
	BIND_CONSTANT(ATTRIB_SHADOW);
}

void NovaTerrainFoliageDef::set_graphic(const String &value) {
	graphic = value;
}

String NovaTerrainFoliageDef::get_graphic() const {
	return graphic;
}

void NovaTerrainFoliageDef::set_color_lower(int value) {
	color_lower = opennova::foliage_normalize_color_mode(value);
}

int NovaTerrainFoliageDef::get_color_lower() const {
	return color_lower;
}

void NovaTerrainFoliageDef::set_color_upper(int value) {
	color_upper = opennova::foliage_normalize_color_mode(value);
}

int NovaTerrainFoliageDef::get_color_upper() const {
	return color_upper;
}

void NovaTerrainFoliageDef::set_match(int value) {
	match = std::clamp(value, -1, 255);
}

int NovaTerrainFoliageDef::get_match() const {
	return match;
}

void NovaTerrainFoliageDef::set_attrib_flags(int value) {
	attrib_flags = clamp_int<int>(opennova::foliage_normalize_attrib_flags(static_cast<uint8_t>(std::clamp(value, 0, 255))), 0, 255);
}

int NovaTerrainFoliageDef::get_attrib_flags() const {
	return attrib_flags;
}

void NovaTerrainFoliageDef::set_shadow(bool enabled) {
	if (enabled) {
		attrib_flags |= ATTRIB_SHADOW;
	} else {
		attrib_flags &= ~ATTRIB_SHADOW;
	}
	attrib_flags = clamp_int<int>(opennova::foliage_normalize_attrib_flags(static_cast<uint8_t>(attrib_flags)), 0, 255);
}

bool NovaTerrainFoliageDef::get_shadow() const {
	return (attrib_flags & ATTRIB_SHADOW) != 0;
}

void NovaTerrainFoliageDef::set_force_on(bool enabled) {
	if (enabled) {
		attrib_flags |= ATTRIB_FORCE_ON;
	} else {
		attrib_flags &= ~ATTRIB_FORCE_ON;
	}
	attrib_flags = clamp_int<int>(opennova::foliage_normalize_attrib_flags(static_cast<uint8_t>(attrib_flags)), 0, 255);
}

bool NovaTerrainFoliageDef::get_force_on() const {
	return (attrib_flags & ATTRIB_FORCE_ON) != 0;
}

Dictionary NovaTerrainFoliageDef::to_dictionary() const {
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

void NovaTerrainFoliageDef::copy_from_native(const opennova::FoliageDef &def) {
	const opennova::FoliageDef normalized = opennova::foliage_normalize_def(def);
	graphic = String(normalized.graphic.c_str());
	color_lower = normalized.color_lower;
	color_upper = normalized.color_upper;
	match = normalized.match;
	attrib_flags = clamp_int<int>(normalized.attrib_flags, 0, 255);
}

opennova::FoliageDef NovaTerrainFoliageDef::to_native() const {
	opennova::FoliageDef def;
	def.graphic = graphic.utf8().get_data();
	def.color_lower = color_lower;
	def.color_upper = color_upper;
	def.match = match;
	def.attrib_flags = clamp_int<uint8_t>(attrib_flags, 0, 255);
	return opennova::foliage_normalize_def(def);
}
