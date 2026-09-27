extends GutTest

# Real placement, avatar and destruction consumers of the native entity sphere.
# The committed pump variants differ only in their authored coarse threshold;
# no model, presenter or placement behavior is replaced by this fixture.

var _fixture_root := ""
var _resources: ResourceRoot
var _items: ItemDatabase

# The per-view bits (render/visual_layers.h): the world bits a split node
# swaps for the main-view bits, and the bit only the Inset camera admits.
const WORLD_BITS := (1 << 0) | (1 << 16)
const MAIN_VIEW_BITS := (1 << 20) | (1 << 22)
const INSET_VIEW := 1 << 21
# The beauty camera (FrameFx kBeautyCameraMask) and the Inset camera it
# derives (HudInsetScope: minus the viewmodel, FP body, caster, main-view and
# main-view foliage bits, plus INSET_VIEW).
const MAIN_MASK := 0x00D78C01
const INSET_MASK := 0x00258401


func before_all() -> void:
	_fixture_root = WorldFixture.stage_minimal_root("entity_projection_lod", false, {
		"items.def": """begin "Projection object"
id 106401
type object
graphic pump_lod20
scale 1.5
end
begin "Projection person"
id 106402
type person
graphic pump_lod20
scale 1.5
end
begin "Projection crate"
id 106403
type object
graphic crate
scale 2
husk pump_lod20
end
begin "Projection parachute"
id 100185
type object
graphic armory
end
""",
		"avatars.def": """define head HEAD
{
 graphic pump_lod80.3di
 camo 32 64 96
 voice 3
 sex m
}
define body BODY
{
 graphic pump_lod20.3di
 camo 100 120 140
}
define arms ARMS
{
 graphic pump_lod20.3di
 camo 200 210 220
}
nationality 0 NAT
{
 alignment good
 division 0 DIV
 {
  combo 2 HEAD BODY ARMS
 }
}
"""})
	_items = ItemDatabase.new()
	assert_eq(_items.load(_fixture_root.path_join("items.def")), OK)
	_resources = ResourceRoot.new()
	assert_eq(_resources.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/synth")), OK)


func after_all() -> void:
	_resources = null
	_items = null
	TestFs.remove_dir_recursive(_fixture_root)


func _placer() -> MissionObjectPlacer:
	return MissionObjectPlacer.create(_resources, _items)


func _data(name: String) -> ObjectData:
	var data := ObjectData.new()
	assert_eq(data.open_from_resource_root(_resources, name + ".3di"), OK)
	return data


func _place(placer: MissionObjectPlacer, item_id: int,
		rotations: Array[Vector3] = [Vector3.ZERO]) -> Dictionary:
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var entities: Array = []
	for rotation in rotations:
		var entity = mission.add_entity(MissionData.KIND_ITEM, item_id,
				Vector3.ZERO, rotation)
		assert_not_null(entity)
		entities.append(entity)
	var parent := Node3D.new()
	add_child_autofree(parent)
	var stats := placer.place(mission, parent)
	return {"parent": parent, "entities": entities, "stats": stats}


func _camera(depth: float) -> Transform3D:
	return Transform3D(Basis.IDENTITY, Vector3(0, 0, depth))


func _static_placer() -> MissionObjectPlacer:
	var placer := _placer()
	assert_true(placer.register_resolved_static_graphic("crate", _data("crate"), [{
		"mesh": BoxMesh.new(), "material": null,
		"offset": Transform3D.IDENTITY, "submesh": 0, "lod_index": 0,
	}, {
		"mesh": BoxMesh.new(), "material": null,
		"offset": Transform3D.IDENTITY, "submesh": 1, "lod_index": 1,
	}], {"thresholds_q16": PackedInt32Array([20 << 16, 0])}))
	return placer


func test_live_object_uses_scaled_cmdl_diagonal_instead_of_ghdr_radius() -> void:
	var placer := _placer()
	var placed := _place(placer, 106401)
	assert_eq(placed.stats.animated, 1)
	var model := placer.get_placed_models()[0] as ObjectModel
	assert_eq(model.get_entity_uniform_scale_q16(), 98304)
	# CMDL radius 213927, scaled once to 320891 Q16: about 16.8 pixels
	# after frame scale at depth 200. GHDR * 1.5 would be 22.1 pixels.
	ObjectModel.update_authored_lods(_camera(200), 70, 640, 480)
	assert_eq(model.get_active_lod(), 1,
			"the collision diagonal selects the coarse level at this camera")


func test_sub_pixel_world_models_are_not_drawn_at_any_level_count() -> void:
	# Retail's sector-entity draw returns before the RLOD walk when the bound
	# sphere projects to at most 0.75 px (retail render_sector_entity
	# @0x5c42d8..0x5c42de), whatever the model's level count: the two-level
	# pump and the one-level crate both drop, and come back when near.
	var placer := _placer()
	placer.register_occlusion_verdict(106401, true)
	placer.register_occlusion_verdict(106403, true)
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_not_null(mission.add_entity(MissionData.KIND_ITEM, 106401, Vector3.ZERO, Vector3.ZERO))
	assert_not_null(mission.add_entity(MissionData.KIND_ITEM, 106403, Vector3.ZERO, Vector3.ZERO))
	var parent := Node3D.new()
	add_child_autofree(parent)
	var stats := placer.place(mission, parent)
	assert_eq(stats.animated, 2)
	var pump: ObjectModel = null
	var crate: ObjectModel = null
	for model: ObjectModel in placer.get_placed_models():
		if model.get_object_data().get_lod_count() == 1:
			crate = model
		else:
			pump = model
	assert_not_null(pump)
	assert_not_null(crate)
	if pump == null or crate == null:
		return
	assert_eq(crate.get_object_data().get_lod_count(), 1, "the crate is a one-level model")
	# 640x480 at 70 deg: focal 343 px. Pump radius 4.9 u -> 0.56 px at 3000 u,
	# crate radius 1.73 u -> 0.2 px.
	ObjectModel.update_authored_lods(_camera(3000), 70, 640, 480)
	assert_true(pump.is_subpixel_hidden())
	assert_false(pump.visible, "a sub-pixel world model is not drawn")
	assert_true(crate.is_subpixel_hidden(), "one-level models take the floor too")
	assert_false(crate.visible)
	ObjectModel.update_authored_lods(_camera(200), 70, 640, 480)
	assert_false(pump.is_subpixel_hidden())
	assert_true(pump.visible, "back above the floor the model draws again")
	assert_false(crate.is_subpixel_hidden())
	assert_true(crate.visible)


func test_camera_view_reads_a_keep_width_fov_as_horizontal() -> void:
	# Retail's focal is half the viewport WIDTH over tan(fov_h / 2) (retail
	# Viewport_BuildProjectionMatrix @0x410fe1..0x410ff7). A KEEP_WIDTH camera
	# names the horizontal fov: 90 deg over 1600 px is an 800 px focal (frame
	# scale 0.8), so the 4.9 u pump sphere 120 u out projects 32.7 px and
	# scales to 26 px, above pump_lod20's 20 px row. Read as a vertical fov
	# the focal would be 450 px and the level the coarse one.
	var placer := _placer()
	var placed := _place(placer, 106401)
	assert_eq(placed.stats.animated, 1)
	var model := placer.get_placed_models()[0] as ObjectModel
	var view := SubViewport.new()
	view.size = Vector2i(1600, 900)
	add_child_autofree(view)
	var camera := Camera3D.new()
	camera.keep_aspect = Camera3D.KEEP_WIDTH
	camera.fov = 90.0
	view.add_child(camera)
	camera.global_transform = _camera(120)
	model.set_active_lod(1)
	ObjectModel.update_authored_lods_for_camera(camera, 1600.0)
	assert_eq(model.get_active_lod(), 0, "the KEEP_WIDTH fov is the horizontal one")
	camera.global_transform = _camera(200)
	ObjectModel.update_authored_lods_for_camera(camera, 1600.0)
	assert_eq(model.get_active_lod(), 1, "and farther out the coarse row applies")


func test_live_object_rotates_its_already_scaled_cmdl_center_once() -> void:
	var placer := _placer()
	_place(placer, 106401, [Vector3(0, 90, 90), Vector3(0, 90, -90)])
	var models: Array = placer.get_placed_models()
	assert_eq(models.size(), 2)
	# Pump CMDL center is z=111360 Q16. At scale 1.5 these rolls put
	# the center at world z=+/-2.549. Depths 167.451/172.549 straddle 20px.
	ObjectModel.update_authored_lods(_camera(170), 70, 640, 480)
	assert_eq((models[0] as ObjectModel).get_active_lod(), 0)
	assert_eq((models[1] as ObjectModel).get_active_lod(), 1,
			"using the entity origin or scaling the radius twice loses this split")


func test_fallback_person_keeps_entity_radius_after_both_construction_orders() -> void:
	var placer := _placer()
	_place(placer, 106402)
	var placed := placer.get_placed_models()[0] as ObjectModel
	var parent := Node3D.new()
	add_child_autofree(parent)
	var streamed := placer.build_player_animated_model(6402, parent)
	assert_not_null(streamed)
	assert_true(placer.resolve_player_visual_spec(6402, 0).fallback)
	# The person's entity radius is RHU(GHDR * 1.5) + 0x1000 = 426690,
	# projecting above 20px. The ordinary CMDL sphere falls below it.
	for model: ObjectModel in [placed, streamed]:
		model.set_active_lod(1)
	ObjectModel.update_authored_lods(_camera(200), 70, 640, 480)
	assert_eq(placed.get_active_lod(), 0, "place() retains person semantics")
	assert_eq(streamed.get_active_lod(), 0, "the fallback build retains person semantics")
	streamed.set_object_data(_data("pump_lod20"))
	streamed.set_active_lod(1)
	ObjectModel.update_authored_lods(_camera(200), 70, 640, 480)
	assert_eq(streamed.get_active_lod(), 0, "a later data rebuild preserves the person source")
	# Rebuild detaches the old ROBJ/notifier subtree and queues its deletion.
	await get_tree().process_frame


func test_static_projection_uses_full_diagonal_rotated_offset_and_scale_once() -> void:
	var placer := _static_placer()
	var placed := _place(placer, 106403,
			[Vector3(0, 90, 90), Vector3(0, 90, -90)])
	assert_eq(placed.stats.batched, 2)
	# Crate CMDL half-diagonal=56755 Q16, scale 2 -> 113510. Its z=0.5
	# center becomes world z=+/-1, so depths 59/61 straddle the 20px row.
	placer.update_static_lods(_camera(60), 70, 640, 480)
	assert_eq(placer.get_static_instance_lod(placed.entities[0].bms_id), 0)
	assert_eq(placer.get_static_instance_lod(placed.entities[1].bms_id), 1,
			"the two instances share geometry but project their own entity spheres")


func test_composed_head_shares_projection_and_selects_its_own_threshold_table() -> void:
	var avatars := AvatarDatabase.new()
	assert_eq(avatars.load(_fixture_root.path_join("avatars.def")), OK)
	var placer := _placer()
	placer.set_avatar_db(avatars)
	var parent := Node3D.new()
	add_child_autofree(parent)
	var body := placer.build_player_animated_model(6402, parent, 0x0400)
	assert_not_null(body)
	var head := body.find_child("PlayerAvatarHead_*", true, false) as ObjectModel
	assert_not_null(head)
	if head == null:
		return
	assert_eq(head.get_authored_lod_projection_owner(), body)
	assert_null(head.get_authored_lod_owner(), "a head owns its own RLOD selector")
	# Deliberately separate the rendered head. Its own center is behind the
	# camera; only the shared entity projection can select its coarse row.
	head.position = Vector3(0, 0, 1000)
	body.set_active_lod(1)
	head.set_active_lod(0)
	ObjectModel.update_authored_lods(_camera(200), 70, 640, 480)
	assert_eq(body.get_active_lod(), 0, "shared radius exceeds the body's 20px row")
	assert_eq(head.get_active_lod(), 1, "the same radius is below the head's 80px row")


func test_wire_parachute_flag_selects_unscaled_type185_radius_on_every_update() -> void:
	var sim := Simulation.new()
	var placer := _placer()
	var parent := Node3D.new()
	add_child_autofree(parent)
	var presenter := EntityPresenter.new()
	add_child_autofree(presenter)
	presenter.setup_wire(sim, placer, parent)
	var row := PackedFloat32Array()
	row.resize(Simulation.PF_STRIDE)
	row[Simulation.PF_TYPE_ID] = 6402
	row[Simulation.PF_WIRE_HANDLE] = 0x2001
	row[Simulation.PF_KIND] = 255
	row[Simulation.PF_INDEX] = 0xFFFFFF
	row[Simulation.PF_ALIVE] = 1
	row[Simulation.PF_ANIM_STATE] = -1
	row[Simulation.PF_WPN_ANIM_STATE] = -1
	presenter.present_wire_snapshot(row, Simulation.PF_STRIDE, 1)
	var model := presenter.resolve_wire_handle(0x2001)
	assert_not_null(model)
	if model == null:
		presenter.teardown()
		return
	ObjectModel.update_authored_lods(_camera(300), 70, 640, 480)
	assert_eq(model.get_active_lod(), 1, "the ordinary person sphere is below 20px")
	row[Simulation.PF_PARACHUTE_DEPLOYED] = 1
	presenter.present_wire_snapshot(row, Simulation.PF_STRIDE, 1)
	ObjectModel.update_authored_lods(_camera(300), 70, 640, 480)
	assert_eq(model.get_active_lod(), 0, "type185 GHDR radius 587526 crosses 20px")
	ObjectModel.update_authored_lods(_camera(400), 70, 640, 480)
	assert_eq(model.get_active_lod(), 1, "the parachute radius does not inherit person scale 1.5")
	row[Simulation.PF_PARACHUTE_DEPLOYED] = 0
	presenter.present_wire_snapshot(row, Simulation.PF_STRIDE, 1)
	ObjectModel.update_authored_lods(_camera(300), 70, 640, 480)
	assert_eq(model.get_active_lod(), 1, "clearing the flag restores the entity bound radius")
	presenter.teardown()


func _present_husk(placer: MissionObjectPlacer, placed: Dictionary,
		individual: bool) -> void:
	var container: Node3D = placed.parent.get_node("MissionObjects")
	var index := EntityIndex.new()
	index.build(placer.get_placed_models(), null)
	var presenter := EntityPresenter.new()
	add_child_autofree(presenter)
	presenter.setup(null, index, placer)
	presenter.setup_passes(container, _items, null, null, null, null, null,
			ItemEffectDirector.new())
	presenter.present_destruction_drained(DestructionDrain.make([
			HuskSwapEvent.make(placed.entities[0].bms_id, 6403)]), [])
	var husks: Array[Node] = container.find_children("HuskModel*", "ObjectModel", true, false)
	assert_eq(husks.size(), 1)
	if not husks.is_empty():
		var husk := husks[0] as ObjectModel
		assert_eq(husk.get_object_data().get_lod_count(), 2)
		if individual:
			assert_eq(husk.get_authored_lod_projection_owner(), placer.get_placed_models()[0])
		# The intact crate at scale 2 has radius 113510. The replacement
		# pump's different CMDL/GHDR would select level 0 at this depth.
		ObjectModel.update_authored_lods(_camera(70), 70, 640, 480)
		assert_eq(husk.get_active_lod(), 1,
				"the husk uses the primary entity sphere with its own RLOD table")
	presenter.teardown()


func test_individual_husk_retains_primary_sphere_with_nonunit_scale() -> void:
	var placer := _placer()
	placer.register_occlusion_verdict(106403, true)
	var placed := _place(placer, 106403)
	assert_eq(placed.stats.animated, 1)
	_present_husk(placer, placed, true)


func test_static_husk_retains_primary_sphere_with_nonunit_scale() -> void:
	var placer := _static_placer()
	var placed := _place(placer, 106403)
	assert_eq(placed.stats.batched, 1)
	_present_husk(placer, placed, false)


# --- the weapon Inset view -----------------------------------------------------
# Retail runs the whole scene pass per view (Render_WeaponInsetScene's own
# Terrain_RenderWorldScene: collector, section masks, sub-pixel floor, RLOD
# walk, each on the pass's frame scale). Where the two views disagree the node
# draws the main view's on the main-view bits and a twin draws the Inset's.

func _view_camera(size: Vector2i, fov: float, depth: float, mask := 0xFFFFF) -> Camera3D:
	var view := SubViewport.new()
	view.size = size
	view.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(view)
	var camera := Camera3D.new()
	camera.keep_aspect = Camera3D.KEEP_WIDTH
	camera.fov = fov
	camera.cull_mask = mask
	view.add_child(camera)
	camera.current = true
	camera.global_transform = _camera(depth)
	return camera


func _slot_layers(model: ObjectModel) -> int:
	var layers := 0
	for node in model.find_children("*", "MeshInstance3D", true, false):
		layers |= (node as MeshInstance3D).layers
	return layers


func test_each_view_selects_its_own_level_and_the_inset_draws_a_twin() -> void:
	var placer := _placer()
	var placed := _place(placer, 106401)
	assert_eq(placed.stats.animated, 1)
	var model := placer.get_placed_models()[0] as ObjectModel
	# The main image: 90 deg KEEP_WIDTH over 640 px (focal 320, frame scale
	# 2) puts the 4.9 u pump sphere 200 u out at 15.7 px, under pump_lod20's
	# 20 px row. The Inset: 10 deg over 256 px (focal 1463, frame scale 5)
	# puts it at 179 px.
	var main := _view_camera(Vector2i(640, 480), 90.0, 200.0)
	var inset := _view_camera(Vector2i(256, 256), 10.0, 200.0)
	ObjectModel.update_authored_lods_for_views(main, 640.0, inset, 256.0)
	assert_eq(model.get_active_lod(), 1, "the node keeps the main view's coarse level")
	assert_eq(model.get_inset_view_lod(), 0, "the narrow Inset selects the fine level")
	assert_true(model.is_view_split(), "the views differ")
	assert_gt(model.get_view_twin_count(), 0, "a twin draws the Inset's level")
	var layers := _slot_layers(model)
	assert_eq(layers & WORLD_BITS, 0, "the split node leaves the world bits")
	assert_ne(layers & MAIN_VIEW_BITS, 0, "and draws for the main view only")
	# A node another owner hides directly draws in neither view.
	model.visible = false
	assert_eq(model.get_view_twin_count(), 0, "no twin outlives a direct hide")
	model.visible = true
	assert_gt(model.get_view_twin_count(), 0, "and it returns with the node")
	# The Inset closes: one node on the world bits again, no twin.
	ObjectModel.update_authored_lods_for_views(main, 640.0, null, 0.0)
	assert_false(model.is_view_split())
	assert_eq(model.get_view_twin_count(), 0)
	assert_eq(model.get_inset_view_lod(), model.get_active_lod())
	layers = _slot_layers(model)
	assert_ne(layers & WORLD_BITS, 0)
	assert_eq(layers & MAIN_VIEW_BITS, 0)


func test_a_sub_pixel_model_in_the_main_view_stays_drawn_in_the_inset_only() -> void:
	var placer := _placer()
	var placed := _place(placer, 106401)
	assert_eq(placed.stats.animated, 1)
	var model := placer.get_placed_models()[0] as ObjectModel
	# 3000 u out the main view projects the 4.9 u sphere to 0.52 px (at or
	# below the 0.75 px floor: the sector draw returns before the RLOD walk);
	# a 2 deg Inset over 256 px (focal 7333) projects it to 12 px.
	var main := _view_camera(Vector2i(640, 480), 90.0, 3000.0)
	var inset := _view_camera(Vector2i(256, 256), 2.0, 3000.0)
	ObjectModel.update_authored_lods_for_views(main, 640.0, inset, 256.0)
	assert_true(model.is_subpixel_hidden())
	assert_false(model.visible, "the main view does not draw it")
	assert_false(model.is_inset_view_subpixel_hidden(), "the Inset does")
	assert_true(model.is_view_split())
	assert_gt(model.get_view_twin_count(), 0, "on its twin")
	ObjectModel.update_authored_lods_for_views(main, 640.0, null, 0.0)
	assert_false(model.is_view_split())
	assert_eq(model.get_view_twin_count(), 0, "the twin goes with the Inset")


func test_static_instance_moves_into_view_twin_populations_while_the_views_differ() -> void:
	var placer := _static_placer()
	var placed := _place(placer, 106403)
	assert_eq(placed.stats.batched, 1)
	var bms_id: int = placed.entities[0].bms_id
	# The scale-2 crate sphere (1.73 u) 60 u out: 18.5 px in the main image,
	# under the 20 px row; about 210 px in the 10 deg Inset.
	var main := _view_camera(Vector2i(640, 480), 90.0, 60.0)
	var inset := _view_camera(Vector2i(256, 256), 10.0, 60.0)
	placer.update_static_lods_for_views(main, 640.0, inset, 256.0)
	assert_eq(placer.get_static_instance_lod(bms_id), 1)
	assert_eq(placer.get_static_instance_inset_lod(bms_id), 0)
	var names: Array = placer.get_static_instance_live_populations(bms_id)
	var main_rows := 0
	var inset_rows := 0
	for population_name in names:
		if String(population_name).ends_with("_MainView"):
			main_rows += 1
		elif String(population_name).ends_with("_InsetView"):
			inset_rows += 1
	assert_eq(names.size(), 2, "no shared row while the views differ: %s" % [names])
	assert_eq(main_rows, 1, "the main view's level in its main-view twin")
	assert_eq(inset_rows, 1, "the Inset's level in its Inset-view twin")
	var parent: Node3D = placed.parent
	for twin in parent.find_children("*_InsetView", "MultiMeshInstance3D", true, false):
		assert_eq((twin as MultiMeshInstance3D).layers, INSET_VIEW)
	for twin in parent.find_children("*_MainView", "MultiMeshInstance3D", true, false):
		assert_eq((twin as MultiMeshInstance3D).layers & WORLD_BITS, 0)
		assert_ne((twin as MultiMeshInstance3D).layers & MAIN_VIEW_BITS, 0)
	# The views converge when the Inset closes: back in the shared population.
	placer.update_static_lods_for_views(main, 640.0, null, 0.0)
	names = placer.get_static_instance_live_populations(bms_id)
	assert_eq(names.size(), 1)
	assert_false(String(names[0]).ends_with("View"), "the shared population: %s" % [names])


func test_a_wire_row_the_main_collect_culls_draws_in_the_inset_only() -> void:
	var sim := Simulation.new()
	var placer := _placer()
	var parent := Node3D.new()
	add_child_autofree(parent)
	var presenter := EntityPresenter.new()
	add_child_autofree(presenter)
	presenter.setup_wire(sim, placer, parent)
	var row := PackedFloat32Array()
	row.resize(Simulation.PF_STRIDE)
	row[Simulation.PF_TYPE_ID] = 6402
	row[Simulation.PF_WIRE_HANDLE] = 0x2001
	row[Simulation.PF_KIND] = 255
	row[Simulation.PF_INDEX] = 0xFFFFFF
	row[Simulation.PF_ALIVE] = 1
	row[Simulation.PF_ANIM_STATE] = -1
	row[Simulation.PF_WPN_ANIM_STATE] = -1
	presenter.present_wire_snapshot(row, Simulation.PF_STRIDE, 1)
	var model := presenter.resolve_wire_handle(0x2001)
	assert_not_null(model)
	if model == null:
		presenter.teardown()
		return
	var main := _view_camera(Vector2i(640, 480), 90.0, 200.0)
	var inset := _view_camera(Vector2i(256, 256), 10.0, 200.0)
	# The main collect culls the row; the Inset's own collect draws it.
	presenter.set_render_culled(0x2001, true)
	presenter.set_wire_inset_view(true)
	presenter.set_render_culled_inset(0x2001, false)
	presenter.present_wire_snapshot(row, Simulation.PF_STRIDE, 1)
	ObjectModel.update_authored_lods_for_views(main, 640.0, inset, 256.0)
	assert_false(model.visible, "the main view does not draw the culled row")
	assert_true(model.is_view_split())
	assert_gt(model.get_view_twin_count(), 0, "the Inset draws it on its twin")
	# Both collects cull it: no view draws it, no twin.
	presenter.set_render_culled_inset(0x2001, true)
	presenter.present_wire_snapshot(row, Simulation.PF_STRIDE, 1)
	ObjectModel.update_authored_lods_for_views(main, 640.0, inset, 256.0)
	assert_eq(model.get_view_twin_count(), 0)
	# The Inset closes and the main collect releases the row: one node again.
	presenter.set_wire_inset_view(false)
	presenter.set_render_culled(0x2001, false)
	presenter.present_wire_snapshot(row, Simulation.PF_STRIDE, 1)
	ObjectModel.update_authored_lods_for_views(main, 640.0, null, 0.0)
	assert_true(model.visible)
	assert_false(model.is_view_split())
	presenter.teardown()


func _grab(camera: Camera3D) -> Image:
	for _frame in 4:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	return (camera.get_viewport() as SubViewport).get_texture().get_image()


func test_captures_draw_each_views_own_level() -> void:
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice capture: run windowed")
		return
	var placer := _placer()
	var placed := _place(placer, 106401)
	assert_eq(placed.stats.animated, 1)
	var model := placer.get_placed_models()[0] as ObjectModel
	var main := _view_camera(Vector2i(160, 120), 90.0, 200.0, MAIN_MASK)
	var inset := _view_camera(Vector2i(256, 256), 10.0, 200.0, INSET_MASK)
	ObjectModel.update_authored_lods_for_views(main, 640.0, inset, 256.0)
	assert_eq(model.get_active_lod(), 1)
	assert_eq(model.get_inset_view_lod(), 0)
	var split_main: Image = await _grab(main)
	var split_inset: Image = await _grab(inset)
	# References: the Inset closed and the one node posed at each level.
	ObjectModel.update_authored_lods_for_views(main, 640.0, null, 0.0)
	model.set_active_lod(1)
	var level1_main: Image = await _grab(main)
	var level1_inset: Image = await _grab(inset)
	model.set_active_lod(0)
	var level0_inset: Image = await _grab(inset)
	assert_ne(level0_inset.get_data(), level1_inset.get_data(),
			"the two levels draw differently through the Inset (level 1 drops the post)")
	assert_eq(split_main.get_data(), level1_main.get_data(),
			"the main capture draws the main view's level 1")
	assert_eq(split_inset.get_data(), level0_inset.get_data(),
			"the Inset capture draws its own level 0 on the twin")


func test_capture_of_a_main_view_sub_pixel_model_draws_in_the_inset_only() -> void:
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice capture: run windowed")
		return
	var main := _view_camera(Vector2i(160, 120), 90.0, 3000.0, MAIN_MASK)
	var inset := _view_camera(Vector2i(256, 256), 2.0, 3000.0, INSET_MASK)
	# The reference with nothing placed.
	var empty_main: Image = await _grab(main)
	var empty_inset: Image = await _grab(inset)
	var placer := _placer()
	var placed := _place(placer, 106401)
	assert_eq(placed.stats.animated, 1)
	var model := placer.get_placed_models()[0] as ObjectModel
	ObjectModel.update_authored_lods_for_views(main, 640.0, inset, 256.0)
	assert_true(model.is_subpixel_hidden())
	var split_main: Image = await _grab(main)
	var split_inset: Image = await _grab(inset)
	# The reference with the Inset view's own selection as the only view (its
	# level, drawn by the one node).
	ObjectModel.update_authored_lods_for_views(inset, 256.0, null, 0.0)
	assert_false(model.is_subpixel_hidden())
	var alone_inset: Image = await _grab(inset)
	assert_eq(split_main.get_data(), empty_main.get_data(), "the main view draws nothing")
	assert_ne(split_inset.get_data(), empty_inset.get_data(), "the Inset draws the model")
	assert_eq(split_inset.get_data(), alone_inset.get_data(),
			"exactly as the one node draws it at the Inset's level")
