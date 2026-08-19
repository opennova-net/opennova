extends GutTest

const FirePresentPass := preload("res://game/world/fire_present_pass.gd")
const DestructionPresentPass := preload(
		"res://game/world/destruction_present_pass.gd")

# The EffectWorld dynamic point-light wiring (D-RLIT-4): the LightScene
# binding round trip, the witnessed <= 4 select + global-parameter push, and
# the EffectLightDirector spawn walk. The portable semantics themselves are
# pinned by ctest renderer_light_scene; this file covers the Godot seam.


# The static walk runs against real committed models: Shed.3di carries one
# authored light record (style 24, atten 0..3 at (-0.0017, 1.2097, 0.0112)),
# House.3di carries none.
func _fixture_object_data(model: String) -> ObjectData:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/3di3")), OK)
	var data := ObjectData.new()
	assert_eq(data.open_from_resource_root(root, model), OK,
			"%s loads as a static light source fixture" % model)
	return data


func _barrel_light_info() -> Dictionary:
	# The FireBrl3 shape: one style-113 light, atten 0..8, 1.12 above base.
	return {
		"position": Vector3(0.0004, 1.1157, -0.0044),
		"atten_start": 0.0,
		"atten_end": 8.0,
		"color_start": Color(1.0, 0.78, 0.47),
		"color_end": Color(0.31, 0.16, 0.04),
		"colorgen_style": 113,
		"colorgen_phase": 0,
		"colorgen_rate": 0,
		"disable_corona": false,
		"disable_lightterrain": false,
		"disable_lightobjects": false,
	}


func test_spawn_select_and_global_push_round_trip() -> void:
	var scene := LightScene.new()
	var handle := scene.spawn_model_light({
		"position": Vector3(4.0, 1.0, -2.0),
		"atten_end": 8.0,
	})
	assert_gt(handle, 0, "a spawned light returns an opaque positive lease")
	assert_true(scene.is_alive(handle))
	var selected := scene.render_frame(Vector3(4.0, 1.0, -6.0), 512.0,
			Vector3.ONE, 0, null)
	assert_eq(selected, 1, "the light selects for a nearby camera")
	var report := scene.get_report()
	assert_eq(int(report.get("live", 0)), 1)
	assert_eq(int(report.get("selected", 0)), 1)
	var rows: Array = report.get("rows", [])
	assert_eq(rows.size(), 1)
	var row: Dictionary = rows[0]
	assert_eq(int(row.get("handle", 0)), handle,
			"diagnostics retain the opaque lease returned to gameplay")
	assert_eq(int(row.get("retail_handle", 0)), handle & 0xffff,
			"diagnostics expose the retail slot word separately for provenance")
	assert_almost_eq(float(row.get("range", 0.0)), 10.0, 0.001,
			"range = atten_end * 1.25 [orig: Light_GetPointLightParams]")
	assert_almost_eq(float(row.get("atten2", 0.0)), 0.15, 0.001,
			"atten2 = 15 / range^2")
	var world_pos: Vector3 = row.get("position", Vector3.ZERO)
	assert_true(world_pos.is_equal_approx(Vector3(4.0, 1.0, -2.0)),
			"the mission<->godot conversion round-trips")
	scene.despawn(handle)
	assert_false(scene.is_alive(handle))
	scene.render_frame(Vector3(4.0, 1.0, -6.0), 512.0, Vector3.ONE, 0, null)
	assert_eq(int(scene.get_report().get("selected", -1)), 0,
			"despawn clears the selection on the next frame")


func test_camera_global_object_select_admits_an_owned_muzzle_light() -> void:
	var scene := LightScene.new()
	var handle := scene.spawn_glow({
		"position": Vector3(4.0, 1.0, -2.0),
		"radius": 8.0,
		"color": Color.WHITE,
		"owner_entity": 77,
	})
	assert_gt(handle, 0)
	assert_eq(scene.render_frame(Vector3(4.0, 1.0, -6.0), 512.0,
			Vector3.ONE, 0, null), 1,
			"the camera-global fallback keeps an owned MF_Light visible")
	var report := scene.get_report()
	assert_eq(report.get("selection_mode", ""), "camera_global_objects")
	assert_eq(report.get("owner_isolation", ""), "unavailable")


