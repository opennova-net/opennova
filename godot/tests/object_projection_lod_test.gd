extends GutTest

# Real placement, avatar and destruction consumers of the native entity sphere.
# The committed pump variants differ only in their authored coarse threshold;
# no model, presenter or placement behavior is replaced by this fixture.

var _fixture_root := ""
var _resources: ResourceRoot
var _items: ItemDatabase


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
	}], {"thresholds_q16": PackedInt32Array([0, 20 << 16])}))
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
	index.build(placer.get_placed_models(), [])
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
