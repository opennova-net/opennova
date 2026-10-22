class_name DeployMenuFixture
extends RefCounted

## The named controls consumed by DeployScreenPresenter, with authored text.
## Our base game's assets/death.mnu compiles in the menu-corpus test
## (mnu/mnu_corpus_test.gd).
## `targets` names the files to author into `res_dir` (death.mnu, gametext.bin,
## menutxt.bin). `exit_confirmation` authors the LEAVE MISSION button and its
## confirmation as death.mnu does: HIDDEN_BACK inside DEATH_SHROUD (Escape shows
## CONFIRM_EXIT and hides the shroud), and the HIDDEN MODAL CONFIRM_EXIT holding
## CONFIRM_YES (Return hides it) and CONFIRM_NO (Escape shows the shroud and hides
## it).
static func stage(test: GutTest, res_dir: String, targets: PackedStringArray,
		exit_confirmation := false) -> void:
	var dir := ProjectSettings.globalize_path(res_dir)
	test.assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	var body := MenuDriverFixture.wnd("list", "SPAWNPOINTS_LIST", 40)
	for name in ["STATIC_LIST_TITLE", "STATIC_RESPAWN_MSG1", "STATIC_PSPRESPAWN_MSG1",
			"STATIC_MEDIC_MSG1", "STATIC_CALLMEDIC_MSG", "STATIC_INSTRUCTIONS_MSG",
			"STATIC_INSTRUCTIONS2_MSG", "SWAP_TEAMS", "BUTTON_TEAMLIST"]:
		body += MenuDriverFixture.wnd("static", name, 200)
	# The LEAVE MISSION button and the exit confirmation's NO, which lower and raise
	# the screen's input pass-through word.
	var leave := ""
	if exit_confirmation:
		leave = MenuDriverFixture.wnd("button", "HIDDEN_BACK", 480,
				'<HOTKEY VIRTUAL>VK_ESCAPE</HOTKEY>'
				+ '<ACTION type="WINDOW" state="SHOW">CONFIRM_EXIT</ACTION>'
				+ '<ACTION type="WINDOW" state="HIDE">DEATH_SHROUD</ACTION>')
	else:
		body += MenuDriverFixture.wnd("button", "HIDDEN_BACK", 560)
		body += MenuDriverFixture.wnd("button", "CONFIRM_NO", 580)
	# The custom-draw MAP window inside the DEATH_SHROUD window, as death.mnu
	# nests them; placed right of the control column so it never covers a row.
	body += ('<WINDOW type="window" name="DEATH_SHROUD"><POSITION><LEFT>300</LEFT>'
			+ '<TOP>20</TOP><RIGHT>780</RIGHT><BOTTOM>545</BOTTOM></POSITION>'
			+ '<WINDOW type="window" name="MAP"><APPEARANCE type="custom" state="default">'
			+ '</APPEARANCE><POSITION><LEFT>10</LEFT><TOP>10</TOP><RIGHT>470</RIGHT>'
			+ '<BOTTOM>470</BOTTOM></POSITION></WINDOW>' + leave + '</WINDOW>')
	if exit_confirmation:
		body += ('<WINDOW type="window" name="CONFIRM_EXIT" HIDDEN MODAL><POSITION>'
				+ '<LEFT>250</LEFT><TOP>200</TOP><RIGHT>550</RIGHT><BOTTOM>300</BOTTOM>'
				+ '</POSITION>'
				+ MenuDriverFixture.wnd("button", "CONFIRM_YES", 10,
						'<HOTKEY VIRTUAL>VK_RETURN</HOTKEY>'
						+ '<ACTION type="WINDOW" state="HIDE">CONFIRM_EXIT</ACTION>')
				+ MenuDriverFixture.wnd("button", "CONFIRM_NO", 40,
						'<HOTKEY VIRTUAL>VK_ESCAPE</HOTKEY>'
						+ '<ACTION type="WINDOW" state="SHOW">DEATH_SHROUD</ACTION>'
						+ '<ACTION type="WINDOW" state="HIDE">CONFIRM_EXIT</ACTION>')
				+ '</WINDOW>')
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
