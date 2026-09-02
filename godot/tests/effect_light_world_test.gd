extends GutTest

const FirePresentPass := preload("res://game/world/fire_present_pass.gd")
const DestructionPresentPass := preload(
		"res://game/world/destruction_present_pass.gd")
const ARMRY_3DI := "res://../fixtures/threedi/synth/armory.3di"
# Authored light variants minted once from the retired edit surface
# (fixtures/README.md); each test reads the authored record
# back before probing the director.
const SYN_SHED_LGHT0_SUB2 := "res://../fixtures/threedi/synth/shed_lght0_sub2_origin_atten100.3di"
const SYN_ARMRY_LGHT0_SUB1 := "res://../fixtures/threedi/synth/armory_lght0_sub1_offset.3di"

# The EffectWorld dynamic point-light wiring (D-RLIT-4): the LightScene
# binding round trip, the witnessed <= 4 select + global-parameter push, and
# the EffectLightDirector spawn walk. The portable semantics themselves are
# pinned by ctest renderer_light_scene; this file covers the Godot seam.


# The static walk runs against the synthetic models: shed.3di carries one
# authored light record (style 24, atten 0..3 at (0, 1.25, 0)), house.3di
# carries none (tests/fixtures/minimal_3di_gen.cpp).
func _fixture_object_data(model: String) -> ObjectData:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/synth")), OK)
	var data := ObjectData.new()
	assert_eq(data.open_from_resource_root(root, model), OK,
			"%s loads as a static light source fixture" % model)
	return data


