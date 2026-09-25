#include "terrain/foliage_dispatcher.h"
#include "util/data_format.h"

#include "render/visual_layers.h"
#include "env/weather.h"


#include "object/object_data.h"
#include "resource_index/resource_root.h"
#include "terrain/terrain.h"
#include "terrain/terrain_data.h"
#include "terrain/terrain_tile_info.h"
#include "terrain/terrain_foliage_def.h"

#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <runtime/terrain/foliage_detail_collector.h>

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/vector4.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace godot {

namespace {

constexpr float INVALID_HEIGHT_THRESHOLD = -1.0e6f;
// The witnessed detail trio (limit / cell / cap) is engine-owned:
// terrain/foliage_detail_collector.h carries the [orig] witness.
constexpr float DETAIL_DISTANCE_LIMIT = opennova::kFoliageDetailDistanceLimit;
constexpr float PREVIEW_CELL_SIZE =
		static_cast<float>(opennova::kFoliageDetailCellSize);
constexpr int PREVIEW_CELL_RADIUS = 4;
constexpr int PREVIEW_CELL_LIMIT = opennova::kFoliageDetailPatchCapacity;

bool valid_height(float p_height) {
  return std::isfinite(p_height) && p_height > INVALID_HEIGHT_THRESHOLD;
}

bool finite_vector(const Vector3 &p_value) {
  return std::isfinite(static_cast<float>(p_value.x)) &&
         std::isfinite(static_cast<float>(p_value.y)) &&
         std::isfinite(static_cast<float>(p_value.z));
}

uint32_t pack_preview_detail_key(int p_cell_min_x, int p_cell_min_z) {
  // The same packing as the runtime collector: HIGH15 = the cell's X-min,
  // LOW15 = its Z-min (engine/runtime/terrain/foliage_detail_collector.cpp
  // carries the witness).
  const uint32_t x = static_cast<uint32_t>(p_cell_min_x) & 0x7FFFu;
  const uint32_t z = static_cast<uint32_t>(p_cell_min_z) & 0x7FFFu;
  return (x << 16u) | z;
}

// The pass names and per-draw uniform names the applier writes every frame.
// Function-local statics: a godot::StringName cannot be a file-scope static
// (its constructor needs godot-cpp's interface, which is not bound when the
// DLL's static initializers run), and building them per command put three
// StringName constructions on every draw of the apply loop.
const StringName &pass_name_high() {
  static const StringName name("high");
  return name;
}

const StringName &pass_name_low() {
  static const StringName name("low");
  return name;
}

const StringName &pass_name_silhouette() {
  static const StringName name("silhouette");
  return name;
}

struct DrawUniformNames {
  StringName fade{"u_fade"};
  StringName alpha_reference{"u_alpha_ref"};
  StringName high_pass_cutoff{"u_high_pass_cutoff"};
  StringName wind_phase{"u_wind_phase"};
  StringName wind_sector_origin_z{"u_wind_sector_origin_z"};
  StringName wind_offset{"u_wind_offset"};
  StringName tile_cache_ready{"u_instance_tile_cache_ready"};
  StringName tile_cache_layer{"u_instance_tile_cache_layer"};
  StringName tile_cache_projection{"u_instance_tile_cache_projection"};
};

const DrawUniformNames &draw_uniform_names() {
  static const DrawUniformNames names;
  return names;
}

int32_t decode_foliage_cell_axis(uint32_t p_packed) {
  const int32_t value = static_cast<int32_t>(p_packed & 0x7FFFu);
  return (value & 0x4000) != 0 ? value - 0x8000 : value;
}

// The material set of one water side: 0 = the far side, 1 = the camera's.
size_t side_index(bool p_far_side) { return p_far_side ? 0u : 1u; }

// The floats of one MODEL-tier MultiMesh instance: the 3x4 transform rows
// carry the first three GridPlacementVS rows, the custom data the fold.
constexpr int kModelInstanceFloats = 16;
static_assert(sizeof(opennova::renderer::FoliageModelInstance) ==
                  kModelInstanceFloats * sizeof(float),
              "a MultiMesh TRANSFORM_3D + custom instance is the block");

Vector2 foliage_detail_cell_center(uint32_t p_cell_key) {
  const int32_t minimum_x = decode_foliage_cell_axis(p_cell_key >> 16u);
  const int32_t minimum_z = decode_foliage_cell_axis(p_cell_key);
  return Vector2(static_cast<float>(minimum_x) + 8.0f,
                 static_cast<float>(minimum_z) + 8.0f);
}

} // namespace

FoliageDispatcher::FoliageDispatcher() = default;
FoliageDispatcher::~FoliageDispatcher() {
  _release_draw_pools();
  mask_pass_.release();
  const Callable terrain_changed =
      callable_mp(this, &FoliageDispatcher::_on_terrain_data_changed);
  if (terrain_data_.is_valid() &&
      terrain_data_->is_connected("terrain_changed", terrain_changed)) {
    terrain_data_->disconnect("terrain_changed", terrain_changed);
  }
  const Callable tile_info_changed =
      callable_mp(this, &FoliageDispatcher::_on_tile_info_changed);
  if (tile_info_.is_valid() &&
      tile_info_->is_connected("changed", tile_info_changed)) {
    tile_info_->disconnect("changed", tile_info_changed);
  }
  const Callable colormap_changed =
      callable_mp(this, &FoliageDispatcher::_on_colormap_source_changed);
  if (colormap_source_.is_valid() &&
      colormap_source_->is_connected("terrain_changed", colormap_changed)) {
    colormap_source_->disconnect("terrain_changed", colormap_changed);
  }
}

void FoliageDispatcher::_bind_methods() {
  ClassDB::bind_method(
      D_METHOD("configure_slots", "defs", "meshes", "fd_textures"),
      &FoliageDispatcher::configure_slots);
  ClassDB::bind_method(D_METHOD("set_terrain", "terrain"),
                       &FoliageDispatcher::set_terrain);
  ClassDB::bind_method(D_METHOD("set_wind_clock_override_ms", "ms"),
                       &FoliageDispatcher::set_wind_clock_override_ms);
  ClassDB::bind_method(D_METHOD("set_thermal_view", "thermal"),
                       &FoliageDispatcher::set_thermal_view);
  ClassDB::bind_method(D_METHOD("is_thermal_view"),
                       &FoliageDispatcher::is_thermal_view);
  ClassDB::bind_method(D_METHOD("set_water_height", "height"),
                       &FoliageDispatcher::set_water_height);
  ClassDB::bind_method(D_METHOD("get_water_height"),
                       &FoliageDispatcher::get_water_height);
  ClassDB::bind_method(D_METHOD("set_terrain_data", "data"),
                       &FoliageDispatcher::set_terrain_data);
  ClassDB::bind_method(D_METHOD("get_terrain_data"),
                       &FoliageDispatcher::get_terrain_data);
  ClassDB::bind_method(D_METHOD("set_tile_info", "tile_info"),
                       &FoliageDispatcher::set_tile_info);
  ClassDB::bind_method(D_METHOD("get_tile_info"),
                       &FoliageDispatcher::get_tile_info);
  ClassDB::bind_method(D_METHOD("set_colormap_source", "data"),
                       &FoliageDispatcher::set_colormap_source);
  ClassDB::bind_method(D_METHOD("get_colormap_source"),
                       &FoliageDispatcher::get_colormap_source);
  ClassDB::bind_method(D_METHOD("set_height_sampler", "sampler"),
                       &FoliageDispatcher::set_height_sampler);
  ClassDB::bind_method(D_METHOD("get_height_sampler"),
                       &FoliageDispatcher::get_height_sampler);
  ClassDB::bind_method(D_METHOD("set_detail_foliage_sampler", "sampler"),
                       &FoliageDispatcher::set_detail_foliage_sampler);
  ClassDB::bind_method(D_METHOD("get_detail_foliage_sampler"),
                       &FoliageDispatcher::get_detail_foliage_sampler);
  ClassDB::bind_method(D_METHOD("set_foliage_sampler", "sampler"),
                       &FoliageDispatcher::set_foliage_sampler);
  ClassDB::bind_method(D_METHOD("get_foliage_sampler"),
                       &FoliageDispatcher::get_foliage_sampler);
  ClassDB::bind_method(D_METHOD("set_silhouette_anchors", "anchors"),
                       &FoliageDispatcher::set_silhouette_anchors);
  ClassDB::bind_method(D_METHOD("get_silhouette_anchors"),
                       &FoliageDispatcher::get_silhouette_anchors);
  ClassDB::bind_method(D_METHOD("render_frame", "camera_xform", "time_ms"),
                       &FoliageDispatcher::render_frame);
  ClassDB::bind_method(D_METHOD("render_preview", "camera_xform", "time_ms"),
                       &FoliageDispatcher::render_preview);
  ClassDB::bind_method(D_METHOD("reset"), &FoliageDispatcher::reset);
  ClassDB::bind_method(D_METHOD("get_total_instances"),
                       &FoliageDispatcher::get_total_instances);
  ClassDB::bind_method(D_METHOD("get_frame_stats"),
                       &FoliageDispatcher::get_frame_stats);
  ClassDB::bind_method(D_METHOD("get_backend_report"),
                       &FoliageDispatcher::get_backend_report);
  ClassDB::bind_method(
      D_METHOD("apply_probe_draw_control", "selection", "isolate",
               "hide_selected", "wind_phase", "fade_adjust"),
      &FoliageDispatcher::apply_probe_draw_control);
  ClassDB::bind_method(D_METHOD("get_slot_diagnostics"),
                       &FoliageDispatcher::get_slot_diagnostics);
  ClassDB::bind_static_method("FoliageDispatcher",
                              D_METHOD("bake_fd_image", "image"),
                              &FoliageDispatcher::bake_fd_image);
  ClassDB::bind_method(D_METHOD("clear_asset_cache"),
                       &FoliageDispatcher::clear_asset_cache);
  ClassDB::bind_method(D_METHOD("asset_cache_entry_count"),
                       &FoliageDispatcher::asset_cache_entry_count);
  ClassDB::bind_method(
      D_METHOD("list_graphics", "resource_root", "force_refresh"),
      &FoliageDispatcher::list_graphics, DEFVAL(false));
  ClassDB::bind_method(D_METHOD("resolve_slot_meshes", "resource_root", "defs"),
                       &FoliageDispatcher::resolve_slot_meshes);
  ClassDB::bind_method(
      D_METHOD("resolve_slot_fd_textures", "resource_root", "defs"),
      &FoliageDispatcher::resolve_slot_fd_textures);
  ClassDB::bind_method(D_METHOD("load_mesh", "resource_root", "graphic"),
                       &FoliageDispatcher::load_mesh);
  ClassDB::bind_static_method("FoliageDispatcher",
                              D_METHOD("aggregate_lod0_submeshes", "submeshes"),
                              &FoliageDispatcher::aggregate_lod0_submeshes);

  ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "terrain_data",
                            PROPERTY_HINT_RESOURCE_TYPE, "TerrainData"),
               "set_terrain_data", "get_terrain_data");
  ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "tile_info",
                            PROPERTY_HINT_RESOURCE_TYPE, "TerrainTileInfo"),
               "set_tile_info", "get_tile_info");
  ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "colormap_source",
                            PROPERTY_HINT_RESOURCE_TYPE, "TerrainData"),
               "set_colormap_source", "get_colormap_source");
  ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "height_sampler"),
               "set_height_sampler", "get_height_sampler");
  ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "detail_foliage_sampler"),
               "set_detail_foliage_sampler", "get_detail_foliage_sampler");
  ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "foliage_sampler"),
               "set_foliage_sampler", "get_foliage_sampler");
  ADD_PROPERTY(
      PropertyInfo(Variant::PACKED_VECTOR3_ARRAY, "silhouette_anchors"),
      "set_silhouette_anchors", "get_silhouette_anchors");

  BIND_ENUM_CONSTANT(PROBE_DRAW_ALL);
  BIND_ENUM_CONSTANT(PROBE_DRAW_DETAIL_HIGH);
  BIND_ENUM_CONSTANT(PROBE_DRAW_DETAIL_LOW_FAR);
  BIND_ENUM_CONSTANT(PROBE_DRAW_DETAIL_AUTO);
}

