extends RefCounted

# Build Godot convex-hull shapes from a NovaObjectData collision volume.
#
# NovaObjectData.get_collision_volumes() returns one dict per parsed collision
# bounding volume (the engine's BVOL gameplay-volume families), already in Godot
# model-local space (single negate-x, matching the render mesh):
#   { type:int, flags:int, min:Vector3, max:Vector3, planes:Array[Plane],
#     part_index:int, object_index:int }
# Each volume is the convex region carved by its bounding planes, clamped to its
# AABB. We hand the resulting hull points to ConvexPolygonShape3D (Godot computes
# the hull). Shared by the Object Editor validation overlay and the mission
# workspace picking bodies so both see identical geometry.
#
# Reference via preload(), not class_name, so it resolves without an editor
# re-import (same convention as mission_object_placer.gd / veg_assets.gd).


# Collidable-type abbreviations (from the reference exporter's map_collidable_type).
# The LETTER CODES are exporter-side naming; the RUNTIME semantics are now witnessed
# (docs/world/world-wac-ai-re.md §15.4): 1 generic CB solid (the only BVOL type
# generic rays clip), 4 CL ladder contact/alignment (climb motor not ported),
# 5 contact-no-force, 6 CA armory zone (Flags 0x400000 gates weapon.mnu),
# 7 VC vehicle-collision solid (vehicle mask), 8 BB blink box (indoors),
# 9 CD door activation touch, 10 CT change-team box, 11 vehicle-loadout zone
# (Flags 0x800 gates vehicle.mnu), 12 masked, 13 CF flag/special-function touch
# (grounded only), 16/17/18 DH/DM/DL damage -50/-6/-1 HP, 19 CP player
# collision (not AI), 20..23 occlusion. [orig: Entity_ComputeBoneCollisionForce @0x4ae150 +
# Entity_TestCollisionSections @0x4aef90 + Entity_RaycastCollisionModel @0x413060]
const TYPE_NAMES := {
	1: "CB", 4: "CL", 5: "CV", 6: "CA", 7: "VC", 8: "BB", 9: "CD",
	10: "CT", 11: "CM", 12: "VK", 13: "CF", 14: "LP",
	16: "DH", 17: "DM", 18: "DL", 19: "CP",
}


static func name_for_type(type: int) -> String:
	return TYPE_NAMES.get(type, "")


# A stable, well-separated color per collidable type. The golden-ratio hue step
# keeps any type -- even ones with no known name -- visually distinct.
static func color_for_type(type: int) -> Color:
	var hue := IndexHue.hue_for_index(type)
	return Color.from_hsv(hue, 0.7, 1.0, 0.9)


static func _box_corners(vmin: Vector3, vmax: Vector3) -> PackedVector3Array:
	var pts := PackedVector3Array()
	for x in [vmin.x, vmax.x]:
		for y in [vmin.y, vmax.y]:
			for z in [vmin.z, vmax.z]:
				pts.push_back(Vector3(x, y, z))
	return pts


# Convex hull points (Godot model-local) for one volume dict. The volume's own
# bounding planes form a closed convex polytope, so the hull is exactly their
# half-space intersection -- we do NOT clamp to the AABB box (that would flatten
# carved/oriented hulls into axis-aligned boxes). A volume with too few planes, or
# whose planes come back degenerate, falls back to its AABB corners so it is never
# silently lost.
static func hull_points(volume: Dictionary) -> PackedVector3Array:
	var planes: Array[Plane] = []
	for p in volume.get("planes", []):
		if p is Plane:
			planes.append(p)
	var pts := PackedVector3Array()
	if planes.size() >= 4:
		pts = Geometry3D.compute_convex_mesh_points(planes)
	if pts.size() < 4:
		var vmin: Vector3 = volume.get("min", Vector3.ZERO)
		var vmax: Vector3 = volume.get("max", Vector3.ZERO)
		if vmin != vmax:
			pts = _box_corners(vmin, vmax)
	return pts


# One ConvexPolygonShape3D per NON-degenerate volume: a volume that yields fewer than 4 hull points
# is skipped, so the returned array may be SHORTER than `volumes` and is NOT positionally 1:1 with
# get_collision_volumes(). Callers that need a shape's owning volume must carry it explicitly rather
# than zipping the two arrays by index.
static func shapes_for(volumes: Array) -> Array:
	var shapes: Array = []
	for v in volumes:
		var pts := hull_points(v)
		if pts.size() < 4:
			continue
		var shape := ConvexPolygonShape3D.new()
		shape.points = pts
		shapes.append(shape)
	return shapes
