#include "terrain/terrain_foliage_def.h"

#include "terrain/terrain_clamp.h"
#include "util/string_convert.h"

#include <algorithm>

using namespace godot;

void TerrainFoliageDef::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_graphic", "value"), &TerrainFoliageDef::set_graphic);
	ClassDB::bind_method(D_METHOD("get_graphic"), &TerrainFoliageDef::get_graphic);
	ClassDB::bind_method(D_METHOD("set_match", "value"), &TerrainFoliageDef::set_match);
	ClassDB::bind_method(D_METHOD("get_match"), &TerrainFoliageDef::get_match);
	ClassDB::bind_method(D_METHOD("set_attrib_flags", "value"), &TerrainFoliageDef::set_attrib_flags);
	ClassDB::bind_method(D_METHOD("get_attrib_flags"), &TerrainFoliageDef::get_attrib_flags);
	ClassDB::bind_method(D_METHOD("set_shadow", "enabled"), &TerrainFoliageDef::set_shadow);
	ClassDB::bind_method(D_METHOD("get_shadow"), &TerrainFoliageDef::get_shadow);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "graphic"), "set_graphic", "get_graphic");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "match"), "set_match", "get_match");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "attrib_flags"), "set_attrib_flags", "get_attrib_flags");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "shadow"), "set_shadow", "get_shadow");

}

void TerrainFoliageDef::set_graphic(const String &value) {
	graphic = value;
}

String TerrainFoliageDef::get_graphic() const {
	return graphic;
}

void TerrainFoliageDef::set_match(const PackedInt32Array &value) {
	opennova::FoliageDef def;
	const int64_t count = std::min<int64_t>(value.size(), opennova::FOLIAGE_MATCH_CODES);
	for (int64_t i = 0; i < count; ++i) {
		def.match[static_cast<size_t>(i)] = value[i];
	}
	match = opennova::foliage_normalize_def(def).match;
}

PackedInt32Array TerrainFoliageDef::get_match() const {
	PackedInt32Array out;
	for (int code : match) {
		if (code >= 0) {
			out.push_back(code);
		}
	}
	return out;
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
	def.graphic = opennova::to_std(graphic);
	def.color_lower = color_lower;
	def.color_upper = color_upper;
	def.match = match;
	def.attrib_flags = clamp_int<uint8_t>(attrib_flags, 0, 255);
	return opennova::foliage_normalize_def(def);
}