void FoliageDispatcher::_notification(int p_what) {
  if (p_what == NOTIFICATION_ENTER_WORLD) {
    _bind_current_scenario();
  } else if (p_what == NOTIFICATION_EXIT_WORLD) {
    _release_draw_pools();
    mask_pass_.release();
  } else if (p_what == NOTIFICATION_VISIBILITY_CHANGED) {
    _set_draw_pool_visibility(is_visible_in_tree());
  }
}

void FoliageDispatcher::configure_slots(const Array &p_defs,
                                            const Array &p_meshes,
                                            const Array &p_fd_textures) {
  palette_masks_.fill(0u);
  slot_diagnostics_.clear();
  authored_slot_count_ = 0;
  enabled_slot_count_ = 0;
  disabled_slot_count_ = 0;
  // The defs the remap walks, indexed by slot. A slot with no renderable mesh
  // keeps an empty graphic so the remap's header-byte gate skips it, exactly
  // the shape of an unloaded retail slot.
  std::vector<opennova::FoliageDef> mask_defs(opennova::FOLIAGE_MAX_DEFS);

  for (int slot = 0; slot < opennova::FOLIAGE_MAX_DEFS; ++slot) {
    runtime_slots_[slot] = opennova::foliage::RuntimeSlot{};
    source_geometry_[slot] = opennova::renderer::FoliageSlotGeometry{};
    fd_textures_[slot].unref();

    Ref<TerrainFoliageDef> def;
    if (slot < p_defs.size()) {
      def = p_defs[slot];
    }

    Ref<Mesh> mesh;
    if (slot < p_meshes.size()) {
      mesh = p_meshes[slot];
    }
    source_geometry_[slot] = _extract_source_geometry(mesh);

    if (slot < p_fd_textures.size()) {
      fd_textures_[slot] = p_fd_textures[slot];
    }

    Dictionary diagnostic;
    diagnostic["slot"] = slot;
    diagnostic["mesh_supplied"] = mesh.is_valid();
    diagnostic["fd_texture_loaded"] = fd_textures_[slot].is_valid();
    diagnostic["source_vertices"] =
        static_cast<int64_t>(source_geometry_[slot].vertices.size());
    diagnostic["source_indices"] =
        static_cast<int64_t>(source_geometry_[slot].indices.size());

    if (def.is_null()) {
      diagnostic["graphic"] = String();
      diagnostic["match"] = PackedInt32Array();
      diagnostic["status"] = "missing_definition";
      slot_diagnostics_.append(diagnostic);
      continue;
    }

    ++authored_slot_count_;
    diagnostic["graphic"] = def->get_graphic();
    diagnostic["match"] = def->get_match();
    if (mesh.is_null()) {
      ++disabled_slot_count_;
      diagnostic["status"] = "missing_mesh";
      slot_diagnostics_.append(diagnostic);
      continue;
    }
    if (!source_geometry_[slot].valid) {
      ++disabled_slot_count_;
      diagnostic["status"] = "invalid_mesh";
      slot_diagnostics_.append(diagnostic);
      continue;
    }

    ++enabled_slot_count_;
    diagnostic["status"] = "enabled";
    slot_diagnostics_.append(diagnostic);
    runtime_slots_[slot].enabled = true;
    runtime_slots_[slot].attrib_flags =
        static_cast<uint8_t>(def->get_attrib_flags() & 0xFF);
    runtime_slots_[slot].model_radius = source_geometry_[slot].radius;
    runtime_slots_[slot].source_vertex_count = static_cast<uint32_t>(
        std::min<size_t>(source_geometry_[slot].vertices.size(),
                         std::numeric_limits<uint32_t>::max()));
    mask_defs[static_cast<size_t>(slot)] = def->to_native();
  }

  // Pixel 0 never matches and a slot with an empty graphic is skipped; the
  // engine remap owns both gates and the OR-of-four code compare.
  for (int pixel = 0; pixel < 256; ++pixel) {
    palette_masks_[static_cast<size_t>(pixel)] =
        opennova::foliage_remap_pixel_to_def_mask(mask_defs, pixel);
  }

  compiler_.configure_slots(runtime_slots_, source_geometry_);
  reset();
}

void FoliageDispatcher::set_terrain(Terrain *p_terrain) {
  terrain_ = p_terrain;
}

void FoliageDispatcher::set_weather(Weather *p_weather) {
  weather_id_ = p_weather != nullptr ? p_weather->get_instance_id() : ObjectID();
}

void FoliageDispatcher::set_wind_clock_override_ms(int64_t p_ms) {
  wind_clock_override_ms_ = p_ms;
}

void FoliageDispatcher::set_thermal_view(bool p_thermal) {
  thermal_view_ = p_thermal;
}

void FoliageDispatcher::set_water_height(float p_height) {
  water_height_ = p_height;
}

Weather *FoliageDispatcher::_weather() const {
  if (!weather_id_.is_valid()) {
    return nullptr;
  }
  return Object::cast_to<Weather>(ObjectDB::get_instance(weather_id_));
}

void FoliageDispatcher::set_terrain_data(
    const Ref<TerrainData> &p_data) {
  if (terrain_data_ == p_data) {
    return;
  }
  const Callable changed =
      callable_mp(this, &FoliageDispatcher::_on_terrain_data_changed);
  if (terrain_data_.is_valid() &&
      terrain_data_->is_connected("terrain_changed", changed)) {
    terrain_data_->disconnect("terrain_changed", changed);
  }
  terrain_data_ = p_data;
  if (terrain_data_.is_valid()) {
    terrain_data_->connect("terrain_changed", changed);
  }
  reset();
}

Ref<TerrainData> FoliageDispatcher::get_terrain_data() const {
  return terrain_data_;
}

void FoliageDispatcher::set_tile_info(
    const Ref<TerrainTileInfo> &p_info) {
  if (tile_info_ == p_info) {
    return;
  }
  const Callable changed =
      callable_mp(this, &FoliageDispatcher::_on_tile_info_changed);
  if (tile_info_.is_valid() &&
      tile_info_->is_connected("changed", changed)) {
    tile_info_->disconnect("changed", changed);
  }
  tile_info_ = p_info;
  if (tile_info_.is_valid()) {
    tile_info_->connect("changed", changed);
  }
  reset();
}

Ref<TerrainTileInfo> FoliageDispatcher::get_tile_info() const {
  return tile_info_;
}

void FoliageDispatcher::_on_tile_info_changed() {
  reset();
}

void FoliageDispatcher::set_colormap_source(
    const Ref<TerrainData> &p_data) {
  if (colormap_source_ == p_data) {
    return;
  }
  const Callable changed =
      callable_mp(this, &FoliageDispatcher::_on_colormap_source_changed);
  if (colormap_source_.is_valid() &&
      colormap_source_->is_connected("terrain_changed", changed)) {
    colormap_source_->disconnect("terrain_changed", changed);
  }
  colormap_source_ = p_data;
  if (colormap_source_.is_valid()) {
    colormap_source_->connect("terrain_changed", changed);
  }
  reset();
}

Ref<TerrainData> FoliageDispatcher::get_colormap_source() const {
  return colormap_source_;
}

void FoliageDispatcher::_on_terrain_data_changed() { reset(); }

void FoliageDispatcher::_on_colormap_source_changed() { reset(); }

void FoliageDispatcher::set_height_sampler(const Callable &p_sampler) {
  if (height_sampler_ == p_sampler) {
    return;
  }
  height_sampler_ = p_sampler;
  reset();
}

Callable FoliageDispatcher::get_height_sampler() const {
  return height_sampler_;
}

void FoliageDispatcher::set_detail_foliage_sampler(
    const Callable &p_sampler) {
  if (detail_foliage_sampler_ == p_sampler) {
    return;
  }
  detail_foliage_sampler_ = p_sampler;
  reset();
}

Callable FoliageDispatcher::get_detail_foliage_sampler() const {
  return detail_foliage_sampler_;
}

void FoliageDispatcher::set_foliage_sampler(const Callable &p_sampler) {
  if (foliage_sampler_ == p_sampler) {
    return;
  }
  foliage_sampler_ = p_sampler;
  reset();
}

Callable FoliageDispatcher::get_foliage_sampler() const {
  return foliage_sampler_;
}

void FoliageDispatcher::set_silhouette_anchors(
    const PackedVector3Array &p_anchors) {
  silhouette_anchors_ = p_anchors;
}

PackedVector3Array FoliageDispatcher::get_silhouette_anchors() const {
  return silhouette_anchors_;
}

bool FoliageDispatcher::bake_fd_image(const Ref<Image> &p_image) {
  if (p_image.is_null() || p_image->get_format() != Image::FORMAT_RGBA8) {
    return false;
  }

  const int width = p_image->get_width();
  const int height = p_image->get_height();
  const int64_t base_byte_count =
      static_cast<int64_t>(width) * static_cast<int64_t>(height) * 4;
  PackedByteArray pixels = p_image->get_data();
  if (base_byte_count <= 0 || pixels.size() < base_byte_count) {
    return false;
  }

  opennova::foliage::Runtime runtime;
  opennova::foliage::FdMipChain chain;
  if (!runtime.build_fd_rgba_mip_chain(pixels.ptr(), width, height, chain)) {
    return false;
  }
  const PackedByteArray packed = to_packed_bytes(chain.rgba);
  const bool has_mipmaps = width > 1 || height > 1;
  p_image->set_data(width, height, has_mipmaps, Image::FORMAT_RGBA8, packed);
  return true;
}

// --- The vegetation asset resolver (the former veg_assets.gd) ---------------

void VegGraphicRow::_bind_methods() {
  ClassDB::bind_method(D_METHOD("get_basename"), &VegGraphicRow::get_basename);
  ClassDB::bind_method(D_METHOD("set_basename", "value"),
                       &VegGraphicRow::set_basename);
  ClassDB::bind_method(D_METHOD("get_model_path"),
                       &VegGraphicRow::get_model_path);
  ClassDB::bind_method(D_METHOD("set_model_path", "value"),
                       &VegGraphicRow::set_model_path);
  ADD_PROPERTY(PropertyInfo(Variant::STRING, "basename"), "set_basename",
               "get_basename");
  ADD_PROPERTY(PropertyInfo(Variant::STRING, "model_path"), "set_model_path",
               "get_model_path");
}

void FoliageDispatcher::configure_slots_from_defs(
    const Ref<ResourceRoot> &p_resource_root, const Array &p_defs) {
  check_asset_cache_epoch();
  configure_slots(p_defs, resolve_slot_meshes(p_resource_root, p_defs),
                  resolve_slot_fd_textures(p_resource_root, p_defs));
}

void FoliageDispatcher::clear_asset_cache() {
  asset_mesh_cache_.clear();
  asset_fd_texture_cache_.clear();
  asset_model_path_cache_.clear();
  asset_graphics_cache_by_root_.clear();
}

void FoliageDispatcher::check_asset_cache_epoch() {
  const uint64_t epoch = static_cast<uint64_t>(ResourceRoot::cache_epoch());
  if (epoch == asset_cache_epoch_) {
    return;
  }
  asset_cache_epoch_ = epoch;
  clear_asset_cache();
}

int FoliageDispatcher::asset_cache_entry_count() {
  check_asset_cache_epoch();
  return static_cast<int>(asset_mesh_cache_.size() +
                          asset_fd_texture_cache_.size() +
                          asset_model_path_cache_.size() +
                          asset_graphics_cache_by_root_.size());
}

