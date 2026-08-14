#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/static_body3d.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include "env/nova_mission_environment.h"
#include "env/nova_weather.h"
#include "nova_terrain_data.h"
#include <terrain/terrain_frame.h>

#include <cstdint>
#include <vector>

namespace godot {

class TerrainTileInfo;
class TerrainSurfaceInputs;
class Water;

using FoliageDetailPatch = opennova::FoliageDetailPatch;

class Terrain : public Node3D {
	GDCLASS(Terrain, Node3D)

private:
	Ref<TerrainData> terrain_data;
	Ref<TerrainTileInfo> tile_info_override;
	NodePath environment_path;
	NodePath weather_path;
	NodePath water_path;
	float lod_quality = 1.0f;
	bool tile_overlay_enabled = true;

	// Per-tile: one single-surface ArrayMesh per LOD level
	struct TileInfo {
		Ref<ArrayMesh> lod_meshes[8];
	};
	std::vector<TileInfo> tile_infos;

	// The engine-owned terrain frame (ADR 0033 R2): the scene snapshot built
	// once per load and the per-frame compiler whose draw list this node applies.
	opennova::TerrainSceneSnapshot scene_snapshot;
	opennova::TerrainFrameCompiler frame_compiler;
	// False until a compile ran for the current frame's camera; the foliage
	// handoff getter returns no cells while it is down (mission teardown, no
	// active camera) rather than a stale draw list's.
	bool frame_draw_list_live = false;

	// Lightweight RenderingServer instance pool for visible patches. The pool
	// size is the engine compiler's draw list budget; draw-list index == pool slot.
	static constexpr int PATCH_POOL_SIZE = opennova::TerrainFrameCompiler::kPatchBudget;
	RID patch_instances[PATCH_POOL_SIZE];
	RID last_mesh_rid[PATCH_POOL_SIZE];
	Transform3D last_transform[PATCH_POOL_SIZE];
	bool patch_visible[PATCH_POOL_SIZE] = {};
	int patches_active = 0;

	// Shader
	Ref<Shader> terrain_shader;
	Ref<ShaderMaterial> terrain_material;
	Ref<ShaderMaterial> shadow_receiver_material;
	Ref<TerrainSurfaceInputs> surface_inputs;
	Vector3 tile_overlay_tint = Vector3(1.0f, 1.0f, 1.0f);

	bool built = false;


	// Cached typed node pointers — avoids per-frame get_node_or_null()
	MissionEnvironment *cached_env_node = nullptr;
	Weather *cached_weather_node = nullptr;
	Water *cached_water_node = nullptr;
	bool terrain_node_cache_valid = false;
	// The noise texture currently bound to u_water_noise: the shared
	// ImageTexture updates in place each frame, so one bind suffices
	// (D-TERRAIN-8, docs/terrain/terrain-re.md underwater section).
	Ref<Texture2D> bound_water_noise;

	void _cache_env_weather_nodes();
	void _render_frame_with_camera(Camera3D *cam);

	// Debug state (the toggles feed the compiler's TraversalConfig input)
	opennova::TraversalConfig traversal_config;
	int debug_mode = 0; // 0=normal, 1=LOD colors, 2=normals

	bool _build_terrain();
	void _load_textures();
	void _clear_derived_textures();
	void _rebuild_tile_overlay_texture();
	void _clear_tile_overlay_texture();
	void _clear_terrain();
	void _hide_visible_patches();
	void _clear_patch_pool();
	void _on_terrain_changed();
	Ref<Shader> _load_terrain_shader();

	static void _strip_to_list(const std::vector<uint16_t>& strip,
	                           PackedInt32Array& out);

	Vector3 _heightmap_normal(const std::vector<uint16_t>& depth, int gx, int gz,
	                          const opennova::terrain::CoordsTaps& taps) const;

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	Terrain();
	~Terrain();

	void set_terrain_data(const Ref<TerrainData> &p_data);
	Ref<TerrainData> get_terrain_data() const;
	Ref<TerrainSurfaceInputs> get_surface_inputs() const;
	Ref<Texture2D> get_heightfield_normal_texture() const;
	Ref<Texture2D> get_tile_overlay_texture() const;
	Vector3 get_tile_overlay_tint() const;

	void set_lod_quality(float p_quality);
	float get_lod_quality() const;

	void set_tile_overlay_enabled(bool p_enabled);
	bool get_tile_overlay_enabled() const;

	void set_tile_info_override(const Ref<TerrainTileInfo> &p_info);
	Ref<TerrainTileInfo> get_tile_info_override() const;
	void rebuild_tile_overlay();

	void set_environment_path(const NodePath& p_path);
	NodePath get_environment_path() const;

	void set_weather_path(const NodePath& p_path);
	NodePath get_weather_path() const;

	void set_water_path(const NodePath& p_path);
	NodePath get_water_path() const;

	// The single shared surface material (GUT seam; precedent
	// Water::get_water_material).
	Ref<ShaderMaterial> get_terrain_material() const { return terrain_material; }

	void build();

	// The terrain frame leg (ADR 0033 R2): compile the engine patch draw list for
	// this node's viewport camera and apply it onto the instance pool. Driven
	// by GameFramePipeline through the concrete terrain leg — this node
	// no longer self-processes.
	void render_frame();
	// The minimap-bake compile: the same pool recompiled for an arbitrary
	// camera. Bake-only — the pool holds this draw list until the next live
	// compile, so callers confine it to the covered load window.
	bool render_frame_for_camera(Camera3D *p_camera);
	bool is_built() const { return built; }

	// Debug API
	Dictionary get_traversal_stats() const;
	PackedInt32Array get_lod_distribution() const;
	int get_patches_active() const;
	int get_visible_patch_count() const;
	const std::vector<FoliageDetailPatch> &get_foliage_detail_patches_native() const;

	void set_debug_no_frustum(bool v);
	bool get_debug_no_frustum() const;
	void set_debug_no_nearfar(bool v);
	bool get_debug_no_nearfar() const;
	void set_debug_no_sideplanes(bool v);
	bool get_debug_no_sideplanes() const;
	void set_debug_no_partial_subdiv(bool v);
	bool get_debug_no_partial_subdiv() const;
	void set_debug_force_leaves(bool v);
	bool get_debug_force_leaves() const;
	void set_debug_force_lod0(bool v);
	bool get_debug_force_lod0() const;

	void set_debug_mode(int mode);
	int get_debug_mode() const;
};

} // namespace godot
