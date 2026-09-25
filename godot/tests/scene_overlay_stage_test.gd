extends GutTest

# The post-particle overlay tail through the real world: GameWorld's
# scene_overlay leg gathers the frame and every view's overlay pass (composed
# right after particle pass B, before the frame effects) receives it. The
# underwater murk quad draws per VIEW from that view's own render eye, at or
# below the water, whatever the camera mode [orig:
# Terrain_RenderWorldScene @ 0x5c96c5..0x5c96f5; the tail order and
# its witnesses live in engine/runtime/renderer/scene_overlay.h]. The draw
# checks need the real rendering device and are pending under the headless
# dummy renderer.

const STAGE_TEST_ROOT := "scene_overlay_stage_test"
const SLOT_LIGHT_CORONAS := 2
const SLOT_UNDERWATER_MURK := 4
const SLOT_SUN_GLARE := 5
const SLOT_MIRROR_DIM := 6
const SLOT_MIRROR_CELESTIAL_BODIES := 7
const SLOT_MIRROR_SUN_GLOW := 8
# A stock-style sky body: an FF_ST_AD_LUM surface whose RGB generator style
# 113 reads CTRL UPL_INTENSITY (the mglare authoring), staged under the name
# the fixture environment's glare_3di line carries.
const GLARE_FIXTURE := "res://../fixtures/threedi/synth/crate_mtrl0_ad_lum_upl113.3di"
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
func _mirror_overlay_report(world: GameWorld) -> Dictionary:
	var renderer := world.get_effect_world().get_node("ParticleRenderer") as ParticleRenderer
	return renderer.get_debug_draw_list_report().get(
			"reflection_overlay_backend", {}) as Dictionary


# The water mirror closes its target in its own overlay pass: the dim over
# the finished mirror after its coronas, then the sun/moon discs and the glow
# redrawn at the mirror camera inside the far depth band [orig:
# render_main_scene @ 0x5c186c (dim), @ 0x5c18fb (render_celestial_bodies(0)),
# @ 0x5c1904 (render_skybox_sun_glow(0, 0))]. The glow's no-occlusion alpha
# follows the MIRROR camera's view of the sun.
func test_the_mirror_closes_with_the_dim_and_the_sky_redraw() -> void:
	var root_dir := WorldFixture.stage_minimal_root("scene_overlay_mirror", true)
	_staged_dirs.append(root_dir)
	for body in ["mglare.3di", "msun.3di"]:
		assert_eq(DirAccess.copy_absolute(ProjectSettings.globalize_path(GLARE_FIXTURE),
				root_dir.path_join(body)), OK)
	var world := WorldFixture.make_world(self)
	assert_eq(WorldFixture.load_mission(world, root_dir, "mnml.bms",
			func(mission: MissionData) -> void:
				assert_true(mission.set_header_string("terrain", "Tmap"))), OK)
	var camera := Camera3D.new()
	camera.position = Vector3(16, 300, -16)
	world.add_child(camera)
	camera.make_current()
	var env := world.get_environment_node() as MissionEnvironment
	var water := world.get_node("Water") as Water
	water.set_mission_water_height_override(10.0)
	var sun_dir: Vector3 = env.get_sun_direction()
	# Look at the sun's image in the water: the mirrored eye then faces the sun.
	var toward_image := Vector3(sun_dir.x, -absf(sun_dir.y), sun_dir.z).normalized()
	var up := Vector3.RIGHT if absf(toward_image.y) > 0.9 else Vector3.UP
	camera.look_at(camera.global_position + toward_image, up)
	await get_tree().process_frame
	world.tick(camera.global_position, camera.get_global_transform(), 0.02)
	world.tick(camera.global_position, camera.get_global_transform(), 0.02)
	var report := _mirror_overlay_report(world)
	var submitted: Array = report.get("submitted_slots", []) as Array
	assert_eq(String(report.get("view_kind", "")), "mirror")
	assert_true(submitted.has(SLOT_MIRROR_DIM), "the mirror's dim reaches its overlay pass")
	assert_true(submitted.has(SLOT_MIRROR_CELESTIAL_BODIES),
			"the sun disc is redrawn after the dim")
	assert_true(submitted.has(SLOT_MIRROR_SUN_GLOW),
			"the mirrored eye faces the sun: the glow is redrawn")
	var main_report := _overlay_report(world)
	assert_false((main_report.get("drawn_slots", []) as Array).has(SLOT_MIRROR_DIM),
			"the scene view never draws the mirror's slots")
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	report = _mirror_overlay_report(world)
	var drawn: Array = report.get("drawn_slots", []) as Array
	assert_true(drawn.has(SLOT_MIRROR_DIM), "the mirror draws its dim")
	assert_true(drawn.has(SLOT_MIRROR_CELESTIAL_BODIES), "and the far-band disc redraw")
	assert_true(drawn.has(SLOT_MIRROR_SUN_GLOW), "and the far-band glow")
	assert_true(drawn.find(SLOT_MIRROR_DIM) < drawn.find(SLOT_MIRROR_SUN_GLOW),
			"the glow follows the dim")


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


