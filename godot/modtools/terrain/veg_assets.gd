class_name VegAssets
extends RefCounted

const SharedVegAssets := preload("res://engine/terrain/veg_assets.gd")


static func list_graphics() -> Array:
	return SharedVegAssets.list_graphics()


static func resolve_slot_meshes(defs: Array) -> Array:
	return SharedVegAssets.resolve_slot_meshes(defs)


static func load_mesh(graphic: String) -> Mesh:
	return SharedVegAssets.load_mesh(graphic)