TypedArray<VegGraphicRow>
FoliageDispatcher::list_graphics(const Ref<ResourceRoot> &p_resource_root,
                                 bool p_force_refresh) {
  check_asset_cache_epoch();
  if (p_resource_root.is_null() ||
      p_resource_root->get_root_dir().is_empty()) {
    return TypedArray<VegGraphicRow>();
  }
  const String key = _asset_root_key(p_resource_root);
  if (!p_force_refresh && asset_graphics_cache_by_root_.has(key)) {
    return asset_graphics_cache_by_root_[key].duplicate();
  }

  TypedArray<VegGraphicRow> out;
  HashMap<String, bool> seen;
  const Array entries = p_resource_root->list_file_entries(".3di");
  for (int64_t i = 0; i < entries.size(); ++i) {
    const Dictionary entry = entries[i];
    String model_name = entry.get("logical_name", entry.get("path", ""));
    String model_ref = entry.get("path", "");
    if (model_ref.is_empty()) {
      model_ref = model_name;
    }
    const String basename = model_name.get_file().get_basename().to_lower();
    if (!basename.contains("veg") || seen.has(basename)) {
      continue;
    }
    seen[basename] = true;
    asset_model_path_cache_[_asset_cache_key(key, basename)] = model_ref;
    Ref<VegGraphicRow> row;
    row.instantiate();
    row->set_basename(basename);
    row->set_model_path(model_ref);
    out.push_back(row);
  }
  // Sorted by basename (the former sort_custom over `a.basename < b.basename`).
  for (int64_t i = 1; i < out.size(); ++i) {
    const Ref<VegGraphicRow> key_row = out[i];
    int64_t j = i - 1;
    while (j >= 0 &&
           key_row->get_basename() < Ref<VegGraphicRow>(out[j])->get_basename()) {
      out[j + 1] = out[j];
      --j;
    }
    out[j + 1] = key_row;
  }
  asset_graphics_cache_by_root_[key] = out;
  return out.duplicate();
}

Array FoliageDispatcher::resolve_slot_meshes(
    const Ref<ResourceRoot> &p_resource_root, const Array &p_defs) {
  check_asset_cache_epoch();
  Array meshes;
  for (int64_t i = 0; i < p_defs.size(); ++i) {
    const Ref<TerrainFoliageDef> def = p_defs[i];
    if (def.is_null()) {
      meshes.push_back(Variant());
      continue;
    }
    const String graphic = def->get_graphic();
    Ref<Mesh> mesh =
        graphic.is_empty() ? Ref<Mesh>() : load_mesh(p_resource_root, graphic);
    meshes.push_back(mesh);
  }
  return meshes;
}

Array FoliageDispatcher::resolve_slot_fd_textures(
    const Ref<ResourceRoot> &p_resource_root, const Array &p_defs) {
  check_asset_cache_epoch();
  Array textures;
  for (int64_t i = 0; i < p_defs.size(); ++i) {
    const Ref<TerrainFoliageDef> def = p_defs[i];
    if (def.is_null()) {
      textures.push_back(Variant());
      continue;
    }
    const String graphic = def->get_graphic();
    textures.push_back(graphic.is_empty()
                           ? Ref<Texture2D>()
                           : load_fd_texture(p_resource_root, graphic));
  }
  return textures;
}

Ref<Texture2D>
FoliageDispatcher::load_fd_texture(const Ref<ResourceRoot> &p_resource_root,
                                   const String &p_graphic) {
  check_asset_cache_epoch();
  if (p_resource_root.is_null() ||
      p_resource_root->get_root_dir().is_empty()) {
    return Ref<Texture2D>();
  }
  const String basename = p_graphic.get_file().get_basename().to_lower();
  if (basename.is_empty()) {
    return Ref<Texture2D>();
  }
  const String key =
      _asset_cache_key(_asset_root_key(p_resource_root), basename);
  if (asset_fd_texture_cache_.has(key)) {
    return asset_fd_texture_cache_[key];
  }

  Ref<Texture2D> diffuse =
      _mesh_albedo_texture(load_mesh(p_resource_root, p_graphic));
  if (diffuse.is_null()) {
    return Ref<Texture2D>();
  }
  Ref<Image> image = diffuse->get_image();
  if (image.is_null()) {
    return Ref<Texture2D>();
  }
  image = image->duplicate();
  if (image->is_compressed()) {
    image->decompress();
  }
  image->convert(Image::FORMAT_RGBA8);

  Ref<Texture2D> fd;
  if (bake_fd_image(image)) {
    fd = ImageTexture::create_from_image(image);
  } else {
    UtilityFunctions::push_warning(vformat(
        "FoliageDispatcher: '%s' diffuse dimensions are unsupported; :fd bake skipped, binding the raw diffuse.",
        basename));
    fd = diffuse;
  }
  asset_fd_texture_cache_[key] = fd;
  return fd;
}

Ref<Texture2D> FoliageDispatcher::_mesh_albedo_texture(const Ref<Mesh> &p_mesh) {
  if (p_mesh.is_null() || p_mesh->get_surface_count() == 0) {
    return Ref<Texture2D>();
  }
  Ref<BaseMaterial3D> material = p_mesh->surface_get_material(0);
  if (material.is_null()) {
    return Ref<Texture2D>();
  }
  return material->get_texture(BaseMaterial3D::TEXTURE_ALBEDO);
}

Ref<Mesh> FoliageDispatcher::load_mesh(const Ref<ResourceRoot> &p_resource_root,
                                       const String &p_graphic) {
  check_asset_cache_epoch();
  if (p_resource_root.is_null() ||
      p_resource_root->get_root_dir().is_empty()) {
    return Ref<Mesh>();
  }
  const String key = _asset_root_key(p_resource_root);
  const String basename = p_graphic.get_file().get_basename().to_lower();
  if (basename.is_empty()) {
    return Ref<Mesh>();
  }
  const String mesh_key = _asset_cache_key(key, basename);
  if (asset_mesh_cache_.has(mesh_key)) {
    // Game_StartMission resets the logical renderer-definition registry, but
    // MainGame deliberately retains this expensive geometry cache with its
    // mounted root. A cache hit in the new mission is still a loaded shared
    // model-def node, so recreate retail's sticky foliage bit without parsing
    // or rebuilding the .3DI.
    const String cached_model_path = asset_model_path_cache_.has(mesh_key)
                                         ? asset_model_path_cache_[mesh_key]
                                         : basename + String(".3di");
    ObjectData::mark_cached_network_challenge_foliage_model(cached_model_path);
    return asset_mesh_cache_[mesh_key];
  }

  const String model_path = _find_model_path(p_resource_root, basename);
  if (model_path.is_empty()) {
    return Ref<Mesh>();
  }

  Ref<ObjectData> data;
  data.instantiate();
  // Retail marks foliage model-def nodes before it freezes the C2S 0x3D
  // renderer-definition snapshot; they remain renderable but are excluded from
  // that network page. [orig: CEffectWorld_MarkModelsDirty @0x5b2220 writes node+0x3D4 before CEffectWorld_RebuildAllModelBuffers @0x5b3a80]
  if (data->open_from_resource_root(p_resource_root, model_path, false) != OK) {
    return Ref<Mesh>();
  }
  const Array submeshes = data->build_lod_submeshes(0);
  Ref<ArrayMesh> mesh = aggregate_lod0_submeshes(submeshes);
  if (mesh.is_valid() && !submeshes.is_empty()) {
    // Retail foliage expands the complete LOD0 model, but owns one :fd
    // binding per definition. Keep the primary submesh's diffuse only as
    // that binding's locator; it does not decide which geometry survives.
    const Dictionary primary = submeshes[0];
    Ref<Texture2D> diffuse =
        _load_diffuse_texture(data, int(primary.get("material_index", 0)));
    if (diffuse.is_valid()) {
      Ref<StandardMaterial3D> mat;
      mat.instantiate();
      mat->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, diffuse);
      mat->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR);
      mat->set_alpha_scissor_threshold(0.33f);
      mat->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
      mat->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
      mat->set_texture_filter(BaseMaterial3D::TEXTURE_FILTER_LINEAR_WITH_MIPMAPS);
      mesh->surface_set_material(0, mat);
    }
  }
  if (mesh.is_valid()) {
    asset_mesh_cache_[mesh_key] = mesh;
  }
  return mesh;
}

Ref<ArrayMesh> FoliageDispatcher::aggregate_lod0_submeshes(
    const Array &p_submeshes) {
  Ref<ArrayMesh> aggregate;
  aggregate.instantiate();
  for (int64_t i = 0; i < p_submeshes.size(); ++i) {
    const Dictionary entry = p_submeshes[i];
    const Ref<Mesh> source = entry.get("mesh", Variant());
    if (source.is_null()) {
      continue;
    }
    // ArrayMesh sources carry their primitive type and surface names; an
    // engine-generated PrimitiveMesh is a nameless triangle list.
    const Ref<ArrayMesh> array_source = source;
    for (int source_surface = 0; source_surface < source->get_surface_count();
         ++source_surface) {
      const Array arrays = source->surface_get_arrays(source_surface);
      if (arrays.size() < Mesh::ARRAY_MAX) {
        continue;
      }
      aggregate->add_surface_from_arrays(
          array_source.is_valid()
              ? array_source->surface_get_primitive_type(source_surface)
              : Mesh::PRIMITIVE_TRIANGLES,
          arrays);
      const String surface_name = array_source.is_valid()
          ? array_source->surface_get_name(source_surface)
          : String();
      if (!surface_name.is_empty()) {
        aggregate->surface_set_name(aggregate->get_surface_count() - 1,
                                    surface_name);
      }
    }
  }
  return aggregate->get_surface_count() > 0 ? aggregate : Ref<ArrayMesh>();
}

Ref<Texture2D>
FoliageDispatcher::_load_diffuse_texture(const Ref<ObjectData> &p_data,
                                         int p_material_index) {
  const int array_index = p_data->find_material_array_index(p_material_index);
  if (array_index < 0) {
    return Ref<Texture2D>();
  }
  for (int want_slot : {1, 2}) {
    Ref<Texture2D> loaded =
        p_data->load_material_slot_texture(array_index, want_slot);
    if (loaded.is_valid()) {
      return loaded;
    }
  }
  return Ref<Texture2D>();
}

String FoliageDispatcher::_find_model_path(
    const Ref<ResourceRoot> &p_resource_root, const String &p_basename) {
  const String key = _asset_root_key(p_resource_root);
  const String path_key = _asset_cache_key(key, p_basename);
  if (asset_model_path_cache_.has(path_key)) {
    return asset_model_path_cache_[path_key];
  }
  list_graphics(p_resource_root);
  if (asset_model_path_cache_.has(path_key)) {
    return asset_model_path_cache_[path_key];
  }

  // Resolve through the VFS (loose or PFF). The logical name is enough: it is
  // only fed back to ObjectData.open_from_resource_root, which reads it
  // through the VFS.
  const String logical = p_basename + String(".3di");
  if (p_resource_root->has_file(logical)) {
    asset_model_path_cache_[path_key] = logical;
    return logical;
  }
  return String();
}

String FoliageDispatcher::_asset_root_key(
    const Ref<ResourceRoot> &p_resource_root) {
  // Multiple live VFS mounts may share one physical directory while selecting
  // different expansion/override chains. Include the root object identity so
  // resolved paths, meshes, and :fd textures never alias between those
  // mounts.
  return vformat(
      "%s|%d",
      p_resource_root->get_root_dir().replace("\\", "/").rstrip("/").to_lower(),
      static_cast<int64_t>(p_resource_root->get_instance_id()));
}

String FoliageDispatcher::_asset_cache_key(const String &p_root_key,
                                           const String &p_basename) {
  return vformat("%s|%s", p_root_key, p_basename);
}

