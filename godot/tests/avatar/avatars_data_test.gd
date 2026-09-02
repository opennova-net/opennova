extends GutTest

# Smoke test for the AvatarDatabase GDExtension binding over the committed
# minted synth_avatars.def fixture (tests/fixtures/minimal_avatars_gen.cpp). Mirrors the
# engine/formats/avatars ctests one layer up through the typed row records; the
# writer round trip is the avatars_roundtrip ctest's.
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

	var seal := db.get_part(AvatarDatabase.PART_HEAD, "SYN_HEAD_BOONIE")
	assert_not_null(seal)
	if seal == null:
		return
	assert_eq(seal.kind, AvatarDatabase.PART_HEAD)
	assert_eq(seal.name, "SYN_HEAD_BOONIE")
	assert_eq(seal.display_name, "AV_SYNTH_BOONIE")
	assert_eq(seal.graphic, "synth_boonie.3di")
	assert_eq(seal.voice, 1)
	assert_eq(seal.sex, AvatarDatabase.SEX_MALE)
	assert_null(db.get_part(AvatarDatabase.PART_BODY, "SYN_HEAD_BOONIE"),
			"a part lookup is kind-scoped")

	assert_eq(db.get_part_names(AvatarDatabase.PART_HEAD).size(), 12, "12 head parts")
	assert_eq(db.get_part_names(AvatarDatabase.PART_BODY).size(), 8, "8 body parts")
	assert_eq(db.get_part_names(AvatarDatabase.PART_ARMS).size(), 6, "6 arms parts")


func test_tree_and_resolve() -> void:
	var db := _load()

	var us := db.get_nationality(0)
	assert_not_null(us)
	assert_eq(us.name_key, "AV_NAT_SYNTH_ALPHA")
	assert_eq(us.alignment, AvatarDatabase.ALIGN_GOOD)
	assert_eq(us.division_count, 4, "nationality 0 has 4 divisions")
	assert_null(db.get_nationality(db.get_nationality_count()), "out of range is null")

	var d0 := db.get_division(0, 0)
	assert_not_null(d0)
	assert_eq(d0.name_key, "AV_DIV_SYNTH_0_0")
	assert_eq(d0.flags, "skipdemo", "trailing flag preserved")
	assert_eq(d0.combo_count, 4)

	var resolved := db.get_combo(0, 0, 0)
	assert_not_null(resolved)
	if resolved == null:
		return
	assert_eq(resolved.head_name, "SYN_HEAD_BOONIE")
	assert_eq(resolved.get_head().graphic, "synth_boonie.3di", "resolved head graphic")
	assert_not_null(resolved.get_body(), "body part reference resolved")
	assert_eq(resolved.alignment, AvatarDatabase.ALIGN_GOOD,
			"the combo carries its nationality's alignment")
	assert_eq([resolved.nationality_index, resolved.division_index, resolved.combo_index],
			[0, 0, 0], "the combo knows its tree indices")
	assert_eq(resolved.character_id, 0x0200,
			"the combo carries the packed id the registry derives for it")
	var by_id := db.resolve_character_id(0x0200, AvatarDatabase.ALIGN_GOOD)
	assert_not_null(by_id)
	assert_eq(by_id.head_name, resolved.head_name, "the packed id resolves to the same combo")
	assert_null(db.resolve_character_id(0x0200, AvatarDatabase.ALIGN_EVIL),
			"an alignment bit that contradicts the expected side resolves to nothing")
	assert_null(db.get_combo(0, 0, 99), "out of range is null")


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


func test_combo_parts_are_parse_time_snapshots() -> void:
	var path := _write_temp_avatars("avatars_snapshot_test.def",
		"define head HEAD_A\n{\n\tgraphic old_head.3di\n}\n"
		+ "define body BODY_A\n{\n\tgraphic body.3di\n}\n"
		+ "define head HEAD_A\n{\n\tgraphic new_head.3di\n}\n"
		+ "nationality N00 AV_NAT\n{\n\talignment good\n\tdivision D00 AV_DIV\n\t{\n"
		+ "\t\tcombo 001 HEAD_A BODY_A\n\t}\n}\n"
		+ "define head HEAD_A\n{\n\tgraphic too_late.3di\n}\n")
	var db := AvatarDatabase.new()
	assert_eq(db.load(path), OK)
	var resolved := db.get_combo(0, 0, 0)
	assert_not_null(resolved)
	if resolved != null:
		assert_eq(resolved.get_head().graphic, "new_head.3di", "last prior duplicate wins")
		assert_false(resolved.has_arms())
		assert_null(resolved.get_arms(), "a combo without arms has no arms row")
	var latest := db.get_part(AvatarDatabase.PART_HEAD, "HEAD_A")
	assert_eq(latest.graphic, "too_late.3di", "a part lookup sees the last definition")
	DirAccess.remove_absolute(path)


func test_diagnostics_expose_skipped_combo_references() -> void:
	var path := _write_temp_avatars("avatars_diagnostics_test.def",
		"define body BODY_A\n{\n\tgraphic body.3di\n}\n"
		+ "nationality N00 AV_NAT\n{\n\talignment good\n\tdivision D00 AV_DIV\n\t{\n"
		+ "\t\tcombo 001 MISSING_HEAD BODY_A\n\t}\n}\n")
	var db := AvatarDatabase.new()
	assert_eq(db.load(path), OK)
	assert_eq(db.get_combo_count(0, 0), 0, "unresolved required combo is skipped")
	var diagnostics := db.get_diagnostics()
	assert_gt(diagnostics.size(), 0)
	if diagnostics.size() > 0:
		var first: AvatarDiagnosticRow = diagnostics[0]
		assert_eq(first.code, "combo_missing_head")
		assert_eq(first.severity, AvatarDatabase.DIAG_WARNING)
	DirAccess.remove_absolute(path)
