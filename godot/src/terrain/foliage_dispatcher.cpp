#include "terrain/foliage_dispatcher.h"

#include "terrain/terrain.h"
#include "terrain/terrain_data.h"
#include "terrain/terrain_tile_info.h"
#include "terrain/terrain_foliage_def.h"

#include <runtime/terrain/foliage_detail_collector.h>

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
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
  // The generator decodes HIGH15 as the X cell origin and LOW15 as the
  // positive-Z edge. Candidates run lowBase - localB, so a preview cell
  // [z, z+cell] stores z+cell in the low half.
  const uint32_t x = static_cast<uint32_t>(p_cell_min_x) & 0x7FFFu;
  const uint32_t z_top = static_cast<uint32_t>(
                             p_cell_min_z + opennova::kFoliageDetailCellSize) &
                         0x7FFFu;
  return (x << 16u) | z_top;
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

Vector2 foliage_detail_cell_center(uint32_t p_cell_key) {
  const int32_t minimum_x = decode_foliage_cell_axis(p_cell_key >> 16u);
  const int32_t maximum_z = decode_foliage_cell_axis(p_cell_key);
  return Vector2(static_cast<float>(minimum_x) + 8.0f,
                 static_cast<float>(maximum_z) - 8.0f);
}

} // namespace

FoliageDispatcher::FoliageDispatcher() = default;
FoliageDispatcher::~FoliageDispatcher() {
  _release_draw_pools();
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
  ClassDB::bind_method(D_METHOD("render_frame", "camera_xform"),
                       &FoliageDispatcher::render_frame);
  ClassDB::bind_method(D_METHOD("render_preview", "camera_xform"),
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
  } else if (p_what == NOTIFICATION_VISIBILITY_CHANGED) {
    _set_draw_pool_visibility(is_visible_in_tree());
  }
}

void FoliageDispatcher::configure_slots(const Array &p_defs,
                                            const Array &p_meshes,
                                            const Array &p_fd_textures) {
  palette_masks_.clear();
  slot_diagnostics_.clear();
  authored_slot_count_ = 0;
  enabled_slot_count_ = 0;
  disabled_slot_count_ = 0;

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
      diagnostic["match"] = -1;
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
    palette_masks_[def->get_match()] |= static_cast<uint32_t>(1u << slot);
  }

  compiler_.configure_slots(runtime_slots_, source_geometry_);
  reset();
}

void FoliageDispatcher::set_terrain(Terrain *p_terrain) {
  terrain_ = p_terrain;
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
  PackedByteArray packed;
  packed.resize(static_cast<int64_t>(chain.rgba.size()));
  for (int64_t index = 0; index < packed.size(); ++index) {
    packed[index] = chain.rgba[static_cast<size_t>(index)];
  }
  const bool has_mipmaps = width > 1 || height > 1;
  p_image->set_data(width, height, has_mipmaps, Image::FORMAT_RGBA8, packed);
  return true;
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

  for (int slot = 0; slot < opennova::FOLIAGE_MAX_DEFS; ++slot) {
    const bool fresh = detail_high_materials_[slot].is_null() ||
                       detail_low_materials_[slot].is_null() ||
                       silhouette_materials_[slot].is_null();
    ensure_material(detail_high_materials_[slot], detail_high_shader_);
    ensure_material(detail_low_materials_[slot], detail_low_shader_);
    ensure_material(silhouette_materials_[slot], silhouette_shader_);
    if (fresh) {
      // A new material holds no parameters yet: force the next write.
      material_inputs_written_ = false;
    }
  }
}

