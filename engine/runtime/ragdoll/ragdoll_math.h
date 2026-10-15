#pragma once

// The small float vector and quaternion algebra the ragdoll solver runs on,
// over the animation library's own Vec3 and w-first Quat so poses cross
// without conversion. Presentation math: nothing here feeds the simulation.

#include <runtime/anim/anim_sample.h>

#include <cmath>

namespace opennova::ragdoll {

using anim::Quat;
using anim::Vec3;
// The animation library's Hamilton product and rotation, the pose math's own.
using anim::quat_mul;
using anim::quat_rotate;

inline Vec3 operator+(const Vec3 &a, const Vec3 &b) {
	return { a.x + b.x, a.y + b.y, a.z + b.z };
}
inline Vec3 operator-(const Vec3 &a, const Vec3 &b) {
	return { a.x - b.x, a.y - b.y, a.z - b.z };
}
inline Vec3 operator-(const Vec3 &a) {
	return { -a.x, -a.y, -a.z };
}
inline Vec3 operator*(const Vec3 &a, float s) {
	return { a.x * s, a.y * s, a.z * s };
}
inline Vec3 operator*(float s, const Vec3 &a) {
	return { a.x * s, a.y * s, a.z * s };
}
inline Vec3 &operator+=(Vec3 &a, const Vec3 &b) {
	a.x += b.x;
	a.y += b.y;
	a.z += b.z;
	return a;
}
inline Vec3 &operator-=(Vec3 &a, const Vec3 &b) {
	a.x -= b.x;
	a.y -= b.y;
	a.z -= b.z;
	return a;
}
inline float dot(const Vec3 &a, const Vec3 &b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline Vec3 cross(const Vec3 &a, const Vec3 &b) {
	return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
inline float length(const Vec3 &a) {
	return std::sqrt(dot(a, a));
}
// The unit vector, or `fallback` when `a` is (near) zero.
inline Vec3 normalized(const Vec3 &a, const Vec3 &fallback = { 0.0f, 1.0f, 0.0f }) {
	const float l = length(a);
	return l > 1e-8f ? a * (1.0f / l) : fallback;
}

inline Quat quat_conj(const Quat &q) {
	return { q.w, -q.x, -q.y, -q.z };
}
inline Quat quat_normalized(const Quat &q) {
	const float l = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
	if (l < 1e-12f)
		return Quat{};
	const float s = 1.0f / l;
	return { q.w * s, q.x * s, q.y * s, q.z * s };
}
inline Quat quat_axis_angle(const Vec3 &unit_axis, float angle) {
	const float h = 0.5f * angle;
	const float s = std::sin(h);
	return { std::cos(h), unit_axis.x * s, unit_axis.y * s, unit_axis.z * s };
}
// The rotation vector (axis * angle, angle in [0, pi]) of a unit quaternion.
inline Vec3 quat_log(const Quat &q_in) {
	Quat q = q_in.w < 0.0f ? Quat{ -q_in.w, -q_in.x, -q_in.y, -q_in.z } : q_in;
	const Vec3 v{ q.x, q.y, q.z };
	const float s = length(v);
	if (s < 1e-9f)
		return v * 2.0f;
	const float angle = 2.0f * std::atan2(s, q.w);
	return v * (angle / s);
}
// q advanced by a small rotation vector `w` (world frame): q + 0.5 (0, w) q.
inline Quat quat_integrate(const Quat &q, const Vec3 &w) {
	const Quat dq = quat_mul(Quat{ 0.0f, w.x, w.y, w.z }, q);
	return quat_normalized(
			{ q.w + 0.5f * dq.w, q.x + 0.5f * dq.x, q.y + 0.5f * dq.y, q.z + 0.5f * dq.z });
}

// A rigid placement with a uniform scale: world = origin + scale * rotation(local).
struct Xform {
	Quat rotation;
	Vec3 origin;
	float scale = 1.0f;
};

inline Vec3 xform_point(const Xform &t, const Vec3 &p) {
	return t.origin + quat_rotate(t.rotation, p) * t.scale;
}
inline Vec3 xform_point_inverse(const Xform &t, const Vec3 &p) {
	return quat_rotate(quat_conj(t.rotation), p - t.origin) * (1.0f / t.scale);
}

} // namespace opennova::ragdoll
