#pragma once

// The environment's world light and fog record and the typed channel it publishes through, split from
// ObjectModel's TU pair (one type per TU pair, ADR 0043).

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>

namespace godot {

// The env-derived world lighting/fog values (ADR 0017's typed record,
// native). Computed once per env change; the object shader family reads the
// same block as the opennova_light_block_* / opennova_fog_* global shader
// parameters MissionEnvironment writes, and the typed record serves the
// light director, the terrain light rows, and the diagnostics.
// (engine: runtime/renderer/light_runtime.h)
class EnvLightValues : public RefCounted {
	GDCLASS(EnvLightValues, RefCounted)

protected:
	static void _bind_methods();

public:
	Vector3 hemi_sky;
	Vector3 dir;
	Vector3 dir_color;
	Vector3 hemi_ground;
	Vector3 ceiling;
	Vector3 floor_color;
	Vector3 gain;
	bool fog_enabled = false;
	bool thermal_view = false;
	Vector3 fog_color;
	float fog_start = 0.0f;
	float fog_end = 0.0f;
	int fog_type = 0;

	// True when `other` carries the same lighting/fog the shaders consume.
	// A null other (first push after rebuild) is never equal.
	bool equals(const Ref<EnvLightValues> &p_other) const;

	// The un-enved default: the RETAIL NOON register (shipped full_00.env tod
	// 1200 block bytes /255), so a preview lights like a JO noon world.
	static Ref<EnvLightValues> retail_noon_defaults();

	// Property surface so the environment system (GDScript) fills the record.
	void set_hemi_sky(const Vector3 &v) { hemi_sky = v; }
	Vector3 get_hemi_sky() const { return hemi_sky; }
	void set_dir(const Vector3 &v) { dir = v; }
	Vector3 get_dir() const { return dir; }
	void set_dir_color(const Vector3 &v) { dir_color = v; }
	Vector3 get_dir_color() const { return dir_color; }
	void set_hemi_ground(const Vector3 &v) { hemi_ground = v; }
	Vector3 get_hemi_ground() const { return hemi_ground; }
	void set_ceiling(const Vector3 &v) { ceiling = v; }
	Vector3 get_ceiling() const { return ceiling; }
	void set_floor_color(const Vector3 &v) { floor_color = v; }
	Vector3 get_floor_color() const { return floor_color; }
	void set_gain(const Vector3 &v) { gain = v; }
	Vector3 get_gain() const { return gain; }
	void set_fog_enabled(bool v) { fog_enabled = v; }
	bool get_fog_enabled() const { return fog_enabled; }
	void set_fog_color(const Vector3 &v) { fog_color = v; }
	Vector3 get_fog_color() const { return fog_color; }
	void set_fog_start(float v) { fog_start = v; }
	float get_fog_start() const { return fog_start; }
	void set_fog_end(float v) { fog_end = v; }
	float get_fog_end() const { return fog_end; }
	void set_fog_type(int v) { fog_type = v; }
	int get_fog_type() const { return fog_type; }
};

// The typed channel between the environment system and its non-shader
// consumers: the env PUBLISHES its current world light/fog values here and
// consumers hold this ref, reading values + generation through typed calls —
// no consumer ever holds the environment object itself.
class EnvLightState : public RefCounted {
	GDCLASS(EnvLightState, RefCounted)

	Ref<EnvLightValues> values_;
	int64_t generation_ = 0;

protected:
	static void _bind_methods();

public:
	void publish(const Ref<EnvLightValues> &p_values,
			bool p_pass_changed = false);
	Ref<EnvLightValues> get_values() const { return values_; }
	int64_t get_generation() const { return generation_; }
};

} // namespace godot
