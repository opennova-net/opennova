#pragma once

// The Variant::Type a C++ field type binds as, for the typed-record bind
// macros (one row per type any record field uses).

#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>

namespace godot {

template <typename T>
constexpr Variant::Type variant_type_of();
template <>
constexpr Variant::Type variant_type_of<bool>() { return Variant::BOOL; }
template <>
constexpr Variant::Type variant_type_of<int>() { return Variant::INT; }
template <>
constexpr Variant::Type variant_type_of<int64_t>() { return Variant::INT; }
template <>
constexpr Variant::Type variant_type_of<float>() { return Variant::FLOAT; }
template <>
constexpr Variant::Type variant_type_of<String>() { return Variant::STRING; }
template <>
constexpr Variant::Type variant_type_of<Vector3>() { return Variant::VECTOR3; }
template <>
constexpr Variant::Type variant_type_of<Color>() { return Variant::COLOR; }
template <>
constexpr Variant::Type variant_type_of<AABB>() { return Variant::AABB; }
template <>
constexpr Variant::Type variant_type_of<Transform3D>() { return Variant::TRANSFORM3D; }
template <>
constexpr Variant::Type variant_type_of<PackedByteArray>() { return Variant::PACKED_BYTE_ARRAY; }
template <>
constexpr Variant::Type variant_type_of<PackedInt32Array>() { return Variant::PACKED_INT32_ARRAY; }
template <>
constexpr Variant::Type variant_type_of<PackedInt64Array>() { return Variant::PACKED_INT64_ARRAY; }
template <>
constexpr Variant::Type variant_type_of<PackedStringArray>() { return Variant::PACKED_STRING_ARRAY; }
template <>
constexpr Variant::Type variant_type_of<PackedVector2Array>() { return Variant::PACKED_VECTOR2_ARRAY; }
template <>
constexpr Variant::Type variant_type_of<PackedVector3Array>() { return Variant::PACKED_VECTOR3_ARRAY; }
template <>
constexpr Variant::Type variant_type_of<PackedColorArray>() { return Variant::PACKED_COLOR_ARRAY; }

} // namespace godot
