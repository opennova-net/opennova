extends GutTest

var _root := ""
var _world: GameWorld

func after_each() -> void:
	if is_instance_valid(_world):
		_world.unload()
	_world = null
	if not _root.is_empty():
		TestFs.remove_dir_recursive(_root)
		_root = ""

func test_mission_field_submits_markers_and_restores_them_on_stop() -> void:
	var defs := FileAccess.get_file_as_string(RuntimeFixture.file("items.def")) + """
begin "Synthetic marked field"
 id 101896
 type decoration
 graphic pump_minefield
 attrib: LandMine notarget NoShadow
 hp 20
 armor -1 -1
 ai_function lndm
 render_function lndm
 ammo_closeattack AM_556MM
 ammo_marker3 AM_556MM
 huskfinal gun
 husk crate
end
"""
	_root = WorldFixture.stage_minimal_root("minefield", false, {"items.def": defs})
	for name in ["pump_minefield", "gun", "crate"]:
		assert_eq(DirAccess.copy_absolute(
				ProjectSettings.globalize_path("res://../fixtures/threedi/synth/%s.3di" % name),
				_root.path_join(name + ".3di")), OK)
	var records: Array[EntityRef] = []
	_world = WorldFixture.boot_minimal(self, _root, func(mission: MissionData) -> void:
		records.append(mission.add_entity(MissionData.KIND_BUILDING, 101896,
				Vector3(30, 0, -30), Vector3(0, 90, 0))))
	var runtime := _world.get_runtime()
	runtime.pause()
	var presenter := runtime.get_entity_presenter()
	presenter.present_passes()
	var source := runtime.get_entity_index().resolve_single(records[0].bms_id) as ObjectModel
	assert_not_null(source, "callback sources retain an individual LOD owner")
	if source == null:
		return
	for part in source.get_render_part_nodes().values():
		for mesh in (part as Node).find_children("*", "MeshInstance3D", true, false):
			assert_false((mesh as MeshInstance3D).visible, "source geometry is never submitted")
	var markers := _world.find_children("MineMarker_*", "ObjectModel", true, false)
	assert_eq(markers.size(), 7, "only the seven marked points among the first fourteen draw")
	var transforms: Array[Transform3D] = []
	for marker: ObjectModel in markers:
		assert_same(marker.get_parent(), source, "source visibility owns its markers")
		transforms.append(marker.global_transform)
		for part in marker.get_render_part_nodes().values():
			assert_true((part as Node3D).transform.is_equal_approx(Transform3D.IDENTITY),
					"every marker part takes the same world matrix")
	source.set_occlusion_hidden(true)
	for marker: ObjectModel in markers:
		assert_false(marker.is_visible_in_tree(), "occlusion hides markers in the same frame")
	source.set_occlusion_hidden(false)
	for marker: ObjectModel in markers:
		assert_true(marker.is_visible_in_tree())
	presenter.present_passes()
	assert_eq(_world.find_children("MineMarker_*", "ObjectModel", true, false).size(), 7)
	runtime.stop()
	await get_tree().process_frame
	presenter.present_passes()
	var restored := _world.find_children("MineMarker_*", "ObjectModel", true, false)
	assert_eq(restored.size(), 7, "stop restores the field and replaces marker lifetimes")
	for i in mini(restored.size(), transforms.size()):
		assert_true((restored[i] as Node3D).global_transform.is_equal_approx(transforms[i]),
				"layout and random rotations rewind together")
