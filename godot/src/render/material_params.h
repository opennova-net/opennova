#pragma once

// Typed reads off a ShaderMaterial and a geometry instance, shared by the
// render adapters that mirror scene materials into their own draw records
// (the Q3 source registry, the slot capture adapter). A null material or a
// parameter of another type reads as the default.

#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/vector4.hpp>

namespace godot {

inline RID server_rid(const Ref<Texture2D> &p_texture) {
	return p_texture.is_valid() ? p_texture->get_rid() : RID();
}

inline Ref<Texture2D> texture_parameter(const Ref<ShaderMaterial> &p_material,
		const StringName &p_name) {
	if (p_material.is_null())
		return Ref<Texture2D>();
	const Variant value = p_material->get_shader_parameter(p_name);
	if (value.get_type() != Variant::OBJECT)
		return Ref<Texture2D>();
	return value;
}

inline float float_parameter(const Ref<ShaderMaterial> &p_material,
		const StringName &p_name, float p_default) {
	if (p_material.is_null())
		return p_default;
	const Variant value = p_material->get_shader_parameter(p_name);
	return value.get_type() == Variant::FLOAT || value.get_type() == Variant::INT
			? static_cast<float>(value) : p_default;
}

inline bool bool_parameter(const Ref<ShaderMaterial> &p_material,
		const StringName &p_name, bool p_default) {
	if (p_material.is_null())
		return p_default;
	const Variant value = p_material->get_shader_parameter(p_name);
	return value.get_type() == Variant::BOOL ? static_cast<bool>(value) : p_default;
}

inline Vector2 vector2_parameter(const Ref<ShaderMaterial> &p_material,
		const StringName &p_name, const Vector2 &p_default) {
	if (p_material.is_null())
		return p_default;
	const Variant value = p_material->get_shader_parameter(p_name);
	return value.get_type() == Variant::VECTOR2 ? static_cast<Vector2>(value) : p_default;
}

inline Vector3 vector3_parameter(const Ref<ShaderMaterial> &p_material,
		const StringName &p_name, const Vector3 &p_default) {
	if (p_material.is_null())
		return p_default;
	const Variant value = p_material->get_shader_parameter(p_name);
	return value.get_type() == Variant::VECTOR3 ? static_cast<Vector3>(value) : p_default;
}

inline Vector4 vector4_parameter(const Ref<ShaderMaterial> &p_material,
		const StringName &p_name, const Vector4 &p_default) {
	if (p_material.is_null())
		return p_default;
	const Variant value = p_material->get_shader_parameter(p_name);
	return value.get_type() == Variant::VECTOR4 ? static_cast<Vector4>(value) : p_default;
}

// The material a surface actually draws with: a MeshInstance3D's active
// material, else the geometry override, else the mesh surface's own.
inline Ref<Material> active_material(GeometryInstance3D *p_source,
		const Ref<Mesh> &p_mesh, int p_surface) {
	if (MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(p_source))
		return mesh_instance->get_active_material(p_surface);
	Ref<Material> material = p_source->get_material_override();
	if (material.is_null() && p_mesh.is_valid())
		material = p_mesh->surface_get_material(p_surface);
	return material;
}

} // namespace godot
