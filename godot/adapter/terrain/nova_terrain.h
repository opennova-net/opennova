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

#include "nova_terrain_data.h"
#include <terrain/terrain_frame.h>

#include <cstdint>
#include <vector>

namespace godot {

class NovaTerrainTileInfo;
class NovaTerrainSurfaceInputs;

using FoliageDetailPatch = opennova::FoliageDetailPatch;

class NovaTerrain : public Node3D {
	GDCLASS(NovaTerrain, Node3D)

private:
	Ref<NovaTerrainData> terrain_data;
	Ref<NovaTerrainTileInfo> tile_info_override;
	NodePath environment_path;
	NodePath weather_path;
	float lod_quality = 1.0f;
	bool tile_overlay_enabled = true;

	// Per-tile: one single-surface ArrayMesh per LOD level
	struct TileInfo {
		Ref<ArrayMesh> lod_meshes[8];
	};
	std::vector<TileInfo> tile_infos;

	// The engine-owned terrain frame (ADR 0033 R2): the scene snapshot built
	// once per load and the per-frame compiler whose packet this node applies.
	opennova::TerrainSceneSnapshot scene_snapshot;
	opennova::TerrainFrameCompiler frame_compiler;
	// False until a compile ran for the current frame's camera; the foliage
	// handoff getter returns no cells while it is down (mission teardown, no
	// active camera) rather than a stale packet's.
	bool frame_packet_live = false;

	// Lightweight RenderingServer instance pool for visible patches. The pool
	// size is the engine compiler's packet budget; packet index == pool slot.
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
	Ref<NovaTerrainSurfaceInputs> surface_inputs;
	Vector3 tile_overlay_tint = Vector3(1.0f, 1.0f, 1.0f);

	bool built = false;


	// Cached node pointers — avoids per-frame get_node_or_null()
	Node *cached_env_node = nullptr;
	Node *cached_weather_node = nullptr;
	bool terrain_node_cache_valid = false;

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
	Ref<Shader> _load_terrain_shader();

	static void _strip_to_list(const std::vector<uint16_t>& strip,
	                           PackedInt32Array& out);

	Vector3 _heightmap_normal(const std::vector<uint16_t>& depth, int gx, int gz,
	                          const opennova::terrain::CoordsTaps& taps) const;

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	NovaTerrain();
	~NovaTerrain();

	void set_terrain_data(const Ref<NovaTerrainData> &p_data);
	Ref<NovaTerrainData> get_terrain_data() const;
	Ref<NovaTerrainSurfaceInputs> get_surface_inputs() const;
	Ref<Texture2D> get_heightfield_normal_texture() const;
	Ref<Texture2D> get_tile_overlay_texture() const;
	Vector3 get_tile_overlay_tint() const;

	void set_lod_quality(float p_quality);
	float get_lod_quality() const;

	void set_tile_overlay_enabled(bool p_enabled);
	bool get_tile_overlay_enabled() const;

	void set_tile_info_override(const Ref<NovaTerrainTileInfo> &p_info);
	Ref<NovaTerrainTileInfo> get_tile_info_override() const;
	void rebuild_tile_overlay();

	void set_environment_path(const NodePath& p_path);
	NodePath get_environment_path() const;

	void set_weather_path(const NodePath& p_path);
	NodePath get_weather_path() const;

	void build();

	// The terrain frame leg (ADR 0033 R2): compile the engine patch packet for
	// this node's viewport camera and apply it onto the instance pool. Driven
	// by the engine FrameDriver through the shell's terrain hook — this node
	// no longer self-processes.
	void render_frame();

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
