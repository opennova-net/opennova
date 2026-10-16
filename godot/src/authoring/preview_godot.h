// The editor's preview values as Godot's, for the authoring appliers that draw them (ADR 0046):
// one home for the conversions they shared. Device marshalling only, no engine fact.
#pragma once

#include <cstdint>

#include <editor/preview/model_preview_camera.h>

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/vector3.hpp>

namespace godot {

// A point of the preview's space (opennova::editor::PreviewVec3: the renderer's model frame) as the
// Vector3 its nodes take; the same axes.
inline Vector3 to_godot(const opennova::editor::PreviewVec3 &v) { return Vector3(v.x, v.y, v.z); }

// The engine's microsecond clock (Time::get_ticks_usec), for the appliers' build and frame timings.
inline int64_t now_us() { return int64_t(Time::get_singleton()->get_ticks_usec()); }

} // namespace godot
