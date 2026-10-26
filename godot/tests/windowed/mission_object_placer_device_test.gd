extends GutTest

# The methods of tests/mission_object_placer_test.gd that need a RenderingDevice, split
# out of it so the windowed run selects them (G-100).

const ITEMS_PATH := "res://../fixtures/def/items.def"


func _abs(res_path: String) -> String:
	return ProjectSettings.globalize_path(res_path)


func _lod_population(container: Node, population_name: String) -> StaticPopulationInstance:
	var mmi := container.get_node_or_null("StaticPopulations/" + population_name) \
			as StaticPopulationInstance
	assert_not_null(mmi, "population %s is emitted" % population_name)
	return mmi


# --- Dense populations: only live rows reach the GPU and the cull -------------
# A population carries rows only for the slots at its level, packed [0, live)
# with visible_instance_count = live; a level switch swap-removes the slot from
# the old level's population (the last live row fills the hole) and appends it
# to the new one; a population with no live row is hidden so Godot's cull tree
# skips it. Headless Godot stores no MultiMesh instance data, so the row order
# is pinned through the placer's bookkeeping (get_static_population_live_bms_ids)
# and the device rows windowed.

const DENSE_NEAR_CAMERA := Transform3D(Basis.IDENTITY, Vector3(10, 0, 220))
const DENSE_MID_CAMERA := Transform3D(Basis.IDENTITY, Vector3(10, 0, 30))


func _dense_lod_switches(placer: MissionObjectPlacer, camera: Transform3D) -> int:
	return placer.update_static_lods(camera, 70.0, 640.0, 480.0)


# Three same-graphic buildings in one 512-unit bin at Godot z = 10 / 100 /
# 200 (BMS y = -10 / -100 / -200), a two-level graphic (level 0 above 20 px). The
# second one optionally carries the BMS NoShadow gate so the level
# populations need a filtered shadow twin.
func _dense_fixture(parent: Node3D, no_shadow_second: bool) -> Dictionary:
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var bms_ids: Array[int] = []
	for y in [-10.0, -100.0, -200.0]:
		var entity := mission.add_entity(
				MissionData.KIND_BUILDING, 105004, Vector3(10, y, 0), Vector3.ZERO)
		assert_not_null(entity)
		bms_ids.append(entity.bms_id)
	if no_shadow_second:
		assert_true(mission.set_entity_property_int(
				MissionData.KIND_BUILDING, 1, "ai_flags", 0x01000000))
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(_abs(ITEMS_PATH)), OK)
	var root := ResourceRoot.new()
	root.set_root_dir(_abs("res://../fixtures/def"))
	var placer := MissionObjectPlacer.create(root, item_db)
	var fine := BoxMesh.new()
	fine.size = Vector3(4, 4, 4)
	var coarse := BoxMesh.new()
	coarse.size = Vector3(3, 3, 3)
	assert_true(placer.register_resolved_static_graphic(
			"StaticCrate1", ObjectData.new(), [{
				"mesh": fine, "material": null,
				"offset": Transform3D.IDENTITY, "submesh": 0, "lod_index": 0,
			}, {
				"mesh": coarse, "material": null,
				"offset": Transform3D.IDENTITY, "submesh": 1, "lod_index": 1,
			}], {
				"thresholds_q16": PackedInt32Array([20 << 16, 0]),
				"sphere_radius": 2.0,
			}))
	var stats := placer.place(mission, parent)
	return {
		"placer": placer,
		"mission": mission,
		"stats": stats,
		"bms": bms_ids,
		"container": parent.get_node_or_null("MissionObjects"),
	}


func _live_bms(placer: MissionObjectPlacer, population: MultiMeshInstance3D) -> Array:
	return Array(placer.get_static_population_live_bms_ids(population))


func _device_multimesh_rows_available() -> bool:
	return DisplayServer.get_name() != "headless" and \
			RenderingServer.get_current_rendering_method() == "forward_plus"


func test_dense_population_device_rows_match_the_bookkeeping() -> void:
	if not _device_multimesh_rows_available():
		pending("MultiMesh instance rows need a windowed Forward+ run")
		return
	var parent := Node3D.new()
	add_child_autofree(parent)
	var fixture := _dense_fixture(parent, false)
	var placer: MissionObjectPlacer = fixture.placer
	var bms: Array[int] = fixture.bms
	var container: Node3D = fixture.container
	if container == null:
		return
	var level0 := _lod_population(container, "Batch_StaticCrate1_0")
	var level1 := _lod_population(container, "Batch_StaticCrate1_1")
	if level0 == null or level1 == null:
		return
	var expected := {}
	for index in range(3):
		expected[bms[index]] = MissionObjectPlacer.entity_transform(
				Vector3(10, [-10.0, -100.0, -200.0][index], 0), Vector3.ZERO)
	var check := func(population: MultiMeshInstance3D, label: String) -> void:
		var rows := _live_bms(placer, population)
		assert_eq(population.multimesh.visible_instance_count, rows.size(),
				"%s draws exactly its live rows" % label)
		for row in range(rows.size()):
			var device: Transform3D = population.multimesh.get_instance_transform(row)
			assert_true(device.is_equal_approx(expected[rows[row]]),
					"%s row %d carries the entity it is booked for" % [label, row])
			assert_gt(absf(device.basis.determinant()), 0.5,
					"no zero-scaled row inside the live range")
	check.call(level0, "level 0 at placement")
	assert_eq(level1.multimesh.visible_instance_count, 0)
	assert_eq(_dense_lod_switches(placer, DENSE_NEAR_CAMERA), 2)
	check.call(level0, "level 0 after the switch")
	check.call(level1, "level 1 after the switch")
	assert_eq(_dense_lod_switches(placer, DENSE_MID_CAMERA), 1)
	check.call(level0, "level 0 after the return")
	check.call(level1, "level 1 after the return")
	assert_true(placer.hide_static_instance(bms[2]) is Transform3D)
	check.call(level0, "level 0 after the carve")
	assert_true(placer.show_static_instance(bms[2]))
	check.call(level0, "level 0 after the restore")
