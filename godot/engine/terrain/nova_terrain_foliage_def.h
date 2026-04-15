#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <foliage/foliage.h>

namespace godot {

class NovaTerrainFoliageDef : public RefCounted {
	GDCLASS(NovaTerrainFoliageDef, RefCounted)

private:
	String graphic;
	int color_lower = static_cast<int>(opennova::FoliageColorMode::MatchGround);
	int color_upper = static_cast<int>(opennova::FoliageColorMode::MatchGround);
	int match = -1;
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
	};

	void set_graphic(const String &value);
	String get_graphic() const;

	void set_color_lower(int value);
	int get_color_lower() const;

	void set_color_upper(int value);
	int get_color_upper() const;

	void set_match(int value);
	int get_match() const;

	void set_attrib_flags(int value);
	int get_attrib_flags() const;

	void set_shadow(bool enabled);
	bool get_shadow() const;

	void set_force_on(bool enabled);
	bool get_force_on() const;

	Dictionary to_dictionary() const;

	void copy_from_native(const opennova::FoliageDef &def);
	opennova::FoliageDef to_native() const;
};

} // namespace godot
