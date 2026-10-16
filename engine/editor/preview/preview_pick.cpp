#include <editor/preview/preview_pick.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace opennova::editor {

namespace {

// Along an axis by less than this the segment runs parallel to the slab.
constexpr float kParallel = 0.00001f;
// A triangle whose determinant is below this lies along the segment.
constexpr float kFlat = 1e-12f;

float axis(const PreviewVec3 &v, int i) {
	return i == 0 ? v.x : i == 1 ? v.y : v.z;
}

PreviewVec3 sub(const PreviewVec3 &a, const PreviewVec3 &b) {
	return PreviewVec3{ a.x - b.x, a.y - b.y, a.z - b.z };
}

} // namespace

bool segment_meets_box(const PreviewVec3 &low, const PreviewVec3 &high, const PreviewVec3 &a, const PreviewVec3 &b,
		float limit) {
	float from = 0.0f, to = limit;
	const PreviewVec3 along = sub(b, a);
	for (int i = 0; i < 3; ++i) {
		const float d = axis(along, i), start = axis(a, i);
		if (std::fabs(d) < kParallel) {
			if (start < axis(low, i) || start > axis(high, i)) return false;
			continue;
		}
		float t0 = (axis(low, i) - start) / d, t1 = (axis(high, i) - start) / d;
		if (t0 > t1) std::swap(t0, t1);
		from = std::max(from, t0);
		to = std::min(to, t1);
		if (from > to) return false;
	}
	return true;
}

bool segment_meets_triangle(const PreviewVec3 &a, const PreviewVec3 &b, const PreviewVec3 &v0, const PreviewVec3 &v1,
		const PreviewVec3 &v2, float &at) {
	const PreviewVec3 along = sub(b, a);
	const PreviewVec3 e1 = sub(v1, v0), e2 = sub(v2, v0);
	const PreviewVec3 p = cross(along, e2);
	const float det = dot(e1, p);
	if (std::fabs(det) < kFlat) return false;
	const float inverse = 1.0f / det;
	const PreviewVec3 t = sub(a, v0);
	const float u = dot(t, p) * inverse;
	if (u < 0.0f || u > 1.0f) return false;
	const PreviewVec3 q = cross(t, e1);
	const float v = dot(along, q) * inverse;
	if (v < 0.0f || u + v > 1.0f) return false;
	at = dot(e2, q) * inverse;
	return at >= 0.0f && at <= 1.0f;
}

bool segment_nearest_face(const PreviewVec3 *faces, size_t count, const PreviewVec3 &low, const PreviewVec3 &high,
		const PreviewVec3 &a, const PreviewVec3 &b, float &best) {
	if (!segment_meets_box(low, high, a, b, best)) return false;
	bool met = false;
	for (size_t i = 0; i + 2 < count; i += 3) {
		float at = 0.0f;
		if (segment_meets_triangle(a, b, faces[i], faces[i + 1], faces[i + 2], at) && at < best) {
			best = at;
			met = true;
		}
	}
	return met;
}

} // namespace opennova::editor
