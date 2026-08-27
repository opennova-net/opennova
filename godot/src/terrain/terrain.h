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
#include "lights/nova_light_scene.h"
#include "nova_terrain_data.h"
#include "nova_terrain_static_shadow_rasterizer.h"
#include "nova_terrain_tile_cache_device.h"
#include <runtime/terrain/terrain_frame.h>

#include <cstdint>
#include <optional>
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
	// The per-instance uniforms each slot holds (written on change only: an
	// instance uniform write is a RenderingServer call per patch per frame).
	bool patch_uniforms_stamped[PATCH_POOL_SIZE] = {};
	Vector2 last_quadrant[PATCH_POOL_SIZE];
	bool last_page_ready[PATCH_POOL_SIZE] = {};
	float last_page_layer[PATCH_POOL_SIZE] = {};
	Vector4 last_page_projection[PATCH_POOL_SIZE];
	int patches_active = 0;

	// Shader
	Ref<Shader> terrain_shader;
	Ref<ShaderMaterial> terrain_material;
	Ref<TerrainSurfaceInputs> surface_inputs;
	// Declared before the device so its non-owning callback target outlives the
	// device during reverse-order member destruction.
	TerrainStaticShadowRasterizer static_shadow_rasterizer;
	TerrainTileCacheDevice tile_cache_device;
	Vector3 tile_overlay_tint = Vector3(1.0f, 1.0f, 1.0f);

	bool built = false;

	// The terrain leg of the EffectWorld light pool: the shell hands this node
	// the shared LightScene + the frame time each light frame (the SlotShadow
	// precedent), and render_frame re-draws every patch with the <= 16 pool
	// lights its own draw list overlaps — packed into one RGBAF rows texture
	// indexed by the pool slot each patch instance carries (retail: the
	// per-light else-arm of render_terrain_sector_batch @0x6092A0 ->
	// Light_SetupTerrainProjectedPass @0x5AA830; the collect/gates/constants
	// are portable in renderer::LightScene::collect_terrain_pass_rows, see
	// docs/render/render-lighting-re.md). Godot instance uniforms carry
	// neither arrays nor samplers and this shader already spends seven of the
	// sixteen instance slots, so the rows ride a texture rather than the object
	// pass's four scalar pairs — retail draws each of the sixteen, not four.
	static constexpr int LIGHT_ROWS_PER_PATCH =
			opennova::renderer::kTerrainLightQueryLimit;
	static constexpr int LIGHT_ROWS_TEXELS = LIGHT_ROWS_PER_PATCH * 2;
	Ref<LightScene> light_scene;
	int light_time_ms = 0;
	Ref<Image> light_rows_image;
	Ref<ImageTexture> light_rows_texture;
	PackedByteArray light_rows_bytes;
	// The bytes the rows texture currently holds: the per-frame rebuild
	// uploads only when they differ (a full RGBAF texture update otherwise).
	PackedByteArray light_rows_uploaded;
	int light_rows_enabled_written = -1; // -1 unset, else the bool last pushed
	bool light_textures_bound = false;
	int light_patches_lit = 0;
	int light_rows_total = 0;
	std::vector<opennova::renderer::TerrainLightPatchBounds> light_patch_bounds;
	std::vector<opennova::renderer::TerrainLightPatchRows> light_patch_rows;
	// Per node rather than process-static: a static Ref would destruct at DLL
	// teardown after Godot's servers are gone.
	Ref<ImageTexture> light_disc_texture;
	Ref<ImageTexture> light_strip_texture;

	void _bind_light_textures();
	void _render_light_rows(const opennova::TerrainDrawList &draw_list);

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
	void _on_tile_info_changed();
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
	void set_static_shadow_placer(
			const Ref<MissionObjectPlacer> &p_placer);
	void set_static_terrain_shadow_enabled(bool p_enabled);
	bool is_static_terrain_shadow_enabled() const;
	void set_tile_cache_capture_diagnostics(bool p_enabled);
	void set_suppressed_static_shadow_bms_ids(
			const PackedInt32Array &p_bms_ids);
	PackedInt32Array get_suppressed_static_shadow_bms_ids() const;
	Ref<TerrainSurfaceInputs> get_surface_inputs() const;
	Ref<Texture2D> get_heightfield_normal_texture() const;
	Ref<Texture2D> get_tile_overlay_texture() const;
	Ref<Texture2DArray> get_tile_cache_texture() const;
	Dictionary get_tile_cache_diagnostics() const;
	bool append_terrain_scorch(int64_t p_texture_index,
			int64_t p_minimum_x_q16, int64_t p_minimum_z_q16,
			int64_t p_maximum_x_q16, int64_t p_maximum_z_q16);
	void clear_terrain_scorches();
	std::optional<opennova::TerrainTilePageBinding>
	get_tile_cache_binding_for_world_point_native(
			float p_world_x, float p_world_z);
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

	// The light-pool context for the NEXT terrain frame: the shared pool (null
	// retires the terrain leg) and the frame time the flicker reads. The
	// pipeline publishes it from render_light_frame, which runs after this
	// frame's terrain draw, so the pool state each patch re-draws with is one
	// frame old at 62 Hz — the device fold; the patch <-> slot match is exact
	// because the rows are collected against this node's own draw list.
	void set_light_context(const Ref<LightScene> &p_scene, int p_time_ms);
	// Diagnostics: patches that received at least one light row / the row
	// total in the last render_frame.
	int get_light_patches_lit() const { return light_patches_lit; }
	int get_light_rows_total() const { return light_rows_total; }

	void build();

	// The terrain frame leg (ADR 0033 R2): compile the engine patch draw list for
	// this node's viewport camera and apply it onto the instance pool. Driven
	// by GameFramePipeline through the concrete terrain leg — this node
	// no longer self-processes.
	void render_frame();

	// The frustum-surviving terrain height range of the last render_frame
	// (the water leg's g_WaterActive input); false when no terrain was in
	// view or no frame compiled.
	bool has_visible_terrain_bounds() const;
	float get_visible_terrain_min_height() const;
	float get_visible_terrain_max_height() const;

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
