extends GutTest

# EditorGamePacker decides what goes INTO the single resource.pff and what stays loose beside
# it. Both halves matter to retail, and the mission rule is the one that cost a debugging
# session: an archived .bms mounts fine and resolves by name, and still never appears in any
# mission list, because retail builds that list from a loose FindFirstFile *.bms walk plus a
# per-archive walk over the localres/language volume PAIRS only
# [orig: MissionList_ScanAndBuildFromFiles @ 0x563170; Mission_BuildMapListFromPFF @ 0x562910].
# resource.pff is in neither. The menu is simply empty, with no error anywhere.

const PackerScript := preload("res://modtools/editor/editor_game_packer.gd")


class StubRoot:
	extends RefCounted
	var dir: String = ""
	func get_root_dir() -> String:
		return dir


func _make_root(files: Dictionary) -> Object:
	var dir := OS.get_user_data_dir().path_join("packer_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir)
	for name in files:
		var f := FileAccess.open(dir.path_join(String(name)), FileAccess.WRITE)
		f.store_string(String(files[name]))
		f.close()
	var root := StubRoot.new()
	root.dir = dir
	return root


func _pack(files: Dictionary) -> Dictionary:
	var root := _make_root(files)
	var out := OS.get_user_data_dir().path_join("packed_%d" % Time.get_ticks_usec())
	var result: Dictionary = PackerScript.pack(root, out)
	result["out_dir"] = out
	return result


func test_a_mission_and_its_title_bin_stay_loose() -> void:
	var result := _pack({
		"mnml.bms": "mission",
		"mnml.bin": "title",
		"items.def": "defs",
	})
	assert_true(result["ok"], "packing succeeded: %s" % result.get("error", ""))
	var loose: PackedStringArray = result["loose"]
	assert_true(loose.has("mnml.bms"),
			"the mission is loose, or retail's mission-list scan never sees it")
	assert_true(loose.has("mnml.bin"),
			"its sibling .bin carries the [Info] TITLE and travels with it")
	var archived: PackedStringArray = result["archived"]
	assert_false(archived.has("mnml.bms"), "and it is NOT also archived, which would double-list it")
	assert_false(archived.has("mnml.bin"), "nor is its title bin")
	# The loose files really landed.
	assert_true(FileAccess.file_exists(String(result["out_dir"]).path_join("mnml.bms")),
			"the .bms is written into the game dir")


func test_boot_string_tables_and_music_bins_still_go_in_the_archive() -> void:
	# Only a mission's OWN sibling .bin is loose. Every other .bin resolves from the archive
	# normally -- retail logs them as `PFF LOADED FILE: gametext.bin`.
	var result := _pack({
		"mnml.bms": "mission",
		"mnml.bin": "title",
		"gametext.bin": "strings",
		"vmacros.bin": "strings",
		"menumus.bin": "music script",
	})
	assert_true(result["ok"], "packing succeeded")
	var archived: PackedStringArray = result["archived"]
	for name in ["gametext.bin", "vmacros.bin", "menumus.bin"]:
		assert_true(archived.has(name), "%s is archived, not loosened by the mission rule" % name)
	var loose: PackedStringArray = result["loose"]
	assert_false(loose.has("gametext.bin"), "a boot string table is not a mission title")


func test_the_music_banks_and_early_error_text_stay_loose() -> void:
	# .sbf streams by path and never resolves through an archive; earlyerr.txt is read BEFORE
	# any mount, so inside an archive it could never be read at all.
	var result := _pack({
		"menumus.sbf": "bank",
		"earlyerr.txt": "error text",
		"items.def": "defs",
	})
	assert_true(result["ok"], "packing succeeded")
	var loose: PackedStringArray = result["loose"]
	assert_true(loose.has("menumus.sbf"), "the music bank stays loose")
	assert_true(loose.has("earlyerr.txt"), "the pre-archive error text stays loose")
	assert_true(PackedStringArray(result["archived"]).has("items.def"),
			"ordinary game data is archived")


func test_a_bin_with_no_mission_beside_it_is_archived() -> void:
	# The rule keys off an actual .bms; a lone .bin is not a mission title.
	var result := _pack({ "mnml.bin": "orphan", "items.def": "defs" })
	assert_true(result["ok"], "packing succeeded")
	assert_true(PackedStringArray(result["archived"]).has("mnml.bin"),
			"with no mnml.bms present, mnml.bin is just another archived file")