func test_camera_global_object_select_filters_disabled_lights_before_the_cap() -> void:
	var scene := LightScene.new()
	for i in range(4):
		scene.spawn_glow({
			"position": Vector3(float(i), 0.0, 0.0),
			"radius": 8.0,
			"color": Color.WHITE,
			"disable_objects": true,
		})
	for i in range(4):
		scene.spawn_glow({
			"position": Vector3(10.0 + float(i), 0.0, 0.0),
			"radius": 8.0,
			"color": Color.WHITE,
		})
	assert_eq(scene.render_frame(Vector3.ZERO, 512.0, Vector3.ONE, 0, null), 4,
			"four nearer object-disabled lights cannot starve eligible lights")


func test_camera_query_bounds_saturate_at_the_mission_fixed_limit() -> void:
	var scene := LightScene.new()
	scene.spawn_glow({
		"position": Vector3(32767.0, 0.0, 0.0),
		"radius": 8.0,
		"color": Color.WHITE,
	})
	assert_eq(scene.render_frame(Vector3(32767.0, 0.0, 0.0), 512.0,
			Vector3.ONE, 0, null), 1,
			"center plus half-extent saturates instead of wrapping the AABB")


func test_select_caps_at_the_witnessed_four() -> void:
	var scene := LightScene.new()
	for i in range(6):
		scene.spawn_model_light({
			"position": Vector3(float(i), 0.0, 0.0),
			"atten_end": 8.0,
		})
	var selected := scene.render_frame(Vector3.ZERO, 512.0, Vector3.ONE, 0, null)
	assert_eq(selected, 4,
			"at most four lights select [orig: update_light_slots @ 0x5abc50]")
	scene.clear()
	scene.render_frame(Vector3.ZERO, 512.0, Vector3.ONE, 0, null)


func test_clear_render_output_preserves_the_live_pool() -> void:
	var scene := LightScene.new()
	scene.spawn_glow({
		"position": Vector3.ZERO,
		"radius": 8.0,
		"color": Color.WHITE,
	})
	assert_eq(scene.render_frame(Vector3.ZERO, 512.0, Vector3.ONE, 0, null), 1)
	scene.clear_render_output()
	var report := scene.get_report()
	assert_eq(int(report.get("live", -1)), 1,
			"dropping camera output does not destroy mission lights")
	assert_eq(int(report.get("selected", -1)), 0,
			"dropping camera output synchronously retires the published selection")
	assert_eq(report.get("selection_mode", ""), "none",
			"dropping camera output resets the selection mode")


func test_transient_glow_fades_and_dies_through_the_tick() -> void:
	var scene := LightScene.new()
	# The impact-flash shape [orig: AmmoDef_ProcessImpactEffect -> mode 2]:
	# blend fades counter/initial per tick, the slot dies at zero.
	var impact := scene.spawn_glow({
		"position": Vector3(1.0, 2.0, 3.0),
		"radius": 10.0,
		"color": Color(1.0, 0.75, 0.375),
		"fade_mode": 2,
		"fade_duration": 3,
	})
	assert_gt(impact, 0, "a transient glow spawns an opaque positive lease")
	scene.advance_fixed_tick()
	scene.advance_fixed_tick()
	assert_true(scene.is_alive(impact), "the flash lives to the last tick")
	scene.advance_fixed_tick()
	assert_false(scene.is_alive(impact),
			"an expired mode-2 flash despawns [orig: the 0x5aa170 tick]")


func test_reused_pool_slot_rejects_the_retired_handle() -> void:
	var scene := LightScene.new()
	var retired := scene.spawn_glow({
		"position": Vector3.ZERO,
		"radius": 2.0,
		"color": Color.WHITE,
	})
	scene.despawn(retired)
	var replacement := scene.spawn_glow({
		"position": Vector3.ONE,
		"radius": 2.0,
		"color": Color.WHITE,
	})
	assert_ne(replacement, retired,
			"a reused retail slot receives a distinct opaque lease")
	assert_false(scene.is_alive(retired), "the retired lease stays dead")
	assert_true(scene.is_alive(replacement), "the replacement lease is live")
	scene.despawn(retired)
	assert_true(scene.is_alive(replacement),
			"a late stale despawn cannot kill the replacement light")