func _barrel_light_info() -> ModelLight:
	# The FireBrl3 shape: one style-113 light, atten 0..8, 1.12 above base.
	var light := ModelLight.new()
	light.position = Vector3(0.0004, 1.1157, -0.0044)
	light.atten_start = 0.0
	light.atten_end = 8.0
	light.color_start = Color(1.0, 0.78, 0.47)
	light.color_end = Color(0.31, 0.16, 0.04)
	light.colorgen_style = 113
	return light


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
	assert_eq(report.live, 1)
	assert_eq(report.selected, 1)
	var rows := report.rows
	assert_eq(rows.size(), 1)
	var row: EffectLightRow = rows[0]
	assert_eq(row.handle, handle,
			"diagnostics retain the opaque lease returned to gameplay")
	assert_eq(row.retail_handle, handle & 0xffff,
			"diagnostics expose the retail slot word separately for provenance")
	assert_almost_eq(row.range, 10.0, 0.001,
			"range = atten_end * 1.25 [orig: Light_GetPointLightParams]")
	assert_almost_eq(row.attenuation_quadratic, 0.15, 0.001,
			"atten2 = 15 / range^2")
	var world_pos: Vector3 = row.position
	assert_true(world_pos.is_equal_approx(Vector3(4.0, 1.0, -2.0)),
			"the mission<->godot conversion round-trips")
	scene.despawn(handle)
	assert_false(scene.is_alive(handle))
	scene.render_frame(Vector3(4.0, 1.0, -6.0), 512.0, Vector3.ONE, 0, null)
	assert_eq(scene.get_report().selected, 0,
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
	assert_eq(report.selection_mode, "camera_global_objects")
	assert_eq(report.owner_isolation, "unavailable")


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
	assert_eq(scene.render_frame(Vector3.ZERO, 512.0, Vector3.ONE, 0, null), 3,
			"four nearer object-disabled lights cannot starve the three eligible slots")


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


func test_select_caps_at_the_witnessed_three() -> void:
	var scene := LightScene.new()
	for i in range(6):
		scene.spawn_model_light({
			"position": Vector3(float(i), 0.0, 0.0),
			"atten_end": 8.0,
		})
	var selected := scene.render_frame(Vector3.ZERO, 512.0, Vector3.ONE, 0, null)
	# The batch entry stores three handles and breaks the visible walk there;
	# the 4 of Light_SelectAndEnableForDraw is the transient D3D enable count
	# FlushBatches tears down per entry (retail: collect_render_objects_for_batch
	# @0x5d9229; CRenderBatchQueue_FlushBatches @0x5da26b / @0x5da5de, see
	# docs/render/render-lighting-re.md).
	assert_eq(selected, 3,
			"at most three lights select")
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
	assert_eq(report.live, 1,
			"dropping camera output does not destroy mission lights")
	assert_eq(report.selected, 0,
			"dropping camera output synchronously retires the published selection")
	assert_eq(report.selection_mode, "none",
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
			"the director hands out the native LightScene report record")
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


func test_model_lght_and_muzzle_share_the_entity_cached_handle() -> void:
	var packed := load("res://game/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	add_child_autofree(world)
	var container := Node3D.new()
	container.name = "MissionObjects"
	world.add_child(container)
	var node := ObjectModel.new()
	container.add_child(node)
	node.set_object_data(_fixture_object_data("shed.3di"))
	node.entity_ref = EntityRef.make(MissionData.KIND_ITEM, -1, 0, 0, 7)
	var director := EffectLightDirector.new()
	director.setup(world, Callable(), Callable())
	director.on_wire_node_spawned(node, MissionData.KIND_ITEM, 0)
	assert_eq(director.get_report().live, 1,
			"the entity starts with its one authored LGHT lease")

	var muzzle_position := Vector3(4.0, 2.0, -3.0)
	director.on_muzzle_fire(7, muzzle_position)
	assert_eq(director.get_report().live, 1,
			"MF_Light reuses entity+0x1B4 instead of allocating beside LGHT")
	var camera := Camera3D.new()
	world.add_child(camera)
	director.render_frame(camera)
	var rows := director.get_report().rows
	assert_eq(rows.size(), 1)
	if rows.size() == 1:
		assert_true(rows[0].position.is_equal_approx(muzzle_position),
				"the shared LGHT lease receives the muzzle position setter")
		assert_almost_eq(float(rows[0].range), 3.75, 0.001,
				"reuse retains shed's authored radius instead of spawning 1.5 units")
	for i in range(5):
		director.advance_fixed_tick()
	assert_eq(director.get_report().live, 0,
			"the muzzle fade retires the reused authored lease after five ticks")


func test_fire_present_dictionary_routes_mf_light_into_selected_output() -> void:
	var packed := load("res://game/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	add_child_autofree(world)
	var director := EffectLightDirector.new()
	director.setup(world, Callable(), Callable())
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
	director.setup(world, Callable(), Callable())
	var presenter := DestructionPresentPass.new()
	presenter.setup(null, null, null, null, null, null, Callable(), Callable(),
			null, Callable(director, "on_death_light"))
	presenter.present_drained(DestructionDrain.make([], [], [],
			[DeathLightEvent.make(Vector3(2.0, 0.0, 0.0), 6.0)]), [])
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
	var lit := _fixture_object_data("shed.3di")
	var plain := _fixture_object_data("house.3di")
	var director := EffectLightDirector.new()
	director.setup(world, func() -> Array:
		return [
			{"object_data": plain, "world_transform": transform},
			{"object_data": lit, "world_transform": transform},
		], Callable())
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
				transform * Vector3(0.0, 1.25, 0.0)),
				"the record position rides the placement transform")
	assert_eq(director.spawn_light_record(null, transform), 0,
			"a missing record is skipped")
	assert_gt(director.spawn_light_record(_barrel_light_info(), transform), 0,
			"the public record seam spawns transient records directly")
	assert_eq(director.get_report().live, 2)
	director.reattach()
	assert_eq(director.get_report().live, 1,
			"reattach respawns from the entity set instead of accumulating")
	director.render_frame(null)


func _synthetic_object_data(res_path: String) -> ObjectData:
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(res_path)), OK,
			"%s loads as an authored light-source variant" % res_path.get_file())
	return data


