#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>

#include <formats/foliage/foliage.h>

#include <array>

namespace godot {

class TerrainFoliageDef : public RefCounted {
	GDCLASS(TerrainFoliageDef, RefCounted)

private:
	String graphic;
	int color_lower = static_cast<int>(opennova::FoliageColorMode::MatchGround);
	int color_upper = static_cast<int>(opennova::FoliageColorMode::MatchGround);
	// The consumed foliagemap match codes (engine FoliageDef::match): at most
	// FOLIAGE_MATCH_CODES entries, each -1..255, compacted to the front.
	std::array<int, opennova::FOLIAGE_MATCH_CODES> match = {
		opennova::FOLIAGE_MATCH_UNSET, opennova::FOLIAGE_MATCH_UNSET,
		opennova::FOLIAGE_MATCH_UNSET, opennova::FOLIAGE_MATCH_UNSET
	};
	int attrib_flags = 0;

protected:
	static void _bind_methods();

public:
	enum {
		COLOR_MATCH_GROUND = static_cast<int>(opennova::FoliageColorMode::MatchGround),
		COLOR_BLEND_50 = static_cast<int>(opennova::FoliageColorMode::Blend50),
		COLOR_RETAIN_FULL = static_cast<int>(opennova::FoliageColorMode::RetainFullColor),
		ATTRIB_FORCE_ON = opennova::FOLIAGE_ATTRIB_FORCE_ON,
		ATTRIB_SHADOW = opennova::FOLIAGE_ATTRIB_SHADOW,
		// The def-slot cap (engine foliage.h FOLIAGE_MAX_DEFS).
		MAX_DEFS = opennova::FOLIAGE_MAX_DEFS,
	};

	void set_graphic(const String &value);
	String get_graphic() const;

	// The authored codes only (unset tail omitted); a longer array is
	// truncated to the four the consumer reads.
	void set_match(const PackedInt32Array &value);
	PackedInt32Array get_match() const;

	void set_attrib_flags(int value);
	int get_attrib_flags() const;

	void set_shadow(bool enabled);
	bool get_shadow() const;

	void copy_from_native(const opennova::FoliageDef &def);
	opennova::FoliageDef to_native() const;
};

} // namespace godot
