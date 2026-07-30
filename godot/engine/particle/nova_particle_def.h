#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <particle/particle.h>

#include "nova_particle_curve_ref.h"
#include "nova_particle_graphic_layer.h"

namespace godot {

// Wraps opennova::particle::ParticleDef (~5204 B in engine heap, ~80 keys).
// Engine: CParticleDef_ParseFromConfigMap @ 0x5ed210 +
// CParticleDef_SaveToFile @ 0x5e4d70.
//
// `flags` and `move` are stored as both uint32 bitfields (engine-faithful)
// and raw strings (round-trip preservation). Helper getters in
// `libs/particle/particle.h` (`particle_flag::*`, `move_flag::*`) document
// the bit assignments.
class NovaParticleDef : public Resource {
	GDCLASS(NovaParticleDef, Resource)

private:
	String id;
	String child_id;
	String flags_raw;
	int flags = 0;        // particle_flag::* bits
	String move_raw;
	int move = 0;         // move_flag::* bits
	float lod = 0.0f;

	// Emission
	float emit_dur = 0.0f, emit_dur_adj = 0.0f;
	float emit_rate = 0.0f, emit_rate_adj = 0.0f;
	Ref<NovaParticleCurveRef> emit_rate_func;
	float emit_delay = 0.0f;
	int emit_burst = 1, emit_maxoverride = 0, emit_shape = 0;
	Vector3 emit_shape_size, emit_shape_size_skip;

	// Position / lifetime
	float y_offset = 0.0f, z_offset = 0.0f;
	float age = 0.0f, age_adj = 0.0f;
	float scale_value = 0.0f, scale_adj = 0.0f;
	Ref<NovaParticleCurveRef> scale_func;

	// Visual
	float alpha = 1.0f;
	Ref<NovaParticleCurveRef> alpha_func, red_func, green_func, blue_func;
	Color color1, color2, color3, color4;
	float bump_scale = 0.0f;

	// Motion
	Vector3 orientation, orientationadj;
	float yaw_rot = 0.0f, yaw_rot_adj = 0.0f;
	float pitch_rot = 0.0f, pitch_rot_adj = 0.0f;
	float roll_rot = 0.0f, roll_rot_adj = 0.0f;
	float speed = 0.0f, speed_adj = 0.0f;
	float elastic = 0.0f, gravity = 0.0f;
	Vector3 gravity_mask = Vector3(1.0f, 1.0f, 1.0f);
	float drag = 0.0f, spread = 0.0f, spread_skip = 0.0f;
	float orbitalspeed = 0.0f, orbitalspeed_adj = 0.0f;
	Vector3 orbital_axis = Vector3(0.0f, 1.0f, 0.0f);

	PackedStringArray collide_sounds;
	TypedArray<NovaParticleGraphicLayer> graphics;
	// Ordered {key,value} dictionaries. Array form preserves duplicate unknown
	// keys and source order across Godot load/edit/save round-trips.
	Array unknown_keys;

protected:
	static void _bind_methods();

public:
	// particle_flag:: bits GDScript composes directly (the full name<->bit
	// table stays introspectable via get_particle_flag_table()). Pinned to
	// libs/particle by static_asserts in the .cpp.
	enum {
		FLAG_YAW_AND_PITCH = 0x100,
		FLAG_FOREVER_EMIT = 0x40000,
		FLAG_POSITION_RELATIVE = 0x80000,
	};

	NovaParticleDef();

	// Trivial getter/setter pairs — full set is too large to inline declarations
	// without hurting readability. See cpp for implementations.
	void set_id(const String &v); String get_id() const;
	void set_child_id(const String &v); String get_child_id() const;
	void set_flags_raw(const String &v); String get_flags_raw() const;
	void set_flags(int v); int get_flags() const;
	void set_move_raw(const String &v); String get_move_raw() const;
	void set_move(int v); int get_move() const;
	void set_lod(float v); float get_lod() const;

	void set_emit_dur(float v); float get_emit_dur() const;
	void set_emit_dur_adj(float v); float get_emit_dur_adj() const;
	void set_emit_rate(float v); float get_emit_rate() const;
	void set_emit_rate_adj(float v); float get_emit_rate_adj() const;
	void set_emit_rate_func(const Ref<NovaParticleCurveRef> &r); Ref<NovaParticleCurveRef> get_emit_rate_func() const;
	void set_emit_delay(float v); float get_emit_delay() const;
	void set_emit_burst(int v); int get_emit_burst() const;
	void set_emit_maxoverride(int v); int get_emit_maxoverride() const;
	void set_emit_shape(int v); int get_emit_shape() const;
	void set_emit_shape_size(const Vector3 &v); Vector3 get_emit_shape_size() const;
	void set_emit_shape_size_skip(const Vector3 &v); Vector3 get_emit_shape_size_skip() const;

