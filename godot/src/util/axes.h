#pragma once

#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <formats/env/env.h>
#include <runtime/mission/placement_traits.h>

#include <array>

namespace godot {

// The render-float (D3D world) <-> Godot world axis map for celestial/sky
// direction and position tuples (witnessed 2026-08-20, the 03tr-sun-sky
// fixture round; docs/env/env-tod-re.md "Celestial bodies").
//
// Derivation: the original getters serve celestial tuples in the RENDER FLOAT
// basis — mission fixed (x east, y north, z up) maps through
// [orig: Math_FixedPointToFloat3_YNegated @ 0x611210, see docs/env/env-tod-re.md] as
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
// z south): (x, y, z)_mission -> (x, z, -y)_godot. The ONE mission ->
// presentation map (world/presentation_frame.h) for every float tuple the
// engine hands the bindings: world::Vec3, env::Vec3, the wire/presenter
// {x,y,z} structs, and the float[3] / std::array<float, 3> forms.
template <typename V>
inline Vector3 mission_to_godot(const V &v) {
	return Vector3(v.x, v.z, -v.y);
}

inline Vector3 mission_to_godot(const float (&v)[3]) {
	return Vector3(v[0], v[2], -v[1]);
}

inline Vector3 mission_to_godot(const std::array<float, 3> &v) {
	return Vector3(v[0], v[2], -v[1]);
}

// The inverse, into any {x, y, z} float struct: env::Vec3 by default,
// world::Vec3 for the record constructors that author Godot-space rows.
template <typename V = opennova::env::Vec3>
inline V godot_to_mission(const Vector3 &v) {
	return V{static_cast<float>(v.x), static_cast<float>(-v.z),
			static_cast<float>(v.y)};
}

// The one BMS rotation -> Godot basis wrapper over the engine's witnessed
// converter (runtime/mission/placement_traits.h carries the derivation and
// citations); MissionObjectPlacer.bms_to_godot_basis is its bound face.
inline Basis bms_to_godot_basis(const Vector3 &rot_deg) {
	const opennova::mission::PlacementBasis b =
			opennova::mission::bms_to_presentation_basis(
					static_cast<float>(rot_deg.x), static_cast<float>(rot_deg.y),
					static_cast<float>(rot_deg.z));
	return Basis(Vector3(b.x.x, b.x.y, b.x.z), Vector3(b.y.x, b.y.y, b.y.z),
			Vector3(b.z.x, b.z.y, b.z.z));
}

// The Godot camera transform of a composed mission view: eye (Godot space)
// and mission-euler yaw / pitch / roll in degrees. The basis is the engine's
// view matrix Rz(90 - yaw) * Ry(-pitch) * Rx(roll) through the same BMS
// converter the entities place with, turned a half circle about Y because a
// Godot camera looks down -Z where the converter's model faces +Z. Built
// straight from the angles — never a look-at from the eye — it keeps every
// direction bit at any distance from the origin, has no pole looking
// straight up or down, and carries the roll in the one construction (the
// hull and its cockpit bank through the same matrix). Every gameplay camera
// the local view drives stamps through this one helper.
inline Transform3D mission_view_transform(const Vector3 &eye, float yaw_deg, float pitch_deg,
		float roll_deg) {
	const Basis model = bms_to_godot_basis(Vector3(pitch_deg, yaw_deg, roll_deg));
	return Transform3D(Basis(-model.get_column(0), model.get_column(1), -model.get_column(2)),
			eye);
}

} // namespace godot
