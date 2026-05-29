extends RefCounted

const DEFAULT_SEARCH_ROOTS := [
	"res://modtools/assets/models/",
	"res://game/assets/models/",
]

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
		var prefix: String = String(prefix_value)
		var dir := DirAccess.open(prefix)
		if dir == null:
			continue
		dir.list_dir_begin()
		var name := dir.get_next()
		while name != "":
			if not dir.current_is_dir():
				var lower := name.to_lower()
				if lower.ends_with(".3di") and lower.contains("veg"):
					var basename := name.get_basename().to_lower()
					if not seen.has(basename):
						var model_path := prefix + name
						seen[basename] = true
						_model_path_cache[basename] = model_path
						out.append({
							"basename": basename,
							"model_path": model_path,
							"scene_path": model_path,
						})
			name = dir.get_next()
		dir.list_dir_end()
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
	if mesh != null:
		_mesh_cache[basename] = mesh
	return mesh


static func _find_model_path(basename: String) -> String:
	if _model_path_cache.has(basename):
		return String(_model_path_cache[basename])
	if not _graphics_cache_valid:
		list_graphics()
	if _model_path_cache.has(basename):
		return String(_model_path_cache[basename])

	for prefix_value in _search_roots:
		var prefix: String = String(prefix_value)
		var lower_path := prefix + basename + ".3di"
		if FileAccess.file_exists(lower_path):
			_model_path_cache[basename] = lower_path
			return lower_path

		var dir := DirAccess.open(prefix)
		if dir == null:
			continue
		dir.list_dir_begin()
		var name := dir.get_next()
		while name != "":
			if not dir.current_is_dir() and name.to_lower().ends_with(".3di"):
				if name.get_basename().to_lower() == basename:
					var scene_path := prefix + name
					_model_path_cache[basename] = scene_path
					dir.list_dir_end()
					return scene_path
			name = dir.get_next()
		dir.list_dir_end()
	return ""


static func _normalize_root(root: String) -> String:
	var clean := root.strip_edges().replace("\\", "/")
	if clean.is_empty():
		return ""
	return clean if clean.ends_with("/") else clean + "/"
