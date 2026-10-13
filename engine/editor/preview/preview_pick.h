#pragma once

#include <cstddef>

#include <editor/preview/model_preview_camera.h>

namespace opennova::editor {

// A segment through the picture's shapes, the mission view's pick (ADR 0046 S14: a click, a drop or
// a drag's ray meeting the placed models' faces before the ground;
// godot/src/authoring/mission_viewport_applier's ray_between): the editor's pick, not a port. A
// segment runs from `a` to `b` in a shape's own space, its points at the parameter t in [0, 1].

// The slab test: whether the segment meets the box [`low`, `high`] at a parameter below `limit` (an axis the
// segment runs along within 1e-5 of nothing tested by where `a` stands on it).
bool segment_meets_box(const PreviewVec3 &low, const PreviewVec3 &high, const PreviewVec3 &a, const PreviewVec3 &b,
		float limit);
// Where the segment meets the triangle (`v0`, `v1`, `v2`, either face): its parameter in [0, 1], false for none.
bool segment_meets_triangle(const PreviewVec3 &a, const PreviewVec3 &b, const PreviewVec3 &v0, const PreviewVec3 &v1,
		const PreviewVec3 &v2, float &at);
// The nearest of `faces` (`count` points, three a triangle) the segment meets below `best`, whose box is [`low`,
// `high`]: true with `best` its parameter; false, `best` as it was, for none (the box missed first).
bool segment_nearest_face(const PreviewVec3 *faces, size_t count, const PreviewVec3 &low, const PreviewVec3 &high,
		const PreviewVec3 &a, const PreviewVec3 &b, float &best);

} // namespace opennova::editor
