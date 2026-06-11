extends GutTest

# Exercises the framework transform gizmo's pure math
# (modtools/framework/transform_gizmo_3d.gd) headlessly: ring-axis derivation,
# screen-space handle hit-testing, axis-constrained translate, ring rotation, and snapping.
# A real Camera3D in an own-world SubViewport drives project/unproject, so the projection
# path is the real one. The gizmo is placed at the origin and viewed straight down (-Y),
# which puts the X/Z translate axes and the yaw ring in the y=0 plane, making the expected
# world points easy to state.
#
# The fixture injects mission's bms basis_builder, so every pre-lift assertion still runs
# against the mission rotation convention — the lift is behavior-preserving by these tests.
# The default (plain euler) builder has its own cases at the bottom.

const MissionGizmo = preload("res://modtools/framework/transform_gizmo_3d.gd")
const MissionObjectPlacer = preload("res://engine/mission/mission_object_placer.gd")

var _sub: SubViewport
var _cam: Camera3D
var _giz


func before_each() -> void:
	_sub = SubViewport.new()
	_sub.own_world_3d = true
	_sub.world_3d = World3D.new()
	_sub.size = Vector2i(800, 600)
	add_child_autofree(_sub)
	_cam = Camera3D.new()
	# Top-down: hover above the origin looking straight down -Y.
	_cam.position = Vector3(0, 100, 0)
	_cam.rotation = Vector3(-PI / 2.0, 0.0, 0.0)
	_cam.current = true
	_sub.add_child(_cam)
	_giz = MissionGizmo.new()
	_giz.basis_builder = MissionObjectPlacer.bms_to_godot_basis
	_sub.add_child(_giz)
	_giz.visible = true
	_giz.show_for(Vector3.ZERO, Vector3.ZERO)


func test_yaw_ring_axis_is_world_up() -> void:
	# With no authored rotation, the yaw ring (component 1) spins about world up, with unit
	# sensitivity (1 swept degree == 1 yaw degree).
	assert_almost_eq(absf((_giz._ring_axis[1] as Vector3).dot(Vector3.UP)), 1.0, 0.001)
	assert_almost_eq(float(_giz._ring_sens[1]), 1.0, 0.01)


func test_show_for_positions_gizmo() -> void:
	_giz.show_for(Vector3(3.0, 4.0, 5.0), Vector3.ZERO)
	assert_eq(_giz.position, Vector3(3.0, 4.0, 5.0))


func test_scale_grows_with_camera_distance() -> void:
	await get_tree().process_frame
	var near: float = _giz._scale_for(_cam)
	_cam.position = Vector3(0, 300, 0)
	await get_tree().process_frame
	var far: float = _giz._scale_for(_cam)
	assert_gt(far, near, "the gizmo scales up as the camera pulls back (constant pixel size)")


func test_pick_returns_x_axis_at_its_arrow() -> void:
	await get_tree().process_frame
	await get_tree().process_frame
	var sc: float = _giz._scale_for(_cam)
	var mid := Vector3.RIGHT * MissionGizmo.ARROW_LEN * sc * 0.5
	var handle: Dictionary = _giz.pick_handle(_cam, _cam.unproject_position(mid))
	assert_eq(String(handle.get("part", "")), "translate")
	assert_eq(int(handle.get("axis", -1)), 0)


func test_pick_misses_far_from_gizmo() -> void:
	await get_tree().process_frame
	var m := _cam.unproject_position(Vector3.ZERO) + Vector2(320, 260)
	assert_true(_giz.pick_handle(_cam, m).is_empty(), "a cursor far from every handle returns {}")


func test_translate_x_drag_is_axis_constrained_and_linear() -> void:
	await get_tree().process_frame
	await get_tree().process_frame
	# Both points lie in the y=0 drag plane; the X drag should report exactly their X difference,
	# with zero Y/Z (translate is constrained to the grabbed axis).
	_giz.begin({ "part": "translate", "axis": 0 }, _cam, _cam.unproject_position(Vector3(5, 0, 3)))
	var d: Dictionary = _giz.update(_cam, _cam.unproject_position(Vector3(15, 0, 3)))
	assert_true(d.has("translate"))
	var tr: Vector3 = d["translate"]
	assert_almost_eq(tr.x, 10.0, 0.05)
	assert_almost_eq(tr.y, 0.0, 0.0001)
	assert_almost_eq(tr.z, 0.0, 0.0001)


func test_translate_zero_when_cursor_unmoved() -> void:
	await get_tree().process_frame
	await get_tree().process_frame
	var m := _cam.unproject_position(Vector3(7, 0, -2))
	_giz.begin({ "part": "translate", "axis": 2 }, _cam, m)
	var tr: Vector3 = (_giz.update(_cam, m) as Dictionary)["translate"]
	assert_almost_eq(tr.length(), 0.0, 0.01, "no cursor movement => no translation")


