extends RefCounted

const SEARCH_PREFIXES := [
	"res://modtools/assets/models/",
	"res://game/assets/models/",
]

static var _mesh_cache: Dictionary = {}
static var _scene_path_cache: Dictionary = {}
static var _graphics_cache: Array = []
static var _graphics_cache_valid: bool = false


## Enumerate all *veg*.glb graphics across the search paths.
## Returns an Array of dictionaries: [{"basename": String, "scene_path": String}, ...]
## Sorted by basename, deduplicated (modtools path wins when both have the same basename).
static func list_graphics(force_refresh: bool = false) -> Array:
	if _graphics_cache_valid and not force_refresh:
		return _graphics_cache.duplicate(true)

	var out: Array = []
	var seen: Dictionary = {}
	_scene_path_cache.clear()
	for prefix_value in SEARCH_PREFIXES:
		var prefix: String = String(prefix_value)
		var dir := DirAccess.open(prefix)
		if dir == null:
			continue
		dir.list_dir_begin()
		var name := dir.get_next()
		while name != "":
			if not dir.current_is_dir():
				var lower := name.to_lower()
				if lower.ends_with(".glb") and lower.contains("veg"):
					var basename := name.get_basename().to_lower()
					if not seen.has(basename):
						var scene_path := prefix + name
						seen[basename] = true
						_scene_path_cache[basename] = scene_path
						out.append({
							"basename": basename,
							"scene_path": scene_path,
						})
			name = dir.get_next()
		dir.list_dir_end()
	out.sort_custom(func(a, b): return String(a.basename) < String(b.basename))
	_graphics_cache = out
	_graphics_cache_valid = true
	return _graphics_cache.duplicate(true)


## Resolve each def's `graphic` name to the first Mesh inside its .glb.
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


## Resolve a graphic name (e.g. "mveg5", "mveg5.3di", "MVEG5.glb") to the first Mesh
## found inside the matching .glb. Returns null if not resolvable.
static func load_mesh(graphic: String) -> Mesh:
	var basename: String = graphic.get_file().get_basename().to_lower()
	if basename.is_empty():
		return null
	if _mesh_cache.has(basename):
		return _mesh_cache[basename]

	var scene_path := _find_scene_path(basename)
	if scene_path.is_empty():
		return null

	var packed: PackedScene = ResourceLoader.load(scene_path, "PackedScene", ResourceLoader.CACHE_MODE_REUSE) as PackedScene
	if packed == null:
		return null
	var root: Node = packed.instantiate()
	var mesh: Mesh = _extract_first_mesh(root)
	root.free()
	if mesh != null:
		_mesh_cache[basename] = mesh
	return mesh


static func _find_scene_path(basename: String) -> String:
	if _scene_path_cache.has(basename):
		return String(_scene_path_cache[basename])
	if not _graphics_cache_valid:
		list_graphics()
	if _scene_path_cache.has(basename):
		return String(_scene_path_cache[basename])

	for prefix_value in SEARCH_PREFIXES:
		var prefix: String = String(prefix_value)
		var lower_path := prefix + basename + ".glb"
		if ResourceLoader.exists(lower_path, "PackedScene"):
			_scene_path_cache[basename] = lower_path
			return lower_path

		var dir := DirAccess.open(prefix)
		if dir == null:
			continue
		dir.list_dir_begin()
		var name := dir.get_next()
		while name != "":
			if not dir.current_is_dir() and name.to_lower().ends_with(".glb"):
				if name.get_basename().to_lower() == basename:
					var scene_path := prefix + name
					_scene_path_cache[basename] = scene_path
					dir.list_dir_end()
					return scene_path
			name = dir.get_next()
		dir.list_dir_end()
	return ""


static func _extract_first_mesh(node: Node) -> Mesh:
	if node is MeshInstance3D:
		var mesh_instance := node as MeshInstance3D
		if mesh_instance.mesh != null:
			return mesh_instance.mesh
	for child in node.get_children():
		var mesh := _extract_first_mesh(child)
		if mesh != null:
			return mesh
	return null
