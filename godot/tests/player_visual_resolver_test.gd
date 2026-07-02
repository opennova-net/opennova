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
