extends GutTest

# Smoke test for the NovaAvatarDatabase GDExtension binding over the committed
# retail Avatars.def fixture (fixtures/avatars/Avatars.def). Mirrors the
# libs/avatars ctests one layer up, and exercises the editor bridge
# (get_model/set_model) and the save path.
const AVATARS_FIXTURE := "res://../fixtures/avatars/Avatars.def"


func _load() -> NovaAvatarDatabase:
	var db := NovaAvatarDatabase.new()
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
	assert_eq(db.get_part_count(), 109, "109 parts (51 head + 36 body + 22 arms)")
	assert_eq(db.get_nationality_count(), 8, "8 nationalities")

	var seal: Dictionary = db.get_part(NovaAvatarDatabase.PART_HEAD, "JO_HEAD_SEAL")
	assert_eq(String(seal.get("display_name", "")), "AV_BOONIEHAT")
	assert_eq(String(seal.get("graphic", "")), "Boonie.3di")
	assert_eq(int(seal.get("voice", -1)), 1)
	assert_eq(int(seal.get("sex", -1)), NovaAvatarDatabase.SEX_MALE)

	assert_eq(db.get_part_names(NovaAvatarDatabase.PART_HEAD).size(), 51, "51 head parts")
	assert_eq(db.get_part_names(NovaAvatarDatabase.PART_BODY).size(), 36, "36 body parts")
	assert_eq(db.get_part_names(NovaAvatarDatabase.PART_ARMS).size(), 22, "22 arms parts")


func test_tree_and_resolve() -> void:
	var db := _load()

	var us: Dictionary = db.get_nationality(0)
	assert_eq(String(us.get("name_key", "")), "AV_NAT_UNITEDSTATES")
	assert_eq(int(us.get("alignment", -1)), NovaAvatarDatabase.ALIGN_GOOD)
	assert_eq(int(us.get("division_count", 0)), 9, "US has 9 divisions")

	var d0: Dictionary = db.get_division(0, 0)
	assert_eq(String(d0.get("name_key", "")), "AV_DIV_SEAL")
	assert_eq(String(d0.get("flags", "")), "skipdemo", "trailing flag preserved")
	assert_eq(int(d0.get("combo_count", 0)), 4)

	var resolved: Dictionary = db.resolve_combo(0, 0, 0)
	assert_eq(String(resolved.get("head_name", "")), "JO_HEAD_SEAL")
	assert_true(resolved.has("head"), "head part reference resolved")
	var head: Dictionary = resolved.get("head", {})
	assert_eq(String(head.get("graphic", "")), "Boonie.3di", "resolved head graphic")
	assert_true(resolved.has("body"), "body part reference resolved")
	assert_eq(int(resolved.get("alignment", -1)), NovaAvatarDatabase.ALIGN_GOOD)


func test_resolve_combo_uses_parse_time_snapshots() -> void:
	var path := _write_temp_avatars("avatars_snapshot_test.def",
		"define head HEAD_A\n{\n\tgraphic old_head.3di\n}\n"
		+ "define body BODY_A\n{\n\tgraphic body.3di\n}\n"
		+ "define head HEAD_A\n{\n\tgraphic new_head.3di\n}\n"
		+ "nationality N00 AV_NAT\n{\n\talignment good\n\tdivision D00 AV_DIV\n\t{\n"
		+ "\t\tcombo 001 HEAD_A BODY_A\n\t}\n}\n"
		+ "define head HEAD_A\n{\n\tgraphic too_late.3di\n}\n")
	var db := NovaAvatarDatabase.new()
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
	var db := NovaAvatarDatabase.new()
	assert_eq(db.load(path), OK)
	assert_eq(db.get_combo_count(0, 0), 0, "unresolved required combo is skipped")
	var diagnostics: Array = db.get_diagnostics()
	assert_gt(diagnostics.size(), 0)
	assert_eq(String(diagnostics[0].get("code", "")), "combo_missing_head")
	assert_eq(int(diagnostics[0].get("severity", 0)), NovaAvatarDatabase.DIAG_WARNING)
	DirAccess.remove_absolute(path)


func test_model_roundtrip_and_save() -> void:
	var db := _load()
	var parts := db.get_part_count()
	var nats := db.get_nationality_count()

	# get_model -> set_model preserves the model (the editor authoring bridge).
	var db2 := NovaAvatarDatabase.new()
	db2.set_model(db.get_model())
	assert_eq(db2.get_part_count(), parts, "set_model preserves part count")
	assert_eq(db2.get_nationality_count(), nats, "set_model preserves nationality count")

	# save_to_path -> reload preserves the model (the editor save path).
	var out_abs := ProjectSettings.globalize_path("user://avatars_save_test.def")
	assert_eq(db.save_to_path(out_abs), OK, "save_to_path")
	var db3 := NovaAvatarDatabase.new()
	assert_eq(db3.load(out_abs), OK, "reload saved file")
	assert_eq(db3.get_part_count(), parts, "save round-trip part count")
	assert_eq(db3.get_nationality_count(), nats, "save round-trip nationality count")
	DirAccess.remove_absolute(out_abs)


func test_create_empty() -> void:
	var db := NovaAvatarDatabase.new()
	db.create_empty()
	assert_true(db.is_loaded(), "empty model counts as loaded (new document)")
	assert_eq(db.get_part_count(), 0)
	assert_eq(db.get_nationality_count(), 0)