func test_director_muzzle_and_round_glow_routes() -> void:
	var director := EffectLightDirector.new()
	var report_contract: Variant = director.get_report()
	assert_true(report_contract is EffectLightReport,
			"the native LightScene dictionary is decoded at the director FFI seam")
	# The muzzle glow: spawn-once per shooter, re-armed per shot, dead five
	# ticks after the last shot — and the cached handle stays dead (the
	# witnessed per-life behavior, light_scene.h map).
	director.on_muzzle_fire(7, Vector3(1.0, 1.0, 1.0))
	assert_eq(director.get_report().live, 1,
			"one shooter spawns one muzzle glow")
	director.on_muzzle_fire(7, Vector3(1.5, 1.0, 1.0))
	assert_eq(director.get_report().live, 1,
			"a repeat shot re-arms instead of spawning")
	for i in range(4):
		director.advance_fixed_tick()
	assert_eq(director.get_report().live, 1)
	director.advance_fixed_tick()
	assert_eq(director.get_report().live, 0,
			"five ticks after the last shot the muzzle glow dies")
	director.on_muzzle_fire(7, Vector3(1.0, 1.0, 1.0))
	assert_eq(director.get_report().live, 0,
			"the cached shooter handle stays dead this life")
	# The light_move round glow follows the sim rows and despawns with them.
	director.sync_round_glows([{"id": 11, "pos": Vector3(5.0, 5.0, 5.0),
			"radius": 6.0, "color": Color(0.5, 0.47, 0.31)}])
	assert_eq(director.get_report().live, 1,
			"a live light_move round row spawns one glow")
	director.sync_round_glows([{"id": 11, "pos": Vector3(9.0, 5.0, 5.0),
			"radius": 6.0, "color": Color(0.5, 0.47, 0.31)}])
	assert_eq(director.get_report().live, 1,
			"a moved round row follows instead of duplicating")
	director.sync_round_glows([])
	assert_eq(director.get_report().live, 0,
			"a dropped round row despawns its glow")


