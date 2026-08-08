#include "nova_foliage_dispatcher.h"

#include "nova_terrain.h"
#include "nova_terrain_data.h"
#include "nova_terrain_tile_info.h"
#include "nova_terrain_foliage_def.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

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
constexpr float DETAIL_DISTANCE_LIMIT = 42.0f;
constexpr float SILHOUETTE_DEPTH_GATE = 38.0f;
constexpr float DETAIL_HEIGHT_SCALE = 0.5f;
constexpr float PREVIEW_CELL_SIZE = 16.0f;
constexpr int PREVIEW_CELL_RADIUS = 4;
constexpr int PREVIEW_CELL_LIMIT = 128;
constexpr float PI_F = 3.14159265358979323846f;

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
  // [z,z+16] stores z+16 in the low half.
  const uint32_t x = static_cast<uint32_t>(p_cell_min_x) & 0x7FFFu;
  const uint32_t z_top = static_cast<uint32_t>(p_cell_min_z + 16) & 0x7FFFu;
  return (x << 16u) | z_top;
}

} // namespace

struct NovaFoliageDispatcher::BatchBuilder {
  std::vector<Vector3> positions;
  std::vector<Vector3> normals;
  std::vector<Vector2> uvs;
  std::vector<Vector2> uv2s;
  std::vector<Color> colors;
  std::vector<int32_t> indices;

  void rollback_vertices(size_t p_size) {
    positions.resize(p_size);
    if (!normals.empty()) {
      normals.resize(p_size);
    }
    uvs.resize(p_size);
    uv2s.resize(p_size);
    colors.resize(p_size);
  }
};

NovaFoliageDispatcher::NovaFoliageDispatcher() = default;
NovaFoliageDispatcher::~NovaFoliageDispatcher() {
  const Callable terrain_changed =
      callable_mp(this, &NovaFoliageDispatcher::_on_terrain_data_changed);
  if (terrain_data_.is_valid() &&
      terrain_data_->is_connected("terrain_changed", terrain_changed)) {
    terrain_data_->disconnect("terrain_changed", terrain_changed);
  }
  const Callable tile_info_changed =
      callable_mp(this, &NovaFoliageDispatcher::_on_tile_info_changed);
  if (tile_info_.is_valid() &&
      tile_info_->is_connected("changed", tile_info_changed)) {
    tile_info_->disconnect("changed", tile_info_changed);
  }
  const Callable colormap_changed =
      callable_mp(this, &NovaFoliageDispatcher::_on_colormap_source_changed);
  if (colormap_source_.is_valid() &&
      colormap_source_->is_connected("terrain_changed", colormap_changed)) {
    colormap_source_->disconnect("terrain_changed", colormap_changed);
  }
}

