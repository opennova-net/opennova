extends GutTest

# GamePacker's two flavours. stage_loose is the retail-test layout: everything
# loose plus the zero-entry resource.pff boot token, run under /d. pack/export_game is the
# tagged-release layout: one localres.pff beside the loose-by-contract files — and the
# archive NAME is the one that cost a debugging session: retail builds its mission list from
# a loose FindFirstFile *.bms walk plus a per-archive walk over the localres/language volumes
# only [orig: MissionList_ScanAndBuildFromFiles @ 0x563170; Mission_BuildMapListFromPFF
# @ 0x562910]. A mission archived in resource.pff is in neither — mounted, loadable by name,
# and absent from every list. Retail keeps all 116 of its own missions in localres.pff.

const PackerScript := preload("res://tools/game_packer.gd")
var _scratch_dirs: Array[String] = []


func after_each() -> void:
	for directory in _scratch_dirs:
		TestFs.remove_dir_recursive(directory)
	_scratch_dirs.clear()


func _write_files(dir: String, files: Dictionary) -> void:
	DirAccess.make_dir_recursive_absolute(dir)
	for name in files:
		var f := FileAccess.open(dir.path_join(String(name)), FileAccess.WRITE)
		f.store_string(String(files[name]))
		f.close()


func _make_root(files: Dictionary) -> String:
	var dir := OS.get_user_data_dir().path_join("packer_%d" % Time.get_ticks_usec())
	_write_files(dir, files)
	_scratch_dirs.append(dir)
	return dir


func _fresh_out_dir() -> String:
	var directory := OS.get_user_data_dir().path_join("packed_%d" % Time.get_ticks_usec())
	_scratch_dirs.append(directory)
	return directory


func _pack(files: Dictionary, out: String = "") -> GamePackResult:
	var root_dir := _make_root(files)
	if out.is_empty():
		out = _fresh_out_dir()
	var result: GamePackResult = PackerScript.pack(root_dir, out)
	return result


func test_the_archive_is_localres_and_missions_go_in_it() -> void:
	var result := _pack({
		"mnml.bms": "mission",
		"mnml.bin": "title",
		"items.def": "defs",
	})
	assert_true(result.ok, "packing succeeded: %s" % result.error)
	assert_eq(String(result.archive).get_file(), "localres.pff",
			"the archive carries the name retail's mission-list scan visits")
	var archived: PackedStringArray = result.archived
	assert_true(archived.has("mnml.bms"), "the mission is archived, where retail ships its own")
	assert_true(archived.has("mnml.bin"), "its title .bin resolves by name from the same archive")
	assert_true(archived.has("items.def"), "ordinary game data is archived")
	var loose: PackedStringArray = result.loose
	assert_false(loose.has("mnml.bms"), "no loose copy: a loose file would shadow the archive under /d")
	assert_false(FileAccess.file_exists(String(result.archive.get_base_dir()).path_join("mnml.bms")),
			"nothing mission-shaped is written loose into the game dir")


func test_the_music_banks_and_early_error_text_stay_loose() -> void:
	# .sbf streams by path and never resolves through an archive; earlyerr.txt is read BEFORE
	# any mount, so inside an archive it could never be read at all.
	var result := _pack({
		"menumus.sbf": "bank",
		"earlyerr.txt": "error text",
		"items.def": "defs",
	})
	assert_true(result.ok, "packing succeeded")
	var loose: PackedStringArray = result.loose
	assert_true(loose.has("menumus.sbf"), "the music bank stays loose")
	assert_true(loose.has("earlyerr.txt"), "the pre-archive error text stays loose")
	assert_true(PackedStringArray(result.archived).has("items.def"),
			"ordinary game data is archived")


func test_the_packed_dir_is_self_ignoring() -> void:
	# The packed dir lands beside whatever root is mounted -- possibly inside another git repo.
	var result := _pack({ "items.def": "defs" })
	assert_true(result.ok, "packing succeeded")
	var marker := String(result.archive.get_base_dir()).path_join(".gitignore")
	assert_true(FileAccess.file_exists(marker), "a .gitignore is written into the packed dir")
	assert_true(FileAccess.get_file_as_string(marker).contains("\n*\n"), "and it ignores everything")