func test_yaw_ring_drag_rotates_about_world_up() -> void:
	await get_tree().process_frame
	await get_tree().process_frame
	# Grab the yaw ring at +X and drag to +Z (both in the y=0 ring plane): a quarter turn.
	_giz.begin({ "part": "rotate", "axis": 1 }, _cam, _cam.unproject_position(Vector3(10, 0, 0)))
	var d: Dictionary = _giz.update(_cam, _cam.unproject_position(Vector3(0, 0, 10)))
	assert_true(d.has("rotate_deg"))
	var deg: float = d["rotate_deg"]
	assert_almost_eq(absf(deg), 90.0, 0.5)
	# Applying that yaw delta really rotates the entity basis about world up by ~90 deg.
	var basis := MissionObjectPlacer.bms_to_godot_basis(Vector3(0, deg, 0))
	var basis0 := MissionObjectPlacer.bms_to_godot_basis(Vector3.ZERO)
	var dq := (basis * basis0.inverse()).get_rotation_quaternion()
	assert_almost_eq(absf(dq.get_axis().dot(Vector3.UP)), 1.0, 0.01)
	assert_almost_eq(rad_to_deg(dq.get_angle()), 90.0, 0.5)


func test_translate_drag_tracks_cursor_across_frames() -> void:
	# Replays the controller's per-frame feedback: after each update it moves the gizmo to ride the
	# object via set_origin. update() must measure against the FROZEN grab origin, so the reported
	# delta stays the absolute distance from the grab point regardless of where the gizmo now sits.
	# (The single-update test cannot catch the collapse this guards against.)
	await get_tree().process_frame
	await get_tree().process_frame
	var start := Vector3.ZERO
	_giz.begin({ "part": "translate", "axis": 0 }, _cam, _cam.unproject_position(Vector3(5, 0, 0)))
	var d1: Dictionary = _giz.update(_cam, _cam.unproject_position(Vector3(10, 0, 0)))
	_giz.set_origin(start + (d1["translate"] as Vector3))
	var d2: Dictionary = _giz.update(_cam, _cam.unproject_position(Vector3(20, 0, 0)))
	_giz.set_origin(start + (d2["translate"] as Vector3))
	# Grab at X=5, final cursor at X=20 -> absolute delta +15, not the +10 a live-origin read gives.
	assert_almost_eq((d1["translate"] as Vector3).x, 5.0, 0.05)
	assert_almost_eq((d2["translate"] as Vector3).x, 15.0, 0.05)


func test_yaw_ring_drag_can_exceed_180_degrees() -> void:
	await get_tree().process_frame
	await get_tree().process_frame
	# Sweep ~200 deg in two <180 steps; the unwrap must accumulate past the atan2 wrap point.
	_giz.begin({ "part": "rotate", "axis": 1 }, _cam, _cam.unproject_position(Vector3(10, 0, 0)))
	_giz.update(_cam, _cam.unproject_position(Vector3(10.0 * cos(deg_to_rad(100.0)), 0.0, 10.0 * sin(deg_to_rad(100.0)))))
	var d: Dictionary = _giz.update(_cam, _cam.unproject_position(Vector3(10.0 * cos(deg_to_rad(200.0)), 0.0, 10.0 * sin(deg_to_rad(200.0)))))
	assert_gt(absf(float(d["rotate_deg"])), 180.0, "a single ring drag can pass 180 deg without sign-flipping")
	assert_almost_eq(absf(float(d["rotate_deg"])), 200.0, 1.0)


func test_pitch_ring_sign_and_magnitude() -> void:
	# Pitch/roll ring axes are horizontal (their drag planes are vertical), so view from a 3/4 angle
	# rather than the straight-down default, which would be edge-on to those planes.
	await _angle_camera()
	# Exercise the pitch ring in a non-trivial (yaw-rotated) frame, where its world axis is nested.
	var base := Vector3(0, 40, 0)
	_giz.show_for(Vector3.ZERO, base)
	var deg := _sweep_ring(0, 5.0)
	assert_almost_eq(deg, 5.0, 0.5, "pitch ring measures the swept angle (unit sensitivity)")
	var r := base
	r.x += deg
	_assert_rotates_about(base, r, _giz._ring_axis[0], 5.0)


func test_roll_ring_sign_and_magnitude() -> void:
	await _angle_camera()
	var base := Vector3(0, 40, 0)
	_giz.show_for(Vector3.ZERO, base)
	var deg := _sweep_ring(2, 5.0)
	assert_almost_eq(deg, 5.0, 0.5, "roll ring measures the swept angle (unit sensitivity)")
	var r := base
	r.z += deg
	_assert_rotates_about(base, r, _giz._ring_axis[2], 5.0)


