extends GutTest

# The F3 overlays' projection (engine overlay_camera.h through
# DevTools.project_mission_point): a mission-frame point projects exactly
# where Godot's own Camera3D.unproject_position puts its Godot-space image
# (mission (x, y, z) -> Godot (x, z, -y)), and a point behind the camera
# projects nowhere. Both flavours carry the seam, so this runs headless.


func _camera_in_viewport(size: Vector2i) -> Camera3D:
	var viewport := SubViewport.new()
	viewport.size = size
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.fov = 70.0
	camera.near = 0.05
	camera.far = 4000.0
	viewport.add_child(camera)
	camera.current = true
	return camera


func _godot_of(mission: Vector3) -> Vector3:
	return Vector3(mission.x, mission.z, -mission.y)


func test_projection_matches_unproject_position() -> void:
	var camera := _camera_in_viewport(Vector2i(640, 360))
	camera.global_transform = Transform3D(Basis.from_euler(Vector3(-0.2, 0.7, 0.05)),
			Vector3(12.0, 4.0, -30.0))
	var points := [
		Vector3(20.0, 40.0, 2.0),
		Vector3(-5.0, 35.0, 10.0),
		Vector3(30.0, 60.0, -3.0),
	]
	var checked := 0
	for mission in points:
		var godot_point := _godot_of(mission)
		if camera.is_position_behind(godot_point):
			continue
		var expected := camera.unproject_position(godot_point)
		var actual := DevTools.project_mission_point(camera, mission)
		assert_almost_eq(actual.x, expected.x, 0.01, "x of %s" % mission)
		assert_almost_eq(actual.y, expected.y, 0.01, "y of %s" % mission)
		checked += 1
	assert_gt(checked, 0, "at least one point sits in front of the camera")


func test_behind_the_camera_projects_nowhere() -> void:
	var camera := _camera_in_viewport(Vector2i(320, 240))
	camera.global_transform = Transform3D(Basis(), Vector3.ZERO)  # looks down Godot -Z (mission +y)
	var ahead := DevTools.project_mission_point(camera, Vector3(0.0, 10.0, 0.0))
	assert_almost_eq(ahead.x, 160.0, 0.01, "straight ahead is the centre column")
	assert_almost_eq(ahead.y, 120.0, 0.01, "straight ahead is the centre row")
	var behind := DevTools.project_mission_point(camera, Vector3(0.0, -10.0, 0.0))
	assert_true(is_nan(behind.x) and is_nan(behind.y), "a point behind the camera is NaN")
