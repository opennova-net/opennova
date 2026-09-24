extends GutTest

# The post-particle overlay tail through the real world: GameWorld's
# scene_overlay leg gathers the frame and every view's overlay pass (composed
# right after particle pass B, before the frame effects) receives it. The
# underwater murk quad draws per VIEW from that view's own render eye, at or
# below the water, whatever the camera mode [orig:
# Terrain_RenderSceneWithReflection @ 0x5c96c5..0x5c96f5; the tail order and
# its witnesses live in engine/runtime/renderer/scene_overlay.h]. The draw
# checks need the real rendering device and are pending under the headless
# dummy renderer.

const STAGE_TEST_ROOT := "scene_overlay_stage_test"
const SLOT_LIGHT_CORONAS := 2
const SLOT_UNDERWATER_MURK := 4
# A slow round whose in-flight glow light (ammo light_move) stays within the
# corona walk's 100-unit admission in front of the camera.
const GLOW_AMMO_DEF := """
ammo AT_NULL
	velocity            0
	max_age             0
	drag                1
	min_damage          0
	max_damage          0
end

ammo AM_556MM
	velocity            1
	max_age             300
	drag                1
	weight_in_grains    62
	min_damage          25
	max_damage          40
	light_move          6.0 255 255 255
end
"""


var _staged_dirs: Array[String] = []


func after_each() -> void:
	TestFs.remove_dir_recursive(OS.get_cache_dir().path_join(STAGE_TEST_ROOT))
	for dir in _staged_dirs:
		TestFs.remove_dir_recursive(dir)
	_staged_dirs.clear()


func _loaded_world(camera: Camera3D) -> GameWorld:
	var packed := load("res://game/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	world.add_child(camera)
	add_child_autofree(world)
	world.set_playable(false)
	var root_dir := OS.get_cache_dir().path_join(STAGE_TEST_ROOT).path_join(
			"world_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(root_dir)
	for source_dir in [
		ProjectSettings.globalize_path("res://../fixtures/terrain/tmap"),
		ProjectSettings.globalize_path(RuntimeFixture.directory()),
	]:
		for file_name in DirAccess.get_files_at(source_dir):
			assert_eq(DirAccess.copy_absolute(
					source_dir.path_join(file_name), root_dir.path_join(file_name)), OK)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(root_dir), OK)
	world.set_resource_root(root)
	var mission := MissionData.new()
	assert_eq(mission.open_from_resource_root(root, "mnml.bms"), OK)
	assert_true(mission.set_header_string("terrain", "Tmap"))
	assert_true(mission.set_header_string("environment", "mnml"))
	assert_eq(world.load_mission_data(mission, "mnml.bms"), OK)
	return world


func _overlay_report(world: GameWorld) -> Dictionary:
	var renderer := world.get_effect_world().get_node("ParticleRenderer") as ParticleRenderer
	return renderer.get_debug_draw_list_report().get("world_overlay_backend", {}) as Dictionary


func test_the_murk_rides_the_overlay_pass_of_the_eye_below_the_water() -> void:
	var camera := Camera3D.new()
	camera.position = Vector3(0, 71, 0)
	camera.current = true
	var world := _loaded_world(camera)
	await get_tree().process_frame
	await get_tree().process_frame
	var water := world.get_node("Water") as Water
	water.set_mission_water_height_override(10.0)

	# Above the water the frame still carries the murk batch (its side test
	# is per view), so the submission holds it either way.
	world.tick(camera.global_position, camera.get_global_transform())
	var report := _overlay_report(world)
	assert_true((report.get("submitted_slots", []) as Array).has(SLOT_UNDERWATER_MURK),
			"an active water plane hands every view the murk batch")
	assert_eq(String(report.get("view_kind", "")), "scene")

	var rendering := RenderingServer.get_rendering_device() != null
	if rendering:
		RenderingServer.force_draw(true)
		RenderingServer.force_sync()
		report = _overlay_report(world)
		assert_false((report.get("drawn_slots", []) as Array).has(SLOT_UNDERWATER_MURK),
				"an eye strictly above the water draws no murk")
		assert_gt(int(report.get("gated_batches", 0)), 0)

	# The eye exactly on the plane: the scissor's jg skips only a higher eye.
	camera.position.y = 10.0
	# The camera's server transform follows on the frame's transform flush.
	await get_tree().process_frame
	world.tick(camera.global_position, camera.get_global_transform())
	if rendering:
		RenderingServer.force_draw(true)
		RenderingServer.force_sync()
		report = _overlay_report(world)
		assert_true((report.get("drawn_slots", []) as Array).has(SLOT_UNDERWATER_MURK),
				"an eye at the waterline draws the murk")
	else:
		pending("RenderingDevice unavailable under this Godot renderer")

	world.unload()


# The coronas ride the same tail: a live light's billboards reach the scene
# view's overlay pass after particle pass B, and the mirror's overlay pass
# admits them too [orig: EffectWorld_RenderLightCoronas(1) @ 0x5c96ad and
# @ 0x5c85fd].
func test_a_live_light_corona_draws_in_the_overlay_pass() -> void:
	var root_dir := WorldFixture.stage_minimal_root("scene_overlay_corona", true,
			{"ammo.def": GLOW_AMMO_DEF})
	_staged_dirs.append(root_dir)
	var world := WorldFixture.make_world(self)
	assert_eq(WorldFixture.load_mission(world, root_dir, "mnml.bms",
			func(mission: MissionData) -> void:
				assert_true(mission.set_header_string("terrain", "Tmap"))), OK)
	var camera := Camera3D.new()
	camera.position = Vector3(16, 300, -16)
	world.add_child(camera)
	camera.make_current()
	await get_tree().process_frame
	assert_gte(int(world.get_sim().debug_spawn_round(
			camera.position + Vector3(0, 0, -10), Vector3.FORWARD, "AM_556MM")), 0)
	world.tick(camera.global_position, camera.get_global_transform(), 0.02)
	assert_eq(world.get_effect_light_report().live, 1, "the round glow is live")
	var report := _overlay_report(world)
	assert_true((report.get("submitted_slots", []) as Array).has(SLOT_LIGHT_CORONAS),
			"the corona billboards reach the overlay submission")
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	report = _overlay_report(world)
	assert_true((report.get("drawn_slots", []) as Array).has(SLOT_LIGHT_CORONAS),
			"the scene view draws the coronas")
