#pragma once

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>

namespace godot {

class ObjectData;
class ResourceRoot;

// The vegetation asset resolver (the former veg_assets.gd, ADR 0043 slice G10),
// FoliageDispatcher's instance state. Vegetation .3di are not bundled; runtime
// and editor callers pass the same flat ResourceRoot used for
// terrain/env/credits. The caches (meshes, :fd textures, model paths, the
// per-root graphics listing) are keyed by root + graphic, survive mission
// reloads (the dispatcher lives beside the world's Terrain for the world's
// whole life) and die with it; clear() empties them explicitly (the shell's
// exit) and every cache-reading verb self-clears them when the global cache
// epoch moved (check_epoch).
class FoliageAssetCache {
public:
  // Empty every cache (the shell's exit).
  void clear();
  // Lifecycle diagnostic: the shell can prove that every retained renderer
  // registry was emptied without exposing any cache for mutation (a cache
  // reader: it runs the epoch self-clear first).
  int entry_count();
  // Resolve each def's `graphic` name to the first Mesh built from its .3di.
  // Returns an Array parallel to `defs`; a null entry disables that retail
  // slot. The cache never manufactures placeholder geometry.
  Array resolve_slot_meshes(const Ref<ResourceRoot> &p_resource_root,
                            const Array &p_defs);
  // Build the per-def ":fd" textures. Returns an Array parallel to `defs`;
  // unsupported diffuses fall back to the raw texture (the retail filter's
  // wrap masks assume pow2 dimensions of at least four), null entries stay
  // null.
  Array resolve_slot_fd_textures(const Ref<ResourceRoot> &p_resource_root,
                                 const Array &p_defs);
  // The ":fd" texture for one graphic: retail's alpha-filtered, progressively
  // gray mip chain of the model's OWN diffuse that BOTH foliage tiers bind
  // [orig: Foliage_LoadDefAssets @ 0x601260 tail; bound by
  // Foliage_DrawModelTileSlot @ 0x601d90 and the expanded detail tier alike]
  // -- the model diffuse through the witnessed custom mip pipeline
  // (bake_fd_image), cached per root+model.
  Ref<Texture2D> load_fd_texture(const Ref<ResourceRoot> &p_resource_root,
                                 const String &p_graphic);
  // Resolve a graphic name (e.g. "mveg5" or "mveg5.3di") to one Mesh
  // containing every surface of every LOD0 submesh built from the matching
  // top-level .3di. Returns null if not resolvable.
  Ref<Mesh> load_mesh(const Ref<ResourceRoot> &p_resource_root,
                      const String &p_graphic);
  // Merge all geometry that build_lod_submeshes(0) emits. ArrayMesh surface
  // arrays are copied into a private resource so callers never mutate
  // ObjectData's shared submesh cache.
  static Ref<ArrayMesh> aggregate_lod0_submeshes(const Array &p_submeshes);
  // Replace a >=4x4 power-of-two RGBA8 Image with retail's complete :fd mip
  // chain, including Godot's required 2x2/1x1 terminal levels.
  static bool bake_fd_image(const Ref<Image> &p_image);
  // The self-clear at the top of every cache-reading verb: the same clear
  // as clear() when the global cache epoch moved since the caches were built.
  void check_epoch();

private:
  // The caches, keyed "<root dir>|<root instance id>|<basename>" so two live
  // mounts of one directory never alias.
  HashMap<String, Ref<Mesh>> mesh_cache_;
  HashMap<String, Ref<Texture2D>> fd_texture_cache_;
  HashMap<String, String> model_path_cache_;
  // The roots whose *veg*.3di graphics _list_graphics has recorded.
  HashMap<String, bool> listed_roots_;
  // The global cache epoch (ResourceRoot.cache_epoch) the caches above were
  // built under; any root mount/rescan/clear moves it and the next access
  // self-clears.
  uint64_t epoch_ = 0;
  static Ref<Texture2D> _mesh_albedo_texture(const Ref<Mesh> &p_mesh);
  // Load the diffuse (slot 1, falling back to detail slot 2) texture for a
  // .3di material, the same row resolution ObjectModel's material builder
  // uses. Returns null if the material has no resolvable texture.
  static Ref<Texture2D> _load_diffuse_texture(const Ref<ObjectData> &p_data,
                                              int p_material_index);
  // Record every top-level *veg*.3di graphic of the root in the model-path
  // cache (the first entry of each lower-cased basename, its path in its real
  // case), once per root: _find_model_path's first source.
  void _list_graphics(const Ref<ResourceRoot> &p_resource_root);
  String _find_model_path(const Ref<ResourceRoot> &p_resource_root,
                          const String &p_basename);
  static String _asset_root_key(const Ref<ResourceRoot> &p_resource_root);
  static String _asset_cache_key(const String &p_root_key,
                                 const String &p_basename);
};

} // namespace godot