func _meshes(node: Node, out: Array[MeshInstance3D]) -> void:
	if node is MeshInstance3D:
		out.append(node as MeshInstance3D)
	for child in node.get_children():
		_meshes(child, out)


# The sun glare closes the tail: the stage takes the glow model out of every
# camera and draws its SELFLUM surfaces after the murk, under the forced
# 1.0 light scale [orig: Terrain_RenderWorldScene @ 0x5c96fd..0x5c9722,
# render_skybox_sun_glow(1, 1) @ 0x5c9714].
func test_the_sun_glare_draws_in_the_overlay_pass_and_leaves_the_cameras() -> void:
	var root_dir := WorldFixture.stage_minimal_root("scene_overlay_glare", true)
	_staged_dirs.append(root_dir)
	assert_eq(DirAccess.copy_absolute(ProjectSettings.globalize_path(GLARE_FIXTURE),
			root_dir.path_join("mglare.3di")), OK)
	var world := WorldFixture.make_world(self)
	assert_eq(WorldFixture.load_mission(world, root_dir, "mnml.bms",
			func(mission: MissionData) -> void:
				assert_true(mission.set_header_string("terrain", "Tmap"))), OK)
	var camera := Camera3D.new()
	camera.position = Vector3(16, 300, -16)
	world.add_child(camera)
	camera.make_current()
	var celestial := world.get_celestial_node() as Celestial
	var env := world.get_environment_node() as MissionEnvironment
	assert_not_null(celestial)
	assert_not_null(env)
	if celestial == null or env == null:
		return
	var sun_dir: Vector3 = env.get_sun_direction()
	var up := Vector3.RIGHT if absf(sun_dir.y) > 0.9 else Vector3.UP
	camera.look_at(camera.global_position + sun_dir, up)
	await get_tree().process_frame
	celestial.settle_glare_occlusion()
	world.tick(camera.global_position, camera.get_global_transform(), 0.02)
	var glare := celestial.get_overlay_body_node("glare")
	assert_not_null(glare, "the fixture environment names a glare model")
	if glare == null:
		return
	var facing: Dictionary = (celestial.get_diagnostics().get("bodies", {}) as Dictionary).get("glare", {})
	assert_true(bool(facing.get("drawn", false)), "facing the unoccluded sun submits the glow")
	var meshes: Array[MeshInstance3D] = []
	_meshes(glare, meshes)
	assert_gt(meshes.size(), 0)
	for mesh in meshes:
		assert_eq(mesh.layers, 0, "the stage owns the glow draw: no camera sees the mesh")
	var report := _overlay_report(world)
	assert_true((report.get("submitted_slots", []) as Array).has(SLOT_SUN_GLARE),
			"the glow reaches the overlay submission")
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	report = _overlay_report(world)
	assert_true((report.get("drawn_slots", []) as Array).has(SLOT_SUN_GLARE),
			"the scene view draws the glow last")

	# Looking away: no glow submit, no glare batch.
	camera.look_at(camera.global_position - sun_dir, up)
	await get_tree().process_frame
	world.tick(camera.global_position, camera.get_global_transform(), 0.02)
	report = _overlay_report(world)
	assert_false((report.get("submitted_slots", []) as Array).has(SLOT_SUN_GLARE),
			"a glow retail skips at a non-positive alpha leaves the tail")
