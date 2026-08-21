#pragma once

// ScarPresenter — the device half of the impact-scar ring: uploads the
// renderer's scar draw list (engine/runtime/renderer/scar_draw_list.h, compiled
// by Simulation::get_scar_draw_list) as ArrayMesh surfaces.
//
// Shared-ring batches (buildings, persons) are world-space quads and land on
// ONE top-level mesh, one surface per (texture strip, building) batch. Entity
// ring batches are SECTION-LOCAL to the owner model's struck section and are
// parented under that section's render-part node, so they ride the carrier's
// pose for free (retail: Scar_RenderCache @0x5CD830 transforms an entity ring's
// slots through `bones + bone << 6` at draw time; the shared ring draws its
// positions as-is — see docs/world/world-wac-ai-re.md §24.9).
//
// The drawer state is the per-strip material (shaders/scar_quad.gdshader):
// unshaded, vertex colour modulate, clamp wrap, alpha test 128, blend mode 0.
// The ONLY device fold is the view-space pull that replaces the D3D depth
// bias (retail: Scar_DrawBatches @0x5ccd10, Scar_RenderAllCaches @0x5CDF70).

#include <cstdint>

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include "resource_index/nova_resource_root.h"

namespace godot {

class ScarPresenter : public Node3D {
	GDCLASS(ScarPresenter, Node3D)

public:
	ScarPresenter();
	~ScarPresenter() override;

	void set_resource_root(const Ref<ResourceRoot> &p_root);
	Ref<ResourceRoot> get_resource_root() const { return resource_root_; }

	// Upload one frame's draw list (the Dictionary Simulation::get_scar_draw_list
	// returns). `p_owner_nodes` maps an entity-ring owner (the packed handle, int)
	// to its live model node (ObjectModel preferred — its render-part nodes host
	// the section-local meshes; any Node3D hosts them at its origin otherwise).
	// Entity batches whose owner is absent from the map draw nothing this frame.
	void present(const Dictionary &p_draw_list, const Dictionary &p_owner_nodes);
	// Drop every scar mesh (the Stop -> Play boundary, teardown).
	void clear();
	// Typed counters: world_surfaces, entity_meshes, batches, vertices,
	// textures_missing (strips whose TGA did not resolve this present).
	Dictionary get_stats() const;

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	Ref<ShaderMaterial> material_for_strip_(int p_strip, const String &p_texture_name);
	Ref<Texture2D> texture_(const String &p_name);
	MeshInstance3D *ensure_world_mesh_();
	void free_entity_meshes_();

	Ref<ResourceRoot> resource_root_;
	Ref<Shader> shader_;
	HashMap<int, Ref<ShaderMaterial>> materials_;
	HashMap<String, Ref<Texture2D>> textures_;
	ObjectID world_mesh_id_;
	// (owner << 8 | section) -> the entity-ring mesh instance under the owner's
	// section node.
	HashMap<uint32_t, ObjectID> entity_meshes_;
	int stat_world_surfaces_ = 0;
	int stat_entity_meshes_ = 0;
	int stat_batches_ = 0;
	int stat_vertices_ = 0;
	int stat_textures_missing_ = 0;
};

} // namespace godot
