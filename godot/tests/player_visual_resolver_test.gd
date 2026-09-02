extends GutTest

const AVATARS_FIXTURE := "res://../fixtures/avatars/synth_avatars.def"


func _load_avatar_db() -> AvatarDatabase:
	var db := AvatarDatabase.new()
	assert_eq(db.load(ProjectSettings.globalize_path(AVATARS_FIXTURE)), OK)
	return db


func test_non_default_character_identity_does_not_collapse_to_us01() -> void:
	var avatar_db := _load_avatar_db()
	# The PLAYER_INFO snapshot shape: the blue side's tree indices in its
	# side_profiles slot.
	var selected := {
		"team": 0,
		"side_profiles": [
			{"team": 0, "nationality": 0, "division": 0, "combo": 1},
			{},
		],
	}
	var profile := NetSessionDrive.character_join_profile_from_database(
			avatar_db, selected)
	var character_id := profile.get_character_id(0)
	var combo := avatar_db.get_combo(0, 0, 1)

	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/terrain/tmap")), OK)
	var placer := MissionObjectPlacer.create(root, null)
	placer.set_avatar_db(avatar_db)
	var spec := placer.resolve_player_visual_spec(0x14B9, character_id)

	assert_eq(character_id, 0x0400, "the chosen combo reaches the visual seam")
	assert_eq(String(spec.get("head", "")), combo.get_head().graphic)
	assert_eq(String(spec.get("body", "")), combo.get_body().graphic)
	assert_false(bool(spec.get("fallback", true)),
			"a valid non-default character must not use Player #1 / US01")


func test_selected_character_builds_composed_head_body_and_per_part_camo() -> void:
	var stamp := Time.get_ticks_usec()
	var item_path := ProjectSettings.globalize_path(
			"user://player_visual_items_%d.def" % stamp)
	var avatar_path := ProjectSettings.globalize_path(
			"user://player_visual_avatars_%d.def" % stamp)
	var items := FileAccess.open(item_path, FileAccess.WRITE)
	assert_not_null(items)
	items.store_string(
			"begin Player\n"
			+ " id 105310\n type person\n graphic person\nend\n")
	items.close()
	var avatars := FileAccess.open(avatar_path, FileAccess.WRITE)
	assert_not_null(avatars)
	avatars.store_string(
			"define head HEAD\n{\n graphic person.3di\n camo 32 64 96\n voice 3\n sex m\n}\n"
			+ "define body BODY\n{\n graphic person.3di\n camo 100 120 140\n}\n"
			+ "define arms ARMS\n{\n graphic person.3di\n camo 200 210 220\n}\n"
			+ "nationality 0 NAT\n{\n alignment good\n division 0 DIV\n {\n"
			+ "  combo 2 HEAD BODY ARMS\n }\n}\n")
	avatars.close()

	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(item_path), OK)
	var avatar_db := AvatarDatabase.new()
	assert_eq(avatar_db.load(avatar_path), OK)
	var resources := ResourceRoot.new()
	assert_eq(resources.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/synth")), OK)
	var placer := MissionObjectPlacer.create(resources, item_db)
	placer.set_avatar_db(avatar_db)
	var parent := Node3D.new()
	add_child(parent)
	var body: ObjectModel = placer.build_player_animated_model(
			0x14B9, parent, 0x0400)
	assert_not_null(body)
	assert_eq(int(body.get_meta("character_id", -1)), 0x0400)
	assert_eq(String(body.get_meta("avatar_part", "")), "body")
	var body_ctrl := body.get_ctrl_values()
	assert_eq(int(body_ctrl.get("TEX_CAMO1", -1)), 100)
	assert_eq(int(body_ctrl.get("TEX_CAMO2", -1)), 120)
	assert_eq(int(body_ctrl.get("TEX_CAMO3", -1)), 140,
			"retail stores authored camo bytes directly, without scaling")
	assert_eq(String(body.name), "PlayerAvatar_person",
			"the composed body keeps the player avatar node name")
	var head := body.find_child("PlayerAvatarHead_*", true, false) as ObjectModel
	assert_not_null(head, "the selected head is composed with the selected body")
	var head_ctrl := head.get_ctrl_values()
	assert_eq(int(head_ctrl.get("TEX_CAMO1", -1)), 32)
	assert_eq(int(head_ctrl.get("TEX_CAMO2", -1)), 64)
	assert_eq(int(head_ctrl.get("TEX_CAMO3", -1)), 96)
	body.set_ctrl_override("test:body_camo", "TEX_CAMO1", 77)
	assert_eq(int(body.get_ctrl_values().get("TEX_CAMO1", -1)), 77)
	assert_eq(int(head.get_ctrl_values().get("TEX_CAMO1", -1)), 32,
			"TEX_CAMO stays local to each retained draw, like retail's per-part stores")

	parent.free()
	DirAccess.remove_absolute(item_path)
	DirAccess.remove_absolute(avatar_path)



func test_runtime_player_type_resolves_to_us01_visual_item() -> void:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path("res://../fixtures/terrain/tmap")), OK)
	var placer := MissionObjectPlacer.create(root, null)
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
	var db := ItemDatabase.new()
	assert_eq(db.load(tmp), OK)
	var placer := MissionObjectPlacer.create(null, db)
	assert_eq(placer.resolve_player_visual_item_id(166), 100166)
	assert_eq(placer.resolve_player_visual_item_id(100166), 100166,
			"already-full authored ids stay unchanged")
	DirAccess.remove_absolute(tmp)
