#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>

#include <formats/particle/particle.h>

#include "particle/particle_curve_ref.h"

namespace godot {

// Wraps opennova::particle::GraphicLayer (~788 B in engine heap).
// Engine: CParticleDefEntry_ParseGraphicProperty @ 0x5e3550 +
// CParticleDef_SaveToFile graphic loop @ 0x5e540b.
//
// blend_mode is exposed as int (matches BlendMode enum order):
//   0 Blend, 1 Additive, 2 Premult, 3 Bump, 4 Mod, 5 Mod2x, 6 Bumpadd, 7 Distort.
// Engine quirk: chained strstr in CParticleDefEntry_ParseBlendMode @ 0x5e29f0
// matches "bump" before "bumpadd"; our parser mirrors this so corpus parses
// identically.
class ParticleGraphicLayer : public Resource {
	GDCLASS(ParticleGraphicLayer, Resource)

private:
	int index = 0;
	bool present = false;
	String texture;
	String blend_mode_raw;
	int blend_mode = 0; // BlendMode enum
	int flip_frames = 1;
	int flip_rate = 8;
	Color color1, color2, color3, color4;
	bool color_overrides_set = false;
	float alpha = 1.0f;
	float scale_value = 0.0f;     // "scale" collides with Resource::scale; rename property
	float scale_adj = 0.0f;
	Ref<ParticleCurveRef> scale_func;
	Ref<ParticleCurveRef> alpha_func;
	Ref<ParticleCurveRef> red_func;
	Ref<ParticleCurveRef> green_func;
	Ref<ParticleCurveRef> blue_func;

	static Color color3_to_godot(const opennova::particle::Color3 &c);
	static opennova::particle::Color3 godot_to_color3(const Color &c);

protected:
	static void _bind_methods();

public:
	ParticleGraphicLayer();

	void set_index(int v); int get_index() const;
	void set_present(bool v); bool get_present() const;
	void set_texture(const String &v); String get_texture() const;
	void set_blend_mode(int v); int get_blend_mode() const;
	void set_color1(const Color &c); Color get_color1() const;
	void set_color2(const Color &c); Color get_color2() const;
	void set_color3_prop(const Color &c); Color get_color3_prop() const;
	void set_color4(const Color &c); Color get_color4() const;
	void set_alpha(float v); float get_alpha() const;
	void set_scale_value(float v); float get_scale_value() const;

	void copy_from_native(const opennova::particle::GraphicLayer &layer);
	opennova::particle::GraphicLayer to_native() const;
};

} // namespace godot