func test_repacking_removes_a_previous_packs_stale_files() -> void:
	# Rename an asset and repack: the old loose file must not survive, or retail's /d lookup
	# (loose wins) keeps serving the deleted one.
	var out := _fresh_out_dir()
	var first := _pack({ "oldsong.sbf": "bank", "items.def": "defs" }, out)
	assert_true(first.ok, "first pack succeeded")
	assert_true(FileAccess.file_exists(out.path_join("oldsong.sbf")), "the first pack wrote its bank")

	var second := _pack({ "newsong.sbf": "bank", "items.def": "defs" }, out)
	assert_true(second.ok, "repacking into the same dir succeeded: %s" % second.error)
	assert_false(FileAccess.file_exists(out.path_join("oldsong.sbf")),
			"the renamed-away bank is gone from the packed dir")
	assert_true(FileAccess.file_exists(out.path_join("newsong.sbf")), "the new bank is there")


func test_a_foreign_non_empty_dir_is_refused_not_wiped() -> void:
	var out := _fresh_out_dir()
	_write_files(out, { "precious.txt": "not ours" })
	var result := _pack({ "items.def": "defs" }, out)
	assert_false(result.ok, "packing into somebody else's non-empty dir is refused")
	assert_true(String(result.error).contains("not empty"), "and says why: %s" % result.error)
	assert_true(FileAccess.file_exists(out.path_join("precious.txt")), "nothing was deleted")


func test_subdirectories_are_reported_not_silently_dropped() -> void:
	var root := _make_root({ "items.def": "defs" })
	DirAccess.make_dir_recursive_absolute(root.path_join("ptl"))
	DirAccess.make_dir_recursive_absolute(root.path_join("src"))
	var result: GamePackResult = PackerScript.pack(root, _fresh_out_dir())
	assert_true(result.ok, "packing succeeded")
	var skipped_dirs: PackedStringArray = result.skipped_dirs
	assert_true(skipped_dirs.has("ptl"), "a data subdirectory the packer did not walk is named")
	assert_false(skipped_dirs.has("src"), "the authoring-sources dir is expected and not reported")


func test_an_entry_name_is_limited_by_bytes_not_characters() -> void:
	# 13 characters, but the accented ones are two bytes each in UTF-8: 16 chars would fit, 16
	# bytes do not. The pff binding rejects on bytes, so the precheck must count the same way.
	var long_name := "ééééééééé.def"  # 9 x 2-byte + 4 = 22 bytes
	var result := _pack({ long_name: "defs", "items.def": "defs" })
	assert_true(result.ok, "packing succeeded")
	assert_true(PackedStringArray(result.skipped).has(long_name),
			"the over-long name is skipped rather than handed to a writer that refuses it")


func test_export_game_updates_a_populated_game_dir_without_touching_the_rest() -> void:
	# The "update the shipped game" seam: the target dir legitimately holds exes and
	# anything else -- only the game's own artifact names are written.
	var root := _make_root({
		"items.def": "defs",
		"mnml.bms": "mission",
		"menumus.sbf": "bank",
		"earlyerr.txt": "error text",
	})
	var game_dir := _fresh_out_dir()
	_write_files(game_dir, { "opennova.exe": "exe", "precious.txt": "keep me" })

	var result: GamePackResult = PackerScript.export_game(root, game_dir)
	assert_true(result.ok, "export succeeded: %s" % result.error)
	assert_true(FileAccess.file_exists(game_dir.path_join("localres.pff")), "the archive landed")
	assert_eq(FileAccess.get_file_as_string(game_dir.path_join("menumus.sbf")), "bank",
			"the loose-by-contract bank landed beside it")
	assert_true(FileAccess.file_exists(game_dir.path_join("earlyerr.txt")), "so did the error text")
	assert_eq(FileAccess.get_file_as_string(game_dir.path_join("opennova.exe")), "exe",
			"the exe already there is untouched")
	assert_eq(FileAccess.get_file_as_string(game_dir.path_join("precious.txt")), "keep me",
			"and so is everything that is not a game artifact")
	assert_false(FileAccess.file_exists(game_dir.path_join(".gitignore")),
			"the pack scratch's marker never reaches the game dir")


