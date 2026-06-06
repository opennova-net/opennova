extends RefCounted

# Build Godot convex-hull shapes from a NovaObjectData collision volume.
#
# NovaObjectData.get_collision_volumes() returns one dict per parsed collision
# bounding volume (the engine's CB/CC collidable primitives), already in Godot
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


# Tentative collidable-type abbreviations (from the reference exporter's
# map_collidable_type; the meanings are inferred, hence the trailing "?" callers add).
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
	var hue := fposmod(float(type) * 0.61803398875, 1.0)
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


# One ConvexPolygonShape3D per volume (volumes that come out degenerate are
# skipped). Order is preserved so callers can correlate with get_collision_volumes().
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
