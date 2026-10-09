#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>

#include <formats/env/env.h>

namespace godot {

// The BMS mission header's environment override layer (env::BmsEnvOverrides):
// the attrib-gated water height (file half-units; `water_height_world` is the
// world-unit form no shell converts), fog distance and fog color, the
// nonzero-gated water color and murk, and the local-play start time. Produced
// by MissionData.get_environment_overrides, consumed by
// EnvFile.apply_mission_overrides; setting a value arms its gate, so a test
// authors one field at a time. The engine builder and apply carry the cites
// (formats/env/env.h).
class MissionEnvironmentOverrides : public RefCounted {
	GDCLASS(MissionEnvironmentOverrides, RefCounted)

	opennova::env::BmsEnvOverrides value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::env::BmsEnvOverrides &p_value) { value_ = p_value; }
	const opennova::env::BmsEnvOverrides &value() const { return value_; }

	// No gate armed: the header overrides nothing.
	bool is_empty() const;

	bool get_has_water_height() const { return value_.has_water_height; }
	float get_water_height() const { return value_.water_height; }
	void set_water_height(float p_value);
	float get_water_height_world() const { return value_.water_height * opennova::env::kWaterHeightUnit; }
	// The height Water's mission rung takes (Water::set_mission_water_height_override):
	// the world-unit height where the layer arms one, else NAN (the rung
	// falls through to the environment's and the terrain's).
	float get_water_height_world_or_nan() const;
	bool get_has_fog_level() const { return value_.has_fog_level; }
	float get_fog_level() const { return value_.fog_level; }
	void set_fog_level(float p_value);
	bool get_has_fog_color() const { return value_.has_fog_color; }
	Color get_fog_color() const;
	void set_fog_color(const Color &p_value);
	bool get_has_water_color() const { return value_.has_water_color; }
	Color get_water_color() const;
	void set_water_color(const Color &p_value);
	bool get_has_water_murk() const { return value_.has_water_murk; }
	float get_water_murk() const { return value_.water_murk; }
	void set_water_murk(float p_value);
	bool get_has_start_time() const { return value_.has_start_time; }
	int get_start_time() const { return value_.start_time; }
	void set_start_time(int p_value);
};

} // namespace godot