void NovaFoliageDispatcher::_bind_methods() {
  ClassDB::bind_method(
      D_METHOD("configure_slots", "defs", "meshes", "fd_textures"),
      &NovaFoliageDispatcher::configure_slots);
  ClassDB::bind_method(D_METHOD("set_terrain_data", "data"),
                       &NovaFoliageDispatcher::set_terrain_data);
  ClassDB::bind_method(D_METHOD("get_terrain_data"),
                       &NovaFoliageDispatcher::get_terrain_data);
  ClassDB::bind_method(D_METHOD("set_tile_info", "tile_info"),
                       &NovaFoliageDispatcher::set_tile_info);
  ClassDB::bind_method(D_METHOD("get_tile_info"),
                       &NovaFoliageDispatcher::get_tile_info);
  ClassDB::bind_method(D_METHOD("set_colormap_source", "data"),
                       &NovaFoliageDispatcher::set_colormap_source);
  ClassDB::bind_method(D_METHOD("get_colormap_source"),
                       &NovaFoliageDispatcher::get_colormap_source);
  ClassDB::bind_method(D_METHOD("set_height_sampler", "sampler"),
                       &NovaFoliageDispatcher::set_height_sampler);
  ClassDB::bind_method(D_METHOD("get_height_sampler"),
                       &NovaFoliageDispatcher::get_height_sampler);
  ClassDB::bind_method(D_METHOD("set_detail_foliage_sampler", "sampler"),
                       &NovaFoliageDispatcher::set_detail_foliage_sampler);
  ClassDB::bind_method(D_METHOD("get_detail_foliage_sampler"),
                       &NovaFoliageDispatcher::get_detail_foliage_sampler);
  ClassDB::bind_method(D_METHOD("set_foliage_sampler", "sampler"),
                       &NovaFoliageDispatcher::set_foliage_sampler);
  ClassDB::bind_method(D_METHOD("get_foliage_sampler"),
                       &NovaFoliageDispatcher::get_foliage_sampler);
  ClassDB::bind_method(D_METHOD("set_silhouette_anchors", "anchors"),
                       &NovaFoliageDispatcher::set_silhouette_anchors);
  ClassDB::bind_method(D_METHOD("get_silhouette_anchors"),
                       &NovaFoliageDispatcher::get_silhouette_anchors);
  ClassDB::bind_method(
      D_METHOD("set_surface_input_overrides", "heightfield_normal",
               "tile_overlay", "tile_overlay_tint"),
      &NovaFoliageDispatcher::set_surface_input_overrides);
  ClassDB::bind_method(D_METHOD("clear_surface_input_overrides"),
                       &NovaFoliageDispatcher::clear_surface_input_overrides);
  ClassDB::bind_method(D_METHOD("render_frame", "camera_xform"),
                       &NovaFoliageDispatcher::render_frame);
  ClassDB::bind_method(D_METHOD("render_preview", "camera_xform"),
                       &NovaFoliageDispatcher::render_preview);
  ClassDB::bind_method(D_METHOD("reset"), &NovaFoliageDispatcher::reset);
  ClassDB::bind_method(D_METHOD("get_total_instances"),
                       &NovaFoliageDispatcher::get_total_instances);
  ClassDB::bind_method(D_METHOD("get_frame_stats"),
                       &NovaFoliageDispatcher::get_frame_stats);
  ClassDB::bind_method(D_METHOD("get_slot_diagnostics"),
                       &NovaFoliageDispatcher::get_slot_diagnostics);
  ClassDB::bind_static_method("NovaFoliageDispatcher",
                              D_METHOD("bake_fd_image", "image"),
                              &NovaFoliageDispatcher::bake_fd_image);

  ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "terrain_data",
                            PROPERTY_HINT_RESOURCE_TYPE, "NovaTerrainData"),
               "set_terrain_data", "get_terrain_data");
  ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "tile_info",
                            PROPERTY_HINT_RESOURCE_TYPE, "NovaTerrainTileInfo"),
               "set_tile_info", "get_tile_info");
  ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "colormap_source",
                            PROPERTY_HINT_RESOURCE_TYPE, "NovaTerrainData"),
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
}

