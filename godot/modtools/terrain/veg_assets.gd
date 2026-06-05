class_name VegAssets
extends RefCounted

const SharedVegAssets := preload("res://engine/terrain/veg_assets.gd")


static func clear_cache() -> void:
	SharedVegAssets.clear_cache()


static func list_graphics(resource_root: NovaResourceRoot, force_refresh: bool = false) -> Array:
	return SharedVegAssets.list_graphics(resource_root, force_refresh)


static func resolve_slot_meshes(resource_root: NovaResourceRoot, defs: Array) -> Array:
	return SharedVegAssets.resolve_slot_meshes(resource_root, defs)


static func load_mesh(resource_root: NovaResourceRoot, graphic: String) -> Mesh:
	return SharedVegAssets.load_mesh(resource_root, graphic)