func test_export_game_repacks_in_place() -> void:
	var game_dir := _fresh_out_dir()
	var first := _make_root({ "items.def": "version one" })
	assert_true(PackerScript.export_game(first, game_dir).ok, "first export")
	var before := FileAccess.get_file_as_bytes(game_dir.path_join("localres.pff"))

	var second := _make_root({ "items.def": "version two, longer than the first" })
	assert_true(PackerScript.export_game(second, game_dir).ok, "second export")
	var after := FileAccess.get_file_as_bytes(game_dir.path_join("localres.pff"))
	assert_ne(before, after, "the archive carries the edit -- exports repack in place")


func test_pack_game_cli_exports_through_the_same_seam() -> void:
	var root := _make_root({ "items.def": "defs" })
	var game_dir := _fresh_out_dir()
	assert_eq(PackGameCli.run(PackedStringArray(["--pack-game", root, game_dir])), 0,
			"the CLI exports and reports success")
	assert_true(FileAccess.file_exists(game_dir.path_join("localres.pff")),
			"and the archive is in the game dir")
	assert_eq(PackGameCli.run(PackedStringArray(["--pack-game", root])), 1,
			"a missing game dir argument is a usage error")
	assert_eq(PackGameCli.run(PackedStringArray(
			["--pack-game", game_dir.path_join("no-such-dir"), game_dir])), 1,
			"a missing source directory is an error")


func test_stage_loose_ships_everything_loose_with_the_boot_token() -> void:
	# The retail play-test layout: every packable file loose (the mission included -- the
	# loose FindFirstFile *.bms walk lists it) plus the 20-byte zero-entry resource.pff
	# that clears retail's archives-OPENED boot gate (witnessed 2026-08-23).
	var root := _make_root({
		"items.def": "defs",
		"mnml.bms": "mission",
		"menumus.sbf": "bank",
		"earlyerr.txt": "error text",
		"README.md": "repo metadata",
	})
	var out := _fresh_out_dir()
	var result: GamePackResult = PackerScript.stage_loose(root, out)
	assert_true(result.ok, "staging succeeded: %s" % result.error)
	for name in ["items.def", "mnml.bms", "menumus.sbf", "earlyerr.txt"]:
		assert_true(FileAccess.file_exists(out.path_join(name)), "%s is staged loose" % name)
	assert_false(FileAccess.file_exists(out.path_join("README.md")),
			"repo metadata stays excluded, exactly as when packing")
	var token := FileAccess.get_file_as_bytes(out.path_join("resource.pff"))
	assert_eq(token.size(), 20, "the boot token is the zero-entry 20-byte archive")
	assert_false(FileAccess.file_exists(out.path_join("localres.pff")),
			"nothing is archived in this flavour")


func test_stage_loose_preserves_an_existing_packed_layout() -> void:
	var root := _make_root({
		"localres.pff": "existing packed game",
		"menumus.sbf": "bank",
	})
	var out := _fresh_out_dir()
	var result: GamePackResult = PackerScript.stage_loose(root, out)
	assert_true(result.ok, "staging succeeded: %s" % result.error)
	assert_eq(FileAccess.get_file_as_string(out.path_join("localres.pff")),
			"existing packed game", "the selected pack is copied unchanged")
	assert_false(FileAccess.file_exists(out.path_join("resource.pff")),
			"an existing boot archive does not get an unrelated token")
	assert_true(FileAccess.file_exists(out.path_join("menumus.sbf")),
			"loose-by-contract files can accompany the pack")


func test_stage_loose_keeps_the_pack_output_discipline() -> void:
	# Same marker/wipe/refuse rules as pack(): a repack drops stale files, and a foreign
	# non-empty dir is refused, never wiped.
	var out := _fresh_out_dir()
	var first: GamePackResult = PackerScript.stage_loose(_make_root({ "old.def": "v1" }), out)
	assert_true(first.ok, "first stage")
	var second: GamePackResult = PackerScript.stage_loose(_make_root({ "new.def": "v2" }), out)
	assert_true(second.ok, "restaging into the same dir succeeded: %s" % second.error)
	assert_false(FileAccess.file_exists(out.path_join("old.def")), "stale loose files are wiped")
	assert_true(FileAccess.file_exists(out.path_join("new.def")), "the new set is there")

	var foreign := _fresh_out_dir()
	_write_files(foreign, { "precious.txt": "not ours" })
	var refused: GamePackResult = PackerScript.stage_loose(_make_root({ "a.def": "defs" }), foreign)
	assert_false(refused.ok, "a foreign non-empty dir is refused")
	assert_true(FileAccess.file_exists(foreign.path_join("precious.txt")), "and untouched")


