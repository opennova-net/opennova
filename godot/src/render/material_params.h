#pragma once

// The texture and material lookups shared by the render adapters that mirror
// scene surfaces into their own draw records (the Q3 source registry, the slot
// capture adapter). Their material values are the producers' published ones
// (render/q3_source_registry.h), never read back off a ShaderMaterial.

#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/rid.hpp>

namespace godot {

inline RID server_rid(const Ref<Texture2D> &p_texture) {
	return p_texture.is_valid() ? p_texture->get_rid() : RID();
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
