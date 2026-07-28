extends RefCounted

# Vegetation .3di are not bundled. Runtime and editor callers pass the same
# flat NovaResourceRoot used for terrain/env/credits, so there is no process
# global search-root state.

static var _mesh_cache: Dictionary = {}
static var _fd_texture_cache: Dictionary = {}
static var _model_path_cache: Dictionary = {}
static var _graphics_cache_by_root: Dictionary = {}
# The global cache epoch (NovaResourceRoot.cache_epoch) the caches above were built
# under; any root mount/rescan/clear moves it and the next access self-clears.
static var _built_epoch: int = 0


static func clear_cache() -> void:
	_mesh_cache.clear()
	_fd_texture_cache.clear()
	_model_path_cache.clear()
	_graphics_cache_by_root.clear()


static func _check_epoch() -> void:
	var epoch := NovaResourceRoot.cache_epoch()
	if epoch == _built_epoch:
		return
	_built_epoch = epoch
	clear_cache()


## Enumerate all top-level *veg*.3di graphics in the resource root.
## Returns dictionaries with basename/model_path, sorted by basename.
static func list_graphics(resource_root: NovaResourceRoot, force_refresh: bool = false) -> Array:
	_check_epoch()
	if resource_root == null or resource_root.get_root_dir().is_empty():
		return []
	var root_key := _root_key(resource_root)
	if not force_refresh and _graphics_cache_by_root.has(root_key):
		return _graphics_cache_by_root[root_key].duplicate(true)

	var out: Array = []
	var seen: Dictionary = {}
	for entry_value in resource_root.list_file_entries(".3di"):
		var entry := entry_value as Dictionary
		var model_name := String(entry.get("logical_name", entry.get("path", "")))
		var model_ref := String(entry.get("path", ""))
		if model_ref.is_empty():
			model_ref = model_name
		var basename := model_name.get_file().get_basename().to_lower()
		if not basename.contains("veg") or seen.has(basename):
			continue
		seen[basename] = true
		_model_path_cache[_cache_key(root_key, basename)] = model_ref
		out.append({
			"basename": basename,
			"model_path": model_ref,
			"scene_path": model_ref,
		})
	out.sort_custom(func(a, b): return String(a.basename) < String(b.basename))
	_graphics_cache_by_root[root_key] = out
	return out.duplicate(true)


## Resolve each def's `graphic` name to the first Mesh built from its .3di.
## Returns an Array parallel to `defs`; a null entry disables that retail slot.
## The fresh dispatcher never manufactures placeholder geometry.
static func resolve_slot_meshes(resource_root: NovaResourceRoot, defs: Array) -> Array:
	var meshes: Array = []
	for def in defs:
		if def == null:
			meshes.append(null)
			continue
		var graphic: String = String(def.graphic)
		var mesh: Mesh = load_mesh(resource_root, graphic) if not graphic.is_empty() else null
		meshes.append(mesh)
	return meshes


## Build the per-def ":fd" textures - retail's alpha-filtered, progressively
## gray mip chain of each model's OWN diffuse that BOTH foliage tiers bind [orig:
## Foliage_LoadDefAssets @ 0x601260 tail; bound by Foliage_DrawModelTileSlot
## @ 0x601d90 and the expanded detail tier alike]. Returns an Array parallel to `defs`;
## unsupported diffuses fall back to the raw texture (the retail filter's wrap
## masks assume pow2 dimensions of at least four), null entries stay null.
static func resolve_slot_fd_textures(resource_root: NovaResourceRoot, defs: Array) -> Array:
	var textures: Array = []
	for def in defs:
		if def == null:
			textures.append(null)
			continue
		var graphic: String = String(def.graphic)
		textures.append(load_fd_texture(resource_root, graphic) if not graphic.is_empty() else null)
	return textures


## The ":fd" texture for one graphic: the model diffuse and the witnessed
## custom mip pipeline (NovaFoliageDispatcher.bake_fd_image), cached per root+model.
static func load_fd_texture(resource_root: NovaResourceRoot, graphic: String) -> Texture2D:
	_check_epoch()
	if resource_root == null or resource_root.get_root_dir().is_empty():
		return null
	var basename: String = graphic.get_file().get_basename().to_lower()
	if basename.is_empty():
		return null
	var cache_key := _cache_key(_root_key(resource_root), basename)
	if _fd_texture_cache.has(cache_key):
		return _fd_texture_cache[cache_key]

	var diffuse := _mesh_albedo_texture(load_mesh(resource_root, graphic))
	if diffuse == null:
		return null
	var image: Image = diffuse.get_image()
	if image == null:
		return null
	image = image.duplicate()
	if image.is_compressed():
		image.decompress()
	image.convert(Image.FORMAT_RGBA8)

	var fd: Texture2D
	if NovaFoliageDispatcher.bake_fd_image(image):
		fd = ImageTexture.create_from_image(image)
	else:
		push_warning("VegAssets: '%s' diffuse dimensions are unsupported; :fd bake skipped, binding the raw diffuse." % basename)
		fd = diffuse
	_fd_texture_cache[cache_key] = fd
	return fd


static func _mesh_albedo_texture(mesh: Mesh) -> Texture2D:
	if mesh == null or mesh.get_surface_count() == 0:
		return null
	var material := mesh.surface_get_material(0) as BaseMaterial3D
	if material == null:
		return null
	return material.albedo_texture


