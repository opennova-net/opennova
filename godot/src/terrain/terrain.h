#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/variant/vector4.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/static_body3d.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/cubemap.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include "env/mission_environment.h"
#include "env/weather.h"
#include "lights/light_scene.h"
#include "terrain/terrain_data.h"
#include "terrain/terrain_static_shadow_rasterizer.h"
#include "terrain/terrain_tile_cache_device.h"
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

public:
	// The terrain shader's debug coloring (u_debug_mode), in the property
	// hint's order: textures, LOD family, sector, surface normal, height.
	enum DebugMode {
		DEBUG_MODE_NORMAL = 0,
		DEBUG_MODE_LOD_COLORS = 1,
		DEBUG_MODE_SECTOR_COLORS = 2,
		DEBUG_MODE_NORMALS = 3,
		DEBUG_MODE_HEIGHTMAP = 4,
	};
	// What one step of a build came to (build_step).
	enum BuildStep {
		BUILD_STEP_MORE = 0,
		BUILD_STEP_DONE = 1,
		BUILD_STEP_FAILED = 2,
	};

private:
	Ref<TerrainData> terrain_data;
	Ref<TerrainTileInfo> tile_info_override;
	NodePath environment_path;
	NodePath weather_path;
	NodePath water_path;
	float lod_quality = 1.0f;
	// The polygon-detail setting 0..3; 3 is the max-quality target.
	int polygon_detail = 3;

	// Per-tile: one single-surface ArrayMesh per LOD level
	struct TileInfo {
		Ref<ArrayMesh> lod_meshes[8];
		Ref<ArrayMesh> flat_lod_meshes[8];
	};
	std::vector<TileInfo> tile_infos;

	// The engine-owned terrain frame (ADR 0033 R2): the scene snapshot built
	// once per load and the per-frame compiler whose draw list this node applies.
	opennova::TerrainSceneSnapshot scene_snapshot;
	opennova::TerrainFrameCompiler frame_compiler;
	// The weapon Inset pass's own traversal (render_inset_frame).
	opennova::TerrainFrameCompiler inset_frame_compiler;
	opennova::TerrainViewInput _view_input_for(Camera3D *p_camera);
	void _apply_frame_draw_list(const opennova::TerrainDrawList &draw_list);
	// False until a compile ran for the current frame's camera; the foliage
	// handoff getter returns no cells while it is down (mission teardown, no
	// active camera) rather than a stale draw list's.
	bool frame_draw_list_live = false;
	// This frame's tracked visible-terrain bounds: the compile's while the
	// terrain draws, the bounds walk alone while it is hidden; down until a
	// frame with a camera ran either.
	opennova::VisibleBounds frame_bounds;
	bool frame_bounds_live = false;
	// The Inset's own frame compiled since it opened (release_inset_frame).
	bool inset_draw_list_live = false;

	// One view's light rows: one RGBAF row per pool slot, two texels per light
	// — (position.xyz Godot world, inv_scale) then (c0, the patch's count).
	static constexpr int LIGHT_ROWS_PER_PATCH =
			opennova::renderer::kTerrainLightQueryLimit;
	static constexpr int LIGHT_ROWS_TEXELS = LIGHT_ROWS_PER_PATCH * 2;
	struct LightRows {
		Ref<Image> image;
		Ref<ImageTexture> texture;
		PackedByteArray bytes;
		// The bytes the rows texture currently holds: the per-frame rebuild
		// uploads only when they differ (a full RGBAF texture update otherwise).
		PackedByteArray uploaded;
		int enabled_written = -1; // -1 unset, else the bool last pushed
	};

	// Lightweight RenderingServer instance pool for one view's visible
	// patches. The pool size is the engine compiler's draw list budget;
	// draw-list index == pool slot == the row of the view's light rows.
	static constexpr int PATCH_POOL_SIZE = opennova::TerrainFrameCompiler::kPatchBudget;
	struct PatchPool {
		RID instances[PATCH_POOL_SIZE];
		RID last_mesh_rid[PATCH_POOL_SIZE];
		Transform3D last_transform[PATCH_POOL_SIZE];
		bool visible[PATCH_POOL_SIZE] = {};
		// The per-instance uniforms each slot holds (written on change only: an
		// instance uniform write is a RenderingServer call per patch per frame).
		bool uniforms_stamped[PATCH_POOL_SIZE] = {};
		Vector2 last_quadrant[PATCH_POOL_SIZE];
		bool last_page_ready[PATCH_POOL_SIZE] = {};
		bool last_zero_height[PATCH_POOL_SIZE] = {};
		float last_page_layer[PATCH_POOL_SIZE] = {};
		Vector4 last_page_projection[PATCH_POOL_SIZE];
		uint32_t last_layer[PATCH_POOL_SIZE] = {};
		int active = 0;
		// The visual layers the pool's authored and flat-fallback patches ride.
		uint32_t world_layer = 0;
		uint32_t flat_layer = 0;
		LightRows rows;
	};
	// The main view's pool, and the weapon Inset pass's own while it renders.
	PatchPool main_pool;
	PatchPool inset_pool;
	// One id per page sweep: each traversal (main or Inset) is its own
	// PolyTrn frame of the shared page cache.
	uint64_t page_sweep_id = 0;
	void _create_pool_instances(PatchPool &r_pool, const RID &p_material);
	void _free_pool_instances(PatchPool &r_pool);
	void _hide_pool(PatchPool &r_pool);
	void _set_pool_world_layer(PatchPool &r_pool, uint32_t p_layer);
	// `p_display_frame_start`: the main traversal also opens the display
	// frame's static-shadow planner (the Inset sweep reuses it).
	const std::vector<opennova::TerrainTilePageBinding> &_compose_pages(
			const opennova::TerrainDrawList &draw_list, bool p_display_frame_start);
	void _apply_pool(PatchPool &r_pool, const opennova::TerrainDrawList &draw_list,
			const std::vector<opennova::TerrainTilePageBinding> &pages);
	// The Inset pool's material: the main one's parameters, synced per Inset
	// frame, except its own light rows and below-water flag.
	Ref<ShaderMaterial> inset_material;
	ObjectID inset_material_source;
	std::vector<StringName> inset_synced_parameters;
	void _sync_inset_material();

	// Shader
	Ref<Shader> terrain_shader;
	Ref<ShaderMaterial> terrain_material;
	Ref<TerrainSurfaceInputs> surface_inputs;
	// Declared before the device so its non-owning callback target outlives the
	// device during reverse-order member destruction.
	TerrainStaticShadowRasterizer static_shadow_rasterizer;
	TerrainTileCacheDevice tile_cache_device;

	bool built = false;
	// A build in flight (build_begin / build_step): the units that ran of its total, and the
	// tallies its tiles add to.
	static constexpr int kBuildTailUnits = 6;
	bool building_ = false;
	int build_done_ = 0;
	int build_total_ = 0;
	int build_total_verts_ = 0;
	int build_total_indices_ = 0;

	// The terrain leg of the EffectWorld light pool: the shell hands this node
	// the shared LightScene + the frame time each light frame (the SlotShadow
	// precedent), and render_frame re-draws every patch with the <= 16 pool
	// lights its own draw list overlaps — packed into one RGBAF rows texture
	// indexed by the pool slot each patch instance carries [orig: the
	// per-light else-arm of Terrain_RenderSectorBatch @0x6092A0 ->
	// Light_SetupTerrainProjectedPass @0x5AA830; the collect/gates/constants
	// are portable in opennova::renderer::LightScene::collect_terrain_pass_rows, see
	// docs/render/render-lighting-re.md]. Godot instance uniforms carry
	// neither arrays nor samplers and this shader already spends seven of the
	// sixteen instance slots, so the rows ride a texture rather than the object
	// pass's four scalar pairs — retail draws each of the sixteen, not four.
	// Each view's pool carries its own rows (PatchPool::rows).
	Ref<LightScene> light_scene;
	int light_time_ms = 0;
	bool light_textures_bound = false;
	int light_patches_lit = 0;
	int light_rows_total = 0;
	std::vector<opennova::renderer::TerrainLightPatchBounds> light_patch_bounds;
	std::vector<opennova::renderer::TerrainLightPatchRows> light_patch_rows;
	// Per node rather than process-static: a static Ref would destruct at DLL
	// teardown after Godot's servers are gone.
	Ref<ImageTexture> light_disc_texture;
	Ref<Cubemap> light_cube_texture;

	void _bind_light_textures();
	void _ensure_light_rows(LightRows &r_rows);
	// Returns the number of patches that received a row; `r_total` the rows.
	int _render_light_rows(const opennova::TerrainDrawList &draw_list, LightRows &r_rows,
			const Ref<ShaderMaterial> &p_material, int &r_total);

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
	DebugMode debug_mode = DEBUG_MODE_NORMAL; // the shader's u_debug_mode

	bool _build_terrain_begin();
	bool _build_terrain_tile(size_t ti);
	void _load_textures();
	void _clear_derived_textures();
	void _rebuild_tile_overlay_pages();
	void _clear_terrain();
	void _hide_visible_patches();
	void _clear_patch_pool();
	void _on_terrain_changed();
	void _on_tile_info_changed();
	Ref<Shader> _load_terrain_shader();

	static void _strip_to_list(const std::vector<uint16_t>& strip,
	                           PackedInt32Array& out);


protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	Terrain();
	~Terrain();

	void set_terrain_data(const Ref<TerrainData> &p_data);
	Ref<TerrainData> get_terrain_data() const;
	// What the terrain built and draws dropped (its patches, its tile cache, its derived textures): a
	// picture with no ground (the editor's mission device, where the mission's terrain names a file the
	// project lacks: the demo round's bug 5, the old ground kept drawing). set_terrain_data leaves what a
	// finished build drew until a build of the new data takes its place. C++ only.
	void clear_built() { _clear_terrain(); }
	void set_static_shadow_placer(
			const Ref<MissionObjectPlacer> &p_placer);
	// A caster graphic's static-shadow geometry resolved ahead of the placer
	// that will name it (TerrainStaticShadowRasterizer::prepare_caster_geometry:
	// the editor's mission device does it a graphic a unit). C++ only.
	bool prepare_static_shadow_caster(const String &p_graphic, const Ref<ObjectData> &p_data) {
		return static_shadow_rasterizer.prepare_caster_geometry(p_graphic, p_data);
	}
	void set_static_terrain_shadow_enabled(bool p_enabled);
	bool is_static_terrain_shadow_enabled() const;
	void set_tile_cache_capture_diagnostics(bool p_enabled);
	void set_suppressed_static_shadow_bms_ids(
			const PackedInt32Array &p_bms_ids);
	PackedInt32Array get_suppressed_static_shadow_bms_ids() const;
	Ref<TerrainSurfaceInputs> get_surface_inputs() const;
	Ref<Texture2D> get_heightfield_normal_texture() const;
	Ref<Texture2DArray> get_tile_cache_texture() const;
	Dictionary get_tile_cache_diagnostics() const;
	bool append_terrain_scorch(int64_t p_texture_index,
			int64_t p_minimum_x_q16, int64_t p_minimum_z_q16,
			int64_t p_maximum_x_q16, int64_t p_maximum_z_q16);
	void clear_terrain_scorches();
	// Retires the cached pages a destroyed entity's Q16 bounds touch; returns
	// how many.
	int64_t invalidate_tile_cache_region(int64_t p_minimum_x_q16,
			int64_t p_minimum_z_q16, int64_t p_maximum_x_q16,
			int64_t p_maximum_z_q16);
	std::optional<opennova::TerrainTilePageBinding>
	get_tile_cache_binding_for_world_point_native(
			float p_world_x, float p_world_z);

	void set_lod_quality(float p_quality);
	float get_lod_quality() const;

	void set_tile_info_override(const Ref<TerrainTileInfo> &p_info);
	Ref<TerrainTileInfo> get_tile_info_override() const;

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
	// The same build a unit at a time (the OpenNova Editor's mission device builds a terrain over
	// its frames): build_begin drops what was built and plans the units (the scene snapshot and
	// the material, a unit per tile's meshes, the patch pool, the surface inputs' heightfield,
	// blend and detail textures, the tile cache's pages, the lights); build_step runs the next and
	// says whether more are left, the terrain is built or the build failed (no valid baked
	// terrain). build() is build_begin and every step. The node must be in the tree, as for
	// build(): its patch pool binds the scenario. False from build_begin: no loaded terrain data.
	bool build_begin();
	BuildStep build_step();
	bool is_built() const { return built; }
	// A stepped build is in flight (begun, its last unit not run). C++ only.
	bool is_building() const { return building_; }
	// The units of the build in flight, how many ran, and what the next one makes ("" none).
	int get_build_step_count() const { return build_total_; }
	int get_build_steps_done() const { return build_done_; }
	String get_build_step_label() const;

	// The terrain frame leg (ADR 0033 R2): compile the engine patch draw list for
	// this node's viewport camera and apply it onto the instance pool. Driven
	// by the GameWorld leg table through the concrete terrain leg — this node
	// no longer self-processes.
	void render_frame();

	// The frustum-surviving terrain height range of the last render_frame
	// (the water leg's water-active input), tracked whether or not the
	// terrain draws; false when no terrain was in view or no frame ran with a
	// camera.
	bool has_visible_terrain_bounds() const;
	float get_visible_terrain_min_height() const;
	float get_visible_terrain_max_height() const;

	// Debug API
	// The last compile's counters (native; the F3 Render window) and whether
	// this frame compiled at all.
	const opennova::TerrainFrameDebugCounters &get_frame_debug_counters_native() const {
		return frame_compiler.last_draw_list().debug;
	}
	bool has_frame_draw_list() const { return frame_draw_list_live; }
	int get_patches_active() const;
	int get_visible_patch_count() const;
	const std::vector<FoliageDetailPatch> &get_foliage_detail_patches_native() const;

	// The weapon Inset pass's own terrain frame over `p_camera`, after the
	// main one (runtime/terrain/terrain_frame.h carries the witness): its own
	// traversal and LOD on a second compiler, its pages swept through the
	// shared page cache, drawn by its own patch pool on INSET_VIEW with its
	// own light rows, while the main pool rides MAIN_VIEW so the Inset camera
	// never draws the main view's patches. The traversal takes the main
	// frame's indoors gate: a hidden terrain releases the Inset frame.
	void render_inset_frame(Camera3D *p_camera);
	// The Inset stopped rendering: free its pool and return the main pool to
	// WORLD.
	void release_inset_frame();
	bool is_inset_frame_live() const { return inset_draw_list_live; }
	// The Inset traversal's foliage handoff (empty while it is not live).
	const std::vector<FoliageDetailPatch> &get_inset_foliage_detail_patches_native() const;
	// Diagnostics: the visual layers of one view's visible patches, in pool
	// order (the Inset's when `p_inset`).
	PackedInt32Array get_visible_patch_layers(bool p_inset) const;

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

	void set_debug_mode(DebugMode mode);
	DebugMode get_debug_mode() const;

	// The editor's ground overlay (ADR 0046 DI-29, the mission view's Show > Surface classes and Foliage):
	// `p_image` (RGBA8; null for none) laid over the terrain from above, its north-west corner at
	// `p_rect.position` on the world's x/z plane and `p_rect.size` its span; past it `p_outside` where
	// `p_outside_on`. Each texel tints the terrain by its alpha after the fog (the shader's u_overlay); the
	// game never sets one.
	void set_ground_overlay(const Ref<Image> &p_image, const Rect2 &p_rect, const Color &p_outside,
			bool p_outside_on);
	void clear_ground_overlay();
	bool has_ground_overlay() const { return ground_overlay_on_; }

private:
	// The overlay as last set, pushed to the material as it is set and as a build makes the material.
	void _apply_ground_overlay();
	Ref<ImageTexture> ground_overlay_texture_;
	Vector4 ground_overlay_rect_;
	Color ground_overlay_outside_;
	bool ground_overlay_on_ = false;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::Terrain::DebugMode);
VARIANT_ENUM_CAST(godot::Terrain::BuildStep);
