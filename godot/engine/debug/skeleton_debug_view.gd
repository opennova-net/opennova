extends Node3D

# Draws the bones of every Skeleton3D under a world subtree as a debug overlay: one line
# per bone (joint -> parent) plus a small axis cross at each joint. The 3D analog of the
# F3 overlay's read-only panes -- a developer tool for eyeballing the skeletal runtime
# (bone placement, locomotion phase sync, FP-arms posing), not engine-witnessed behavior.
#
# Everything is rebuilt into ONE ImmediateMesh every frame (the same redraw-from-live-state
# model NetEventView uses), so spawning / despawning models needs no invalidation: the next
# frame's walk simply finds the current set. Built / freed by GameWorld on the F3 overlay's
# "Show skeletons" toggle.

const MissionOverlayUtil := preload("res://engine/mission/mission_overlay_util.gd")

# Bone segment colour, and the per-joint axis-cross palette (X=red, Y=green, Z=blue), matching
# the editor gizmo convention (transform_gizmo_3d.gd).
const COL_BONE := Color(0.30, 1.0, 0.50)
const COL_AXIS_X := Color(0.95, 0.26, 0.24)
const COL_AXIS_Y := Color(0.36, 0.86, 0.30)
const COL_AXIS_Z := Color(0.28, 0.52, 0.96)
const CROSS := 0.05    # metres: half-length of each joint axis-cross arm

var _root: Node                 # the subtree walked each frame (the GameWorld)
var _mesh: ImmediateMesh
var _segments: Array = []       # [{ a: Vector3, b: Vector3, color: Color }] gathered per frame
var _drawable_count := 0        # skeletons with at least one drawable bone


# `root` is the node whose subtree is searched for Skeleton3D nodes (the GameWorld); the
# player avatar, NPC soldiers, net entities, and the FP viewmodel all live under it.
func setup(root: Node) -> void:
	_root = root
	_mesh = ImmediateMesh.new()
	var mi := MeshInstance3D.new()
	mi.name = "SkeletonDebugLines"
	mi.mesh = _mesh
	# Unshaded, vertex-coloured, depth-test off so the bones read over the character meshes
	# (the same overlay recipe as NetEventView's markers).
	var mat := MissionOverlayUtil.line_material()
	mat.no_depth_test = true
	mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	mi.material_override = mat
	add_child(mi)


func _process(_delta: float) -> void:
	if _mesh == null or _root == null or not is_instance_valid(_root):
		_drawable_count = 0
		return
	_mesh.clear_surfaces()
	_segments.clear()
	var skeletons := _collect_skeletons(_root)
	_drawable_count = 0
	for skel in skeletons:
		if skel.get_bone_count() <= 0:
			continue
		_drawable_count += 1
		_draw_skeleton(skel)
	# An empty surface is invalid -- skip emit when nothing was gathered (mirrors NetEventView).
	if _segments.is_empty():
		return
	MissionOverlayUtil.emit_line_segments(_mesh, _segments)


## Number of skeletons currently contributing geometry to this overlay.
func get_debug_drawable_count() -> int:
	return _drawable_count


# Every Skeleton3D in `node`'s subtree (recursive). A per-frame walk is cheap for the handful
# of character skeletons in a scene and stays correct as models come and go.
func _collect_skeletons(node: Node) -> Array:
	var out: Array = []
	for child in node.get_children():
		if child is Skeleton3D:
			out.append(child)
		if child.get_child_count() > 0:
			out.append_array(_collect_skeletons(child))
	return out


func _draw_skeleton(skel: Skeleton3D) -> void:
	var to_world := skel.global_transform
	var count := skel.get_bone_count()
	for i in range(count):
		var world_i := to_world * skel.get_bone_global_pose(i)
		var origin: Vector3 = world_i.origin
		var parent := skel.get_bone_parent(i)
		if parent >= 0:
			var parent_origin: Vector3 = (to_world * skel.get_bone_global_pose(parent)).origin
			_line(origin, parent_origin, COL_BONE)
		# A small axis cross at the joint: shows orientation and makes root / leaf bones visible.
		var b := world_i.basis
		_axis(origin, b.x, COL_AXIS_X)
		_axis(origin, b.y, COL_AXIS_Y)
		_axis(origin, b.z, COL_AXIS_Z)


func _line(a: Vector3, b: Vector3, color: Color) -> void:
	_segments.append({ "a": a, "b": b, "color": color })


# One arm of a joint cross: `dir` is a basis column (orientation); normalised so every arm is
# the same world length regardless of bone scale.
func _axis(origin: Vector3, dir: Vector3, color: Color) -> void:
	if dir.length() < 0.0001:
		return
	_line(origin, origin + dir.normalized() * CROSS, color)