# Move the camera to a 3/4 overhead angle (so no ring plane is edge-on) and settle a frame.
func _angle_camera() -> void:
	_cam.position = Vector3(60, 80, 60)
	_cam.look_at(Vector3.ZERO, Vector3.UP)
	await get_tree().process_frame
	await get_tree().process_frame


# Grab ring `axis_idx` at a point on its plane and sweep `sweep_deg` about its axis; returns the
# gizmo's reported authored-degree delta.
func _sweep_ring(axis_idx: int, sweep_deg: float) -> float:
	var a: Vector3 = _giz._ring_axis[axis_idx]
	var u: Vector3 = _giz._perp(a)
	var w: Vector3 = a.cross(u).normalized()
	var sr := deg_to_rad(sweep_deg)
	var p0 := u * 10.0
	var p1 := (u * cos(sr) + w * sin(sr)) * 10.0
	_giz.begin({ "part": "rotate", "axis": axis_idx }, _cam, _cam.unproject_position(p0))
	return float((_giz.update(_cam, _cam.unproject_position(p1)) as Dictionary)["rotate_deg"])


# Applying the move from `base` -> `applied` must rotate the entity about +`axis` (same sense the
# cursor swept) by ~`expect_deg`.
func _assert_rotates_about(base: Vector3, applied: Vector3, axis: Vector3, expect_deg: float) -> void:
	var dq := (MissionObjectPlacer.bms_to_godot_basis(applied) * MissionObjectPlacer.bms_to_godot_basis(base).inverse()).get_rotation_quaternion()
	assert_gt(dq.get_axis().dot(axis), 0.0, "object rotates the same way the cursor swept")
	assert_almost_eq(rad_to_deg(dq.get_angle()), expect_deg, 0.5)


# --- Default (plain euler) basis builder + snapping -----------------------------

func test_default_builder_ring_axes_are_world_axes() -> void:
	# With no basis_builder injected and no authored rotation, each ring spins about its
	# matching world axis with unit sensitivity — the framework default needs no domain.
	var giz := MissionGizmo.new()
	_sub.add_child(giz)
	giz.show_for(Vector3.ZERO, Vector3.ZERO)
	for i in 3:
		var expected: Vector3 = [Vector3.RIGHT, Vector3.UP, Vector3.BACK][i]
		assert_almost_eq(absf((giz._ring_axis[i] as Vector3).dot(expected)), 1.0, 0.001)
		assert_almost_eq(float(giz._ring_sens[i]), 1.0, 0.01)
	giz.free()


func test_default_builder_rings_parallel_to_injected_at_identity() -> void:
	# At zero authored rotation both conventions spin each ring about the same world
	# LINE — the bms convention's axes point the opposite way (e.g. +yaw is a -Y
	# rotation via RotY(90 - yaw)) with the sign packed into the axis, so the
	# invariant is parallelism, not equality. Ring geometry (the visible circles)
	# is therefore identical across builders.
	var giz := MissionGizmo.new()
	_sub.add_child(giz)
	giz.show_for(Vector3.ZERO, Vector3.ZERO)
	for i in 3:
		var dot := absf((giz._ring_axis[i] as Vector3).dot(_giz._ring_axis[i] as Vector3))
		assert_almost_eq(dot, 1.0, 0.001)
	giz.free()


func test_translate_snap_quantizes_axis_delta() -> void:
	await get_tree().process_frame
	await get_tree().process_frame
	_giz.translate_snap = 2.0
	_giz.begin({ "part": "translate", "axis": 0 }, _cam, _cam.unproject_position(Vector3(5, 0, 3)))
	var d: Dictionary = _giz.update(_cam, _cam.unproject_position(Vector3(10.7, 0, 3)))
	# Raw delta ~5.7 snaps to the nearest multiple of 2.
	assert_almost_eq((d["translate"] as Vector3).x, 6.0, 0.001)


func test_rotate_snap_quantizes_swept_degrees() -> void:
	await get_tree().process_frame
	await get_tree().process_frame
	_giz.rotate_snap_deg = 15.0
	_giz.begin({ "part": "rotate", "axis": 1 }, _cam, _cam.unproject_position(Vector3(10, 0, 0)))
	var d: Dictionary = _giz.update(_cam, _cam.unproject_position(Vector3(10.0 * cos(deg_to_rad(100.0)), 0.0, 10.0 * sin(deg_to_rad(100.0)))))
	# Raw sweep ~100 deg snaps to the nearest multiple of 15.
	assert_almost_eq(absf(float(d["rotate_deg"])), 105.0, 0.001)


func test_snaps_default_off() -> void:
	# Fresh gizmo: both snaps are 0.0 = disabled, so deltas pass through unquantized
	# (the mission adoption relies on this being inert).
	var giz := MissionGizmo.new()
	assert_eq(giz.translate_snap, 0.0)
	assert_eq(giz.rotate_snap_deg, 0.0)
	giz.free()
