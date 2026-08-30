#pragma once

#include <array>
#include <cmath>

namespace opennova::renderer {

// The retail direction look-at frame, the one engine home of
// [orig: build_direction_look_at_matrix @ 0x612c90]: forward = dir / |dir|
// (zero for a zero direction @ 0x612d05), right = (fwd.z, 0, -fwd.x) /
// |(fwd.z, -fwd.x)| (all zero when that length is zero @ 0x612d5b), up =
// normalize(fwd x right) (zero when degenerate @ 0x612dfa). Retail stores the
// three as the columns of its row-vector render matrix (@ 0x612e18..0x612e6d).
// Consumed by the slot silhouette capture view (setup_shadow_cascade_matrices
// @ 0x58d300) and the addeweap attachment frame (build_bone_attachment_matrix
// @ 0x56c630).
//
// A vertical direction leaves right and up ZERO in retail (the degenerate
// zenith frame); both consumers substitute the world x axis for the right
// vector so the frame keeps a usable basis, and `degenerate` reports that the
// substitution (or a zero direction) happened.
template <typename T>
struct DirectionLookAt {
	std::array<T, 3> right{};
	std::array<T, 3> up{};
	std::array<T, 3> forward{};
	bool degenerate = false;
};

template <typename T>
DirectionLookAt<T> direction_look_at(const std::array<T, 3> &direction) {
	DirectionLookAt<T> frame;
	const T length = std::sqrt(direction[0] * direction[0] +
			direction[1] * direction[1] + direction[2] * direction[2]);
	if (length > T(0)) {
		frame.forward = {direction[0] / length, direction[1] / length,
				direction[2] / length};
	}
	const T fx = frame.forward[0];
	const T fy = frame.forward[1];
	const T fz = frame.forward[2];
	const T horizontal = std::sqrt(fx * fx + fz * fz);
	if (horizontal > T(0)) {
		frame.right = {fz / horizontal, T(0), -fx / horizontal};
	} else {
		frame.degenerate = true;
		frame.right = {T(1), T(0), T(0)};
	}
	const std::array<T, 3> &r = frame.right;
	const std::array<T, 3> up = {fy * r[2] - r[1] * fz, fz * r[0] - r[2] * fx,
			r[1] * fx - r[0] * fy};
	const T up_length = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
	if (up_length > T(0)) {
		frame.up = {up[0] / up_length, up[1] / up_length, up[2] / up_length};
	} else {
		frame.degenerate = true;
	}
	return frame;
}

}  // namespace opennova::renderer
