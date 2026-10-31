#include "terrain/foliage_asset_cache.h"
#include "util/data_format.h"
#include "util/string_convert.h"

#include "object/object_data.h"
#include "resource_index/resource_root.h"
#include "terrain/terrain_foliage_def.h"

#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <formats/foliage/runtime.h>
#include <runtime/assets/foliage_assets.h>

#include <cstdint>
#include <utility>

namespace godot {

bool FoliageAssetCache::bake_fd_image(const Ref<Image> &p_image) {
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

void FoliageAssetCache::clear() {
  model_assets_.clear();
  fd_texture_cache_.clear();
  model_path_cache_.clear();
  listed_roots_.clear();
}

void FoliageAssetCache::check_epoch() {
  const uint64_t epoch = static_cast<uint64_t>(ResourceRoot::cache_epoch());
  if (epoch == epoch_) {
    return;
  }
  epoch_ = epoch;
  clear();
}

int FoliageAssetCache::entry_count() {
  check_epoch();
  return static_cast<int>(model_assets_.size() + fd_texture_cache_.size() +
                          model_path_cache_.size() +
                          listed_roots_.size());
}

void FoliageAssetCache::_list_graphics(
    const Ref<ResourceRoot> &p_resource_root) {
  if (p_resource_root.is_null() ||
      p_resource_root->get_root_dir().is_empty()) {
    return;
  }
  const String key = _asset_root_key(p_resource_root);
  if (listed_roots_.has(key)) {
    return;
  }

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
    model_path_cache_[_asset_cache_key(key, basename)] = model_ref;
  }
  listed_roots_[key] = true;
}

std::vector<opennova::renderer::FoliageSlotGeometry>
FoliageAssetCache::resolve_slot_geometry(const Ref<ResourceRoot> &p_resource_root,
                                         const Array &p_defs,
                                         std::vector<bool> &r_supplied) {
  check_epoch();
  std::vector<opennova::renderer::FoliageSlotGeometry> geometry(
      static_cast<size_t>(p_defs.size()));
  r_supplied.assign(static_cast<size_t>(p_defs.size()), false);
  for (int64_t i = 0; i < p_defs.size(); ++i) {
    const Ref<TerrainFoliageDef> def = p_defs[i];
    if (def.is_null() || def->get_graphic().is_empty()) {
      continue;
    }
    if (const ModelAsset *asset = _load_asset(p_resource_root, def->get_graphic())) {
      geometry[static_cast<size_t>(i)] = asset->geometry;
      r_supplied[static_cast<size_t>(i)] = true;
    }
  }
  return geometry;
}

Array FoliageAssetCache::resolve_slot_fd_textures(
    const Ref<ResourceRoot> &p_resource_root, const Array &p_defs) {
  check_epoch();
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
FoliageAssetCache::load_fd_texture(const Ref<ResourceRoot> &p_resource_root,
                                   const String &p_graphic) {
  check_epoch();
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
  if (fd_texture_cache_.has(key)) {
    return fd_texture_cache_[key];
  }

  const ModelAsset *asset = _load_asset(p_resource_root, p_graphic);
  const Ref<Texture2D> source = asset != nullptr ? asset->fd_source : Ref<Texture2D>();
  if (source.is_null()) {
    return Ref<Texture2D>();
  }
  Ref<Image> image = source->get_image();
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
    fd = source;
  }
  fd_texture_cache_[key] = fd;
  return fd;
}

Ref<ObjectData> FoliageAssetCache::_open_model(const Ref<ResourceRoot> &p_resource_root,
                                               const String &p_basename) {
  const String model_path = _find_model_path(p_resource_root, p_basename);
  if (model_path.is_empty()) {
    return Ref<ObjectData>();
  }
  Ref<ObjectData> data;
  data.instantiate();
  if (data->open_from_resource_root(p_resource_root, model_path) != OK) {
    return Ref<ObjectData>();
  }
  return data;
}

const FoliageAssetCache::ModelAsset *
FoliageAssetCache::_load_asset(const Ref<ResourceRoot> &p_resource_root,
                               const String &p_graphic) {
  check_epoch();
  if (p_resource_root.is_null() ||
      p_resource_root->get_root_dir().is_empty()) {
    return nullptr;
  }
  const String basename = opennova::to_gd(
      opennova::assets::foliage_model_basename(opennova::to_std(p_graphic)));
  if (basename.is_empty()) {
    return nullptr;
  }
  const String key = _asset_cache_key(_asset_root_key(p_resource_root), basename);
  if (const ModelAsset *cached = model_assets_.getptr(key)) {
    return cached->loaded ? cached : nullptr;
  }
  ModelAsset asset;
  const Ref<ObjectData> data = _open_model(p_resource_root, basename);
  if (data.is_valid()) {
    asset.loaded = true;
    // The slot's geometry, its bounds and its :fd source row are the
    // engine's, from the model itself (assets::foliage_model_asset).
    opennova::assets::FoliageModelAsset model = opennova::assets::foliage_model_asset(
        data->native_model());
    asset.geometry = std::move(model.geometry);
    if (model.fd_material_array_index >= 0 && model.fd_texture_row >= 0) {
      asset.fd_source = data->load_material_texture(model.fd_material_array_index,
                                                    model.fd_texture_row);
    }
  }
  model_assets_[key] = asset;
  const ModelAsset *stored = model_assets_.getptr(key);
  return stored->loaded ? stored : nullptr;
}

String FoliageAssetCache::_find_model_path(
    const Ref<ResourceRoot> &p_resource_root, const String &p_basename) {
  const String key = _asset_root_key(p_resource_root);
  const String path_key = _asset_cache_key(key, p_basename);
  if (model_path_cache_.has(path_key)) {
    return model_path_cache_[path_key];
  }
  _list_graphics(p_resource_root);
  if (model_path_cache_.has(path_key)) {
    return model_path_cache_[path_key];
  }

  // Resolve through the VFS (loose or PFF). The logical name is enough: it is
  // only fed back to ObjectData.open_from_resource_root, which reads it
  // through the VFS.
  const String logical = p_basename + String(".3di");
  if (p_resource_root->has_file(logical)) {
    model_path_cache_[path_key] = logical;
    return logical;
  }
  return String();
}

String FoliageAssetCache::_asset_root_key(
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

String FoliageAssetCache::_asset_cache_key(const String &p_root_key,
                                           const String &p_basename) {
  return vformat("%s|%s", p_root_key, p_basename);
}

} // namespace godot
