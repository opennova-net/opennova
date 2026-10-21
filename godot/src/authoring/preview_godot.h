// The editor's preview values as Godot's, for the authoring appliers that draw them (ADR 0046):
// one home for the conversions they shared. Device marshalling only, no engine fact.
#pragma once

#include <cstdint>

#include <editor/preview/model_preview_camera.h>

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

namespace godot {

// A point of the preview's space (opennova::editor::PreviewVec3: the renderer's model frame) as the
// Vector3 its nodes take; the same axes.
inline Vector3 to_godot(const opennova::editor::PreviewVec3 &v) { return Vector3(v.x, v.y, v.z); }

// The orbit camera's pose on a Godot camera: its axes as the basis, its eye as the origin (a scene's layers', whose
// planes are the world pass's).
inline void place_orbit_pose(Camera3D &camera, const opennova::editor::OrbitCamera &orbit) {
	opennova::editor::PreviewVec3 right, up, back;
	orbit.axes(right, up, back);
	camera.set_transform(Transform3D(Basis(to_godot(right), to_godot(up), to_godot(back)), to_godot(orbit.eye())));
}

// An orbit device's camera where the orbit camera is: its pose, its near and far planes and, with `fov`, its field of
// view (a first-person eye's renderfov, else the game's view; the effect's keeps the one its stage gave it).
inline void place_orbit_camera(Camera3D &camera, const opennova::editor::OrbitCamera &orbit, bool fov) {
	place_orbit_pose(camera, orbit);
	camera.set_near(orbit.near_plane);
	camera.set_far(orbit.far_plane);
	if (fov) camera.set_fov(orbit.fov_degrees());
}

// The engine's microsecond clock (Time::get_ticks_usec), for the appliers' build and frame timings.
inline int64_t now_us() { return int64_t(Time::get_singleton()->get_ticks_usec()); }

} // namespace godot
