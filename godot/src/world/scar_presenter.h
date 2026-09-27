#pragma once

// ScarPresenter — the impact-scar ring: the device half uploads the
// renderer's scar draw list (engine/runtime/renderer/scar_draw_list.h,
// compiled by Simulation::get_scar_draw_list) as ArrayMesh surfaces; the
// present-pass half (the former scar_present_pass.gd folded in, ADR 0043 d9,
// as EntityPresenter's owned "Scars" child) pulls that list once per present
// frame and resolves each entity-ring owner to its live model node.
//
// Shared-ring batches (buildings, persons) are world-space quads and land on
// ONE top-level mesh, one surface per (texture strip, building) batch. Entity
// ring batches are SECTION-LOCAL to the owner model's struck section and are
// parented under that section's render-part node, so they ride the carrier's
// pose for free [orig: Scar_RenderCache @0x5CD830 transforms an entity ring's
// slots through `bones + bone << 6` at draw time; the shared ring draws its
// positions as-is — see docs/world/world-wac-ai-re.md §24.9].
//
// The drawer state is per strip: the GfxShader mode word the loader built the
// strip's effect from (the draw list's `strip_mode_words`, decoded through
// opennova::renderer::decode_scar_strip_mode) selects the material — the scorch state
// (shaders/scar_quad.gdshader: SRCALPHA/INVSRCALPHA blend, MODULATE2X colour,
// fog, no z-write, CCW cull, NO alpha test) or the bullet-hole state
// (shaders/scar_quad_hole.gdshader: the same plus the GREATER/128 alpha test,
// z-write, cull none). The ONLY device folds are the view-space pull that
// replaces the D3D depth bias and the winding the sim packer applies with the
// coordinate fold [orig: the scar batch drawer Scar_DrawBatches (ex Terrain_RenderFoliageBatches)
// @0x5ccd10 — the IDB's kong misnomer, Scar_DrawBatches proposed — under
// Scar_RenderAllCaches @0x5CDF70; the strip table Scar_LoadTextures @0x5CC2E0].
//
// The present pass: pulls the sim's scar draw list once per present frame
// (Simulation.get_scar_draw_list — World::scars compiled by
// renderer::compile_scar_draws) and hands it to the device, which
// uploads the shared ring as one world mesh and each entity ring under its
// owner's struck section node. The sim stays render-free; the pass supplies
// the three device inputs the renderer's compile reads — the camera ground
// position and fog distance for the fog-box cull, and the combined terrain
// light colour folded onto every vertex — and resolves each entity-ring owner
// to its live model node. Every viewing peer runs it: retail draws its own
// caches on every client from the same impact processor
// [orig: Scar_RenderAllCaches @0x5CDF70 from Terrain_CollectVisibleEntities
//  @0x5c91b7].
//
// [orig map — docs/world/world-wac-ai-re.md §24.9:
//  Scar_RenderAllCaches @0x5CDF70 (the shared ring first, then every live
//    entity ring) -> Scar_RenderCache @0x5CD830 (the fog box
//    `|p - cam| <= fog + r` on the ground axes, the owner-visibility gate, the
//    six vertices per slot coloured Env_TerrainLightCombined | FF000000);
//  the drawer Scar_DrawBatches (ex Terrain_RenderFoliageBatches) @0x5ccd10 (the IDB's kong misnomer;
//  the Scar_DrawBatches rename is proposed) after the lit sector entities.]

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
#include <godot_cpp/variant/vector3.hpp>

#include "resource_index/resource_root.h"
#include "simulation/present_stats.h"
#include "world/scar_draw_list.h"

namespace godot {

class EntityIndex;
class EntityPresenter;
class MissionEnvironment;
class Simulation;

class ScarPresenter : public Node3D {
	GDCLASS(ScarPresenter, Node3D)

public:
	ScarPresenter();
	~ScarPresenter() override;

	void set_resource_root(const Ref<ResourceRoot> &p_root);
	Ref<ResourceRoot> get_resource_root() const { return resource_root_; }

	// Upload one frame's draw list (the record Simulation::get_scar_draw_list
	// returns). `p_owner_nodes` maps an entity-ring owner (the packed handle, int)
	// to its live model node (ObjectModel preferred — its render-part nodes are
	// the mounts for the section-local meshes; any Node3D mounts them at its
	// origin otherwise).
	// Entity batches whose owner is absent from the map draw nothing this frame.
	void present(const Ref<ScarDrawList> &p_draw_list, const Dictionary &p_owner_nodes);
	// Drop every scar mesh (the Stop -> Play boundary, teardown).
	void clear();
	// The typed counter snapshot (ScarPresenterStats: world_surfaces,
	// entity_meshes, batches, vertices, textures_missing, strips_unsupported).
	Ref<ScarPresenterStats> get_stats_record() const;

	// --- The present pass ----------------------------------------------------

