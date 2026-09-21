#pragma once

#include <runtime/mission/static_sources.h>
#include <godot_cpp/variant/transform3d.hpp>

namespace godot {

inline opennova::mission::StaticSourceTransform to_static_source_transform(
		const Transform3D &value) {
	opennova::mission::StaticSourceTransform out;
	for (int row = 0; row < 3; ++row) {
		for (int column = 0; column < 3; ++column)
			out[row * 3 + column] = value.basis.rows[row][column];
		out[9 + row] = value.origin[row];
	}
	return out;
}

inline Transform3D from_static_source_transform(
		const opennova::mission::StaticSourceTransform &value) {
	Transform3D out;
	for (int row = 0; row < 3; ++row) {
		for (int column = 0; column < 3; ++column)
			out.basis.rows[row][column] = value[row * 3 + column];
		out.origin[row] = value[9 + row];
	}
	return out;
}

} // namespace godot