func test_fire_present_dictionary_routes_mf_light_into_selected_output() -> void:
	var packed := load("res://game/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	add_child_autofree(world)
	var director := EffectLightDirector.new()
	director.setup(world, Callable())
	var presenter := FirePresentPass.new()
	presenter.setup(null, null, Callable(), Callable(), Callable(), Callable(),
			Callable(director, "on_muzzle_fire"))
	presenter.present_fires([{
		"shooter_handle": 77,
		"origin": Vector3(1.0, 0.0, 0.0),
		"mf_light": 1,
		"is_local_player": true,
	}])
	assert_eq(director.get_report().live, 1,
			"the presented MF_Light dictionary creates one muzzle glow")
	var camera := Camera3D.new()
	world.add_child(camera)
	director.render_frame(camera)
	assert_eq(director.get_report().selected, 1,
			"the owned muzzle glow reaches camera-global object output")
	presenter.present_fires([{
		"shooter_handle": 78,
		"origin": Vector3.ZERO,
		"mf_light": 0,
		"is_local_player": true,
	}])
	assert_eq(director.get_report().live, 1,
			"an MF_Light-off dictionary is the negative control")
	presenter.teardown()


func test_destruction_present_dictionary_routes_death_light_into_output() -> void:
	var packed := load("res://game/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	add_child_autofree(world)
	var director := EffectLightDirector.new()
	director.setup(world, Callable())
	var presenter := DestructionPresentPass.new()
	presenter.setup(null, null, null, null, null, null, Callable(), Callable(),
			null, Callable(director, "on_death_light"))
	presenter.present_drained({
		"death_lights": [{"pos": Vector3(2.0, 0.0, 0.0), "radius": 6.0}],
	}, [])
	assert_eq(director.get_report().live, 1,
			"the destruction drain creates one death flash")
	var camera := Camera3D.new()
	world.add_child(camera)
	director.render_frame(camera)
	assert_eq(director.get_report().selected, 1,
			"the death flash reaches camera-global object output")
	presenter.teardown()


func test_director_spawns_model_lights_from_static_sources() -> void:
	var packed := load("res://game/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	add_child_autofree(world)
	var transform := Transform3D(Basis.IDENTITY, Vector3(10.0, 27.0, 350.0))
	var lit := _fixture_object_data("Shed.3di")
	var plain := _fixture_object_data("House.3di")
	var director := EffectLightDirector.new()
	director.setup(world, func() -> Array:
		return [
			{"object_data": plain, "world_transform": transform},
			{"object_data": lit, "world_transform": transform},
		])
	director.reattach()
	assert_eq(director.get_report().live, 1,
			"only the model with an authored light record spawns a pool light")
	var camera := Camera3D.new()
	camera.position = Vector3(10.0, 28.0, 346.0)
	world.add_child(camera)
	director.render_frame(camera)
	var rows := director.get_report().rows
	assert_eq(rows.size(), 1, "the placed record selects for a nearby camera")
	if rows.size() == 1:
		var world_pos := rows[0].position
		assert_true(world_pos.is_equal_approx(
				transform * Vector3(-0.0017, 1.2097, 0.0112)),
				"the record position rides the placement transform")
	assert_eq(director.spawn_light_record({}, transform), 0,
			"an empty record dictionary is skipped")
	assert_gt(director.spawn_light_record(_barrel_light_info(), transform), 0,
			"the public record seam spawns transient records directly")
	assert_eq(director.get_report().live, 2)
	director.reattach()
	assert_eq(director.get_report().live, 1,
			"reattach respawns from the entity set instead of accumulating")
	director.render_frame(null)


func test_director_null_camera_clears_output_without_destroying_the_pool() -> void:
	var packed := load("res://game/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	add_child_autofree(world)
	var director := EffectLightDirector.new()
	director.setup(world, Callable())
	assert_gt(director.spawn_light_record(
			_barrel_light_info(), Transform3D.IDENTITY), 0)
	var camera := Camera3D.new()
	world.add_child(camera)
	director.render_frame(camera)
	assert_eq(director.get_report().selected, 1)
	director.render_frame(null)
	var report := director.get_report()
	assert_eq(report.live, 1,
			"temporary camera loss preserves the mission light pool")
	assert_eq(report.selected, 0,
			"temporary camera loss synchronously clears shader output")
	assert_eq(report.selection_mode, "none",
			"a null camera synchronously resets the selection mode")


func test_director_reset_retires_pool_and_published_output() -> void:
	var packed := load("res://game/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	add_child_autofree(world)
	var director := EffectLightDirector.new()
	director.setup(world, Callable())
	director.on_muzzle_fire(17, Vector3.ZERO)
	var camera := Camera3D.new()
	world.add_child(camera)
	director.render_frame(camera)
	assert_eq(director.get_report().selected, 1)
	director.reset()
	var report := director.get_report()
	assert_eq(report.live, 0,
			"reset retires every mission light")
	assert_eq(report.selected, 0,
			"reset synchronously clears shader output")


func test_wire_node_exit_retires_its_lights_without_duplicate_registration() -> void:
	var director := EffectLightDirector.new()
	var node := ObjectModel.new()
	add_child(node)
	node.set_object_data(_fixture_object_data("Shed.3di"))
	director.on_wire_node_spawned(node, 0, 0)
	director.on_wire_node_spawned(node, 0, 0)
	assert_eq(director.get_report().live, 1,
			"repeat delivery of one live wire node remains idempotent")
	node.queue_free()
	await get_tree().process_frame
	assert_eq(director.get_report().live, 0,
			"tree exit retires every light owned by that wire node")


func test_reattach_rebinds_one_wire_exit_hook_without_accumulating_lights() -> void:
	var packed := load("res://game/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	add_child_autofree(world)
	var container := Node3D.new()
	container.name = "MissionObjects"
	world.add_child(container)
	var node := ObjectModel.new()
	container.add_child(node)
	node.set_object_data(_fixture_object_data("Shed.3di"))
	node.set_meta("entity_ref", {"wire_handle": 91})
	var director := EffectLightDirector.new()
	director.setup(world, Callable())
	director.reattach()
	director.reattach()
	assert_eq(director.get_report().live, 1,
			"repeat reattach replaces the pool and exit hook instead of duplicating")
	node.queue_free()
	await get_tree().process_frame
	assert_eq(director.get_report().live, 0,
			"the rebound one-shot hook retires the node's replacement lease")