opennova::renderer::FoliageSlotGeometry
FoliageDispatcher::_extract_source_geometry(const Ref<Mesh> &p_mesh) const {
  opennova::renderer::FoliageSlotGeometry result;
  if (p_mesh.is_null()) {
    return result;
  }

  Vector3 minimum(std::numeric_limits<real_t>::max(),
                  std::numeric_limits<real_t>::max(),
                  std::numeric_limits<real_t>::max());
  Vector3 maximum(std::numeric_limits<real_t>::lowest(),
                  std::numeric_limits<real_t>::lowest(),
                  std::numeric_limits<real_t>::lowest());

  for (int surface = 0; surface < p_mesh->get_surface_count(); ++surface) {
    // VegAssets supplies an ArrayMesh aggregate. Validate its public surface
    // topology before concatenating; PrimitiveMesh inputs (used by previews
    // and tests) are engine-generated triangle meshes.
    const ArrayMesh *array_mesh =
        Object::cast_to<ArrayMesh>(p_mesh.ptr());
    if (array_mesh != nullptr &&
        array_mesh->surface_get_primitive_type(surface) !=
            Mesh::PRIMITIVE_TRIANGLES) {
      continue;
    }
    const Array arrays = p_mesh->surface_get_arrays(surface);
    if (arrays.size() < Mesh::ARRAY_MAX ||
        arrays[Mesh::ARRAY_VERTEX].get_type() !=
            Variant::PACKED_VECTOR3_ARRAY) {
      continue;
    }

    const PackedVector3Array positions = arrays[Mesh::ARRAY_VERTEX];
    if (positions.is_empty()) {
      continue;
    }

    PackedVector2Array uvs;
    if (arrays[Mesh::ARRAY_TEX_UV].get_type() ==
        Variant::PACKED_VECTOR2_ARRAY) {
      uvs = arrays[Mesh::ARRAY_TEX_UV];
    }

    PackedInt32Array source_indices;
    if (arrays[Mesh::ARRAY_INDEX].get_type() == Variant::PACKED_INT32_ARRAY) {
      source_indices = arrays[Mesh::ARRAY_INDEX];
    }

    const int32_t vertex_base = static_cast<int32_t>(result.vertices.size());
    for (int vertex = 0; vertex < positions.size(); ++vertex) {
      const Vector3 position = positions[vertex];
      if (!finite_vector(position)) {
        return opennova::renderer::FoliageSlotGeometry{};
      }
      opennova::renderer::FoliageSourceVertex source_vertex;
      source_vertex.x = static_cast<float>(position.x);
      source_vertex.y = static_cast<float>(position.y);
      source_vertex.z = static_cast<float>(position.z);
      if (vertex < uvs.size()) {
        source_vertex.u = static_cast<float>(uvs[vertex].x);
        source_vertex.v = static_cast<float>(uvs[vertex].y);
      }
      result.vertices.push_back(source_vertex);
      minimum.x = std::min(minimum.x, position.x);
      minimum.y = std::min(minimum.y, position.y);
      minimum.z = std::min(minimum.z, position.z);
      maximum.x = std::max(maximum.x, position.x);
      maximum.y = std::max(maximum.y, position.y);
      maximum.z = std::max(maximum.z, position.z);
    }

    if (source_indices.is_empty()) {
      for (int index = 0; index + 2 < positions.size(); index += 3) {
        result.indices.push_back(vertex_base + index);
        result.indices.push_back(vertex_base + index + 1);
        result.indices.push_back(vertex_base + index + 2);
      }
      continue;
    }

    for (int index = 0; index + 2 < source_indices.size(); index += 3) {
      const int32_t a = source_indices[index];
      const int32_t b = source_indices[index + 1];
      const int32_t c = source_indices[index + 2];
      if (a < 0 || b < 0 || c < 0 || a >= positions.size() ||
          b >= positions.size() || c >= positions.size()) {
        continue;
      }
      result.indices.push_back(vertex_base + a);
      result.indices.push_back(vertex_base + b);
      result.indices.push_back(vertex_base + c);
    }
  }

  if (result.vertices.empty() || result.indices.empty()) {
    return opennova::renderer::FoliageSlotGeometry{};
  }

  result.center_x = static_cast<float>((minimum.x + maximum.x) * 0.5);
  result.center_z = static_cast<float>((minimum.z + maximum.z) * 0.5);
  result.radius = std::max(static_cast<float>((maximum.x - minimum.x) * 0.5),
                           static_cast<float>((maximum.z - minimum.z) * 0.5));
  result.valid = std::isfinite(result.radius) && result.radius > 1.0e-6f;
  return result;
}

void FoliageDispatcher::_ensure_visuals() {
  ResourceLoader *loader = ResourceLoader::get_singleton();
  if (detail_high_shader_.is_null() && loader != nullptr) {
    detail_high_shader_ =
        loader->load("res://shaders/foliage_detail_high.gdshader", "Shader");
  }
  if (detail_low_shader_.is_null() && loader != nullptr) {
    detail_low_shader_ =
        loader->load("res://shaders/foliage_detail_low.gdshader", "Shader");
  }
  if (silhouette_shader_.is_null() && loader != nullptr) {
    silhouette_shader_ =
        loader->load("res://shaders/foliage_silhouette.gdshader", "Shader");
  }
  auto ensure_material = [](Ref<ShaderMaterial> &r_material,
                            const Ref<Shader> &p_shader) {
    if (r_material.is_null()) {
      r_material.instantiate();
    }
    if (r_material->get_shader() != p_shader) {
      r_material->set_shader(p_shader);
    }
  };

  for (size_t side = 0; side < 2; ++side) {
    for (int slot = 0; slot < opennova::FOLIAGE_MAX_DEFS; ++slot) {
      const bool fresh = detail_high_materials_[side][slot].is_null() ||
                         detail_low_materials_[side][slot].is_null() ||
                         silhouette_materials_[side][slot].is_null();
      ensure_material(detail_high_materials_[side][slot], detail_high_shader_);
      ensure_material(detail_low_materials_[side][slot], detail_low_shader_);
      ensure_material(silhouette_materials_[side][slot], silhouette_shader_);
      if (fresh) {
        // A new material holds no parameters yet: force the next write.
        material_inputs_written_ = false;
      }
    }
  }
}

bool FoliageDispatcher::MaterialInputs::operator==(
    const MaterialInputs &p_other) const {
  if (colormap != p_other.colormap ||
      heightfield_normal != p_other.heightfield_normal ||
      tile_cache != p_other.tile_cache) {
    return false;
  }
  for (int slot = 0; slot < opennova::FOLIAGE_MAX_DEFS; ++slot) {
    if (fd_textures[slot] != p_other.fd_textures[slot]) {
      return false;
    }
  }
  return true;
}

void FoliageDispatcher::_update_materials() {
  Ref<TerrainData> color_source =
      terrain_data_.is_valid() ? terrain_data_ : colormap_source_;
  Ref<Texture2D> colormap;
  if (color_source.is_valid()) {
    colormap = color_source->get_colormap();
  }
  const bool has_colormap = colormap.is_valid();
  Ref<Texture2D> heightfield_normal;
  Ref<Texture2DArray> tile_cache;
  if (terrain_ != nullptr) {
    heightfield_normal = terrain_->get_heightfield_normal_texture();
    tile_cache = terrain_->get_tile_cache_texture();
  }
  const bool has_heightfield_normal = heightfield_normal.is_valid();
  const bool has_tile_cache = tile_cache.is_valid();

  // Steady frames write nothing: the inputs are retained textures, so their
  // identities decide whether the material parameters moved.
  MaterialInputs inputs;
  inputs.colormap = has_colormap ? colormap->get_rid() : RID();
  inputs.heightfield_normal =
      has_heightfield_normal ? heightfield_normal->get_rid() : RID();
  inputs.tile_cache = has_tile_cache ? tile_cache->get_rid() : RID();
  for (int slot = 0; slot < opennova::FOLIAGE_MAX_DEFS; ++slot) {
    inputs.fd_textures[slot] =
        fd_textures_[slot].is_valid() ? fd_textures_[slot]->get_rid() : RID();
  }
  if (material_inputs_written_ && inputs == material_inputs_) {
    return;
  }
  material_inputs_ = inputs;
  material_inputs_written_ = true;

  for (size_t side = 0; side < 2; ++side) {
  for (int slot = 0; slot < opennova::FOLIAGE_MAX_DEFS; ++slot) {
    const Ref<Texture2D> fd_texture = fd_textures_[slot];
    const bool has_fd_texture = fd_texture.is_valid();

    const Ref<ShaderMaterial> detail_materials[2] = {
        detail_high_materials_[side][slot],
        detail_low_materials_[side][slot],
    };
    for (const Ref<ShaderMaterial> &material : detail_materials) {
      if (material.is_null()) {
        continue;
      }
      material->set_shader_parameter("u_fd_texture", fd_texture);
      material->set_shader_parameter("u_has_fd_texture", has_fd_texture);
      material->set_shader_parameter("u_colormap", colormap);
      material->set_shader_parameter("u_has_colormap", has_colormap);
      material->set_shader_parameter("u_heightfield_normal",
                                     heightfield_normal);
      material->set_shader_parameter("u_has_heightfield_normal",
                                     has_heightfield_normal);
      material->set_shader_parameter("u_tile_cache", tile_cache);
      material->set_shader_parameter("u_has_tile_cache", has_tile_cache);
      frame_stats_.backend_material_parameter_writes += 8;
    }

    const Ref<ShaderMaterial> silhouette = silhouette_materials_[side][slot];
    if (silhouette.is_valid()) {
      silhouette->set_shader_parameter("u_fd_texture", fd_texture);
      silhouette->set_shader_parameter("u_has_fd_texture", has_fd_texture);
      frame_stats_.backend_material_parameter_writes += 2;
    }
  }
  }
}

void FoliageDispatcher::_refresh_model_meshes() {
  if (model_mesh_generation_ == compiler_.model_mesh_generation()) {
    return;
  }
  model_mesh_generation_ = compiler_.model_mesh_generation();
  for (int slot = 0; slot < opennova::FOLIAGE_MAX_DEFS; ++slot) {
    const opennova::renderer::FoliageSlotModelMesh &source =
        compiler_.model_mesh(slot);
    Ref<ArrayMesh> &mesh = model_meshes_[static_cast<size_t>(slot)];
    mesh.unref();
    if (!source.valid) {
      continue;
    }
    const int64_t vertex_count = static_cast<int64_t>(source.vertices.size());
    PackedVector3Array positions;
    PackedVector2Array uvs;
    PackedInt32Array indices;
    positions.resize(vertex_count);
    uvs.resize(vertex_count);
    for (int64_t i = 0; i < vertex_count; ++i) {
      const opennova::renderer::FoliageModelVertex &v =
          source.vertices[static_cast<size_t>(i)];
      positions.set(i, Vector3(v.x, v.y, v.z));
      uvs.set(i, Vector2(v.u, v.v));
    }
    indices.resize(static_cast<int64_t>(source.indices.size()));
    for (size_t i = 0; i < source.indices.size(); ++i) {
      indices.set(static_cast<int64_t>(i),
                  static_cast<int32_t>(source.indices[i]));
    }
    Array arrays;
    arrays.resize(Mesh::ARRAY_MAX);
    arrays[Mesh::ARRAY_VERTEX] = positions;
    arrays[Mesh::ARRAY_TEX_UV] = uvs;
    arrays[Mesh::ARRAY_INDEX] = indices;
    mesh.instantiate();
    mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
  }
}