func test_stage_retail_fails_when_the_runtime_is_incomplete() -> void:
	# A never-launched install has no game.cfg, and retail's first launch without one hangs in
	# video enumeration before the menu. That must be an error, not a silent partial stage.
	var root := _make_root({ "items.def": "defs" })
	var retail := _fresh_out_dir()
	_write_files(retail, { "Jointops.exe": "exe", "binkw32_.dll": "bink" })
	var result: RetailStageResult = PackerScript.stage_retail(root, retail, _fresh_out_dir())
	assert_false(result.ok, "an incomplete retail runtime does not stage")
	assert_true(result.error.contains("game.cfg"), "and names the missing file: %s" % result.error)
	assert_true(PackerScript.retail_install_error(retail).contains("game.cfg"),
			"readiness reports the same actionable problem before launch")


func test_stage_retail_stages_every_runtime_file() -> void:
	var root := _make_root({ "items.def": "defs", "mnml.bms": "mission" })
	var retail := _fresh_out_dir()
	_write_files(retail, { "Jointops.exe": "exe", "binkw32_.dll": "real bink",
			"binkw32.dll": "hook shim", "game.cfg": "cfg" })
	var result: RetailStageResult = PackerScript.stage_retail(root, retail, _fresh_out_dir())
	assert_true(result.ok, "staging succeeded: %s" % result.error)
	var out := result.packed_dir
	assert_eq(result.exe, out.path_join("Jointops.exe"), "the exe to launch is the staged copy")
	assert_eq(FileAccess.get_file_as_string(out.path_join("binkw32.dll")), "real bink",
			"the underscored real Bink is staged under the name retail loads, not the hook shim")
	assert_true(FileAccess.file_exists(out.path_join("game.cfg")), "game.cfg is staged")
	assert_true(FileAccess.file_exists(out.path_join("mnml.bms")), "the mission is loose (the list walk finds it)")
	assert_true(FileAccess.file_exists(out.path_join("resource.pff")), "the boot token is beside them")


func test_pack_and_stage_refuse_overlapping_source_and_output_even_with_a_marker() -> void:
	var root := _make_root({"items.def": "original", ".gitignore": PackerScript.MARKER_TEXT})
	for output in [root, root.path_join("child"), root.get_base_dir(), root.path_join("../" + root.get_file())]:
		assert_false(PackerScript.pack(root, output).ok)
		assert_false(PackerScript.stage_loose(root, output).ok)
		assert_false(PackerScript.export_game(root, output).ok)
		assert_eq(FileAccess.get_file_as_string(root.path_join("items.def")), "original")
	assert_false(DirAccess.dir_exists_absolute(root.path_join("child")))


func test_a_foreign_directory_containing_only_subdirectories_is_preserved() -> void:
	var root := _make_root({"items.def": "defs"})
	var output := _fresh_out_dir()
	_write_files(output.path_join("precious"), {"notes.txt": "keep"})
	assert_false(PackerScript.pack(root, output).ok)
	assert_false(PackerScript.stage_loose(root, output).ok)
	assert_eq(FileAccess.get_file_as_string(output.path_join("precious/notes.txt")), "keep")
	assert_false(FileAccess.file_exists(output.path_join(PackerScript.MARKER_NAME)))


func test_packing_archive_backed_sources_fails_before_changing_output() -> void:
	var root := _make_root({"resource.pff": "archive", "items.def": "override"})
	var output := _fresh_out_dir()
	_write_files(output, {"menumus.sbf": "previous", ".gitignore": PackerScript.MARKER_TEXT})
	var result := PackerScript.pack(root, output)
	assert_false(result.ok)
	assert_string_contains(result.error, "loose source")
	assert_eq(FileAccess.get_file_as_string(output.path_join("menumus.sbf")), "previous")


func test_retail_install_cannot_be_its_own_stage() -> void:
	var root := _make_root({"items.def": "defs"})
	var stage := _fresh_out_dir()
	var result := PackerScript.stage_retail(root, stage, stage)
	assert_false(result.ok)
	assert_string_contains(result.error, "outside the source tree")