	// Once per present, after the entity rows (their section nodes host the
	// entity-ring meshes). `camera` is the same listener position the fire
	// pass draws its ribbons around (the camera IS the listener; a non-finite
	// value means none yet and reads as the origin); `environment` (nullable)
	// is the live MissionEnvironment for the fog distance + terrain light;
	// `index` resolves authored-entity owners, `wire` runtime-only (wire)
	// owners. No sim: nothing presents.
	void present_frame(Simulation *p_sim, const Vector3 &p_camera,
			MissionEnvironment *p_environment, EntityIndex *p_index, EntityPresenter *p_wire);
	// The pure-data presentation leg (the present_snapshot precedent): production
	// present_frame() feeds the sim's draw list; tests author the same record.
	void present_draw_list(const Ref<ScarDrawList> &p_draw_list, EntityIndex *p_index,
			EntityPresenter *p_wire);
	// Discard mission-run presentation state (the Stop -> Play boundary): every
	// scar mesh goes; the presenter and its texture cache stay.
	void reset_runtime_state();
	// The weapon Inset view (OcclusionFrame::apply_inset_frame / release_inset).
	// Retail's Inset scene core compiles and draws the scar caches inside its
	// own collect (engine: world/occlusion.h OcclusionView carries the
	// witness): while it renders, present_frame compiles the list a second
	// time over the Inset's section masks and eye. The main list's world mesh
	// then draws for the main view only, the Inset list's on INSET_VIEW; an
	// entity-ring group of an owner whose views differ (ObjectModel split)
	// takes an Inset twin the owner poses with its Inset part pose, while the
	// owner's node scars follow the node's main-view bits.
	void set_inset_view(bool p_active, const Vector3 &p_camera);
	// The Inset leg's typed read-back: its world surfaces and entity twins.
	int get_inset_world_surface_count() const { return stat_inset_world_surfaces_; }
	int get_inset_entity_twin_count() const { return static_cast<int>(inset_twins_.size()); }
	// The Inset leg over an authored list (the present_draw_list precedent),
	// and its owner-map form (the present precedent).
	void present_inset_draw_list(const Ref<ScarDrawList> &p_draw_list, EntityIndex *p_index,
			EntityPresenter *p_wire);
	void present_inset(const Ref<ScarDrawList> &p_draw_list, const Dictionary &p_owner_nodes);
	// Typed diagnostic counters of the pass (ADR 0017: cross-object contracts
	// are typed records) — probes assert the presentation leg actually ran.
	Ref<ScarPresentStats> get_present_stats() const;

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	// One strip's material: the shader its mode word selects, the TGA bound
	// once it resolves.
	struct StripMaterial {
		Ref<ShaderMaterial> material;
		bool texture_bound = false;
		bool unsupported = false;
	};
	StripMaterial &material_for_strip_(int p_strip, const String &p_texture_name,
			uint32_t p_mode_word);
	Ref<Shader> shader_for_mode_(uint32_t p_mode_word, bool &r_unsupported);
	Ref<Texture2D> texture_(const String &p_name);
	MeshInstance3D *ensure_world_mesh_();
	void free_entity_meshes_();
	// Every entity-ring owner in the list -> its live node (packed handle ->
	// Node3D), into the map the device leg consumes.
	void resolve_owner_nodes_(const Ref<ScarDrawList> &p_draw_list, EntityIndex *p_index,
			EntityPresenter *p_wire, Dictionary &r_owner_nodes);
	static Node3D *resolve_owner_(int p_bms_id, int64_t p_spawn_origin, int p_wire_handle,
			EntityIndex *p_index, EntityPresenter *p_wire);

	Ref<ResourceRoot> resource_root_;
	Ref<Shader> shader_scorch_;
	Ref<Shader> shader_hole_;
	HashMap<int, StripMaterial> materials_;
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
	int stat_strips_unsupported_ = 0;
	// The pass-level counters: the sim's slot census and the owners the
	// resolve could not reach.
	int stat_slots_live_ = 0;
	int stat_slots_culled_ = 0;
	int stat_rings_leased_ = 0;
	int stat_owners_unresolved_ = 0;
	// The weapon Inset view's leg (set_inset_view).
	struct InsetScarTwin {
		RID instance;
		Ref<ArrayMesh> mesh;
		ObjectID owner;
	};
	bool inset_active_ = false;
	Vector3 inset_camera_;
	ObjectID inset_world_mesh_id_;
	HashMap<uint32_t, InsetScarTwin> inset_twins_;
	int stat_inset_world_surfaces_ = 0;
	void present_inset_(const Ref<ScarDrawList> &p_draw_list, const Dictionary &p_owner_nodes);
	void release_inset_();
	// The main world mesh's view bits: the world layer, or the main view's
	// alone while the Inset draws its own list.
	void apply_world_mesh_view_(bool p_split);
};

} // namespace godot