RID FoliageDispatcher::_ensure_draw_instance(
    RenderingServer *p_server, std::vector<RID> &r_pool,
    std::vector<DrawInstanceStamp> &r_stamps, size_t p_index,
    bool p_model_tier) {
  if (r_stamps.size() <= p_index) {
    r_stamps.resize(p_index + 1);
  }
  // The caller bound the scenario once for the whole apply; an unbound
  // dispatcher (outside a World3D) creates nothing.
  if (p_server == nullptr || !draw_scenario_.is_valid()) {
    return RID();
  }
  RenderingServer *server = p_server;
  while (r_pool.size() <= p_index) {
    const RID instance = server->instance_create();
    ++frame_stats_.backend_instance_creates;
    server->instance_set_scenario(instance, draw_scenario_);
    ++frame_stats_.backend_scenario_writes;
    server->instance_set_transform(instance, Transform3D());
    // Fresh audit: attrib shadow (0x02) is parsed but never read, and both
    // foliage tiers are excluded from retail shadow-caster passes. They still
    // receive static model projection through retail's composed tile cache.
    server->instance_geometry_set_cast_shadows_setting(
        instance, RenderingServer::SHADOW_CASTING_SETTING_OFF);
    // The generic attenuation catcher cannot reproduce foliage-card alpha,
    // two-sided rasterization, and wind deformation without dark rectangles.
    // Foliage rides its own visual layer, admitted by every beauty camera and
    // excluded from the water mirror: retail's reflection prerender hands
    // PolyTrn a context with foliage collection OFF, so its mirror draws no
    // near-foliage patches (see docs/env/env-tod-re.md #30 and
    // visual_layers::TERRAIN_FOLIAGE).
    server->instance_set_layer_mask(instance, visual_layers::TERRAIN_FOLIAGE);
    server->instance_set_extra_visibility_margin(instance, 8.0f);
    frame_stats_.backend_configuration_writes += 4;
    server->instance_set_visible(instance, false);
    ++frame_stats_.backend_visibility_writes;
    if (p_model_tier) {
      // A MODEL draw is one MultiMesh of up to a full tile's instances: the
      // TRANSFORM_3D rows and the custom data carry the block.
      const RID multimesh = server->multimesh_create();
      server->multimesh_allocate_data(
          multimesh, opennova::foliage::kModelTileInstanceCap,
          RenderingServer::MULTIMESH_TRANSFORM_3D, false, true);
      server->multimesh_set_visible_instances(multimesh, 0);
      server->instance_set_base(instance, multimesh);
      frame_stats_.backend_configuration_writes += 2;
      ++frame_stats_.backend_base_writes;
      model_multimesh_pool_.push_back(multimesh);
    }
    r_pool.push_back(instance);
  }
  return r_pool[p_index];
}

bool FoliageDispatcher::_bind_current_scenario() {
  RID scenario;
  if (is_inside_tree()) {
    const Ref<World3D> world = get_world_3d();
    if (world.is_valid()) {
      scenario = world->get_scenario();
    }
  }
  if (scenario == draw_scenario_) {
    return scenario.is_valid();
  }
  RenderingServer *server = RenderingServer::get_singleton();
  if (server == nullptr) {
    draw_scenario_ = RID();
    return false;
  }
  const auto rebind = [&](const std::vector<RID> &p_pool) {
    for (const RID &instance : p_pool) {
      if (!instance.is_valid()) {
        continue;
      }
      server->instance_set_scenario(instance, scenario);
      ++frame_stats_.backend_scenario_writes;
    }
  };
  rebind(detail_draw_pool_);
  rebind(model_draw_pool_);
  draw_scenario_ = scenario;
  return scenario.is_valid();
}

void FoliageDispatcher::_set_draw_pool_visibility(bool p_visible) {
  RenderingServer *server = RenderingServer::get_singleton();
  if (server == nullptr) {
    return;
  }
  const auto update = [&](const std::vector<RID> &p_pool,
                          std::vector<DrawInstanceStamp> &r_stamps) {
    const size_t count = std::min(p_pool.size(), r_stamps.size());
    for (size_t index = 0; index < count; ++index) {
      DrawInstanceStamp &stamp = r_stamps[index];
      const bool visible = p_visible && stamp.bound;
      if (!p_pool[index].is_valid() || stamp.visible == visible) {
        continue;
      }
      server->instance_set_visible(p_pool[index], visible);
      ++frame_stats_.backend_visibility_writes;
      stamp.visible = visible;
    }
  };
  update(detail_draw_pool_, detail_draw_stamps_);
  update(model_draw_pool_, model_draw_stamps_);
}

void FoliageDispatcher::_release_draw_pools() {
  RenderingServer *server = RenderingServer::get_singleton();
  if (server != nullptr) {
    const auto release = [&](std::vector<RID> &r_pool) {
      for (RID &instance : r_pool) {
        if (instance.is_valid()) {
          server->free_rid(instance);
        }
        instance = RID();
      }
      r_pool.clear();
    };
    release(detail_draw_pool_);
    release(model_draw_pool_);
    release(model_multimesh_pool_);
  } else {
    detail_draw_pool_.clear();
    model_draw_pool_.clear();
    model_multimesh_pool_.clear();
  }
  detail_draw_stamps_.clear();
  model_draw_stamps_.clear();
  draw_scenario_ = RID();
}

void FoliageDispatcher::_hide_pool_tail(
    std::vector<RID> &r_pool,
    std::vector<DrawInstanceStamp> &r_stamps, size_t p_first,
    bool p_model_tier) {
  RenderingServer *server = RenderingServer::get_singleton();
  if (server == nullptr) {
    return;
  }
  for (size_t i = p_first; i < r_pool.size(); ++i) {
    const RID instance = r_pool[i];
    DrawInstanceStamp *stamp =
        i < r_stamps.size() ? &r_stamps[i] : nullptr;
    if (!instance.is_valid() || stamp == nullptr || !stamp->bound) {
      continue;
    }
    if (stamp->visible) {
      server->instance_set_visible(instance, false);
      ++frame_stats_.backend_visibility_writes;
    }
    if (!p_model_tier) {
      // Drop the draw's mesh ownership: resident meshes stay owned by the
      // caches, an evicted identity frees with its last binding. A MODEL
      // instance keeps its MultiMesh base (the slot meshes are long-lived).
      server->instance_set_base(instance, RID());
      ++frame_stats_.backend_base_writes;
    }
    *stamp = DrawInstanceStamp{};
  }
}

void FoliageDispatcher::reset() {
  compiler_.reset();
  frame_stats_ = FrameStats{};
  total_frame_calls_ = 0;
  detail_mesh_cache_.clear();
  _release_draw_pools();
  mask_pass_.clear();
}

int FoliageDispatcher::get_total_instances() const {
  const int64_t total = frame_stats_.detail_high_instances +
                        frame_stats_.detail_low_instances +
                        frame_stats_.silhouette_instances;
  return static_cast<int>(
      std::min<int64_t>(total, std::numeric_limits<int>::max()));
}

int64_t FoliageDispatcher::backend_server_writes() const {
  return frame_stats_.backend_instance_creates +
         frame_stats_.backend_scenario_writes +
         frame_stats_.backend_configuration_writes +
         frame_stats_.backend_base_writes +
         frame_stats_.backend_material_writes +
         frame_stats_.backend_material_parameter_writes +
         frame_stats_.backend_uniform_writes +
         frame_stats_.backend_visibility_writes;
}

Ref<FoliageFrameStats> FoliageDispatcher::get_frame_stats() const {
  Ref<FoliageFrameStats> stats;
  stats.instantiate();
#define FOLIAGE_FRAME_COUNTER_COPY(m_type, m_name) \
  stats->set_##m_name(frame_stats_.m_name);
  FOLIAGE_FRAME_COUNTERS(FOLIAGE_FRAME_COUNTER_COPY)
#undef FOLIAGE_FRAME_COUNTER_COPY
  stats->set_foliage_backend("rendering_server_rid");
  stats->set_backend_pool_size(static_cast<int64_t>(
      detail_draw_pool_.size() + model_draw_pool_.size()));
  int64_t backend_active_draws = 0;
  int64_t backend_visible_draws = 0;
  const auto count_draws = [&](const std::vector<DrawInstanceStamp> &p_stamps) {
    for (const DrawInstanceStamp &stamp : p_stamps) {
      if (!stamp.bound) {
        continue;
      }
      ++backend_active_draws;
      if (stamp.visible) {
        ++backend_visible_draws;
      }
    }
  };
  count_draws(detail_draw_stamps_);
  count_draws(model_draw_stamps_);
  stats->set_backend_active_draws(backend_active_draws);
  stats->set_backend_visible_draws(backend_visible_draws);
  stats->set_backend_server_writes(backend_server_writes());
  stats->set_authored_slots(authored_slot_count_);
  stats->set_enabled_slots(enabled_slot_count_);
  stats->set_disabled_slots(disabled_slot_count_);
  return stats;
}

Dictionary FoliageDispatcher::get_backend_report() const {
  Dictionary result;
  result["backend"] = "rendering_server_rid";
  result["scenario_bound"] = draw_scenario_.is_valid();
  result["detail_pool_size"] =
      static_cast<int64_t>(detail_draw_pool_.size());
  result["model_pool_size"] =
      static_cast<int64_t>(model_draw_pool_.size());
  result["pool_size"] = static_cast<int64_t>(
      detail_draw_pool_.size() + model_draw_pool_.size());

  Array draws;
  int64_t active_draws = 0;
  int64_t visible_draws = 0;
  const auto append_rows = [&](const std::vector<RID> &p_pool,
                               const std::vector<DrawInstanceStamp> &p_stamps,
                               const StringName &p_tier) {
    const size_t count = std::min(p_pool.size(), p_stamps.size());
    for (size_t index = 0; index < count; ++index) {
      const DrawInstanceStamp &stamp = p_stamps[index];
      if (!p_pool[index].is_valid() || !stamp.bound) {
        continue;
      }
      Dictionary row;
      row["order"] = stamp.order;
      row["pool_index"] = static_cast<int64_t>(index);
      row["tier"] = p_tier;
      row["pass"] = stamp.pass;
      row["slot"] = stamp.slot;
      row["submission_id"] = stamp.submission_id;
      row["cell_key"] = stamp.cell_key;
      row["revision"] = stamp.revision;
      row["visible"] = stamp.visible;
      row["mesh"] = stamp.mesh;
      row["material"] = stamp.material;
      row["fade"] = stamp.fade;
      row["alpha_reference"] = stamp.alpha_reference;
      row["high_pass_cutoff"] = stamp.high_pass_cutoff;
      row["wind_phase"] = stamp.wind_phase;
      row["wind_sector_origin_z"] = stamp.wind_sector_origin_z;
      row["far_side"] = stamp.far_side;
      row["render_priority"] = stamp.render_rung;
      row["sorting_offset"] = stamp.sorting_offset;
      row["instance_count"] = stamp.instance_count;
      row["wind_offset"] = stamp.wind_offset;
      row["tile_cache_ready"] = stamp.tile_cache_ready;
      row["tile_cache_layer"] = stamp.tile_cache_layer;
      row["tile_cache_projection"] = stamp.tile_cache_projection;
      row["casts_shadows"] = false;
      row["layer_mask"] =
          static_cast<int64_t>(visual_layers::TERRAIN_FOLIAGE);
      draws.append(row);
      ++active_draws;
      if (stamp.visible) {
        ++visible_draws;
      }
    }
  };
  append_rows(detail_draw_pool_, detail_draw_stamps_, StringName("detail"));
  append_rows(model_draw_pool_, model_draw_stamps_, pass_name_silhouette());
  result["active_draws"] = active_draws;
  result["visible_draws"] = visible_draws;
  result["draws"] = draws;

  result["instance_creates"] = frame_stats_.backend_instance_creates;
  result["scenario_writes"] = frame_stats_.backend_scenario_writes;
  result["configuration_writes"] = frame_stats_.backend_configuration_writes;
  result["base_writes"] = frame_stats_.backend_base_writes;
  result["material_writes"] = frame_stats_.backend_material_writes;
  result["material_parameter_writes"] =
      frame_stats_.backend_material_parameter_writes;
  result["uniform_writes"] = frame_stats_.backend_uniform_writes;
  result["visibility_writes"] = frame_stats_.backend_visibility_writes;
  result["server_writes"] = backend_server_writes();
  const FoliageMaskReport mask = mask_pass_.get_report();
  Dictionary mask_row;
  mask_row["callback_seen"] = mask.callback_seen;
  mask_row["status"] = String::utf8(mask.status.c_str());
  mask_row["failure"] = String::utf8(mask.failure.c_str());
  mask_row["drawn_frame_id"] = static_cast<int64_t>(mask.drawn_frame_id);
  mask_row["drawn_draws"] = mask.drawn_draws;
  mask_row["views"] = mask.views;
  mask_row["installed"] = mask.installed;
  mask_row["active"] = mask.active;
  mask_row["target_size"] = mask.target_size;
  mask_row["draws"] = mask.draws;
  mask_row["instances"] = mask.instances;
  result["mask"] = mask_row;
  return result;
}