	void set_y_offset(float v); float get_y_offset() const;
	void set_z_offset(float v); float get_z_offset() const;
	void set_age(float v); float get_age() const;
	void set_age_adj(float v); float get_age_adj() const;
	void set_scale_value(float v); float get_scale_value() const;
	void set_scale_adj(float v); float get_scale_adj() const;
	void set_scale_func(const Ref<NovaParticleCurveRef> &r); Ref<NovaParticleCurveRef> get_scale_func() const;

	void set_alpha(float v); float get_alpha() const;
	void set_alpha_func(const Ref<NovaParticleCurveRef> &r); Ref<NovaParticleCurveRef> get_alpha_func() const;
	void set_red_func(const Ref<NovaParticleCurveRef> &r); Ref<NovaParticleCurveRef> get_red_func() const;
	void set_green_func(const Ref<NovaParticleCurveRef> &r); Ref<NovaParticleCurveRef> get_green_func() const;
	void set_blue_func(const Ref<NovaParticleCurveRef> &r); Ref<NovaParticleCurveRef> get_blue_func() const;
	void set_color1(const Color &c); Color get_color1() const;
	void set_color2(const Color &c); Color get_color2() const;
	void set_color3_prop(const Color &c); Color get_color3_prop() const;
	void set_color4(const Color &c); Color get_color4() const;
	void set_bump_scale(float v); float get_bump_scale() const;

	void set_orientation(const Vector3 &v); Vector3 get_orientation() const;
	void set_orientationadj(const Vector3 &v); Vector3 get_orientationadj() const;
	void set_yaw_rot(float v); float get_yaw_rot() const;
	void set_yaw_rot_adj(float v); float get_yaw_rot_adj() const;
	void set_pitch_rot(float v); float get_pitch_rot() const;
	void set_pitch_rot_adj(float v); float get_pitch_rot_adj() const;
	void set_roll_rot(float v); float get_roll_rot() const;
	void set_roll_rot_adj(float v); float get_roll_rot_adj() const;
	void set_speed(float v); float get_speed() const;
	void set_speed_adj(float v); float get_speed_adj() const;
	void set_elastic(float v); float get_elastic() const;
	void set_gravity(float v); float get_gravity() const;
	void set_gravity_mask(const Vector3 &v); Vector3 get_gravity_mask() const;
	void set_drag(float v); float get_drag() const;
	void set_spread(float v); float get_spread() const;
	void set_spread_skip(float v); float get_spread_skip() const;
	void set_orbitalspeed(float v); float get_orbitalspeed() const;
	void set_orbitalspeed_adj(float v); float get_orbitalspeed_adj() const;
	void set_orbital_axis(const Vector3 &v); Vector3 get_orbital_axis() const;

	void set_collide_sounds(const PackedStringArray &v); PackedStringArray get_collide_sounds() const;
	void set_graphics(const TypedArray<NovaParticleGraphicLayer> &v); TypedArray<NovaParticleGraphicLayer> get_graphics() const;
	void set_unknown_keys(const Array &v); Array get_unknown_keys() const;

	void copy_from_native(const opennova::particle::ParticleDef &def);
	opennova::particle::ParticleDef to_native() const;

	// Editor introspection of the canonical name<->bit tables (single source of
	// truth in libs/particle). The Particle workspace builds flag/move pickers
	// and blend-mode dropdowns from these instead of hand-copying the tables.
	static Dictionary get_particle_flag_table();   // {name(String): bit(int)}, engine table order
	static Dictionary get_move_flag_table();        // {name(String): bit(int)}
	static PackedStringArray get_blend_mode_names(); // indexed by BlendMode value 0..7
	static String format_particle_flags(int bits);  // canonical " NAME1 NAME2 " display string
	static String format_move_flags(int bits);

	// Deep copy of every field (incl. the 4 graphic layers and curve refs) via
	// the native round-trip. Backs the editor's Duplicate action.
	Ref<NovaParticleDef> clone() const;
};

} // namespace godot
