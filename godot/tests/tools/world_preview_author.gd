extends SceneTree
## Recreate the original example mission through the native BMS writer.

func _initialize() -> void:
	var root := ResourceRoot.new()
	var directory := ProjectSettings.globalize_path("res://../examples/world_preview")
	assert(root.set_root_dir(directory) == OK)
	var terrain := TerrainData.new()
	assert(terrain.load_from_resource_root(root, "Tmap.trn") == OK)
	var mission := MissionData.new()
	assert(mission.create_default() == OK)
	assert(mission.set_header_string("mission_name", "World preview"))
	assert(mission.set_header_string("terrain", "Tmap"))
	assert(mission.set_header_string("environment", "mnml"))
	assert(mission.set_header_int("start_time", 12 * 256))
	var start := Vector3(0, terrain.get_height_world(Vector3.ZERO), 0)
	assert(mission.add_entity(MissionData.KIND_MARKER, MissionData.PLAYER_START_ITEM_ID,
			MissionObjectPlacer.godot_to_bms_position(start), Vector3.ZERO) != null)
	for point in [Vector3(-10, 0, -12), Vector3(10, 0, -12), Vector3(-10, 0, 12), Vector3(10, 0, 12)]:
		var position: Vector3 = point
		position.y = terrain.get_height_world(position)
		assert(mission.add_entity(MissionData.KIND_BUILDING, 108002,
				MissionObjectPlacer.godot_to_bms_position(position), Vector3.ZERO) != null)
	for x in range(-2, 3):
		var position := Vector3(x * 1.5, 0, 0)
		position.y = terrain.get_height_world(position)
		assert(mission.add_entity(MissionData.KIND_ITEM, 108001,
				MissionObjectPlacer.godot_to_bms_position(position), Vector3.ZERO) != null)
	assert(mission.save_as(directory.path_join("preview.bms")) == OK)
	print("Authored preview.bms; center terrain height: ", terrain.get_height_world(Vector3.ZERO))
	quit()