bool FoliageDispatcher::MaterialInputs::operator==(
    const MaterialInputs &p_other) const {
  if (colormap != p_other.colormap ||
      heightfield_normal != p_other.heightfield_normal ||
      tile_overlay != p_other.tile_overlay ||
      tile_cache != p_other.tile_cache ||
      tile_overlay_tint != p_other.tile_overlay_tint) {
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
  Ref<Texture2D> tile_overlay;
  Ref<Texture2DArray> tile_cache;
  Vector3 tile_overlay_tint(1.0f, 1.0f, 1.0f);
  if (terrain_ != nullptr) {
    heightfield_normal = terrain_->get_heightfield_normal_texture();
    tile_overlay = terrain_->get_tile_overlay_texture();
    tile_overlay_tint = terrain_->get_tile_overlay_tint();
    tile_cache = terrain_->get_tile_cache_texture();
  }
  const bool has_heightfield_normal = heightfield_normal.is_valid();
  const bool has_tile_overlay = tile_overlay.is_valid();
  const bool has_tile_cache = tile_cache.is_valid();

  // Steady frames write nothing: the inputs are retained textures + one
  // tint, so their identities decide whether the material parameters moved.
  MaterialInputs inputs;
  inputs.colormap = has_colormap ? colormap->get_rid() : RID();
  inputs.heightfield_normal =
      has_heightfield_normal ? heightfield_normal->get_rid() : RID();
  inputs.tile_overlay = has_tile_overlay ? tile_overlay->get_rid() : RID();
  inputs.tile_cache = has_tile_cache ? tile_cache->get_rid() : RID();
  inputs.tile_overlay_tint = tile_overlay_tint;
  for (int slot = 0; slot < opennova::FOLIAGE_MAX_DEFS; ++slot) {
    inputs.fd_textures[slot] =
        fd_textures_[slot].is_valid() ? fd_textures_[slot]->get_rid() : RID();
  }
  if (material_inputs_written_ && inputs == material_inputs_) {
    return;
  }
  material_inputs_ = inputs;
  material_inputs_written_ = true;

  for (int slot = 0; slot < opennova::FOLIAGE_MAX_DEFS; ++slot) {
    const Ref<Texture2D> fd_texture = fd_textures_[slot];
    const bool has_fd_texture = fd_texture.is_valid();

    const Ref<ShaderMaterial> detail_materials[2] = {
        detail_high_materials_[slot],
        detail_low_materials_[slot],
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
      material->set_shader_parameter("u_tile_overlay", tile_overlay);
      material->set_shader_parameter("u_has_tile_overlay", has_tile_overlay);
      material->set_shader_parameter("u_tile_overlay_tint",
                                     tile_overlay_tint);
      material->set_shader_parameter("u_tile_cache", tile_cache);
      material->set_shader_parameter("u_has_tile_cache", has_tile_cache);
      frame_stats_.backend_material_parameter_writes += 11;
    }

    const Ref<ShaderMaterial> silhouette = silhouette_materials_[slot];
    if (silhouette.is_valid()) {
      silhouette->set_shader_parameter("u_fd_texture", fd_texture);
      silhouette->set_shader_parameter("u_has_fd_texture", has_fd_texture);
      frame_stats_.backend_material_parameter_writes += 2;
    }
  }
}

RID FoliageDispatcher::_ensure_draw_instance(
    RenderingServer *p_server, std::vector<RID> &r_pool,
    std::vector<DrawInstanceStamp> &r_stamps, size_t p_index) {
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
    // Keep foliage on the ordinary world layer until the retail tile-cache
    // compositor (which supplies the alpha-lighting term before this pass) is
    // ported.
    server->instance_set_layer_mask(instance, 1u << 0);
    server->instance_set_extra_visibility_margin(instance, 8.0f);
    frame_stats_.backend_configuration_writes += 4;
    server->instance_set_visible(instance, false);
    ++frame_stats_.backend_visibility_writes;
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
  } else {
    detail_draw_pool_.clear();
    model_draw_pool_.clear();
  }
  detail_draw_stamps_.clear();
  model_draw_stamps_.clear();
  draw_scenario_ = RID();
}

void FoliageDispatcher::_hide_pool_tail(
    std::vector<RID> &r_pool,
    std::vector<DrawInstanceStamp> &r_stamps, size_t p_first) {
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
    // Drop the draw's mesh ownership: resident meshes stay owned by the
    // caches, an evicted identity frees with its last binding.
    server->instance_set_base(instance, RID());
    ++frame_stats_.backend_base_writes;
    *stamp = DrawInstanceStamp{};
  }
}

