extends RefCounted

# Vegetation .3di are not bundled. Runtime and editor callers pass the same
# flat NovaResourceRoot used for terrain/env/credits, so there is no process
# global search-root state.

static var _mesh_cache: Dictionary = {}
static var _model_path_cache: Dictionary = {}
static var _graphics_cache_by_root: Dictionary = {}


static func clear_cache() -> void:
	_mesh_cache.clear()
	_model_path_cache.clear()
	_graphics_cache_by_root.clear()


## Enumerate all top-level *veg*.3di graphics in the resource root.
## Returns dictionaries with basename/model_path, sorted by basename.
static func list_graphics(resource_root: NovaResourceRoot, force_refresh: bool = false) -> Array:
	if resource_root == null or resource_root.get_root_dir().is_empty():
		return []
	var root_key := _root_key(resource_root)
	if not force_refresh and _graphics_cache_by_root.has(root_key):
		return _graphics_cache_by_root[root_key].duplicate(true)

	var out: Array = []
	var seen: Dictionary = {}
	for model_path in resource_root.list_files(".3di"):
		var basename := String(model_path).get_file().get_basename().to_lower()
		if not basename.contains("veg") or seen.has(basename):
			continue
		seen[basename] = true
		_model_path_cache[_cache_key(root_key, basename)] = model_path
		out.append({
			"basename": basename,
			"model_path": model_path,
			"scene_path": model_path,
		})
	out.sort_custom(func(a, b): return String(a.basename) < String(b.basename))
	_graphics_cache_by_root[root_key] = out
	return out.duplicate(true)


## Resolve each def's `graphic` name to the first Mesh built from its .3di.
## Returns an Array parallel to `defs`; null entries fall back to BoxMesh inside
## the C++ dispatcher.
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


## Resolve a graphic name (e.g. "mveg5" or "mveg5.3di") to the first
## Mesh built from the matching top-level .3di. Returns null if not resolvable.
static func load_mesh(resource_root: NovaResourceRoot, graphic: String) -> Mesh:
	if resource_root == null or resource_root.get_root_dir().is_empty():
		return null
	var root_key := _root_key(resource_root)
	var basename: String = graphic.get_file().get_basename().to_lower()
	if basename.is_empty():
		return null
	var cache_key := _cache_key(root_key, basename)
	if _mesh_cache.has(cache_key):
		return _mesh_cache[cache_key]

	var model_path := _find_model_path(resource_root, basename)
	if model_path.is_empty():
		return null

	var data := NovaObjectData.new()
	if data.open_file(model_path) != OK:
		return null
	var submeshes: Array = data.build_lod_submeshes(0)
	var mesh: Mesh = null
	if not submeshes.is_empty():
		var first: Dictionary = submeshes[0]
		mesh = first.get("mesh") as Mesh
		if mesh != null and mesh.get_surface_count() > 0:
			var diffuse := _load_diffuse_texture(data, int(first.get("material_index", 0)))
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

	var resolved := resource_root.resolve_file(basename + ".3di")
	if not resolved.is_empty():
		_model_path_cache[cache_key] = resolved
		return resolved
	return ""


static func _root_key(resource_root: NovaResourceRoot) -> String:
	return resource_root.get_root_dir().replace("\\", "/").rstrip("/").to_lower()


static func _cache_key(root_key: String, basename: String) -> String:
	return "%s|%s" % [root_key, basename]
