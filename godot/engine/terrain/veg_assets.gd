extends RefCounted

# Vegetation .3di are not bundled (neither game/ nor modtools/ ship them) — both
# the runtime and the editor set their search roots from a user-chosen resource
# directory. Empty default == no models until a root is set.
const DEFAULT_SEARCH_ROOTS: Array = []

static var _mesh_cache: Dictionary = {}
static var _model_path_cache: Dictionary = {}
static var _graphics_cache: Array = []
static var _graphics_cache_valid: bool = false
static var _search_roots: Array = DEFAULT_SEARCH_ROOTS.duplicate()


static func set_search_roots(roots: Array) -> void:
	_search_roots = []
	for root_value in roots:
		var root := _normalize_root(String(root_value))
		if not root.is_empty():
			_search_roots.append(root)
	if _search_roots.is_empty():
		_search_roots = DEFAULT_SEARCH_ROOTS.duplicate()
	clear_cache()


static func get_search_roots() -> Array:
	return _search_roots.duplicate()


static func clear_cache() -> void:
	_mesh_cache.clear()
	_model_path_cache.clear()
	_graphics_cache = []
	_graphics_cache_valid = false


## Enumerate all *veg*.3di graphics across the search roots.
## Returns dictionaries with basename/model_path, sorted by basename.
static func list_graphics(force_refresh: bool = false) -> Array:
	if _graphics_cache_valid and not force_refresh:
		return _graphics_cache.duplicate(true)

	var out: Array = []
	var seen: Dictionary = {}
	_model_path_cache.clear()
	for prefix_value in _search_roots:
		for model_path in NovaPaths.list_files(String(prefix_value), ".3di"):
			var basename := String(model_path).get_file().get_basename().to_lower()
			if not basename.contains("veg") or seen.has(basename):
				continue
			seen[basename] = true
			_model_path_cache[basename] = model_path
			out.append({
				"basename": basename,
				"model_path": model_path,
				"scene_path": model_path,
			})
	out.sort_custom(func(a, b): return String(a.basename) < String(b.basename))
	_graphics_cache = out
	_graphics_cache_valid = true
	return _graphics_cache.duplicate(true)


## Resolve each def's `graphic` name to the first Mesh built from its .3di.
## Returns an Array parallel to `defs`; null entries fall back to BoxMesh inside
## the C++ dispatcher.
static func resolve_slot_meshes(defs: Array) -> Array:
	var meshes: Array = []
	for def in defs:
		if def == null:
			meshes.append(null)
			continue
		var graphic: String = String(def.graphic)
		var mesh: Mesh = load_mesh(graphic) if not graphic.is_empty() else null
		meshes.append(mesh)
	return meshes


## Resolve a graphic name (e.g. "mveg5" or "mveg5.3di") to the first
## Mesh built from the matching .3di. Returns null if not resolvable.
static func load_mesh(graphic: String) -> Mesh:
	var basename: String = graphic.get_file().get_basename().to_lower()
	if basename.is_empty():
		return null
	if _mesh_cache.has(basename):
		return _mesh_cache[basename]

	var model_path := _find_model_path(basename)
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
		# build_lod_submeshes leaves the surface material-less, so the foliage
		# billboards render as solid white quads (no leaf texture, no alpha
		# cutout). Attach the .3di's own diffuse so the alpha-cutout cross-quads
		# read as vegetation. The dispatcher (_update_slot_material) pulls the
		# albedo off this BaseMaterial3D into foliage.gdshader, and the veg-picker
		# preview renders the mesh directly with it.
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
		_mesh_cache[basename] = mesh
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


static func _find_model_path(basename: String) -> String:
	if _model_path_cache.has(basename):
		return String(_model_path_cache[basename])
	if not _graphics_cache_valid:
		list_graphics()
	if _model_path_cache.has(basename):
		return String(_model_path_cache[basename])

	for prefix_value in _search_roots:
		var resolved := NovaPaths.resolve_file(String(prefix_value), basename + ".3di")
		if not resolved.is_empty():
			_model_path_cache[basename] = resolved
			return resolved
	return ""


static func _normalize_root(root: String) -> String:
	var clean := root.strip_edges().replace("\\", "/")
	if clean.is_empty():
		return ""
	return clean if clean.ends_with("/") else clean + "/"