func test_director_selects_static_building_lght_into_its_exact_robj_row() -> void:
	var packed := load("res://game/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	add_child_autofree(world)
	# shed's one LGHT authored onto subobject 2 at the origin with a 100-wu
	# radius; the atlas pixel below compares against the entity origin, so the
	# authored position is asserted, not assumed.
	var data := _synthetic_object_data(SYN_SHED_LGHT0_SUB2)
	assert_eq(data.get_light_count(), 1)
	var light := data.get_light_info(0)
	assert_eq(light.subobject, 2)
	assert_true((light.position as Vector3).is_equal_approx(Vector3.ZERO))
	assert_almost_eq(light.atten_end, 100.0, 0.001)
	var xform := Transform3D(Basis.IDENTITY, Vector3(5.0, 1.0, 0.0))
	var source := {
		"source_index": 0,
		"kind": MissionData.KIND_BUILDING,
		"entity_index": 0,
		"bms_id": 7001,
		"item_id": 1,
		"object_data": data,
		"world_transform": xform,
	}
	var draw := {
		"atlas_row": 0,
		"source_index": 0,
		"kind": MissionData.KIND_BUILDING,
		"entity_index": 0,
		"bms_id": 7001,
		"item_id": 1,
		"robj_index": 2,
		"world_bounds": AABB(Vector3(-5.0, -5.0, -5.0),
				Vector3(20.0, 20.0, 20.0)),
		"active": true,
	}
	var director := EffectLightDirector.new()
	director.setup(world, func() -> Array: return [source],
			func() -> Array: return [draw])
	director.reattach()
	assert_ne(EffectLightDirector.owner_id_for_static_source(0), 0)
	assert_ne(EffectLightDirector.owner_id_for_static_source(0),
			EffectLightDirector.owner_id_for_wire(0),
			"static and wire handle zero occupy distinct non-world owner domains")
	var camera := Camera3D.new()
	world.add_child(camera)
	camera.position = Vector3(5.0, 2.0, 8.0)
	director.render_frame(camera)
	var report := director.get_report()
	assert_eq(report.static_rows, 1)
	assert_eq(report.static_draws, 1)
	assert_eq(report.lit_static_draws, 1,
			"the section-2 LGHT reaches exactly the section-2 static draw")
	var atlas := director.scene().get_static_light_rows_image()
	assert_not_null(atlas)
	if atlas == null:
		return
	assert_almost_eq(atlas.get_pixel(0, 0).r, 1.0, 0.001)
	var posr := atlas.get_pixel(1, 0)
	assert_true(Vector3(posr.r, posr.g, posr.b).is_equal_approx(xform.origin),
			"the atlas carries the authored LGHT transformed by its static entity")


## Corona billboards (the D-RLIT-4 corona leg): the binding surfaces the
## portable walk's quads [orig: EffectWorld_RenderLightCoronas @ 0x5aaf40 —
## three segments toward the camera, the authored corona-disable, the
## 100-wu cull; semantics pinned by ctest renderer_light_scene].
func test_corona_rows_surface_the_witnessed_segments() -> void:
	var scene := LightScene.new()
	assert_gt(scene.spawn_model_light({
		"position": Vector3(0.0, 1.0, 0.0),
		"atten_end": 4.0,
	}), 0)
	var no_models: Array[Node3D] = []
	var rows: Array = scene.collect_corona_rows(Vector3(0.0, 1.0, 10.0),
			Vector3(0.0, 0.0, -1.0), Vector3.ONE, 0, 0, null, no_models,
			PackedInt64Array(), {})
	assert_eq(rows.size(), 3, "an enabled corona draws three segments")
	if rows.size() == 3:
		var first: Dictionary = rows[0]
		# Segments march 0.1 x radius toward the camera; half-size radius/2.
		assert_almost_eq(float(first.get("half_size")), 2.0, 0.001)
		var pos: Vector3 = first.get("position")
		assert_almost_eq(pos.z, 0.4, 0.02,
				"the first segment steps 0.1 x radius toward the camera")
		var color: Color = first.get("color")
		# White record color x 1/16 at full fade.
		assert_almost_eq(color.r, 255.0 / 256.0 / 16.0, 0.002)
	assert_gt(scene.spawn_model_light({
		"position": Vector3(2.0, 1.0, 0.0),
		"atten_end": 4.0,
		"disable_corona": true,
	}), 0)
	rows = scene.collect_corona_rows(Vector3(0.0, 1.0, 10.0),
			Vector3(0.0, 0.0, -1.0), Vector3.ONE, 0, 0, null, no_models,
			PackedInt64Array(), {})
	assert_eq(rows.size(), 3,
			"the corona-disabled record adds nothing to the first light's three segments")
	# Fog-to-black [orig: CD3DDevice_SetFogAndBlendMode(dev, 2) @ 0x5aafb6]:
	# past the fog end the corona color folds to black but the quads remain.
	rows = scene.collect_corona_rows(Vector3(0.0, 1.0, 10.0),
			Vector3(0.0, 0.0, -1.0), Vector3.ONE, 0, 0, null, no_models,
			PackedInt64Array(),
			{"enabled": true, "type": 1, "start": 2.0, "end": 8.0})
	assert_eq(rows.size(), 3)
	if rows.size() == 3:
		var fogged: Color = rows[0].get("color")
		assert_almost_eq(fogged.r, 0.0, 0.0001,
				"a corona past the fog end fades fully to black")


## The hot corona path packs the same rows as ONE MultiMesh buffer write.
## Pin the interleaved TRANSFORM_3D + color float layout against the
## Dictionary seam row by row through the headless-safe buffer seam (the
## dummy RenderingServer stores no MultiMesh instance data).
func test_fill_corona_multimesh_matches_the_row_seam() -> void:
	var scene := LightScene.new()
	assert_gt(scene.spawn_model_light({
		"position": Vector3(0.0, 1.0, 0.0),
		"atten_end": 4.0,
	}), 0)
	assert_gt(scene.spawn_model_light({
		"position": Vector3(3.0, 2.0, -1.0),
		"atten_end": 6.0,
	}), 0)
	var no_models: Array[Node3D] = []
	var fog := {"enabled": true, "type": 1, "start": 2.0, "end": 40.0}
	var rows: Array = scene.collect_corona_rows(Vector3(0.0, 1.0, 10.0),
			Vector3(0.0, 0.0, -1.0), Vector3.ONE, 12345, 2, null, no_models,
			PackedInt64Array(), fog)
	assert_gt(rows.size(), 0, "the seam produced comparison rows")
	var mesh := MultiMesh.new()
	mesh.transform_format = MultiMesh.TRANSFORM_3D
	mesh.use_colors = true
	var count := scene.fill_corona_multimesh(Vector3(0.0, 1.0, 10.0),
			Vector3(0.0, 0.0, -1.0), Vector3.ONE, 12345, 2, null, no_models,
			PackedInt64Array(), fog, mesh)
	assert_eq(count, rows.size(), "both seams walk the same quads")
	var buffer := scene.get_last_corona_buffer()
	assert_true(buffer.size() >= count * 16, "one 16-float record per row")
	for i in range(count):
		var row: Dictionary = rows[i]
		var half := float(row.get("half_size"))
		var center: Vector3 = row.get("position")
		var color: Color = row.get("color")
		var base := i * 16
		assert_almost_eq(buffer[base + 0], half, 0.000001)
		assert_almost_eq(buffer[base + 5], half, 0.000001)
		assert_almost_eq(buffer[base + 10], half, 0.000001)
		assert_almost_eq(buffer[base + 3], center.x, 0.000001)
		assert_almost_eq(buffer[base + 7], center.y, 0.000001)
		assert_almost_eq(buffer[base + 11], center.z, 0.000001)
		assert_almost_eq(buffer[base + 12], color.r, 0.000001)
		assert_almost_eq(buffer[base + 13], color.g, 0.000001)
		assert_almost_eq(buffer[base + 14], color.b, 0.000001)
		assert_almost_eq(buffer[base + 15], 1.0, 0.000001)
	# A camera past the 100-wu cull empties the frame; the mesh keeps its
	# high-water capacity and hides every instance instead of reallocating.
	var far_count := scene.fill_corona_multimesh(Vector3(0.0, 1.0, 500.0),
			Vector3(0.0, 0.0, -1.0), Vector3.ONE, 12345, 2, null, no_models,
			PackedInt64Array(), fog, mesh)
	assert_eq(far_count, 0)
	assert_eq(mesh.visible_instance_count, 0)
	assert_eq(mesh.instance_count, count,
			"capacity persists at the high-water mark")


## Static-row dirty maintenance: steady frames rewrite only gen-animated rows
## (plus every row when a blend or the gain moved) in the resident payload.
## Equivalence: a mirror scene forced down the full-rebuild path every frame
## (fresh rows_revision per call) must produce identical atlas bytes across
## flicker time, a blend write, a fade ramp, and a gain change.
func test_static_rows_dirty_maintenance_matches_full_rebuild() -> void:
	var fast := LightScene.new()
	var ref := LightScene.new()
	var handles_fast: Array[int] = []
	var handles_ref: Array[int] = []
	for config: Dictionary in [
		{"position": Vector3(1.0, 0.0, 1.0), "atten_end": 8.0},
		{"position": Vector3(4.0, 1.0, -2.0), "atten_end": 8.0,
				"style": 113, "color_start": Color(1.0, 0.8, 0.4),
				"color_end": Color(0.3, 0.15, 0.05)},
		{"position": Vector3(-3.0, 0.5, 2.0), "atten_end": 6.0},
	]:
		handles_fast.append(int(fast.spawn_model_light(config)))
		handles_ref.append(int(ref.spawn_model_light(config)))
	var bounds := PackedVector3Array([
		Vector3(-2.0, -2.0, -2.0), Vector3(6.0, 6.0, 6.0),
		Vector3(2.0, -1.0, -4.0), Vector3(5.0, 5.0, 5.0),
		Vector3(-5.0, -1.0, 0.0), Vector3(4.0, 4.0, 4.0),
	])
	var owners := PackedInt64Array([0, 0, 0])
	var sections := PackedInt32Array([0, 0, 0])
	var active := PackedByteArray([1, 1, 1])
	var ref_revision := 100
	var compare := func(gain: Vector3, time_ms: int, label: String) -> void:
		fast.render_static_frame(bounds, owners, sections,
				PackedInt64Array([0, 0, 0]), PackedInt32Array([0, 0, 0]),
				active, gain, time_ms, null, 1)
		ref_revision += 1
		ref.render_static_frame(bounds, owners, sections,
				PackedInt64Array([0, 0, 0]), PackedInt32Array([0, 0, 0]),
				active, gain, time_ms, null, ref_revision)
		var img_fast: Image = fast.get_static_light_rows_image()
		var img_ref: Image = ref.get_static_light_rows_image()
		assert_not_null(img_fast)
		assert_not_null(img_ref)
		if img_fast == null or img_ref == null:
			return
		assert_eq(img_fast.get_data(), img_ref.get_data(), label)
	compare.call(Vector3.ONE, 1000, "first frame builds identical atlases")
	compare.call(Vector3.ONE, 1000, "a quiet steady frame holds identical bytes")
	compare.call(Vector3.ONE, 1250, "flicker time moves only the gen-animated row")
	fast.set_light_blend(handles_fast[0], 0.5)
	ref.set_light_blend(handles_ref[0], 0.5)
	compare.call(Vector3.ONE, 1250, "a blend write recomputes every row identically")
	compare.call(Vector3.ONE, 1300, "the frame after the blend settles back to dirty rows")
	fast.set_light_fade(handles_fast[2], 2, 8)
	ref.set_light_fade(handles_ref[2], 2, 8)
	for i in range(3):
		fast.advance_fixed_tick()
		ref.advance_fixed_tick()
		compare.call(Vector3.ONE, 1300 + i,
				"each fade-ramp tick matches the full rebuild")
	compare.call(Vector3(0.7, 0.8, 0.9), 1400,
			"a gain change recomputes every row identically")
	compare.call(Vector3(0.7, 0.8, 0.9), 1400,
			"the frame after the gain change settles again")


func test_director_null_camera_clears_output_without_destroying_the_pool() -> void:
	var packed := load("res://game/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	add_child_autofree(world)
	var director := EffectLightDirector.new()
	director.setup(world, Callable(), Callable())
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
	director.setup(world, Callable(), Callable())
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


func test_wire_node_exit_retires_its_cached_light_without_duplicate_registration() -> void:
	var director := EffectLightDirector.new()
	var node := ObjectModel.new()
	add_child(node)
	node.set_object_data(_fixture_object_data("shed.3di"))
	director.on_wire_node_spawned(node, 0, 0)
	director.on_wire_node_spawned(node, 0, 0)
	assert_eq(director.get_report().live, 1,
			"repeat delivery of one live wire node remains idempotent")
	node.queue_free()
	await get_tree().process_frame
	assert_eq(director.get_report().live, 0,
			"Entity_Destroy retires the one final entity+0x1B4 lease")


func test_powerup_respawn_routes_authored_lght_once_per_live_entity() -> void:
	var director := EffectLightDirector.new()
	var first := ObjectModel.new()
	add_child_autofree(first)
	first.set_object_data(_fixture_object_data("shed.3di"))
	first.entity_ref = EntityRef.make(MissionData.KIND_ITEM, -1, 0, 0, 41)
	director.on_wire_node_spawned(first, MissionData.KIND_ITEM, 0)
	director.on_wire_node_spawned(first, MissionData.KIND_ITEM, 0)
	assert_eq(director.get_report().live, 1,
			"one late powerup node spawns its LGHT exactly once")
	first.queue_free()
	await get_tree().process_frame
	assert_eq(director.get_report().live, 0,
			"pickup retirement clears the powerup's cached LGHT lease")

	var respawn := ObjectModel.new()
	add_child_autofree(respawn)
	respawn.set_object_data(_fixture_object_data("shed.3di"))
	respawn.entity_ref = EntityRef.make(MissionData.KIND_ITEM, -1, 0, 0, 41)
	director.on_wire_node_spawned(respawn, MissionData.KIND_ITEM, 0)
	assert_eq(director.get_report().live, 1,
			"the replacement node takes the retail powerup_respawn LGHT path")


func test_live_model_light_uses_spawn_time_entity_matrix_only() -> void:
	var packed := load("res://game/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	add_child_autofree(world)
	var container := Node3D.new()
	container.name = "MissionObjects"
	world.add_child(container)
	# armory with its LGHT 0 authored onto ROBJ 1 at (0.25, 0.5, -0.75), a
	# 1000-wu radius and light objects enabled.
	var data := _synthetic_object_data(SYN_ARMRY_LGHT0_SUB1)
	assert_gt(data.get_light_count(), 0,
			"the committed multi-part fixture exposes an authored light")
	if data.get_light_count() <= 0:
		return
	var light := data.get_light_info(0)
	var attach_part := light.subobject
	assert_gt(attach_part, 0,
			"the fixture attaches its LGHT to a nonzero ROBJ")
	if attach_part <= 0:
		return
	var authored_position := Vector3(0.25, 0.5, -0.75)
	assert_true((light.position as Vector3).is_equal_approx(
			authored_position))
	assert_almost_eq(light.atten_end, 1000.0, 0.001)
	assert_false(light.disable_lightobjects)
	var node := ObjectModel.new()
	container.add_child(node)
	node.set_object_data(data)
	assert_true(node.get_render_part_nodes().has(attach_part),
			"the authored attach ROBJ is a live render part")
	await get_tree().process_frame
	var part := node.get_render_part_nodes().get(attach_part) as Node3D
	assert_not_null(part)
	if part == null:
		return
	part.position += Vector3(2.0, 0.0, 0.0)
	var spawn_position := node.global_transform * authored_position
	var attached_position := node.get_model_light_world_position(0)
	assert_false(attached_position.is_equal_approx(spawn_position),
			"the control ROBJ transform differs from the entity placement matrix")
	node.entity_ref = EntityRef.make(-1, -1, 0, 0, 33)
	var director := EffectLightDirector.new()
	director.setup(world, Callable(), Callable())
	director.on_wire_node_spawned(node, MissionData.KIND_ITEM, 0)
	var camera := Camera3D.new()
	world.add_child(camera)
	camera.position = Vector3(0.0, 2.0, 8.0)
	director.render_frame(camera)
	var rows := director.get_report().rows
	assert_gt(rows.size(), 0)
	var saw_spawn_position := false
	var saw_attached_position := false
	for row in rows:
		if row.position.is_equal_approx(spawn_position):
			saw_spawn_position = true
		if row.position.is_equal_approx(attached_position):
			saw_attached_position = true
	assert_true(saw_spawn_position,
			"LGHT position uses the entity placement matrix at spawn")
	assert_false(saw_attached_position,
			"subobject selects an owner section, not a position transform")

	node.position += Vector3(3.0, 0.0, 0.0)
	part.position += Vector3(2.0, 0.0, 0.0)
	var moved_position := node.global_transform * authored_position
	assert_false(moved_position.is_equal_approx(spawn_position))
	director.render_frame(camera)
	rows = director.get_report().rows
	var still_at_spawn := false
	var followed_entity := false
	for row in rows:
		if row.position.is_equal_approx(spawn_position):
			still_at_spawn = true
		if row.position.is_equal_approx(moved_position):
			followed_entity = true
	assert_true(still_at_spawn,
			"authored LGHT remains at its spawn-time world position")
	assert_false(followed_entity,
			"authored LGHT has no per-frame entity follow path in retail")


func test_reattach_rebinds_one_wire_exit_hook_without_accumulating_lights() -> void:
	var packed := load("res://game/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	add_child_autofree(world)
	var container := Node3D.new()
	container.name = "MissionObjects"
	world.add_child(container)
	var node := ObjectModel.new()
	container.add_child(node)
	node.set_object_data(_fixture_object_data("shed.3di"))
	node.entity_ref = EntityRef.make(-1, -1, 0, 0, 91)
	var director := EffectLightDirector.new()
	director.setup(world, Callable(), Callable())
	director.reattach()
	director.reattach()
	assert_eq(director.get_report().live, 1,
			"repeat reattach replaces the pool and exit hook instead of duplicating")
	node.queue_free()
	await get_tree().process_frame
	assert_eq(director.get_report().live, 0,
			"the rebound one-shot hook retires the node's replacement lease")
