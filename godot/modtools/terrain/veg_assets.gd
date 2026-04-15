class_name VegAssets
extends RefCounted

const SEARCH_PREFIXES := [
	"res://modtools/assets/models/",
	"res://game/assets/models/",
]

static var _mesh_cache: Dictionary = {}


## Enumerate all *veg*.glb graphics across the search paths.
## Returns an Array of dictionaries: [{"basename": String, "scene_path": String}, ...]
## Sorted by basename, deduplicated (modtools path wins when both have the same basename).
static func list_graphics() -> Array:
	var out: Array = []
	var seen: Dictionary = {}
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
						seen[basename] = true
						out.append({
							"basename": basename,
							"scene_path": prefix + name,
						})
			name = dir.get_next()
		dir.list_dir_end()
	out.sort_custom(func(a, b): return String(a.basename) < String(b.basename))
	return out


## Resolve a graphic name (e.g. "mveg5", "mveg5.3di", "MVEG5.glb") to the first Mesh
## found inside the matching .glb. Returns null if not resolvable.
static func load_mesh(graphic: String) -> Mesh:
	var basename: String = graphic.get_file().get_basename().to_lower()
	if basename.is_empty():
		return null
	if _mesh_cache.has(basename):
		return _mesh_cache[basename]
	for prefix_value in SEARCH_PREFIXES:
		var prefix: String = String(prefix_value)
		var scene_path: String = prefix + basename + ".glb"
		if not ResourceLoader.exists(scene_path, "PackedScene"):
			continue
		var packed: PackedScene = ResourceLoader.load(scene_path, "PackedScene", ResourceLoader.CACHE_MODE_REUSE) as PackedScene
		if packed == null:
			continue
		var root: Node = packed.instantiate()
		var mesh: Mesh = _extract_first_mesh(root)
		root.free()
		if mesh != null:
			_mesh_cache[basename] = mesh
			return mesh
	return null


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
