#pragma once

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
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

#include <foliage/runtime.h>

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace godot {

class Image;
class NovaTerrainData;
class NovaTerrainTileInfo;

// Godot render adapter for the portable foliage runtime.
//
// The portable module owns both retail placement algorithms. This adapter owns
// only Godot-facing concerns: source Mesh extraction, terrain/editor sampler
// bindings, visible-frame input, CPU vertex expansion, and ArrayMesh uploads.
// [orig: generate_foliage_instances_0 @ 0x5ffdd0;
// Foliage_GenerateModelTileInstances @ 0x600980]
class NovaFoliageDispatcher : public Node3D {
  GDCLASS(NovaFoliageDispatcher, Node3D)

public:
  NovaFoliageDispatcher();
  ~NovaFoliageDispatcher();

  // Configure the four retail definition slots in one atomic operation.
  // Arrays are parallel; a missing definition or triangle mesh disables that
  // slot. No placeholder geometry is manufactured.
  void configure_slots(const Array &p_defs, const Array &p_meshes,
                       const Array &p_fd_textures);

  // Runtime fast path. Height, authored foliage-map, and terrain-atlas
  // projection all come directly from this resource.
  void set_terrain_data(const Ref<NovaTerrainData> &p_data);
  Ref<NovaTerrainData> get_terrain_data() const;

  // The mission .til array shared by terrain overlays and retail's foliage
  // candidate blocker.
  void set_tile_info(const Ref<NovaTerrainTileInfo> &p_info);
  Ref<NovaTerrainTileInfo> get_tile_info() const;

  // Editor atlas/colormap source. Editor height and palette sampling remains
  // live through the Callables below.
  void set_colormap_source(const Ref<NovaTerrainData> &p_data);
  Ref<NovaTerrainData> get_colormap_source() const;

  // Editor samplers: (world_x, world_z) -> height / foliage palette index.
  // Detail uses retail's flat wrapped map lookup; foliage_sampler retains the
  // sector-routed MODEL lookup and is the compatibility fallback for detail.
  void set_height_sampler(const Callable &p_sampler);
  Callable get_height_sampler() const;
  void set_detail_foliage_sampler(const Callable &p_sampler);
  Callable get_detail_foliage_sampler() const;
  void set_foliage_sampler(const Callable &p_sampler);
  Callable get_foliage_sampler() const;

  // Anchors for the distant silhouette/depth-mask tier: crouched/prone
  // infantry standing on terrain, supplied per frame by the binding (the sim's
  // stance query in the game; none in the editor preview)
  // [orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7dc2/0x5c7ded].
  void set_silhouette_anchors(const PackedVector3Array &p_anchors);
  PackedVector3Array get_silhouette_anchors() const;

  // ONED's foliage dispatcher is parented to TerrainFoliagePreview rather than
  // NovaTerrain. Supply the editor's shared derived surface inputs explicitly
  // so detail foliage uses the same height normal and mission-tile composite.
  // An explicit override may contain null textures to intentionally clear them.
  void set_surface_input_overrides(
      const Ref<Texture2D> &p_heightfield_normal,
      const Ref<Texture2D> &p_tile_overlay, const Vector3 &p_tile_overlay_tint);
  void clear_surface_input_overrides();

  // Runtime frame. Reads exact detail patch keys/distances from the parent
  // NovaTerrain::get_foliage_detail_patches_native().
  void render_frame(const Transform3D &p_camera_xform);

  // Editor frame. Builds a deterministic 16-unit preview cell set whose live
  // terrain centers are at most 42 units in 3D from the supplied camera.
  void render_preview(const Transform3D &p_camera_xform);

  void reset();
  int get_total_instances() const;
  Dictionary get_frame_stats() const;
  // Persistent configuration result, one row per retail slot. Authored slots
  // report enabled, missing_mesh, or invalid_mesh instead of failing silently.
  Array get_slot_diagnostics() const;

  // Replace a >=4x4 power-of-two RGBA8 Image with retail's complete :fd mip
  // chain, including Godot's required 2x2/1x1 terminal levels.
  static bool bake_fd_image(const Ref<Image> &p_image);

protected:
  static void _bind_methods();

private:
  struct SourceVertex {
    Vector3 position;
    Vector2 uv;
  };

  struct SourceGeometry {
    std::vector<SourceVertex> vertices;
    std::vector<int32_t> indices;
    float center_x = 0.0f;
    float center_z = 0.0f;
    float radius = 0.0f;
    bool valid = false;
  };

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
    int64_t terrain_scene_counter = 0;
    bool native_detail_source = false;
    bool preview_detail_source = false;
    bool path_blocker_available = false;
  };

  struct BatchBuilder;

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

  opennova::foliage::Runtime runtime_;
  std::array<opennova::foliage::RuntimeSlot, opennova::FOLIAGE_MAX_DEFS>
      runtime_slots_{};
  std::array<SourceGeometry, opennova::FOLIAGE_MAX_DEFS> source_geometry_{};
  std::array<Ref<Texture2D>, opennova::FOLIAGE_MAX_DEFS> fd_textures_{};
  std::unordered_map<int, uint32_t> palette_masks_;
  Array slot_diagnostics_;
  int authored_slot_count_ = 0;
  int enabled_slot_count_ = 0;
  int disabled_slot_count_ = 0;

  Ref<NovaTerrainData> terrain_data_;
  Ref<NovaTerrainTileInfo> tile_info_;
  Ref<NovaTerrainData> colormap_source_;
  Callable height_sampler_;
  Callable detail_foliage_sampler_;
  Callable foliage_sampler_;
  PackedVector3Array silhouette_anchors_;
  bool surface_input_overrides_ = false;
  Ref<Texture2D> override_heightfield_normal_;
  Ref<Texture2D> override_tile_overlay_;
  Vector3 override_tile_overlay_tint_ =
      Vector3(1.0f, 1.0f, 1.0f);

  Ref<Shader> detail_high_shader_;
  Ref<Shader> detail_low_shader_;
  Ref<Shader> silhouette_shader_;
  std::array<Ref<ShaderMaterial>, opennova::FOLIAGE_MAX_DEFS>
      detail_high_materials_{};
  std::array<Ref<ShaderMaterial>, opennova::FOLIAGE_MAX_DEFS>
      detail_low_materials_{};
  std::array<Ref<ShaderMaterial>, opennova::FOLIAGE_MAX_DEFS>
      silhouette_materials_{};
  std::unordered_map<MeshCacheKey, CachedMesh, MeshCacheKeyHash>
      detail_mesh_cache_;
  std::unordered_map<MeshCacheKey, CachedMesh, MeshCacheKeyHash>
      model_mesh_cache_;
  std::vector<MeshInstance3D *> detail_draw_pool_;
  std::vector<MeshInstance3D *> model_draw_pool_;

  FrameStats frame_stats_{};
  int64_t total_frame_calls_ = 0;
  int64_t model_wind_counter_ = 0;

  SourceGeometry _extract_source_geometry(const Ref<Mesh> &p_mesh) const;
  void _ensure_visuals();
  void _update_materials();
  MeshInstance3D *_ensure_draw_node(std::vector<MeshInstance3D *> &r_pool,
                                    size_t p_index, const String &p_prefix);
  void _hide_draw_pools();
  void _clear_meshes();
  void _on_terrain_data_changed();
  void _on_tile_info_changed();
  void _on_colormap_source_changed();

  opennova::foliage::FrameRequest
  _runtime_request(const Transform3D &p_camera_xform);
  opennova::foliage::FrameRequest
  _preview_request(const Transform3D &p_camera_xform);
  void _append_visible_anchors(opennova::foliage::FrameRequest &r_request,
                               const Transform3D &p_camera_xform);
  std::vector<opennova::foliage::DetailCell>
  _preview_cells(const Vector3 &p_camera_position) const;
  void _render_request(opennova::foliage::FrameRequest p_request,
                       const Transform3D &p_camera_xform);

  opennova::foliage::WorldSamplers _world_samplers();
  float _sample_height(float p_world_x, float p_world_z) const;
  int _sample_detail_foliage_index(int32_t p_world_x_fixed,
                                   int32_t p_world_z_fixed) const;
  int _sample_model_foliage_index(int32_t p_world_x_fixed,
                                  int32_t p_world_z_fixed) const;
  uint32_t _mask_for_palette_index(int p_index) const;
  Vector2 _terrain_uv(float p_world_x, float p_world_z) const;

  bool
  _append_detail_instance(const opennova::foliage::DetailInstance &p_instance,
                          BatchBuilder &r_batch);
  bool _append_silhouette_instance(
      const opennova::foliage::SilhouetteInstance &p_instance,
      BatchBuilder &r_batch);
  CachedMesh _build_mesh(const BatchBuilder &p_batch,
                         int64_t p_instance_count) const;
  void _erase_cache_identities(
      const std::vector<opennova::foliage::CacheIdentity> &p_identities,
      std::unordered_map<MeshCacheKey, CachedMesh, MeshCacheKeyHash> &r_cache);
};

} // namespace godot
