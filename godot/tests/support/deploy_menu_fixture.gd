class_name DeployMenuFixture
extends RefCounted

## The named controls consumed by DeployScreenPresenter, with authored text.
## Shipped death.mnu remains covered by the retail menu-corpus compilation test.
## `targets` names the files to author into `res_dir` (death.mnu, gametext.bin,
## menutxt.bin).
static func stage(test: GutTest, res_dir: String, targets: PackedStringArray) -> void:
	var dir := ProjectSettings.globalize_path(res_dir)
	test.assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	var body := MenuDriverFixture.wnd("list", "SPAWNPOINTS_LIST", 40)
	for name in ["STATIC_LIST_TITLE", "STATIC_RESPAWN_MSG1", "STATIC_PSPRESPAWN_MSG1",
			"STATIC_MEDIC_MSG1", "STATIC_CALLMEDIC_MSG", "STATIC_INSTRUCTIONS_MSG",
			"STATIC_INSTRUCTIONS2_MSG", "SWAP_TEAMS", "BUTTON_TEAMLIST"]:
		body += MenuDriverFixture.wnd("static", name, 200)
	# The custom-draw MAP window inside the DEATH_SHROUD window, as death.mnu
	# nests them; placed right of the control column so it never covers a row.
	body += ('<WINDOW type="window" name="DEATH_SHROUD"><POSITION><LEFT>300</LEFT>'
			+ '<TOP>20</TOP><RIGHT>780</RIGHT><BOTTOM>545</BOTTOM></POSITION>'
			+ '<WINDOW type="window" name="MAP"><APPEARANCE type="custom" state="default">'
			+ '</APPEARANCE><POSITION><LEFT>10</LEFT><TOP>10</TOP><RIGHT>470</RIGHT>'
			+ '<BOTTOM>470</BOTTOM></POSITION></WINDOW></WINDOW>')
	var texts := RtxtStringFile.new()
	texts.add_section("Overlays")
	for key in ["STROVER_RESPAWN1", "STROVER_RESPAWN2"]:
		texts.add_entry(key, "Choose an authored spawn point.", 0, Vector2i())
	for target in targets:
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