void NovaFoliageDispatcher::configure_slots(const Array &p_defs,
                                            const Array &p_meshes,
                                            const Array &p_fd_textures) {
  palette_masks_.clear();
  slot_diagnostics_.clear();
  authored_slot_count_ = 0;
  enabled_slot_count_ = 0;
  disabled_slot_count_ = 0;

  for (int slot = 0; slot < opennova::FOLIAGE_MAX_DEFS; ++slot) {
    runtime_slots_[slot] = opennova::foliage::RuntimeSlot{};
    source_geometry_[slot] = SourceGeometry{};
    fd_textures_[slot].unref();

    Ref<NovaTerrainFoliageDef> def;
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

  reset();
}

void NovaFoliageDispatcher::set_terrain_data(
    const Ref<NovaTerrainData> &p_data) {
  if (terrain_data_ == p_data) {
    return;
  }
  const Callable changed =
      callable_mp(this, &NovaFoliageDispatcher::_on_terrain_data_changed);
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

Ref<NovaTerrainData> NovaFoliageDispatcher::get_terrain_data() const {
  return terrain_data_;
}

void NovaFoliageDispatcher::set_tile_info(
    const Ref<NovaTerrainTileInfo> &p_info) {
  if (tile_info_ == p_info) {
    return;
  }
  const Callable changed =
      callable_mp(this, &NovaFoliageDispatcher::_on_tile_info_changed);
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

Ref<NovaTerrainTileInfo> NovaFoliageDispatcher::get_tile_info() const {
  return tile_info_;
}

void NovaFoliageDispatcher::_on_tile_info_changed() {
  reset();
}

void NovaFoliageDispatcher::set_colormap_source(
    const Ref<NovaTerrainData> &p_data) {
  if (colormap_source_ == p_data) {
    return;
  }
  const Callable changed =
      callable_mp(this, &NovaFoliageDispatcher::_on_colormap_source_changed);
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

Ref<NovaTerrainData> NovaFoliageDispatcher::get_colormap_source() const {
  return colormap_source_;
}

void NovaFoliageDispatcher::_on_terrain_data_changed() { reset(); }

void NovaFoliageDispatcher::_on_colormap_source_changed() { reset(); }

void NovaFoliageDispatcher::set_height_sampler(const Callable &p_sampler) {
  if (height_sampler_ == p_sampler) {
    return;
  }
  height_sampler_ = p_sampler;
  reset();
}

Callable NovaFoliageDispatcher::get_height_sampler() const {
  return height_sampler_;
}

void NovaFoliageDispatcher::set_detail_foliage_sampler(
    const Callable &p_sampler) {
  if (detail_foliage_sampler_ == p_sampler) {
    return;
  }
  detail_foliage_sampler_ = p_sampler;
  reset();
}

Callable NovaFoliageDispatcher::get_detail_foliage_sampler() const {
  return detail_foliage_sampler_;
}

void NovaFoliageDispatcher::set_foliage_sampler(const Callable &p_sampler) {
  if (foliage_sampler_ == p_sampler) {
    return;
  }
  foliage_sampler_ = p_sampler;
  reset();
}

Callable NovaFoliageDispatcher::get_foliage_sampler() const {
  return foliage_sampler_;
}

void NovaFoliageDispatcher::set_silhouette_anchors(
    const PackedVector3Array &p_anchors) {
  silhouette_anchors_ = p_anchors;
}

PackedVector3Array NovaFoliageDispatcher::get_silhouette_anchors() const {
  return silhouette_anchors_;
}

void NovaFoliageDispatcher::set_surface_input_overrides(
    const Ref<Texture2D> &p_heightfield_normal,
    const Ref<Texture2D> &p_tile_overlay,
    const Vector3 &p_tile_overlay_tint) {
  surface_input_overrides_ = true;
  override_heightfield_normal_ = p_heightfield_normal;
  override_tile_overlay_ = p_tile_overlay;
  override_tile_overlay_tint_ =
      finite_vector(p_tile_overlay_tint)
          ? p_tile_overlay_tint
          : Vector3(1.0f, 1.0f, 1.0f);
  _update_materials();
}

void NovaFoliageDispatcher::clear_surface_input_overrides() {
  surface_input_overrides_ = false;
  override_heightfield_normal_.unref();
  override_tile_overlay_.unref();
  override_tile_overlay_tint_ = Vector3(1.0f, 1.0f, 1.0f);
  _update_materials();
}

bool NovaFoliageDispatcher::bake_fd_image(const Ref<Image> &p_image) {
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

NovaFoliageDispatcher::SourceGeometry
NovaFoliageDispatcher::_extract_source_geometry(const Ref<Mesh> &p_mesh) const {
  SourceGeometry result;
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
        return SourceGeometry{};
      }
      SourceVertex source_vertex;
      source_vertex.position = position;
      source_vertex.uv = vertex < uvs.size() ? uvs[vertex] : Vector2();
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
    return SourceGeometry{};
  }

  result.center_x = static_cast<float>((minimum.x + maximum.x) * 0.5);
  result.center_z = static_cast<float>((minimum.z + maximum.z) * 0.5);
  result.radius = std::max(static_cast<float>((maximum.x - minimum.x) * 0.5),
                           static_cast<float>((maximum.z - minimum.z) * 0.5));
  result.valid = std::isfinite(result.radius) && result.radius > 1.0e-6f;
  return result;
}

void NovaFoliageDispatcher::_ensure_visuals() {
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
    ensure_material(detail_high_materials_[slot], detail_high_shader_);
    ensure_material(detail_low_materials_[slot], detail_low_shader_);
    ensure_material(silhouette_materials_[slot], silhouette_shader_);
  }
}

void NovaFoliageDispatcher::_update_materials() {
  Ref<NovaTerrainData> color_source =
      terrain_data_.is_valid() ? terrain_data_ : colormap_source_;
  Ref<Texture2D> colormap;
  if (color_source.is_valid()) {
    colormap = color_source->get_colormap();
  }
  const bool has_colormap = colormap.is_valid();
  Ref<Texture2D> heightfield_normal;
  Ref<Texture2D> tile_overlay;
  Vector3 tile_overlay_tint(1.0f, 1.0f, 1.0f);
  if (surface_input_overrides_) {
    heightfield_normal = override_heightfield_normal_;
    tile_overlay = override_tile_overlay_;
    tile_overlay_tint = override_tile_overlay_tint_;
  } else {
    NovaTerrain *terrain = Object::cast_to<NovaTerrain>(get_parent());
    if (terrain != nullptr) {
      heightfield_normal = terrain->get_heightfield_normal_texture();
      tile_overlay = terrain->get_tile_overlay_texture();
      tile_overlay_tint = terrain->get_tile_overlay_tint();
    }
  }
  const bool has_heightfield_normal = heightfield_normal.is_valid();
  const bool has_tile_overlay = tile_overlay.is_valid();

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
    }

    const Ref<ShaderMaterial> silhouette = silhouette_materials_[slot];
    if (silhouette.is_valid()) {
      silhouette->set_shader_parameter("u_fd_texture", fd_texture);
      silhouette->set_shader_parameter("u_has_fd_texture", has_fd_texture);
    }
  }
}

