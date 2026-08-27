#pragma once

#include <godot_cpp/variant/vector3.hpp>

#include <formats/env/env.h>

namespace godot {

// The render-float (D3D world) <-> Godot world axis map for celestial/sky
// direction and position tuples (witnessed 2026-08-20, the 03tr-sun-sky
// fixture round; docs/env/env-tod-re.md "Celestial bodies").
//
// Derivation: the original getters serve celestial tuples in the RENDER FLOAT
// basis — mission fixed (x east, y north, z up) maps through
// (retail: Math_FixedPointToFloat3_YNegated @ 0x611210, see docs/env/env-tod-re.md) as
// (x, y, z)_mission -> (-y, z, x)_render (render x = south, y = up,
// z = east). OpenNova's Godot world was built from mission axes as
// (x, y, z)_mission -> (x, z, -y)_godot (godot x = east, y = up, z = south).
// Composing: (x, y, z)_render -> (z, y, x)_godot — a pure x/z swap (its own
// inverse; the handedness flip between the two worlds).
//
// Consumers that keep the RAW render-float tuple deliberately do their own
// mapping downstream and must NOT route through this seam: the
// opennova_sun_direction shader global and the terrain u_sun_direction
// uniform (terrain_lighting.gdshaderinc / foliage_detail.gdshaderinc
// re-swizzle into the engine texture basis), and the StarField cull (star
// instance directions live in render-float axes engine-side).
inline Vector3 render_float_to_godot(const opennova::env::Vec3 &v) {
	return Vector3(v.z, v.y, v.x);
}

inline opennova::env::Vec3 godot_to_render_float(const Vector3 &v) {
	return opennova::env::Vec3{static_cast<float>(v.z),
			static_cast<float>(v.y), static_cast<float>(v.x)};
}

// Mission fixed axes (x east, y north, z up) <-> Godot world (x east, y up,
// z south): (x, y, z)_mission -> (x, z, -y)_godot. The engine/formats/env
// glint and veil laws take mission-axis tuples.
inline Vector3 mission_to_godot(const opennova::env::Vec3 &v) {
	return Vector3(v.x, v.z, -v.y);
}

inline opennova::env::Vec3 godot_to_mission(const Vector3 &v) {
	return opennova::env::Vec3{static_cast<float>(v.x),
			static_cast<float>(-v.z), static_cast<float>(v.y)};
}

} // namespace godot
