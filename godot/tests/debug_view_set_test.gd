extends GutTest

const DebugViewStatus := preload(
		"res://game/debug/debug_view_status.gd")


func test_world_reports_overlay_installation_and_drawable_state() -> void:
	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame

	var initial := _status_by_id(world.get_debug_view_statuses())
	var initial_skeleton: DebugViewStatus = initial[&"show_skeletons"]
	assert_false(initial_skeleton.enabled)
	assert_false(initial_skeleton.installed)
	assert_eq(initial_skeleton.drawable_count, 0)
	assert_eq(initial_skeleton.reason, "Disabled")

	var skeleton := Skeleton3D.new()
	skeleton.add_bone("root")
	world.add_child(skeleton)
	world.set_skeleton_debug(true)
	await get_tree().process_frame
	var enabled := _status_by_id(world.get_debug_view_statuses())
	var enabled_skeleton: DebugViewStatus = enabled[&"show_skeletons"]
	assert_true(enabled_skeleton.enabled)
	assert_true(enabled_skeleton.installed)
	assert_eq(enabled_skeleton.drawable_count, 1)
	assert_eq(enabled_skeleton.reason, "Drawing 1 skeleton")

	world.unload()
	var waiting := _status_by_id(world.get_debug_view_statuses())
	var waiting_skeleton: DebugViewStatus = waiting[&"show_skeletons"]
	assert_true(waiting_skeleton.enabled)
	assert_false(waiting_skeleton.installed)
	assert_eq(waiting_skeleton.drawable_count, 0)
	assert_eq(waiting_skeleton.reason, "Waiting for a loaded world")
	await get_tree().process_frame


func test_every_installed_overlay_reports_when_it_has_no_drawable_data() -> void:
	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame

	world.set_skeleton_debug(true)
	world.set_user_point_debug(true)
	world.set_collision_debug(true)
	world.set_particle_debug(true)
	world.set_occlusion_debug(true)
	world.set_round_debug(true)
	world.set_ray_debug(true)
	world.set_hitbox_debug(true)
	world.set_ai_debug_option(&"show_ai_overlay", true)

	var expected_empty_reasons := {
		"show_skeletons": "No skeletons to draw",
		"show_user_points": "No user points to draw",
		"show_collision": "No collision shapes to draw",
		"show_effect_boxes": "No live effect bounds to draw",
		"show_portal_faces": "No portal faces in range",
		"show_round_trails": "No recent rounds to draw",
		"show_rays": "No recent rays to draw",
		"show_hit_meshes": "No hit meshes in range",
		"show_ai_overlay": "No AI brains to draw",
	}
	var status := _status_by_id(world.get_debug_view_statuses())
	assert_eq(status.size(), expected_empty_reasons.size(),
			"every F3 world-overlay option has one status row")
	for option_id in expected_empty_reasons:
		var row: DebugViewStatus = status[StringName(option_id)]
		assert_true(row.enabled, "%s reflects toggle intent" % option_id)
		assert_true(row.installed, "%s has an installed view" % option_id)
		assert_eq(row.drawable_count, 0,
				"%s distinguishes an empty view from an unknowable one" % option_id)
		assert_eq(row.reason, expected_empty_reasons[option_id])

	world.set_skeleton_debug(false)
	world.set_user_point_debug(false)
	world.set_collision_debug(false)
	world.set_particle_debug(false)
	world.set_occlusion_debug(false)
	world.set_round_debug(false)
	world.set_ray_debug(false)
	world.set_hitbox_debug(false)
	world.set_ai_debug_option(&"show_ai_overlay", false)
	await get_tree().process_frame


func _make_world() -> GameWorld:
	var world := GameWorld.new()
	var terrain := Terrain.new()
	terrain.name = "Terrain"
	world.add_child(terrain)
	return world


func _status_by_id(rows: Array[DebugViewStatus]) -> Dictionary:
	var result := {}
	for row in rows:
		result[row.id] = row
	return result