void FoliageDispatcher::reset() {
  compiler_.reset();
  frame_stats_ = FrameStats{};
  total_frame_calls_ = 0;
  detail_mesh_cache_.clear();
  model_mesh_cache_.clear();
  _release_draw_pools();
}

int FoliageDispatcher::get_total_instances() const {
  const int64_t total = frame_stats_.detail_high_instances +
                        frame_stats_.detail_low_instances +
                        frame_stats_.silhouette_instances;
  return static_cast<int>(
      std::min<int64_t>(total, std::numeric_limits<int>::max()));
}

Dictionary FoliageDispatcher::get_frame_stats() const {
  Dictionary result;
  result["frame_calls"] = frame_stats_.frame_calls;
  result["detail_cells"] = frame_stats_.detail_cells;
  result["silhouette_anchors_input"] = frame_stats_.silhouette_anchors_input;
  result["silhouette_anchors_visible"] =
      frame_stats_.silhouette_anchors_visible;
  result["runtime_detail_intents"] = frame_stats_.runtime_detail_intents;
  result["runtime_silhouette_intents"] =
      frame_stats_.runtime_silhouette_intents;
  result["detail_high_instances"] = frame_stats_.detail_high_instances;
  result["detail_low_instances"] = frame_stats_.detail_low_instances;
  result["silhouette_instances"] = frame_stats_.silhouette_instances;
  result["detail_vertices"] = frame_stats_.detail_vertices;
  result["silhouette_vertices"] = frame_stats_.silhouette_vertices;
  result["render_batches"] = frame_stats_.render_batches;
  result["detail_cache_hits"] = frame_stats_.detail_cache_hits;
  result["detail_cache_misses"] = frame_stats_.detail_cache_misses;
  result["detail_cache_regenerations"] =
      frame_stats_.detail_cache_regenerations;
  result["detail_cache_evictions"] = frame_stats_.detail_cache_evictions;
  result["detail_cache_residents"] = frame_stats_.detail_cache_residents;
  result["detail_cache_submissions"] = frame_stats_.detail_cache_submissions;
  result["model_cache_hits"] = frame_stats_.model_cache_hits;
  result["model_cache_misses"] = frame_stats_.model_cache_misses;
  result["model_cache_regenerations"] =
      frame_stats_.model_cache_regenerations;
  result["model_cache_evictions"] = frame_stats_.model_cache_evictions;
  result["model_cache_residents"] = frame_stats_.model_cache_residents;
  result["model_cache_submissions"] = frame_stats_.model_cache_submissions;
  result["detail_mesh_hits"] = frame_stats_.detail_mesh_hits;
  result["detail_mesh_uploads"] = frame_stats_.detail_mesh_uploads;
  result["model_mesh_hits"] = frame_stats_.model_mesh_hits;
  result["model_mesh_uploads"] = frame_stats_.model_mesh_uploads;
  result["foliage_backend"] = "rendering_server_rid";
  result["backend_pool_size"] = static_cast<int64_t>(
      detail_draw_pool_.size() + model_draw_pool_.size());
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
  result["backend_active_draws"] = backend_active_draws;
  result["backend_visible_draws"] = backend_visible_draws;
  result["backend_instance_creates"] =
      frame_stats_.backend_instance_creates;
  result["backend_scenario_writes"] =
      frame_stats_.backend_scenario_writes;
  result["backend_configuration_writes"] =
      frame_stats_.backend_configuration_writes;
  result["backend_base_writes"] = frame_stats_.backend_base_writes;
  result["backend_material_writes"] = frame_stats_.backend_material_writes;
  result["backend_material_parameter_writes"] =
      frame_stats_.backend_material_parameter_writes;
  result["backend_uniform_writes"] = frame_stats_.backend_uniform_writes;
  result["backend_visibility_writes"] =
      frame_stats_.backend_visibility_writes;
  result["backend_server_writes"] =
      frame_stats_.backend_instance_creates +
      frame_stats_.backend_scenario_writes +
      frame_stats_.backend_configuration_writes +
      frame_stats_.backend_base_writes +
      frame_stats_.backend_material_writes +
      frame_stats_.backend_material_parameter_writes +
      frame_stats_.backend_uniform_writes +
      frame_stats_.backend_visibility_writes;
  result["terrain_scene_counter"] = frame_stats_.terrain_scene_counter;
  result["native_detail_source"] = frame_stats_.native_detail_source;
  result["preview_detail_source"] = frame_stats_.preview_detail_source;
  result["path_blocker_available"] = frame_stats_.path_blocker_available;
  result["authored_slots"] = authored_slot_count_;
  result["enabled_slots"] = enabled_slot_count_;
  result["disabled_slots"] = disabled_slot_count_;
  return result;
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
      row["tile_cache_ready"] = stamp.tile_cache_ready;
      row["tile_cache_layer"] = stamp.tile_cache_layer;
      row["tile_cache_projection"] = stamp.tile_cache_projection;
      row["casts_shadows"] = false;
      row["layer_mask"] = static_cast<int64_t>(1u << 0);
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

  const Dictionary stats = get_frame_stats();
  result["instance_creates"] = stats["backend_instance_creates"];
  result["scenario_writes"] = stats["backend_scenario_writes"];
  result["configuration_writes"] = stats["backend_configuration_writes"];
  result["base_writes"] = stats["backend_base_writes"];
  result["material_writes"] = stats["backend_material_writes"];
  result["material_parameter_writes"] =
      stats["backend_material_parameter_writes"];
  result["uniform_writes"] = stats["backend_uniform_writes"];
  result["visibility_writes"] = stats["backend_visibility_writes"];
  result["server_writes"] = stats["backend_server_writes"];
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
        server->instance_geometry_set_shader_parameter(
            instance, draw_uniform_names().wind_phase, p_wind_phase);
        ++frame_stats_.backend_uniform_writes;
        stamp.wind_phase = p_wind_phase;
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

void FoliageDispatcher::render_frame(const Transform3D &p_camera_xform) {
  frame_stats_ = FrameStats{};
  frame_stats_.frame_calls = ++total_frame_calls_;
  frame_stats_.native_detail_source = terrain_ != nullptr;

  opennova::renderer::FoliageViewInput view = _view_input(p_camera_xform);
  if (terrain_ != nullptr) {
    const auto &patches = terrain_->get_foliage_detail_patches_native();
    view.detail_cells.reserve(patches.size());
    for (const auto &patch : patches) {
      view.detail_cells.push_back(opennova::foliage::DetailCell{
          patch.key,
          patch.distance,
      });
    }
  }
  _compile_and_apply(view);
}

void FoliageDispatcher::render_preview(const Transform3D &p_camera_xform) {
  frame_stats_ = FrameStats{};
  frame_stats_.frame_calls = ++total_frame_calls_;
  frame_stats_.preview_detail_source = true;

  opennova::renderer::FoliageViewInput view = _view_input(p_camera_xform);
  view.detail_cells = _preview_cells(p_camera_xform.origin);
  _compile_and_apply(view);
}

opennova::renderer::FoliageViewInput
FoliageDispatcher::_view_input(const Transform3D &p_camera_xform) const {
  opennova::renderer::FoliageViewInput input;
  input.cam_x = static_cast<float>(p_camera_xform.origin.x);
  input.cam_y = static_cast<float>(p_camera_xform.origin.y);
  input.cam_z = static_cast<float>(p_camera_xform.origin.z);

  // Column-major view matrix from the camera's inverse transform (the same
  // construction Terrain feeds TerrainFrameCompiler).
  const Transform3D view = p_camera_xform.affine_inverse();
  const Basis &b = view.basis;
  const Vector3 &o = view.origin;
  input.view[0] = b[0][0]; input.view[1] = b[1][0]; input.view[2] = b[2][0]; input.view[3] = 0;
  input.view[4] = b[0][1]; input.view[5] = b[1][1]; input.view[6] = b[2][1]; input.view[7] = 0;
  input.view[8] = b[0][2]; input.view[9] = b[1][2]; input.view[10] = b[2][2]; input.view[11] = 0;
  input.view[12] = o.x; input.view[13] = o.y; input.view[14] = o.z; input.view[15] = 1;

  Camera3D *active_camera = nullptr;
  if (is_inside_tree()) {
    Viewport *viewport = get_viewport();
    if (viewport != nullptr) {
      active_camera = viewport->get_camera_3d();
    }
  }
  if (active_camera != nullptr) {
    const Projection proj = active_camera->get_camera_projection();
    for (int col = 0; col < 4; ++col) {
      input.proj[col * 4 + 0] = proj.columns[col][0];
      input.proj[col * 4 + 1] = proj.columns[col][1];
      input.proj[col * 4 + 2] = proj.columns[col][2];
      input.proj[col * 4 + 3] = proj.columns[col][3];
    }
  } else {
    // A preview without a live camera gates anchors on view depth alone.
    input.no_frustum = true;
  }

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

      cells.push_back(opennova::foliage::DetailCell{
          pack_preview_detail_key(static_cast<int>(min_x),
                                  static_cast<int>(min_z)),
          distance,
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
  world.detail_foliage_mask_at = [this](int32_t p_world_x_fixed,
                                        int32_t p_world_z_fixed) {
    return _mask_for_palette_index(
        _sample_detail_foliage_index(p_world_x_fixed, p_world_z_fixed));
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
    int32_t p_world_x_fixed, int32_t p_world_z_fixed) const {
  if (terrain_data_.is_valid()) {
    return terrain_data_->get_detail_foliage_index_fixed(
        p_world_x_fixed, p_world_z_fixed);
  }
  if (detail_foliage_sampler_.is_valid()) {
    Array arguments;
    arguments.push_back(static_cast<double>(p_world_x_fixed) / 65536.0);
    arguments.push_back(static_cast<double>(p_world_z_fixed) / 65536.0);
    return static_cast<int>(detail_foliage_sampler_.callv(arguments));
  }
  if (foliage_sampler_.is_valid()) {
    Array arguments;
    arguments.push_back(static_cast<double>(p_world_x_fixed) / 65536.0);
    arguments.push_back(static_cast<double>(p_world_z_fixed) / 65536.0);
    return static_cast<int>(foliage_sampler_.callv(arguments));
  }
  if (colormap_source_.is_valid()) {
    return colormap_source_->get_detail_foliage_index_fixed(
        p_world_x_fixed, p_world_z_fixed);
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
  const auto found = palette_masks_.find(p_index);
  return found == palette_masks_.end() ? 0u : found->second;
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
  _apply_draw_list(draw_list);
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
  // 1) Upload every mesh the compiler built this frame (empty builds cache an
  // empty entry so repeated submissions of a barren identity stay cheap).
  for (const opennova::renderer::FoliageMeshBuild &build : p_draw_list.mesh_builds) {
    CachedMesh entry;
    entry.mesh = _upload_mesh_build(p_draw_list, build);
    entry.instances = build.instance_count;
    entry.vertices = static_cast<int64_t>(build.vertex_count);
    const MeshCacheKey key{build.slot, build.cell_key, build.revision};
    if (build.tier == opennova::renderer::FoliageTier::Detail) {
      detail_mesh_cache_[key] = std::move(entry);
    } else {
      model_mesh_cache_[key] = std::move(entry);
    }
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
  for (const opennova::renderer::FoliageDrawCommand &command : p_draw_list.commands) {
    const int slot = command.slot;
    if (slot < 0 || slot >= opennova::FOLIAGE_MAX_DEFS) {
      continue;
    }
    const MeshCacheKey key{command.slot, command.cell_key, command.revision};
    const bool detail = command.tier == opennova::renderer::FoliageTier::Detail;
    auto &cache = detail ? detail_mesh_cache_ : model_mesh_cache_;
    const auto found = cache.find(key);
    if (found == cache.end() || found->second.mesh.is_null()) {
      // The compiler only commands identities it built or knows resident; a
      // miss means the applier's cache went out of sync with the draw_list.
      continue;
    }

    const size_t draw_index = detail ? detail_draw_index++ : model_draw_index++;
    std::vector<DrawInstanceStamp> &stamps =
        detail ? detail_draw_stamps_ : model_draw_stamps_;
    if (server == nullptr || !scenario_bound) {
      continue;
    }
    const RID draw = detail
                         ? _ensure_draw_instance(server, detail_draw_pool_,
                                                 stamps, draw_index)
                         : _ensure_draw_instance(server, model_draw_pool_,
                                                 stamps, draw_index);
    if (!draw.is_valid()) {
      continue;
    }
    // Diff-apply against what the server instance already holds. A stable
    // draw list avoids every base/material/visibility write; only values whose
    // portable compiler clock advanced reach the server as uniform writes.
    DrawInstanceStamp &stamp = stamps[draw_index];
    const bool fresh = !stamp.bound;
    const Ref<Mesh> mesh = found->second.mesh;
    if (fresh || stamp.mesh != mesh) {
      server->instance_set_base(draw, mesh->get_rid());
      ++frame_stats_.backend_base_writes;
      stamp.mesh = mesh;
    }
    Ref<Material> material;
    bool high = false;
    if (detail) {
      high = command.pass == opennova::foliage::DetailPass::HighAlphaTest;
      material = high ? detail_high_materials_[slot]
                      : detail_low_materials_[slot];
    } else {
      material = silhouette_materials_[slot];
    }
    if (fresh || stamp.material != material) {
      server->instance_geometry_set_material_override(
          draw, material.is_valid() ? material->get_rid() : RID());
      ++frame_stats_.backend_material_writes;
      stamp.material = material;
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
      // [orig: Foliage_SetupFarSlotDraw @ 0x6008fc..0x600912, see docs/foliage/foliage-re.md]
      server->instance_geometry_set_shader_parameter(
          draw, uniform.high_pass_cutoff, command.high_pass_cutoff);
      ++frame_stats_.backend_uniform_writes;
      stamp.high_pass_cutoff = command.high_pass_cutoff;
    }
    if (fresh || stamp.wind_phase != command.wind_phase) {
      server->instance_geometry_set_shader_parameter(
          draw, uniform.wind_phase, command.wind_phase);
      ++frame_stats_.backend_uniform_writes;
      stamp.wind_phase = command.wind_phase;
    }
    if (detail) {
      bool ready = false;
      float layer = 0.0f;
      Vector4 projection_row;
      if (terrain_ != nullptr) {
        const Vector2 center = foliage_detail_cell_center(command.cell_key);
        const std::optional<opennova::TerrainTilePageBinding> page =
            terrain_->get_tile_cache_binding_for_world_point_native(
                static_cast<float>(center.x), static_cast<float>(center.y));
        if (page.has_value() && page->ready) {
          const std::optional<opennova::TerrainTilePageProjection> projection =
              opennova::TerrainTileCompositionCache::page_projection(
                  page->page);
          if (projection.has_value()) {
            ready = true;
            layer = static_cast<float>(page->layer);
            projection_row = Vector4(projection->world_origin_x,
                                     projection->world_origin_z,
                                     projection->inverse_world_span,
                                     projection->world_span);
          }
        }
      }
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
  _hide_pool_tail(detail_draw_pool_, detail_draw_stamps_, detail_draw_index);
  _hide_pool_tail(model_draw_pool_, model_draw_stamps_, model_draw_index);

  // 3) A regenerated identity may still have been submitted earlier in this
  // same draw_list. Draw instances retain its Ref<ArrayMesh>; remove cache ownership
  // only after every command has consumed the frame.
  _erase_cache_identities(p_draw_list.detail_evicted, detail_mesh_cache_);
  _erase_cache_identities(p_draw_list.model_evicted, model_mesh_cache_);

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
  frame_stats_.model_mesh_hits = debug.model_mesh_hits;
  frame_stats_.model_mesh_uploads = debug.model_mesh_uploads;
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
