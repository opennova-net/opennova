class_name DeployMenuFixture
extends RefCounted

## The named controls consumed by DeployScreenPresenter, with authored text.
## Shipped death.mnu remains covered by the retail menu-corpus compilation test.
static func stage(test: GutTest, res_dir: String, staged: Dictionary) -> void:
	var dir := ProjectSettings.globalize_path(res_dir)
	test.assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	var body := MenuDriverFixture.wnd("list", "SPAWNPOINTS_LIST", 40)
	for name in ["STATIC_LIST_TITLE", "STATIC_RESPAWN_MSG1", "STATIC_PSPRESPAWN_MSG1",
			"STATIC_MEDIC_MSG1", "STATIC_CALLMEDIC_MSG", "STATIC_INSTRUCTIONS_MSG",
			"STATIC_INSTRUCTIONS2_MSG", "SWAP_TEAMS", "BUTTON_TEAMLIST"]:
		body += MenuDriverFixture.wnd("static", name, 200)
	var texts := RtxtStringFile.new()
	texts.add_section("Overlays")
	for key in ["STROVER_RESPAWN1", "STROVER_RESPAWN2"]:
		texts.add_entry(key, "Choose an authored spawn point.", 0, Vector2i())
	for target in staged.values():
		var bytes := PackedByteArray()
		match String(target).to_lower():
			"death.mnu":
				bytes = MenuDriverFixture.screen_xml("DEATH", body).to_utf8_buffer()
			"gametext.bin":
				bytes = texts.to_byte_array()
			"menutxt.bin":
				bytes = RtxtStringFile.new().to_byte_array()
			_:
				test.fail("unexpected deploy fixture file: %s" % target)
		var file := FileAccess.open(dir.path_join(target), FileAccess.WRITE)
		test.assert_not_null(file)
		if file != null:
			file.store_buffer(bytes)
			file.close()