Dictionary FoliageDispatcher::apply_probe_draw_control(
    int p_selection, bool p_isolate, bool p_hide_selected,
    float p_wind_phase, float p_fade_adjust) {
  Dictionary result;
  const bool valid_selection = p_selection >= PROBE_DRAW_ALL &&
                               p_selection <= PROBE_DRAW_DETAIL_AUTO;
  result["valid_selection"] = valid_selection;
  result["kept"] = 0;
  result["fade_min"] = 0.0f;
  result["fade_max"] = 0.0f;
  RenderingServer *server = RenderingServer::get_singleton();
  if (!valid_selection || server == nullptr || !std::isfinite(p_wind_phase) ||
      !std::isfinite(p_fade_adjust)) {
    return result;
  }

  int64_t kept = 0;
  float fade_min = std::numeric_limits<float>::infinity();
  float fade_max = -std::numeric_limits<float>::infinity();
  const auto apply = [&](const std::vector<RID> &p_pool,
                         std::vector<DrawInstanceStamp> &r_stamps,
                         bool p_detail) {
    const size_t count = std::min(p_pool.size(), r_stamps.size());
    for (size_t index = 0; index < count; ++index) {
      const RID instance = p_pool[index];
      DrawInstanceStamp &stamp = r_stamps[index];
      if (!instance.is_valid() || !stamp.bound) {
        continue;
      }
      bool matches = false;
      switch (p_selection) {
        case PROBE_DRAW_ALL:
          matches = true;
          break;
        case PROBE_DRAW_DETAIL_HIGH:
          matches = p_detail && stamp.pass == pass_name_high();
          break;
        case PROBE_DRAW_DETAIL_LOW_FAR:
          matches = p_detail && stamp.pass == pass_name_low() &&
                    stamp.high_pass_cutoff <= 0.0f;
          break;
        case PROBE_DRAW_DETAIL_AUTO:
          matches = p_detail;
          break;
      }

      const bool selected_visible = matches && stamp.visible;
      if (selected_visible) {
        ++kept;
        fade_min = std::min(fade_min, stamp.fade);
        fade_max = std::max(fade_max, stamp.fade);
        if (p_detail) {
          server->instance_geometry_set_shader_parameter(
              instance, draw_uniform_names().wind_phase, p_wind_phase);
          stamp.wind_phase = p_wind_phase;
        } else {
          // The MODEL tier's c9.x term for the pinned angle.
          stamp.wind_offset =
              opennova::renderer::foliage_model_wind_offset(p_wind_phase);
          server->instance_geometry_set_shader_parameter(
              instance, draw_uniform_names().wind_offset, stamp.wind_offset);
        }
        ++frame_stats_.backend_uniform_writes;
        if (p_detail && p_fade_adjust != 0.0f) {
          const float adjusted_fade =
              std::max(stamp.fade + p_fade_adjust, 0.0f);
          server->instance_geometry_set_shader_parameter(
              instance, draw_uniform_names().fade, adjusted_fade);
          ++frame_stats_.backend_uniform_writes;
          stamp.fade = adjusted_fade;
        }
      }

      bool desired_visible = stamp.visible;
      if (p_isolate) {
        desired_visible = selected_visible && !p_hide_selected;
      } else if (selected_visible && p_hide_selected) {
        desired_visible = false;
      }
      if (stamp.visible != desired_visible) {
        server->instance_set_visible(instance, desired_visible);
        ++frame_stats_.backend_visibility_writes;
        stamp.visible = desired_visible;
      }
    }
  };
  apply(detail_draw_pool_, detail_draw_stamps_, true);
  apply(model_draw_pool_, model_draw_stamps_, false);

  result["kept"] = kept;
  if (kept > 0) {
    result["fade_min"] = fade_min;
    result["fade_max"] = fade_max;
  }
  return result;
}

Array FoliageDispatcher::get_slot_diagnostics() const {
  return slot_diagnostics_.duplicate(true);
}

void FoliageDispatcher::render_frame(const Transform3D &p_camera_xform, int64_t p_time_ms) {
  frame_stats_ = FrameStats{};
  frame_stats_.frame_calls = ++total_frame_calls_;
  frame_stats_.native_detail_source = terrain_ != nullptr;

  opennova::renderer::FoliageViewInput view = _view_input(p_camera_xform, p_time_ms);
  if (terrain_ != nullptr) {
    const auto &patches = terrain_->get_foliage_detail_patches_native();
    view.detail_cells.reserve(patches.size());
    for (const auto &patch : patches) {
      view.detail_cells.push_back(opennova::foliage::DetailCell{
          patch.key,
          patch.distance,
          patch.max_height,
          patch.atlas_x,
          patch.atlas_z,
      });
    }
  }
  _compile_and_apply(view);
}

void FoliageDispatcher::render_preview(const Transform3D &p_camera_xform, int64_t p_time_ms) {
  frame_stats_ = FrameStats{};
  frame_stats_.frame_calls = ++total_frame_calls_;
  frame_stats_.preview_detail_source = true;

  opennova::renderer::FoliageViewInput view = _view_input(p_camera_xform, p_time_ms);
  view.detail_cells = _preview_cells(p_camera_xform.origin);
  _compile_and_apply(view);
}

opennova::renderer::FoliageViewInput
FoliageDispatcher::_view_input(const Transform3D &p_camera_xform, int64_t p_time_ms) const {
  opennova::renderer::FoliageViewInput input;
  input.cam_x = static_cast<float>(p_camera_xform.origin.x);
  input.cam_y = static_cast<float>(p_camera_xform.origin.y);
  input.cam_z = static_cast<float>(p_camera_xform.origin.z);
  // The detail sway clock (retail GetTickCount) and the weather oscillator's
  // ring slot 0; the compiler carries the witness.
  input.time_ms = static_cast<uint32_t>(
      wind_clock_override_ms_ >= 0
          ? wind_clock_override_ms_
          : p_time_ms);
  if (const Weather *weather = _weather(); weather != nullptr) {
    input.wind_osc_ring0 = weather->runtime().core().oscillator.osc_ring[0];
  }
  input.thermal_view = thermal_view_;
  input.water_height = water_height_;

  // Column-major view matrix from the camera's inverse transform (the same
  // construction Terrain feeds TerrainFrameCompiler).
  const Transform3D view = p_camera_xform.affine_inverse();
  const Basis &b = view.basis;
  const Vector3 &o = view.origin;
  input.view[0] = b[0][0]; input.view[1] = b[1][0]; input.view[2] = b[2][0]; input.view[3] = 0;
  input.view[4] = b[0][1]; input.view[5] = b[1][1]; input.view[6] = b[2][1]; input.view[7] = 0;
  input.view[8] = b[0][2]; input.view[9] = b[1][2]; input.view[10] = b[2][2]; input.view[11] = 0;
  input.view[12] = o.x; input.view[13] = o.y; input.view[14] = o.z; input.view[15] = 1;

  input.silhouette_anchors.reserve(silhouette_anchors_.size());
  for (int index = 0; index < silhouette_anchors_.size(); ++index) {
    const Vector3 anchor = silhouette_anchors_[index];
    input.silhouette_anchors.push_back({
        static_cast<float>(anchor.x),
        static_cast<float>(anchor.y),
        static_cast<float>(anchor.z),
    });
  }
  return input;
}

std::vector<opennova::foliage::DetailCell>
FoliageDispatcher::_preview_cells(const Vector3 &p_camera_position) const {
  std::vector<opennova::foliage::DetailCell> cells;
  const int camera_cell_x = static_cast<int>(
      std::floor(static_cast<float>(p_camera_position.x) / PREVIEW_CELL_SIZE));
  const int camera_cell_z = static_cast<int>(
      std::floor(static_cast<float>(p_camera_position.z) / PREVIEW_CELL_SIZE));

  for (int cell_z = camera_cell_z - PREVIEW_CELL_RADIUS;
       cell_z <= camera_cell_z + PREVIEW_CELL_RADIUS; ++cell_z) {
    for (int cell_x = camera_cell_x - PREVIEW_CELL_RADIUS;
         cell_x <= camera_cell_x + PREVIEW_CELL_RADIUS; ++cell_x) {
      const float min_x = static_cast<float>(cell_x) * PREVIEW_CELL_SIZE;
      const float min_z = static_cast<float>(cell_z) * PREVIEW_CELL_SIZE;
      const float max_x = min_x + PREVIEW_CELL_SIZE;
      const float max_z = min_z + PREVIEW_CELL_SIZE;
      const float closest_x =
          std::clamp(static_cast<float>(p_camera_position.x), min_x, max_x);
      const float closest_z =
          std::clamp(static_cast<float>(p_camera_position.z), min_z, max_z);
      const float dx = static_cast<float>(p_camera_position.x) - closest_x;
      const float dz = static_cast<float>(p_camera_position.z) - closest_z;
      const float horizontal_distance_squared = dx * dx + dz * dz;
      if (horizontal_distance_squared >
          DETAIL_DISTANCE_LIMIT * DETAIL_DISTANCE_LIMIT) {
        continue;
      }
      // Runtime's Terrain_CollectNearFoliagePatches measures the camera in
      // three dimensions against each 16-unit patch's representative terrain
      // height. The editor has no live height mipchain, so use the surface at
      // the patch center as a live approximation instead of treating every
      // camera as ground-level. This is especially important for Mission's
      // default aerial framing: XZ-only distance expanded thousands of cards.
      const float center_y =
          _sample_height((min_x + max_x) * 0.5f, (min_z + max_z) * 0.5f);
      if (!valid_height(center_y)) {
        continue;
      }
      const float dy = static_cast<float>(p_camera_position.y) - center_y;
      const float distance =
          std::sqrt(horizontal_distance_squared + dy * dy);
      if (distance > DETAIL_DISTANCE_LIMIT) {
        continue;
      }

      // The preview's one sampled height stands in for the leaf's maximum,
      // and without a sector routing its world minimum stands in for the
      // atlas minimum.
      cells.push_back(opennova::foliage::DetailCell{
          pack_preview_detail_key(static_cast<int>(min_x),
                                  static_cast<int>(min_z)),
          distance,
          center_y,
          static_cast<int32_t>(min_x),
          static_cast<int32_t>(min_z),
      });
    }
  }

  std::sort(cells.begin(), cells.end(),
            [](const opennova::foliage::DetailCell &p_left,
               const opennova::foliage::DetailCell &p_right) {
              if (p_left.camera_distance != p_right.camera_distance) {
                return p_left.camera_distance < p_right.camera_distance;
              }
              return p_left.key < p_right.key;
            });
  if (cells.size() > PREVIEW_CELL_LIMIT) {
    cells.resize(PREVIEW_CELL_LIMIT);
  }
  return cells;
}

