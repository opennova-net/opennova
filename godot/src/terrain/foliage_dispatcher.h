#pragma once

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <formats/foliage/runtime.h>
#include <runtime/renderer/foliage_frame.h>

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace godot {

class Image;
class RenderingServer;
class Terrain;
class Weather;
class TerrainData;
class TerrainTileInfo;

// Godot render applier for the portable foliage frame (ADR 0033 R2).
//
// opennova::renderer::FoliageFrameCompiler owns the whole frame compilation: the anchor
// gate, both retail placement algorithms, per-identity vertex expansion, the
// per-submission uniform state, and both wind clocks. This node applies the
// typed FoliageDrawList: source Mesh extraction at configure time, sampler
// bindings, ArrayMesh uploads for the draw list's mesh builds, retained
// scenario-instance pooling, and material binding.
// [orig: generate_foliage_instances_0 @ 0x5ffdd0;
// Foliage_GenerateModelTileInstances @ 0x600980, see docs/foliage/foliage-re.md]
class FoliageDispatcher : public Node3D {
  GDCLASS(FoliageDispatcher, Node3D)

public:
  // Diagnostic-only selection keys for the raster probes. These classify the
  // portable draw-list record itself, never a node name or shader filename.
  enum ProbeDrawSelection {
    PROBE_DRAW_ALL = 0,
    PROBE_DRAW_DETAIL_HIGH = 1,
    PROBE_DRAW_DETAIL_LOW_FAR = 2,
    PROBE_DRAW_DETAIL_AUTO = 3,
  };

  FoliageDispatcher();
  ~FoliageDispatcher();

  // Configure the four retail definition slots in one atomic operation.
  // Arrays are parallel; a missing definition or triangle mesh disables that
  // slot. No placeholder geometry is manufactured.
  void configure_slots(const Array &p_defs, const Array &p_meshes,
                       const Array &p_fd_textures);

  // The owning Terrain, wired by the game at world load. Supplies the native
  // detail-cell handoff and composed surface textures when available.
  void set_terrain(Terrain *p_terrain);
  // The weather whose oscillator ring feeds the detail sway phase (retail
  // Env_WaveOscRing[0] in Foliage_SetupVertexShaderConstants @ 0x60075e);
  // null leaves the clock term alone.
  void set_weather(Weather *p_weather);
  // Tests and raster probes pin the detail sway clock (the wall-clock
  // milliseconds behind the phase); a negative value restores the live clock.
  void set_wind_clock_override_ms(int64_t p_ms);

  // Runtime fast path. Height, authored foliage-map, and terrain-atlas
  // projection all come directly from this resource.
  void set_terrain_data(const Ref<TerrainData> &p_data);
  Ref<TerrainData> get_terrain_data() const;

  // The mission .til array shared by terrain overlays and retail's foliage
  // candidate blocker.
  void set_tile_info(const Ref<TerrainTileInfo> &p_info);
  Ref<TerrainTileInfo> get_tile_info() const;

  // Optional atlas/colormap source for callers without an owning Terrain.
  // Height and palette sampling can remain live through the Callables below.
  void set_colormap_source(const Ref<TerrainData> &p_data);
  Ref<TerrainData> get_colormap_source() const;

  // Optional samplers: (world_x, world_z) -> height / foliage palette index.
  // Detail uses retail's flat wrapped map lookup; foliage_sampler retains the
  // sector-routed MODEL lookup and is the compatibility fallback for detail.
  void set_height_sampler(const Callable &p_sampler);
  Callable get_height_sampler() const;
  void set_detail_foliage_sampler(const Callable &p_sampler);
  Callable get_detail_foliage_sampler() const;
  void set_foliage_sampler(const Callable &p_sampler);
  Callable get_foliage_sampler() const;

  // Anchors for the distant silhouette/depth-mask tier: crouched/prone
  // infantry standing on terrain, supplied per frame by the binding from the
  // simulation's stance query. Callers without entity data leave this empty.
  // [orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7dc2/0x5c7ded, see docs/foliage/foliage-re.md].
  void set_silhouette_anchors(const PackedVector3Array &p_anchors);

  // The scene sun's foliage casters (the SHADOWS_ONLY detail twins). ON by
  // default; the settings seam may disable them wholesale.
  void set_foliage_shadows_enabled(bool p_enabled);
  bool get_foliage_shadows_enabled() const;
  PackedVector3Array get_silhouette_anchors() const;

  // Runtime frame. Reads exact detail patch keys/distances from the wired
  // Terrain::get_foliage_detail_patches_native().
  void render_frame(const Transform3D &p_camera_xform);

  // Standalone frame. Builds a deterministic 16-unit preview cell set whose
  // sampled terrain centers are at most 42 units in 3D from the camera.
  void render_preview(const Transform3D &p_camera_xform);

  void reset();
  int get_total_instances() const;
  Dictionary get_frame_stats() const;
  // Device-only diagnostics for tests and live inspection. `draws` is the
  // active draw-list order; the retained RenderingServer RIDs stay opaque.
  Dictionary get_backend_report() const;
  // Immediate diagnostic control for the current retained draw list. Raster
  // probes use this after render_frame/render_preview to pin wind, optionally
  // isolate one retail pass family, and nudge its fade without discovering
  // server instances through scene children. The next compiled frame restores
  // ordinary runtime state through the normal diff applier.
  // Bound unconditionally on purpose: it is the stable diagnostic seam the
  // source-only game_probe raster probes (ADR 0041) drive, and it is
  // self-healing (every write lands in the stamp diff, so the next apply
  // restores the compiled state); gating it per build flavour would only
  // make the probes flavour-dependent.
  Dictionary apply_probe_draw_control(int p_selection, bool p_isolate,
                                      bool p_hide_selected,
                                      float p_wind_phase,
                                      float p_fade_adjust);
  // Persistent configuration result, one row per retail slot. Authored slots
  // report enabled, missing_mesh, or invalid_mesh instead of failing silently.
  Array get_slot_diagnostics() const;

  // Replace a >=4x4 power-of-two RGBA8 Image with retail's complete :fd mip
  // chain, including Godot's required 2x2/1x1 terminal levels.
  static bool bake_fd_image(const Ref<Image> &p_image);

protected:
  static void _bind_methods();
  void _notification(int p_what);

private:
  struct FrameStats {
    int64_t frame_calls = 0;
    int64_t detail_cells = 0;
    int64_t silhouette_anchors_input = 0;
    int64_t silhouette_anchors_visible = 0;
    int64_t runtime_detail_intents = 0;
    int64_t runtime_silhouette_intents = 0;
    int64_t detail_high_instances = 0;
    int64_t detail_low_instances = 0;
    int64_t silhouette_instances = 0;
    int64_t detail_vertices = 0;
    int64_t silhouette_vertices = 0;
    int64_t render_batches = 0;
    int64_t detail_cache_hits = 0;
    int64_t detail_cache_misses = 0;
    int64_t detail_cache_regenerations = 0;
    int64_t detail_cache_evictions = 0;
    int64_t detail_cache_residents = 0;
    int64_t detail_cache_submissions = 0;
    int64_t model_cache_hits = 0;
    int64_t model_cache_misses = 0;
    int64_t model_cache_regenerations = 0;
    int64_t model_cache_evictions = 0;
    int64_t model_cache_residents = 0;
    int64_t model_cache_submissions = 0;
    int64_t detail_mesh_hits = 0;
    int64_t detail_mesh_uploads = 0;
    int64_t model_mesh_hits = 0;
    int64_t model_mesh_uploads = 0;
    int64_t backend_instance_creates = 0;
    int64_t backend_scenario_writes = 0;
    int64_t backend_configuration_writes = 0;
    int64_t backend_base_writes = 0;
    int64_t backend_material_writes = 0;
    int64_t backend_material_parameter_writes = 0;
    int64_t backend_uniform_writes = 0;
    int64_t backend_visibility_writes = 0;
    int64_t terrain_scene_counter = 0;
    bool native_detail_source = false;
    bool preview_detail_source = false;
    bool path_blocker_available = false;
  };

  struct MeshCacheKey {
    uint8_t slot = 0;
    uint32_t key = 0;
    uint64_t revision = 0;

    bool operator==(const MeshCacheKey &p_other) const {
      return slot == p_other.slot && key == p_other.key &&
             revision == p_other.revision;
    }
  };

  struct MeshCacheKeyHash {
    size_t operator()(const MeshCacheKey &p_key) const noexcept {
      size_t result = std::hash<uint64_t>{}(p_key.revision);
      result ^= std::hash<uint32_t>{}(p_key.key) +
                static_cast<size_t>(0x9e3779b9U) + (result << 6U) +
                (result >> 2U);
      result ^= std::hash<uint8_t>{}(p_key.slot) +
                static_cast<size_t>(0x9e3779b9U) + (result << 6U) +
                (result >> 2U);
      return result;
    }
  };

  struct CachedMesh {
    Ref<ArrayMesh> mesh;
    int64_t instances = 0;
    int64_t vertices = 0;
  };

  opennova::renderer::FoliageFrameCompiler compiler_;
  std::array<opennova::foliage::RuntimeSlot, opennova::FOLIAGE_MAX_DEFS>
      runtime_slots_{};
  std::array<opennova::renderer::FoliageSlotGeometry, opennova::FOLIAGE_MAX_DEFS>
      source_geometry_{};
  std::array<Ref<Texture2D>, opennova::FOLIAGE_MAX_DEFS> fd_textures_{};
  std::unordered_map<int, uint32_t> palette_masks_;
  Array slot_diagnostics_;
  int authored_slot_count_ = 0;
  int enabled_slot_count_ = 0;
  int disabled_slot_count_ = 0;

  Terrain *terrain_ = nullptr;
  // The weather by instance id: the node may go before the dispatcher.
  ObjectID weather_id_;
  Weather *_weather() const;
  int64_t wind_clock_override_ms_ = -1;
  Ref<TerrainData> terrain_data_;
  Ref<TerrainTileInfo> tile_info_;
  Ref<TerrainData> colormap_source_;
  Callable height_sampler_;
  Callable detail_foliage_sampler_;
  Callable foliage_sampler_;
  PackedVector3Array silhouette_anchors_;

  Ref<Shader> detail_high_shader_;
  Ref<Shader> detail_low_shader_;
  Ref<Shader> silhouette_shader_;
  Ref<Shader> foliage_shadow_shader_;
  std::array<Ref<ShaderMaterial>, opennova::FOLIAGE_MAX_DEFS>
      detail_high_materials_{};
  std::array<Ref<ShaderMaterial>, opennova::FOLIAGE_MAX_DEFS>
      detail_low_materials_{};
  std::array<Ref<ShaderMaterial>, opennova::FOLIAGE_MAX_DEFS>
      silhouette_materials_{};
  std::array<Ref<ShaderMaterial>, opennova::FOLIAGE_MAX_DEFS>
      shadow_materials_{};
  std::unordered_map<MeshCacheKey, CachedMesh, MeshCacheKeyHash>
      detail_mesh_cache_;
  std::unordered_map<MeshCacheKey, CachedMesh, MeshCacheKeyHash>
      model_mesh_cache_;
  std::vector<RID> detail_draw_pool_;
  std::vector<RID> model_draw_pool_;
  // ADR 0043: each near-detail draw carries a SHADOWS_ONLY twin running the
  // opaque alpha-scissor variant — Godot's shadow pass skips the transparent
  // detail materials, so the twin is what the scene sun's CSM rasterizes.
  std::vector<RID> detail_shadow_pool_;
  bool foliage_shadows_enabled_ = true;
  RID draw_scenario_;
  // What each pool instance currently holds on the RenderingServer. The draw
  // list is diff-applied per slot (mesh, material, instance uniforms), so a
  // stable frame only writes fields whose compiler clock advanced; only the
  // pool tail past this frame's command count is hidden. `bound` means the
  // instance has an active mesh.
  struct DrawInstanceStamp {
    bool bound = false;
    bool visible = false;
    Ref<Mesh> mesh;
    Ref<Material> material;
    int64_t order = 0;
    int64_t submission_id = 0;
    int64_t cell_key = 0;
    int64_t revision = 0;
    int slot = 0;
    StringName pass;
    float fade = 0.0f;
    float alpha_reference = 0.0f;
    float high_pass_cutoff = 0.0f;
    float wind_phase = 0.0f;
    bool shadow_visible = false;
    bool tile_cache_ready = false;
    float tile_cache_layer = 0.0f;
    Vector4 tile_cache_projection;
  };
  std::vector<DrawInstanceStamp> detail_draw_stamps_;
  std::vector<DrawInstanceStamp> model_draw_stamps_;
  // The material inputs last written (texture RIDs + tint): _update_materials
  // writes the ~100 material parameters only when one of them changes.
  struct MaterialInputs {
    RID colormap;
    RID heightfield_normal;
    RID tile_overlay;
    RID tile_cache;
    Vector3 tile_overlay_tint;
    RID fd_textures[opennova::FOLIAGE_MAX_DEFS];
    bool operator==(const MaterialInputs &p_other) const;
  };
  MaterialInputs material_inputs_{};
  bool material_inputs_written_ = false;

  FrameStats frame_stats_{};
  int64_t total_frame_calls_ = 0;

  opennova::renderer::FoliageSlotGeometry
  _extract_source_geometry(const Ref<Mesh> &p_mesh) const;
  void _ensure_visuals();
  void _update_materials();
  // Grow one pool to cover p_index. The apply loop binds the scenario once
  // (_bind_current_scenario) and passes the server down: nothing here walks
  // the tree per draw.
  RID _ensure_draw_instance(RenderingServer *p_server,
                            std::vector<RID> &r_pool,
                            std::vector<DrawInstanceStamp> &r_stamps,
                            size_t p_index);
  RID _ensure_shadow_instance(RenderingServer *p_server, size_t p_index);
  bool _bind_current_scenario();
  void _set_draw_pool_visibility(bool p_visible);
  void _release_draw_pools();
  // Hide (and unbind) every pool instance from p_first on; earlier instances keep
  // this frame's bindings.
  void _hide_pool_tail(std::vector<RID> &r_pool,
                       std::vector<DrawInstanceStamp> &r_stamps,
                       size_t p_first);
  void _on_terrain_data_changed();
  void _on_tile_info_changed();
  void _on_colormap_source_changed();

  opennova::renderer::FoliageViewInput
  _view_input(const Transform3D &p_camera_xform) const;
  std::vector<opennova::foliage::DetailCell>
  _preview_cells(const Vector3 &p_camera_position) const;
  void _compile_and_apply(const opennova::renderer::FoliageViewInput &p_view);
  void _apply_draw_list(const opennova::renderer::FoliageDrawList &p_draw_list);
  Ref<ArrayMesh> _upload_mesh_build(const opennova::renderer::FoliageDrawList &p_draw_list,
                                    const opennova::renderer::FoliageMeshBuild &p_build) const;

  opennova::foliage::WorldSamplers _world_samplers();
  float _sample_height(float p_world_x, float p_world_z) const;
  int _sample_detail_foliage_index(int32_t p_world_x_fixed,
                                   int32_t p_world_z_fixed) const;
  int _sample_model_foliage_index(int32_t p_world_x_fixed,
                                  int32_t p_world_z_fixed) const;
  uint32_t _mask_for_palette_index(int p_index) const;
  Vector2 _terrain_uv(float p_world_x, float p_world_z) const;
  void _erase_cache_identities(
      const std::vector<opennova::foliage::CacheIdentity> &p_identities,
      std::unordered_map<MeshCacheKey, CachedMesh, MeshCacheKeyHash> &r_cache);
};

} // namespace godot

VARIANT_ENUM_CAST(godot::FoliageDispatcher::ProbeDrawSelection);