MeshInstance3D *NovaFoliageDispatcher::_ensure_draw_node(
    std::vector<MeshInstance3D *> &r_pool, size_t p_index,
    const String &p_prefix) {
  while (r_pool.size() <= p_index) {
    MeshInstance3D *instance = memnew(MeshInstance3D);
    instance->set_name(p_prefix +
                       String::num_int64(static_cast<int64_t>(r_pool.size())));
    add_child(instance);
    instance->set_as_top_level(true);
    instance->set_transform(Transform3D());
    // Fresh audit: attrib shadow (0x02) is parsed but never read, and both
    // foliage tiers are excluded from retail shadow-caster passes. They still
    // receive static model projection through retail's composed tile cache.
    instance->set_cast_shadows_setting(
        GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
    // The generic attenuation catcher cannot reproduce foliage-card alpha,
    // two-sided rasterization, and wind deformation without dark rectangles.
    // Keep foliage on the ordinary world layer until the retail tile-cache
    // compositor (which supplies the alpha-lighting term before this pass) is
    // ported.
    instance->set_layer_mask(1u << 0);
    instance->set_extra_cull_margin(8.0f);
    instance->set_visible(false);
    r_pool.push_back(instance);
  }
  return r_pool[p_index];
}

void NovaFoliageDispatcher::_hide_draw_pools() {
  for (MeshInstance3D *instance : detail_draw_pool_) {
    if (instance != nullptr) {
      instance->set_visible(false);
      // Drop the prior frame's draw ownership before runtime eviction events
      // are applied. Resident meshes remain owned by detail_mesh_cache_.
      instance->set_mesh(Ref<Mesh>());
    }
  }
  for (MeshInstance3D *instance : model_draw_pool_) {
    if (instance != nullptr) {
      instance->set_visible(false);
      instance->set_mesh(Ref<Mesh>());
    }
  }
}

void NovaFoliageDispatcher::_clear_meshes() {
  _hide_draw_pools();
  for (MeshInstance3D *instance : detail_draw_pool_) {
    if (instance != nullptr) {
      instance->set_mesh(Ref<Mesh>());
    }
  }
  for (MeshInstance3D *instance : model_draw_pool_) {
    if (instance != nullptr) {
      instance->set_mesh(Ref<Mesh>());
    }
  }
}

void NovaFoliageDispatcher::reset() {
  runtime_.reset();
  frame_stats_ = FrameStats{};
  total_frame_calls_ = 0;
  model_wind_counter_ = 0;
  detail_mesh_cache_.clear();
  model_mesh_cache_.clear();
  _clear_meshes();
}

int NovaFoliageDispatcher::get_total_instances() const {
  const int64_t total = frame_stats_.detail_high_instances +
                        frame_stats_.detail_low_instances +
                        frame_stats_.silhouette_instances;
  return static_cast<int>(
      std::min<int64_t>(total, std::numeric_limits<int>::max()));
}

Dictionary NovaFoliageDispatcher::get_frame_stats() const {
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
  result["terrain_scene_counter"] = frame_stats_.terrain_scene_counter;
  result["native_detail_source"] = frame_stats_.native_detail_source;
  result["preview_detail_source"] = frame_stats_.preview_detail_source;
  result["path_blocker_available"] = frame_stats_.path_blocker_available;
  result["surface_input_overrides"] = surface_input_overrides_;
  result["surface_override_has_heightfield_normal"] =
      override_heightfield_normal_.is_valid();
  result["surface_override_has_tile_overlay"] =
      override_tile_overlay_.is_valid();
  result["surface_override_tile_tint"] = override_tile_overlay_tint_;
  result["authored_slots"] = authored_slot_count_;
  result["enabled_slots"] = enabled_slot_count_;
  result["disabled_slots"] = disabled_slot_count_;
  return result;
}

Array NovaFoliageDispatcher::get_slot_diagnostics() const {
  return slot_diagnostics_.duplicate(true);
}

void NovaFoliageDispatcher::render_frame(const Transform3D &p_camera_xform) {
  frame_stats_ = FrameStats{};
  frame_stats_.frame_calls = ++total_frame_calls_;
  _render_request(_runtime_request(p_camera_xform), p_camera_xform);
}

void NovaFoliageDispatcher::render_preview(const Transform3D &p_camera_xform) {
  frame_stats_ = FrameStats{};
  frame_stats_.frame_calls = ++total_frame_calls_;
  frame_stats_.preview_detail_source = true;
  _render_request(_preview_request(p_camera_xform), p_camera_xform);
}

opennova::foliage::FrameRequest
NovaFoliageDispatcher::_runtime_request(const Transform3D &p_camera_xform) {
  opennova::foliage::FrameRequest request;
  request.slots = runtime_slots_;

  NovaTerrain *terrain = Object::cast_to<NovaTerrain>(get_parent());
  frame_stats_.native_detail_source = terrain != nullptr;
  if (terrain != nullptr) {
    const auto &patches = terrain->get_foliage_detail_patches_native();
    request.detail_cells.reserve(patches.size());
    for (const auto &patch : patches) {
      request.detail_cells.push_back(opennova::foliage::DetailCell{
          patch.key,
          patch.distance,
      });
    }
  }
  frame_stats_.detail_cells = static_cast<int64_t>(request.detail_cells.size());
  _append_visible_anchors(request, p_camera_xform);
  return request;
}

opennova::foliage::FrameRequest
NovaFoliageDispatcher::_preview_request(const Transform3D &p_camera_xform) {
  opennova::foliage::FrameRequest request;
  request.slots = runtime_slots_;
  request.detail_cells = _preview_cells(p_camera_xform.origin);
  frame_stats_.detail_cells = static_cast<int64_t>(request.detail_cells.size());
  _append_visible_anchors(request, p_camera_xform);
  return request;
}

void NovaFoliageDispatcher::_append_visible_anchors(
    opennova::foliage::FrameRequest &r_request,
    const Transform3D &p_camera_xform) {
  frame_stats_.silhouette_anchors_input = silhouette_anchors_.size();

  Camera3D *active_camera = nullptr;
  if (is_inside_tree()) {
    Viewport *viewport = get_viewport();
    if (viewport != nullptr) {
      active_camera = viewport->get_camera_3d();
    }
  }

  const Transform3D view = p_camera_xform.affine_inverse();
  const Vector3 camera_position = p_camera_xform.origin;
  r_request.silhouette_anchors.reserve(silhouette_anchors_.size());

  for (int index = 0; index < silhouette_anchors_.size(); ++index) {
    const Vector3 anchor = silhouette_anchors_[index];
    if (!finite_vector(anchor)) {
      continue;
    }
    const float view_depth = static_cast<float>(-view.xform(anchor).z);
    if (!std::isfinite(view_depth) ||
        view_depth < SILHOUETTE_DEPTH_GATE) {
      continue;
    }
    if (active_camera != nullptr &&
        !active_camera->is_position_in_frustum(anchor)) {
      continue;
    }

    opennova::foliage::SilhouetteAnchor runtime_anchor;
    runtime_anchor.position = {
        static_cast<float>(anchor.x),
        static_cast<float>(anchor.z),
    };
    runtime_anchor.view_depth = view_depth;
    runtime_anchor.camera_distance =
        static_cast<float>(anchor.distance_to(camera_position));
    if (!std::isfinite(runtime_anchor.camera_distance)) {
      continue;
    }
    r_request.silhouette_anchors.push_back(runtime_anchor);
  }

  frame_stats_.silhouette_anchors_visible =
      static_cast<int64_t>(r_request.silhouette_anchors.size());
}

std::vector<opennova::foliage::DetailCell>
NovaFoliageDispatcher::_preview_cells(const Vector3 &p_camera_position) const {
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

opennova::foliage::WorldSamplers NovaFoliageDispatcher::_world_samplers() {
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
    // [orig: Foliage_PathBlockedByPlacedTile @ 0x606490]
    return tile_info_.is_valid() &&
           tile_info_->blocks_foliage(p_world_x, p_world_z, p_radius);
  };
  return world;
}

float NovaFoliageDispatcher::_sample_height(float p_world_x,
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

int NovaFoliageDispatcher::_sample_detail_foliage_index(
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

int NovaFoliageDispatcher::_sample_model_foliage_index(
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

uint32_t NovaFoliageDispatcher::_mask_for_palette_index(int p_index) const {
  const auto found = palette_masks_.find(p_index);
  return found == palette_masks_.end() ? 0u : found->second;
}

Vector2 NovaFoliageDispatcher::_terrain_uv(float p_world_x,
                                           float p_world_z) const {
  const bool runtime_mapping = terrain_data_.is_valid();
  Ref<NovaTerrainData> source =
      runtime_mapping ? terrain_data_ : colormap_source_;
  if (source.is_null()) {
    return Vector2();
  }
  const Ref<Texture2D> colormap = source->get_colormap();
  if (colormap.is_null() || colormap->get_width() <= 0 ||
      colormap->get_height() <= 0) {
    return Vector2();
  }
  const Vector2 atlas = runtime_mapping
                            ? source->world_to_runtime_source_coords(p_world_x,
                                                                      p_world_z)
                            : source->world_to_source_coords(p_world_x,
                                                             p_world_z);
  if (atlas.x < 0.0f || atlas.y < 0.0f) {
    return Vector2();
  }
  return Vector2(atlas.x / static_cast<float>(colormap->get_width()),
                 atlas.y / static_cast<float>(colormap->get_height()));
}

bool NovaFoliageDispatcher::_append_detail_instance(
    const opennova::foliage::DetailInstance &p_instance,
    BatchBuilder &r_batch) {
  const int slot = p_instance.slot;
  if (slot < 0 || slot >= opennova::FOLIAGE_MAX_DEFS ||
      !source_geometry_[slot].valid) {
    return false;
  }
  const SourceGeometry &source = source_geometry_[slot];
  if (r_batch.positions.size() >
      static_cast<size_t>(std::numeric_limits<int32_t>::max()) -
          source.vertices.size()) {
    return false;
  }

  const size_t vertex_base = r_batch.positions.size();
  const Basis rotation(Vector3(0.0f, 1.0f, 0.0f),
                       p_instance.yaw_radians + PI_F);

  for (const SourceVertex &vertex : source.vertices) {
    const Vector3 planar =
        rotation.xform(Vector3(vertex.position.x, 0.0f, vertex.position.z));
    const float world_x = p_instance.center.x + static_cast<float>(planar.x);
    const float world_z = p_instance.center.z + static_cast<float>(planar.z);
    const float ground = _sample_height(world_x, world_z);
    const float height_left = _sample_height(world_x - 1.0f, world_z);
    const float height_right = _sample_height(world_x + 1.0f, world_z);
    const float height_previous = _sample_height(world_x, world_z - 1.0f);
    const float height_next = _sample_height(world_x, world_z + 1.0f);
    if (!valid_height(ground) || !valid_height(height_left) ||
        !valid_height(height_right) || !valid_height(height_previous) ||
        !valid_height(height_next)) {
      r_batch.rollback_vertices(vertex_base);
      return false;
    }

    const Vector3 world_position(
        world_x,
        ground + static_cast<float>(vertex.position.y) * DETAIL_HEIGHT_SCALE,
        world_z);
    if (!finite_vector(world_position)) {
      r_batch.rollback_vertices(vertex_base);
      return false;
    }

    const int bend_byte = std::clamp(
        static_cast<int>(static_cast<float>(vertex.position.y) * 128.0f), 0,
        255);
    r_batch.positions.push_back(world_position);
    r_batch.normals.push_back(
        Vector3(height_left - height_right, 2.0f,
                height_previous - height_next)
            .normalized());
    r_batch.uvs.push_back(vertex.uv);
    r_batch.uv2s.push_back(_terrain_uv(world_x, world_z));
    // Only the source-height bend byte is resident geometry. Fade, alpha
    // reference, and pass are per-submission state on the draw node.
    r_batch.colors.push_back(
        Color(static_cast<float>(bend_byte) / 255.0f, 0.0f, 0.0f, 1.0f));
  }

  const int32_t index_base = static_cast<int32_t>(vertex_base);
  for (const int32_t index : source.indices) {
    r_batch.indices.push_back(index_base + index);
  }
  return true;
}

bool NovaFoliageDispatcher::_append_silhouette_instance(
    const opennova::foliage::SilhouetteInstance &p_instance,
    BatchBuilder &r_batch) {
  const int slot = p_instance.slot;
  if (slot < 0 || slot >= opennova::FOLIAGE_MAX_DEFS ||
      !source_geometry_[slot].valid) {
    return false;
  }
  for (const opennova::foliage::GroundCorner &corner : p_instance.corners) {
    if (!valid_height(corner.height)) {
      return false;
    }
  }

  const SourceGeometry &source = source_geometry_[slot];
  if (source.radius <= 1.0e-6f ||
      r_batch.positions.size() >
          static_cast<size_t>(std::numeric_limits<int32_t>::max()) -
              source.vertices.size()) {
    return false;
  }

  const float inverse_span = 1.0f / (2.0f * source.radius);
  const size_t vertex_base = r_batch.positions.size();

  for (const SourceVertex &vertex : source.vertices) {
    // The 3DI import negates source X. Convert back to retail local A
    // while local B remains Godot Z.
    const float x_normalized =
        0.5f - (static_cast<float>(vertex.position.x) - source.center_x) *
                   inverse_span;
    const float z_normalized =
        0.5f + (static_cast<float>(vertex.position.z) - source.center_z) *
                   inverse_span;
    const float one_minus_x = 1.0f - x_normalized;
    const float one_minus_z = 1.0f - z_normalized;
    const float weights[4] = {
        one_minus_x * one_minus_z,
        x_normalized * one_minus_z,
        one_minus_x * z_normalized,
        x_normalized * z_normalized,
    };

    float world_x = 0.0f;
    float world_z = 0.0f;
    float ground = 0.0f;
    for (int corner = 0; corner < 4; ++corner) {
      world_x += weights[corner] * p_instance.corners[corner].x;
      world_z += weights[corner] * p_instance.corners[corner].z;
      ground += weights[corner] * p_instance.corners[corner].height;
    }

    const float u = 2.0f * x_normalized - 1.0f;
    const float v = 2.0f * z_normalized - 1.0f;
    ground += (1.0f - v * v) * (p_instance.fold[0] + u * p_instance.fold[1]) +
              (1.0f - u * u) * (p_instance.fold[2] + v * p_instance.fold[3]);

    const float half_height =
        static_cast<float>(vertex.position.y) * DETAIL_HEIGHT_SCALE;
    const Vector3 world_position(world_x, ground + half_height, world_z);
    if (!finite_vector(world_position)) {
      r_batch.rollback_vertices(vertex_base);
      return false;
    }

    r_batch.positions.push_back(world_position);
    r_batch.uvs.push_back(vertex.uv);
    r_batch.uv2s.push_back(Vector2(half_height, 0.0f));
    r_batch.colors.push_back(Color(0.0f, 0.0f, 0.0f, 1.0f));
  }

  const int32_t index_base = static_cast<int32_t>(vertex_base);
  for (const int32_t index : source.indices) {
    r_batch.indices.push_back(index_base + index);
  }
  return true;
}

NovaFoliageDispatcher::CachedMesh NovaFoliageDispatcher::_build_mesh(
    const BatchBuilder &p_batch, int64_t p_instance_count) const {
  CachedMesh result;
  result.instances = p_instance_count;
  result.vertices = static_cast<int64_t>(p_batch.positions.size());
  if (p_batch.positions.empty() || p_batch.indices.empty()) {
    return result;
  }

  const int64_t vertex_count = static_cast<int64_t>(p_batch.positions.size());
  const int64_t index_count = static_cast<int64_t>(p_batch.indices.size());
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
  if (p_batch.normals.size() == p_batch.positions.size()) {
    normals.resize(vertex_count);
  }

  for (int64_t vertex = 0; vertex < vertex_count; ++vertex) {
    positions.set(vertex, p_batch.positions[static_cast<size_t>(vertex)]);
    uvs.set(vertex, p_batch.uvs[static_cast<size_t>(vertex)]);
    uv2s.set(vertex, p_batch.uv2s[static_cast<size_t>(vertex)]);
    colors.set(vertex, p_batch.colors[static_cast<size_t>(vertex)]);
    if (!normals.is_empty()) {
      normals.set(vertex, p_batch.normals[static_cast<size_t>(vertex)]);
    }
  }
  for (int64_t index = 0; index < index_count; ++index) {
    indices.set(index, p_batch.indices[static_cast<size_t>(index)]);
  }

  Array arrays;
  arrays.resize(Mesh::ARRAY_MAX);
  arrays[Mesh::ARRAY_VERTEX] = positions;
  if (!normals.is_empty()) {
    arrays[Mesh::ARRAY_NORMAL] = normals;
  }
  arrays[Mesh::ARRAY_COLOR] = colors;
  arrays[Mesh::ARRAY_TEX_UV] = uvs;
  arrays[Mesh::ARRAY_TEX_UV2] = uv2s;
  arrays[Mesh::ARRAY_INDEX] = indices;

  result.mesh.instantiate();
  result.mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
  return result;
}

void NovaFoliageDispatcher::_erase_cache_identities(
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

void NovaFoliageDispatcher::_render_request(
    opennova::foliage::FrameRequest p_request,
    const Transform3D &p_camera_xform) {
  (void)p_camera_xform;
  _ensure_visuals();
  _update_materials();
  _hide_draw_pools();

  const opennova::foliage::FrameOutput output =
      runtime_.render_frame(p_request, _world_samplers());
  const opennova::foliage::RuntimeStats &runtime_stats = runtime_.get_stats();
  frame_stats_.runtime_detail_intents =
      static_cast<int64_t>(output.detail.size());
  frame_stats_.runtime_silhouette_intents =
      static_cast<int64_t>(output.silhouettes.size());
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

  const float detail_wind_phase =
      static_cast<float>(runtime_stats.terrain_scene_counter) * 0.001f;
  size_t detail_draw_index = 0;
  for (size_t begin = 0; begin < output.detail.size();) {
    const opennova::foliage::DetailInstance &first = output.detail[begin];
    size_t end = begin + 1;
    while (end < output.detail.size() &&
           output.detail[end].submission_id == first.submission_id) {
      ++end;
    }

    const int slot = first.slot;
    if (slot < 0 || slot >= opennova::FOLIAGE_MAX_DEFS) {
      begin = end;
      continue;
    }
    const MeshCacheKey cache_key{
        first.slot,
        first.cell_key,
        first.cache_revision,
    };
    auto cached = detail_mesh_cache_.find(cache_key);
    if (cached == detail_mesh_cache_.end()) {
      BatchBuilder batch;
      int64_t instance_count = 0;
      for (size_t index = begin; index < end; ++index) {
        if (_append_detail_instance(output.detail[index], batch)) {
          ++instance_count;
        }
      }
      CachedMesh built = _build_mesh(batch, instance_count);
      if (built.mesh.is_valid()) {
        ++frame_stats_.detail_mesh_uploads;
      }
      cached =
          detail_mesh_cache_.emplace(cache_key, std::move(built)).first;
    } else {
      ++frame_stats_.detail_mesh_hits;
    }

    const CachedMesh &resident = cached->second;
    if (resident.mesh.is_valid()) {
      const bool high =
          first.pass == opennova::foliage::DetailPass::HighAlphaTest;
      MeshInstance3D *draw =
          _ensure_draw_node(detail_draw_pool_, detail_draw_index++,
                            String("FoliageDetailDraw"));
      draw->set_mesh(resident.mesh);
      draw->set_material_override(
          high ? detail_high_materials_[slot] : detail_low_materials_[slot]);
      draw->set_instance_shader_parameter(StringName("u_fade"), first.alpha);
      draw->set_instance_shader_parameter(
          StringName("u_alpha_ref"),
          static_cast<float>(first.alpha_reference) / 255.0f);
      // The near secondary LOW draw runs under strict D3DCMP_LESS in retail;
      // the cutoff discard keeps it off every texel the HIGH pass accepted.
      // [orig: Foliage_SetupFarSlotDraw @ 0x6008fc..0x600912]
      draw->set_instance_shader_parameter(
          StringName("u_high_pass_cutoff"),
          first.near_secondary ? 180.0f / 255.0f : 0.0f);
      draw->set_instance_shader_parameter(StringName("u_wind_phase"),
                                          detail_wind_phase);
      draw->set_visible(true);

      if (high) {
        frame_stats_.detail_high_instances += resident.instances;
      } else {
        frame_stats_.detail_low_instances += resident.instances;
      }
      frame_stats_.detail_vertices += resident.vertices;
      ++frame_stats_.render_batches;
    }
    begin = end;
  }

  size_t model_draw_index = 0;
  for (size_t begin = 0; begin < output.silhouettes.size();) {
    const opennova::foliage::SilhouetteInstance &first =
        output.silhouettes[begin];
    size_t end = begin + 1;
    while (end < output.silhouettes.size() &&
           output.silhouettes[end].submission_id == first.submission_id) {
      ++end;
    }

    const int slot = first.slot;
    if (slot < 0 || slot >= opennova::FOLIAGE_MAX_DEFS) {
      begin = end;
      continue;
    }
    const MeshCacheKey cache_key{
        first.slot,
        first.cell_key,
        first.cache_revision,
    };
    auto cached = model_mesh_cache_.find(cache_key);
    if (cached == model_mesh_cache_.end()) {
      BatchBuilder batch;
      int64_t instance_count = 0;
      for (size_t index = begin; index < end; ++index) {
        if (_append_silhouette_instance(output.silhouettes[index], batch)) {
          ++instance_count;
        }
      }
      CachedMesh built = _build_mesh(batch, instance_count);
      if (built.mesh.is_valid()) {
        ++frame_stats_.model_mesh_uploads;
      }
      cached = model_mesh_cache_.emplace(cache_key, std::move(built)).first;
    } else {
      ++frame_stats_.model_mesh_hits;
    }

    const CachedMesh &resident = cached->second;
    if (resident.mesh.is_valid()) {
      MeshInstance3D *draw =
          _ensure_draw_node(model_draw_pool_, model_draw_index++,
                            String("FoliageModelDraw"));
      draw->set_mesh(resident.mesh);
      draw->set_material_override(silhouette_materials_[slot]);
      draw->set_instance_shader_parameter(
          StringName("u_alpha_ref"),
          static_cast<float>(first.alpha_reference) / 255.0f);
      // Retail pre-increments the wind counter once per actual nonempty model
      // draw, including repeated submissions of one resident cache entry.
      const float wind_phase =
          static_cast<float>(++model_wind_counter_) * 0.001f;
      draw->set_instance_shader_parameter(StringName("u_wind_phase"),
                                          wind_phase);
      draw->set_visible(true);

      frame_stats_.silhouette_instances += resident.instances;
      frame_stats_.silhouette_vertices += resident.vertices;
      ++frame_stats_.render_batches;
    }
    begin = end;
  }

  // A regenerated identity may still have been submitted earlier in this
  // same output. Draw nodes retain its Ref<ArrayMesh>; remove cache ownership
  // only after every submission has consumed the frame.
  _erase_cache_identities(output.detail_evicted, detail_mesh_cache_);
  _erase_cache_identities(output.model_evicted, model_mesh_cache_);
}

} // namespace godot