opennova::foliage::WorldSamplers FoliageDispatcher::_world_samplers() {
  opennova::foliage::WorldSamplers world;
  world.height_at = [this](float p_world_x, float p_world_z) {
    return _sample_height(p_world_x, p_world_z);
  };
  world.detail_foliage_mask_at = [this](int32_t p_atlas_x_fixed,
                                        int32_t p_atlas_z_fixed) {
    return _mask_for_palette_index(
        _sample_detail_foliage_index(p_atlas_x_fixed, p_atlas_z_fixed));
  };
  world.model_foliage_mask_at = [this](int32_t p_world_x_fixed,
                                       int32_t p_world_z_fixed) {
    return _mask_for_palette_index(
        _sample_model_foliage_index(p_world_x_fixed, p_world_z_fixed));
  };
  frame_stats_.path_blocker_available = tile_info_.is_valid();
  world.path_blocked = [this](float p_world_x, float p_world_z, float p_radius) {
    // til_world_z_from_fixed already decodes the format's stored-negated
    // z_fixed into the terrain/Godot plane. Do not mirror the query again.
    // [orig: Foliage_PathBlockedByPlacedTile @ 0x606490, see docs/foliage/foliage-re.md]
    return tile_info_.is_valid() &&
           tile_info_->blocks_foliage(p_world_x, p_world_z, p_radius);
  };
  return world;
}

float FoliageDispatcher::_sample_height(float p_world_x,
                                            float p_world_z) const {
  if (terrain_data_.is_valid()) {
    return terrain_data_->get_height_world_bilinear(
        Vector3(p_world_x, 0.0f, p_world_z));
  }
  if (!height_sampler_.is_valid()) {
    return INVALID_HEIGHT_THRESHOLD;
  }
  Array arguments;
  arguments.push_back(p_world_x);
  arguments.push_back(p_world_z);
  const Variant result = height_sampler_.callv(arguments);
  if (result.get_type() != Variant::FLOAT &&
      result.get_type() != Variant::INT) {
    return INVALID_HEIGHT_THRESHOLD;
  }
  return static_cast<float>(static_cast<double>(result));
}

int FoliageDispatcher::_sample_detail_foliage_index(
    int32_t p_atlas_x_fixed, int32_t p_atlas_z_fixed) const {
  if (terrain_data_.is_valid()) {
    return terrain_data_->get_detail_foliage_index_fixed(
        p_atlas_x_fixed, p_atlas_z_fixed);
  }
  if (detail_foliage_sampler_.is_valid()) {
    Array arguments;
    arguments.push_back(static_cast<double>(p_atlas_x_fixed) / 65536.0);
    arguments.push_back(static_cast<double>(p_atlas_z_fixed) / 65536.0);
    return static_cast<int>(detail_foliage_sampler_.callv(arguments));
  }
  if (foliage_sampler_.is_valid()) {
    Array arguments;
    arguments.push_back(static_cast<double>(p_atlas_x_fixed) / 65536.0);
    arguments.push_back(static_cast<double>(p_atlas_z_fixed) / 65536.0);
    return static_cast<int>(foliage_sampler_.callv(arguments));
  }
  if (colormap_source_.is_valid()) {
    return colormap_source_->get_detail_foliage_index_fixed(
        p_atlas_x_fixed, p_atlas_z_fixed);
  }
  return 0;
}

int FoliageDispatcher::_sample_model_foliage_index(
    int32_t p_world_x_fixed, int32_t p_world_z_fixed) const {
  const float world_x = static_cast<float>(p_world_x_fixed) / 65536.0f;
  const float world_z = static_cast<float>(p_world_z_fixed) / 65536.0f;
  if (terrain_data_.is_valid()) {
    return terrain_data_->get_foliage_index_world(world_x, world_z);
  }
  if (foliage_sampler_.is_valid()) {
    Array arguments;
    arguments.push_back(static_cast<double>(p_world_x_fixed) / 65536.0);
    arguments.push_back(static_cast<double>(p_world_z_fixed) / 65536.0);
    return static_cast<int>(foliage_sampler_.callv(arguments));
  }
  if (colormap_source_.is_valid()) {
    return colormap_source_->get_foliage_index_world(world_x, world_z);
  }
  return 0;
}

uint32_t FoliageDispatcher::_mask_for_palette_index(int p_index) const {
  if (p_index < 0 || p_index >= static_cast<int>(palette_masks_.size())) {
    return 0u;
  }
  return palette_masks_[static_cast<size_t>(p_index)];
}

Vector2 FoliageDispatcher::_terrain_uv(float p_world_x,
                                           float p_world_z) const {
  Ref<TerrainData> source = terrain_data_.is_valid() ? terrain_data_ : colormap_source_;
  if (source.is_null()) {
    return Vector2();
  }
  const Ref<Texture2D> colormap = source->get_colormap();
  if (colormap.is_null() || colormap->get_width() <= 0 ||
      colormap->get_height() <= 0) {
    return Vector2();
  }
  const Vector2 atlas = source->world_to_runtime_source_coords(p_world_x,
                                                                p_world_z);
  if (atlas.x < 0.0f || atlas.y < 0.0f) {
    return Vector2();
  }
  return Vector2(atlas.x / static_cast<float>(colormap->get_width()),
                 atlas.y / static_cast<float>(colormap->get_height()));
}

void FoliageDispatcher::_compile_and_apply(
    const opennova::renderer::FoliageViewInput &p_view) {
  _ensure_visuals();
  _update_materials();

  opennova::renderer::FoliageExpansionSamplers expansion;
  expansion.terrain_uv_at = [this](float p_world_x, float p_world_z,
                                   float &r_u, float &r_v) {
    const Vector2 uv = _terrain_uv(p_world_x, p_world_z);
    r_u = static_cast<float>(uv.x);
    r_v = static_cast<float>(uv.y);
    return uv != Vector2();
  };

  const opennova::renderer::FoliageDrawList &draw_list =
      compiler_.compile(p_view, _world_samplers(), expansion);
  _refresh_model_meshes();
  _apply_draw_list(draw_list);
  if (is_visible_in_tree()) {
    mask_pass_.publish(this, draw_list, compiler_, fd_textures_);
  } else {
    mask_pass_.clear();
  }
}

Ref<ArrayMesh> FoliageDispatcher::_upload_mesh_build(
    const opennova::renderer::FoliageDrawList &p_draw_list,
    const opennova::renderer::FoliageMeshBuild &p_build) const {
  Ref<ArrayMesh> mesh;
  if (p_build.vertex_count == 0 || p_build.index_count == 0) {
    return mesh;
  }

  const int64_t vertex_count = static_cast<int64_t>(p_build.vertex_count);
  const int64_t index_count = static_cast<int64_t>(p_build.index_count);
  const bool detail = p_build.tier == opennova::renderer::FoliageTier::Detail;

  PackedVector3Array positions;
  PackedVector3Array normals;
  PackedVector2Array uvs;
  PackedVector2Array uv2s;
  PackedColorArray colors;
  PackedInt32Array indices;
  positions.resize(vertex_count);
  uvs.resize(vertex_count);
  uv2s.resize(vertex_count);
  colors.resize(vertex_count);
  indices.resize(index_count);
  if (detail) {
    normals.resize(vertex_count);
  }

  for (int64_t i = 0; i < vertex_count; ++i) {
    const opennova::renderer::FoliageVertex &v =
        p_draw_list.vertices[p_build.first_vertex + static_cast<size_t>(i)];
    positions.set(i, Vector3(v.x, v.y, v.z));
    uvs.set(i, Vector2(v.u, v.v));
    uv2s.set(i, Vector2(v.u2, v.v2));
    // Only the source-height bend byte is resident geometry. Fade, alpha
    // reference, and pass are per-submission state on the draw instance.
    colors.set(i, Color(v.bend, 0.0f, 0.0f, 1.0f));
    if (detail) {
      normals.set(i, Vector3(v.nx, v.ny, v.nz));
    }
  }
  for (int64_t i = 0; i < index_count; ++i) {
    indices.set(i, static_cast<int32_t>(
                       p_draw_list.indices[p_build.first_index +
                                        static_cast<size_t>(i)]));
  }

  Array arrays;
  arrays.resize(Mesh::ARRAY_MAX);
  arrays[Mesh::ARRAY_VERTEX] = positions;
  if (detail) {
    arrays[Mesh::ARRAY_NORMAL] = normals;
  }
  arrays[Mesh::ARRAY_COLOR] = colors;
  arrays[Mesh::ARRAY_TEX_UV] = uvs;
  arrays[Mesh::ARRAY_TEX_UV2] = uv2s;
  arrays[Mesh::ARRAY_INDEX] = indices;

  mesh.instantiate();
  mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
  return mesh;
}

