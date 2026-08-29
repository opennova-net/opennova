extends GutTest

# Smoke test for the AvatarDatabase GDExtension binding over the committed
# minted synth_avatars.def fixture (tests/fixtures/minimal_avatars_gen.cpp). Mirrors the
# engine/formats/avatars ctests one layer up, and exercises the model bridge
# (get_model/set_model) and the save path.
const AVATARS_FIXTURE := "res://../fixtures/avatars/synth_avatars.def"


func _load() -> AvatarDatabase:
	var db := AvatarDatabase.new()
	var path := ProjectSettings.globalize_path(AVATARS_FIXTURE)
	assert_eq(db.load(path), OK, "Avatars.def fixture loads")
	return db


func _write_temp_avatars(name: String, text: String) -> String:
	var path := ProjectSettings.globalize_path("user://%s" % name)
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file, "temp avatar file opens for write")
	file.store_string(text)
	file.close()
	return path


func test_load_counts_and_spot_values() -> void:
	var db := _load()
	assert_true(db.is_loaded(), "is_loaded after load")
	assert_eq(db.get_part_count(), 26, "26 parts (12 head + 8 body + 6 arms)")
	assert_eq(db.get_nationality_count(), 8, "8 nationalities")

	var seal: Dictionary = db.get_part(AvatarDatabase.PART_HEAD, "SYN_HEAD_BOONIE")
	assert_eq(String(seal.get("display_name", "")), "AV_SYNTH_BOONIE")
	assert_eq(String(seal.get("graphic", "")), "synth_boonie.3di")
	assert_eq(int(seal.get("voice", -1)), 1)
	assert_eq(int(seal.get("sex", -1)), AvatarDatabase.SEX_MALE)

	assert_eq(db.get_part_names(AvatarDatabase.PART_HEAD).size(), 12, "12 head parts")
	assert_eq(db.get_part_names(AvatarDatabase.PART_BODY).size(), 8, "8 body parts")
	assert_eq(db.get_part_names(AvatarDatabase.PART_ARMS).size(), 6, "6 arms parts")


func test_tree_and_resolve() -> void:
	var db := _load()

	var us: Dictionary = db.get_nationality(0)
	assert_eq(String(us.get("name_key", "")), "AV_NAT_SYNTH_ALPHA")
	assert_eq(int(us.get("alignment", -1)), AvatarDatabase.ALIGN_GOOD)
	assert_eq(int(us.get("division_count", 0)), 4, "nationality 0 has 4 divisions")

	var d0: Dictionary = db.get_division(0, 0)
	assert_eq(String(d0.get("name_key", "")), "AV_DIV_SYNTH_0_0")
	assert_eq(String(d0.get("flags", "")), "skipdemo", "trailing flag preserved")
	assert_eq(int(d0.get("combo_count", 0)), 4)

	var resolved: Dictionary = db.resolve_combo(0, 0, 0)
	assert_eq(String(resolved.get("head_name", "")), "SYN_HEAD_BOONIE")
	assert_true(resolved.has("head"), "head part reference resolved")
	var head: Dictionary = resolved.get("head", {})
	assert_eq(String(head.get("graphic", "")), "synth_boonie.3di", "resolved head graphic")
	assert_true(resolved.has("body"), "body part reference resolved")
	assert_eq(int(resolved.get("alignment", -1)), AvatarDatabase.ALIGN_GOOD)


func test_simulation_installs_every_avatar_character_sex_row() -> void:
	var db := _load()
	var expected := 0
	for nat_index in range(db.get_nationality_count()):
		for div_index in range(db.get_division_count(nat_index)):
			expected += db.get_combo_count(nat_index, div_index)
	var sim := Simulation.new()
	assert_eq(sim.set_character_avatar_database(db), expected,
			"the sim receives one packed sex row per retail avatar combo")
	sim.free()


func test_resolve_combo_uses_parse_time_snapshots() -> void:
	var path := _write_temp_avatars("avatars_snapshot_test.def",
		"define head HEAD_A\n{\n\tgraphic old_head.3di\n}\n"
		+ "define body BODY_A\n{\n\tgraphic body.3di\n}\n"
		+ "define head HEAD_A\n{\n\tgraphic new_head.3di\n}\n"
		+ "nationality N00 AV_NAT\n{\n\talignment good\n\tdivision D00 AV_DIV\n\t{\n"
		+ "\t\tcombo 001 HEAD_A BODY_A\n\t}\n}\n"
		+ "define head HEAD_A\n{\n\tgraphic too_late.3di\n}\n")
	var db := AvatarDatabase.new()
	assert_eq(db.load(path), OK)
	var resolved: Dictionary = db.resolve_combo(0, 0, 0)
	var head: Dictionary = resolved.get("head", {})
	assert_eq(String(head.get("graphic", "")), "new_head.3di", "last prior duplicate wins")
	DirAccess.remove_absolute(path)


func test_diagnostics_expose_skipped_combo_references() -> void:
	var path := _write_temp_avatars("avatars_diagnostics_test.def",
		"define body BODY_A\n{\n\tgraphic body.3di\n}\n"
		+ "nationality N00 AV_NAT\n{\n\talignment good\n\tdivision D00 AV_DIV\n\t{\n"
		+ "\t\tcombo 001 MISSING_HEAD BODY_A\n\t}\n}\n")
	var db := AvatarDatabase.new()
	assert_eq(db.load(path), OK)
	assert_eq(db.get_combo_count(0, 0), 0, "unresolved required combo is skipped")
	var diagnostics: Array = db.get_diagnostics()
	assert_gt(diagnostics.size(), 0)
	assert_eq(String(diagnostics[0].get("code", "")), "combo_missing_head")
	assert_eq(int(diagnostics[0].get("severity", 0)), AvatarDatabase.DIAG_WARNING)
	DirAccess.remove_absolute(path)


func test_model_roundtrip_and_save() -> void:
	var db := _load()
	var parts := db.get_part_count()
	var nats := db.get_nationality_count()

	# get_model -> set_model preserves the model (the format mutation bridge).
	var db2 := AvatarDatabase.new()
	db2.set_model(db.get_model())
	assert_eq(db2.get_part_count(), parts, "set_model preserves part count")
	assert_eq(db2.get_nationality_count(), nats, "set_model preserves nationality count")

	# save_to_path -> reload preserves the model.
	var out_abs := ProjectSettings.globalize_path("user://avatars_save_test.def")
	assert_eq(db.save_to_path(out_abs), OK, "save_to_path")
	var db3 := AvatarDatabase.new()
	assert_eq(db3.load(out_abs), OK, "reload saved file")
	assert_eq(db3.get_part_count(), parts, "save round-trip part count")
	assert_eq(db3.get_nationality_count(), nats, "save round-trip nationality count")
	DirAccess.remove_absolute(out_abs)


func test_create_empty() -> void:
	var db := AvatarDatabase.new()
	db.create_empty()
	assert_true(db.is_loaded(), "empty model counts as loaded (new document)")
	assert_eq(db.get_part_count(), 0)
	assert_eq(db.get_nationality_count(), 0)