## Resolve a graphic name (e.g. "mveg5" or "mveg5.3di") to one Mesh containing
## every surface of every LOD0 submesh built from the matching top-level .3di.
## Returns null if not resolvable.
static func load_mesh(resource_root: NovaResourceRoot, graphic: String) -> Mesh:
	_check_epoch()
	if resource_root == null or resource_root.get_root_dir().is_empty():
		return null
	var root_key := _root_key(resource_root)
	var basename: String = graphic.get_file().get_basename().to_lower()
	if basename.is_empty():
		return null
	var cache_key := _cache_key(root_key, basename)
	if _mesh_cache.has(cache_key):
		# Game_StartMission resets the logical renderer-definition registry, but
		# MainGame deliberately retains this expensive geometry cache with its
		# mounted root. A cache hit in the new mission is still a loaded shared
		# model-def node, so recreate retail's sticky foliage bit without parsing
		# or rebuilding the .3DI.
		var cached_model_path := String(_model_path_cache.get(
				cache_key, basename + ".3di"))
		NovaObjectData.mark_cached_network_challenge_foliage_model(
				cached_model_path)
		return _mesh_cache[cache_key]

	var model_path := _find_model_path(resource_root, basename)
	if model_path.is_empty():
		return null

	var data := NovaObjectData.new()
	# Retail marks foliage model-def nodes before it freezes the C2S 0x3D
	# renderer-definition snapshot; they remain renderable but are excluded from
	# that network page. [orig: sub_5B2220 writes node+0x3D4 before sub_5B3A80]
	if data.open_from_resource_root(resource_root, model_path, false) != OK:
		return null
	var submeshes: Array = data.build_lod_submeshes(0)
	var mesh: ArrayMesh = _aggregate_lod0_submeshes(submeshes)
	if mesh != null and not submeshes.is_empty():
		# Retail foliage expands the complete LOD0 model, but owns one :fd
		# binding per definition. Keep the primary submesh's diffuse only as
		# that binding's locator; it does not decide which geometry survives.
		var primary: Dictionary = submeshes[0]
		var diffuse := _load_diffuse_texture(data, int(primary.get("material_index", 0)))
		if diffuse != null:
			var mat := StandardMaterial3D.new()
			mat.albedo_texture = diffuse
			mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
			mat.alpha_scissor_threshold = 0.33
			mat.cull_mode = BaseMaterial3D.CULL_DISABLED
			mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
			mat.texture_filter = BaseMaterial3D.TEXTURE_FILTER_LINEAR_WITH_MIPMAPS
			mesh.surface_set_material(0, mat)
	if mesh != null:
		_mesh_cache[cache_key] = mesh
	return mesh


## Merge all geometry that build_lod_submeshes(0) emits. ArrayMesh surface
## arrays are copied into a private resource so callers never mutate
## NovaObjectData's shared submesh cache.
static func _aggregate_lod0_submeshes(submeshes: Array) -> ArrayMesh:
	var aggregate := ArrayMesh.new()
	for entry_value in submeshes:
		var entry: Dictionary = entry_value
		var source := entry.get("mesh") as Mesh
		if source == null:
			continue
		for source_surface in range(source.get_surface_count()):
			var arrays := source.surface_get_arrays(source_surface)
			if arrays.size() < Mesh.ARRAY_MAX:
				continue
			aggregate.add_surface_from_arrays(
				source.surface_get_primitive_type(source_surface), arrays
			)
			var surface_name: String = source.surface_get_name(source_surface)
			if not surface_name.is_empty():
				aggregate.surface_set_name(aggregate.get_surface_count() - 1, surface_name)
	return aggregate if aggregate.get_surface_count() > 0 else null


## Load the diffuse (slot 1, falling back to detail slot 2) texture for a .3di
## material, mirroring nova_object_model._load_texture_for_slot. Returns null if
## the material has no resolvable texture.
static func _load_diffuse_texture(data: NovaObjectData, material_index: int) -> Texture2D:
	var material_defs := {}
	for material in data.get_materials():
		var mi := int(material.get("material_index", material.get("index", 0)))
		material_defs[mi] = material
		var ai := int(material.get("index", mi))
		if not material_defs.has(ai):
			material_defs[ai] = material

	var material_def: Dictionary = material_defs.get(material_index, {})
	if material_def.is_empty():
		return null
	var array_index := int(material_def.get("index", -1))
	if array_index < 0:
		return null
	var textures: Array = material_def.get("textures", [])
	for want_slot in [1, 2]:
		for i in range(textures.size()):
			var texture: Dictionary = textures[i]
			if int(texture.get("slot", 0)) == want_slot:
				var loaded: Texture2D = data.load_material_texture(array_index, i)
				if loaded != null:
					return loaded
	return null


static func _find_model_path(resource_root: NovaResourceRoot, basename: String) -> String:
	var root_key := _root_key(resource_root)
	var cache_key := _cache_key(root_key, basename)
	if _model_path_cache.has(cache_key):
		return String(_model_path_cache[cache_key])
	list_graphics(resource_root)
	if _model_path_cache.has(cache_key):
		return String(_model_path_cache[cache_key])

	# Resolve through the VFS (loose or PFF). The logical name is enough: it is only
	# fed back to NovaObjectData.open_from_resource_root, which reads it through the VFS.
	var logical := basename + ".3di"
	if resource_root.has_file(logical):
		_model_path_cache[cache_key] = logical
		return logical
	return ""


static func _root_key(resource_root: NovaResourceRoot) -> String:
	# Multiple live VFS mounts may share one physical directory while selecting
	# different expansion/override chains. The global epoch invalidates remounts,
	# but it cannot distinguish two roots that remain live in the same epoch.
	# Include the root object identity so resolved paths, meshes, and :fd textures
	# never alias between those mounts.
	return '%s|%d' % [
		resource_root.get_root_dir().replace('\\', '/').rstrip('/').to_lower(),
		resource_root.get_instance_id(),
	]


static func _cache_key(root_key: String, basename: String) -> String:
	return "%s|%s" % [root_key, basename]