void FoliageDispatcher::_apply_draw_list(
    const opennova::renderer::FoliageDrawList &p_draw_list) {
  // 1) Upload every detail mesh the compiler built this frame (empty builds
  // cache an empty entry so repeated submissions of a barren identity stay
  // cheap). The MODEL tier draws the slots' normalized meshes, instanced.
  for (const opennova::renderer::FoliageMeshBuild &build : p_draw_list.mesh_builds) {
    if (build.tier != opennova::renderer::FoliageTier::Detail) {
      continue;
    }
    CachedMesh entry;
    entry.mesh = _upload_mesh_build(p_draw_list, build);
    entry.instances = build.instance_count;
    entry.vertices = static_cast<int64_t>(build.vertex_count);
    detail_mesh_cache_[MeshCacheKey{build.slot, build.cell_key, build.revision}] =
        std::move(entry);
  }

  // 2) Bind the draw list's draw commands onto the pools, in draw-list order.
  // The scenario binding and tree visibility cannot change inside one apply:
  // resolve both once here instead of per command (the scenario bind is an
  // is_inside_tree + World3D Ref round-trip; visibility walks the ancestors).
  RenderingServer *server = RenderingServer::get_singleton();
  const bool scenario_bound = _bind_current_scenario();
  const bool visible = is_visible_in_tree();
  const DrawUniformNames &uniform = draw_uniform_names();
  size_t detail_draw_index = 0;
  size_t model_draw_index = 0;
  int64_t draw_order = 0;
  PackedFloat32Array model_buffer;
  for (const opennova::renderer::FoliageDrawCommand &command : p_draw_list.commands) {
    const int slot = command.slot;
    if (slot < 0 || slot >= opennova::FOLIAGE_MAX_DEFS) {
      continue;
    }
    const bool detail = command.tier == opennova::renderer::FoliageTier::Detail;
    Ref<Mesh> mesh;
    if (detail) {
      const auto found = detail_mesh_cache_.find(
          MeshCacheKey{command.slot, command.cell_key, command.revision});
      if (found != detail_mesh_cache_.end()) {
        mesh = found->second.mesh;
      }
    } else {
      mesh = model_meshes_[static_cast<size_t>(slot)];
    }
    if (mesh.is_null()) {
      // The compiler only commands identities it built or knows resident; a
      // miss means the applier's cache went out of sync with the draw_list.
      continue;
    }
    // A detail patch borrows its t1 from the terrain page the point lookup
    // finds for it; a patch with no resident page is not drawn at all (the
    // lookup's null result skips the patch's slot draw), so under a Terrain
    // there is no cold fallback. Only a terrain-less preview draws through
    // the analytic colormap. Retail Foliage_RenderDetailPatches: the lookup
    // Terrain_FindSectorPatchRT @ 0x60a1de, the null skip @ 0x60a1e6..0x60a1e8
    // to the slot loop's next iteration @ 0x60a6a2.
    bool page_ready = false;
    float page_layer = 0.0f;
    Vector4 page_projection;
    if (detail && terrain_ != nullptr) {
      const Vector2 center = foliage_detail_cell_center(command.cell_key);
      const std::optional<opennova::TerrainTilePageBinding> page =
          terrain_->get_tile_cache_binding_for_world_point_native(
              static_cast<float>(center.x), static_cast<float>(center.y));
      if (page.has_value() && page->ready) {
        const std::optional<opennova::TerrainTilePageProjection> projection =
            opennova::TerrainTileCompositionCache::page_projection(page->page);
        if (projection.has_value()) {
          page_ready = true;
          page_layer = static_cast<float>(page->layer);
          page_projection = Vector4(projection->world_origin_x,
                                    projection->world_origin_z,
                                    projection->inverse_world_span,
                                    projection->world_span);
        }
      }
      if (!page_ready) {
        continue;
      }
    }

    const size_t draw_index = detail ? detail_draw_index++ : model_draw_index++;
    std::vector<DrawInstanceStamp> &stamps =
        detail ? detail_draw_stamps_ : model_draw_stamps_;
    if (server == nullptr || !scenario_bound) {
      continue;
    }
    const RID draw = detail
                         ? _ensure_draw_instance(server, detail_draw_pool_,
                                                 stamps, draw_index, false)
                         : _ensure_draw_instance(server, model_draw_pool_,
                                                 stamps, draw_index, true);
    if (!draw.is_valid()) {
      continue;
    }
    // Diff-apply against what the server instance already holds. A stable
    // draw list avoids every base/material/visibility write; only values whose
    // portable compiler clock advanced reach the server as uniform writes.
    DrawInstanceStamp &stamp = stamps[draw_index];
    const bool fresh = !stamp.bound;
    const size_t side = side_index(command.far_side);
    Ref<ShaderMaterial> material;
    bool high = false;
    if (detail) {
      high = command.pass == opennova::foliage::DetailPass::HighAlphaTest;
      material = high ? detail_high_materials_[side][slot]
                      : detail_low_materials_[side][slot];
      if (fresh || stamp.mesh != mesh) {
        server->instance_set_base(draw, mesh->get_rid());
        ++frame_stats_.backend_base_writes;
        stamp.mesh = mesh;
      }
    } else {
      // The MultiMesh content: the slot mesh and this submission's blocks.
      // A resident cache entry's blocks only change with its revision.
      const RID multimesh = model_multimesh_pool_[draw_index];
      if (fresh || stamp.mesh != mesh) {
        server->multimesh_set_mesh(multimesh, mesh->get_rid());
        ++frame_stats_.backend_base_writes;
        stamp.mesh = mesh;
      }
      const int64_t instance_count = static_cast<int64_t>(command.instance_count);
      if (fresh || stamp.slot != slot || stamp.cell_key != int64_t(command.cell_key) ||
          stamp.revision != int64_t(command.revision) ||
          stamp.instance_count != instance_count) {
        model_buffer.resize(opennova::foliage::kModelTileInstanceCap *
                            kModelInstanceFloats);
        model_buffer.fill(0.0f);
        const int64_t copied = std::min<int64_t>(
            instance_count, opennova::foliage::kModelTileInstanceCap);
        std::memcpy(model_buffer.ptrw(),
                    p_draw_list.model_instances.data() + command.first_instance,
                    static_cast<size_t>(copied) * sizeof(float) *
                        kModelInstanceFloats);
        server->multimesh_set_buffer(multimesh, model_buffer);
        server->multimesh_set_visible_instances(multimesh,
                                                static_cast<int32_t>(copied));
        server->multimesh_set_custom_aabb(
            multimesh,
            AABB(Vector3(command.aabb_min[0], command.aabb_min[1],
                         command.aabb_min[2]),
                 Vector3(command.aabb_max[0] - command.aabb_min[0],
                         command.aabb_max[1] - command.aabb_min[1],
                         command.aabb_max[2] - command.aabb_min[2])));
        frame_stats_.backend_configuration_writes += 3;
        stamp.instance_count = instance_count;
      }
      material = silhouette_materials_[side][slot];
    }
    // The material of a side carries that side's ladder rung.
    if (material.is_valid() &&
        material->get_render_priority() != command.render_rung) {
      material->set_render_priority(command.render_rung);
      ++frame_stats_.backend_material_parameter_writes;
    }
    if (fresh || stamp.material != material) {
      server->instance_geometry_set_material_override(
          draw, material.is_valid() ? material->get_rid() : RID());
      ++frame_stats_.backend_material_writes;
      stamp.material = material;
    }
    if (fresh || stamp.sorting_offset != command.sorting_offset) {
      server->instance_set_pivot_data(draw, command.sorting_offset, true);
      ++frame_stats_.backend_configuration_writes;
      stamp.sorting_offset = command.sorting_offset;
    }
    if (detail && (fresh || stamp.fade != command.fade)) {
      server->instance_geometry_set_shader_parameter(
          draw, uniform.fade, command.fade);
      ++frame_stats_.backend_uniform_writes;
      stamp.fade = command.fade;
    }
    if (fresh || stamp.alpha_reference != command.alpha_reference) {
      server->instance_geometry_set_shader_parameter(
          draw, uniform.alpha_reference, command.alpha_reference);
      ++frame_stats_.backend_uniform_writes;
      stamp.alpha_reference = command.alpha_reference;
    }
    if (detail && (fresh || stamp.high_pass_cutoff != command.high_pass_cutoff)) {
      // The near secondary LOW draw runs under strict D3DCMP_LESS in retail;
      // the cutoff discard keeps it off every texel the HIGH pass accepted.
      // [orig: Foliage_SetupDetailSlotDraw @ 0x6008fc..0x600912, see docs/foliage/foliage-re.md]
      server->instance_geometry_set_shader_parameter(
          draw, uniform.high_pass_cutoff, command.high_pass_cutoff);
      ++frame_stats_.backend_uniform_writes;
      stamp.high_pass_cutoff = command.high_pass_cutoff;
    }
    if (detail && (fresh || stamp.wind_phase != command.wind_phase)) {
      server->instance_geometry_set_shader_parameter(
          draw, uniform.wind_phase, command.wind_phase);
      ++frame_stats_.backend_uniform_writes;
      stamp.wind_phase = command.wind_phase;
    }
    if (!detail && (fresh || stamp.wind_offset != command.wind_offset)) {
      server->instance_geometry_set_shader_parameter(
          draw, uniform.wind_offset, command.wind_offset);
      ++frame_stats_.backend_uniform_writes;
      stamp.wind_offset = command.wind_offset;
    }
    if (detail && (fresh || stamp.wind_sector_origin_z !=
                                command.wind_sector_origin_z)) {
      server->instance_geometry_set_shader_parameter(
          draw, uniform.wind_sector_origin_z, command.wind_sector_origin_z);
      ++frame_stats_.backend_uniform_writes;
      stamp.wind_sector_origin_z = command.wind_sector_origin_z;
    }
    if (detail) {
      const bool ready = page_ready;
      const float layer = page_layer;
      const Vector4 projection_row = page_projection;
      if (fresh || stamp.tile_cache_ready != ready) {
        server->instance_geometry_set_shader_parameter(
            draw, uniform.tile_cache_ready, ready);
        ++frame_stats_.backend_uniform_writes;
        stamp.tile_cache_ready = ready;
      }
      if (fresh || stamp.tile_cache_layer != layer) {
        server->instance_geometry_set_shader_parameter(
            draw, uniform.tile_cache_layer, layer);
        ++frame_stats_.backend_uniform_writes;
        stamp.tile_cache_layer = layer;
      }
      if (fresh || stamp.tile_cache_projection != projection_row) {
        server->instance_geometry_set_shader_parameter(
            draw, uniform.tile_cache_projection, projection_row);
        ++frame_stats_.backend_uniform_writes;
        stamp.tile_cache_projection = projection_row;
      }
    }
    stamp.order = draw_order++;
    stamp.submission_id = static_cast<int64_t>(command.submission_id);
    stamp.cell_key = static_cast<int64_t>(command.cell_key);
    stamp.revision = static_cast<int64_t>(command.revision);
    stamp.slot = slot;
    stamp.far_side = command.far_side;
    stamp.render_rung = command.render_rung;
    stamp.pass = detail ? (high ? pass_name_high() : pass_name_low())
                        : pass_name_silhouette();
    stamp.bound = true;
    if (stamp.visible != visible) {
      server->instance_set_visible(draw, visible);
      ++frame_stats_.backend_visibility_writes;
      stamp.visible = visible;
    }
  }
  // Pool instances past this frame's command count held the previous frame's
  // draws: hide them once (they stay hidden until rebound).
  _hide_pool_tail(detail_draw_pool_, detail_draw_stamps_, detail_draw_index,
                  false);
  _hide_pool_tail(model_draw_pool_, model_draw_stamps_, model_draw_index, true);

  // 3) A regenerated identity may still have been submitted earlier in this
  // same draw_list. Draw instances retain its Ref<ArrayMesh>; remove cache ownership
  // only after every command has consumed the frame.
  _erase_cache_identities(p_draw_list.detail_evicted, detail_mesh_cache_);

  // 4) Mirror the draw list's debug counters into the stable stats surface.
  const opennova::renderer::FoliageFrameDebugCounters &debug = p_draw_list.debug;
  frame_stats_.detail_cells = debug.detail_cells;
  frame_stats_.silhouette_anchors_input = debug.silhouette_anchors_input;
  frame_stats_.silhouette_anchors_visible = debug.silhouette_anchors_visible;
  frame_stats_.runtime_detail_intents = debug.runtime_detail_intents;
  frame_stats_.runtime_silhouette_intents = debug.runtime_silhouette_intents;
  frame_stats_.detail_high_instances = debug.detail_high_instances;
  frame_stats_.detail_low_instances = debug.detail_low_instances;
  frame_stats_.silhouette_instances = debug.silhouette_instances;
  frame_stats_.detail_vertices = debug.detail_vertices;
  frame_stats_.silhouette_vertices = debug.silhouette_vertices;
  frame_stats_.render_batches = debug.render_batches;
  frame_stats_.detail_mesh_hits = debug.detail_mesh_hits;
  frame_stats_.detail_mesh_uploads = debug.detail_mesh_uploads;
  const opennova::foliage::RuntimeStats &runtime_stats = debug.runtime;
  frame_stats_.detail_cache_hits =
      static_cast<int64_t>(runtime_stats.detail.hits);
  frame_stats_.detail_cache_misses =
      static_cast<int64_t>(runtime_stats.detail.misses);
  frame_stats_.detail_cache_regenerations =
      static_cast<int64_t>(runtime_stats.detail.regenerations);
  frame_stats_.detail_cache_evictions =
      static_cast<int64_t>(runtime_stats.detail.evictions);
  frame_stats_.detail_cache_residents =
      static_cast<int64_t>(runtime_stats.detail.residents);
  frame_stats_.detail_cache_submissions =
      static_cast<int64_t>(runtime_stats.detail.submissions);
  frame_stats_.model_cache_hits =
      static_cast<int64_t>(runtime_stats.model.hits);
  frame_stats_.model_cache_misses =
      static_cast<int64_t>(runtime_stats.model.misses);
  frame_stats_.model_cache_regenerations =
      static_cast<int64_t>(runtime_stats.model.regenerations);
  frame_stats_.model_cache_evictions =
      static_cast<int64_t>(runtime_stats.model.evictions);
  frame_stats_.model_cache_residents =
      static_cast<int64_t>(runtime_stats.model.residents);
  frame_stats_.model_cache_submissions =
      static_cast<int64_t>(runtime_stats.model.submissions);
  frame_stats_.terrain_scene_counter =
      static_cast<int64_t>(runtime_stats.terrain_scene_counter);
}

void FoliageDispatcher::_erase_cache_identities(
    const std::vector<opennova::foliage::CacheIdentity> &p_identities,
    std::unordered_map<MeshCacheKey, CachedMesh, MeshCacheKeyHash> &r_cache) {
  for (const opennova::foliage::CacheIdentity &identity : p_identities) {
    r_cache.erase(MeshCacheKey{
        identity.slot,
        identity.key,
        identity.revision,
    });
  }
}

} // namespace godot
