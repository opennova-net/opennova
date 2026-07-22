extends GutTest

const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")


func test_runtime_player_type_resolves_to_us01_visual_item() -> void:
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path("res://../fixtures/godot/dvxi5")), OK)
	var placer := MissionObjectPlacer.new(root)
	var db := placer.get_item_db()
	assert_eq(db.get_graphic(0x14B9), "", "runtime type id is not an items.def authoring id")

	var visual_id := int(placer.resolve_player_visual_item_id(0x14B9))
	assert_eq(visual_id, 105310)
	assert_eq(db.get_graphic(visual_id), "US01")
	assert_eq(db.get_anim_def(visual_id), "US01")


func test_raw_runtime_item_type_resolves_to_full_items_def_id() -> void:
	var tmp := ProjectSettings.globalize_path(
			"user://runtime_item_resolver_%d.def" % Time.get_ticks_usec())
	var file := FileAccess.open(tmp, FileAccess.WRITE)
	assert_not_null(file)
	file.store_string(
			"begin AttachedTurret\n"
			+ "  id 100166\n"
			+ "  graphic M1trret\n"
			+ "end\n")
	file.close()
	var db := NovaItemDatabase.new()
	assert_eq(db.load(tmp), OK)
	var placer := MissionObjectPlacer.new(null, db)
	assert_eq(placer.resolve_player_visual_item_id(166), 100166)
	assert_eq(placer.resolve_player_visual_item_id(100166), 100166,
			"already-full authored ids stay unchanged")
	DirAccess.remove_absolute(tmp)
