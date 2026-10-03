extends GutTest

## The expansion's text-override table, end to end over a real packed install: the
## only file that serves it is a LOOSE expansion/<n>/<n>.bin, never the archived
## copy (JO:CA keeps jox01.bin only inside jox01L.pff); a fresh mount loads the
## loose file (retail's boot, where no archive is open yet), and a root switched in
## place (retail's menu and join switches, the old archives still open) loads it
## only under /d. Strings installs the root's table and follows every remount.
## The witness lives at the engine home (engine/base/vfs/vfs.h
## vfs_expansion_override_table); docs/interface/rtxt-strings-re.md "The override
## table's source" (D-RTXT-10).

const EXPANSION := "jox01"

var _dirs: Array[String] = []


func before_each() -> void:
	Strings.clear()


func after_each() -> void:
	Strings.clear()
	for dir in _dirs:
		_remove_install(dir)
	_dirs.clear()


func test_only_a_loose_bin_serves_a_fresh_mount() -> void:
	var dir := _make_install()
	var root := ResourceRoot.new()
	assert_eq(root.mount_runtime(dir, EXPANSION), OK)
	assert_eq(root.get_expansion(), EXPANSION, "the expansion mounted")
	assert_true(root.has_file(EXPANSION + ".bin"), "the archived <n>.bin is in the mounted set")
	assert_true(root.get_expansion_override_table().is_empty(),
		"the archived <n>.bin never serves the override table")

	_write_loose_bin(dir, "Loose title")
	var fresh := ResourceRoot.new()
	assert_eq(fresh.mount_runtime(dir, EXPANSION), OK)
	assert_eq(_title_of(fresh.get_expansion_override_table()), "Loose title",
		"a fresh mount loads the loose expansion/<n>/<n>.bin")

	var base := ResourceRoot.new()
	assert_eq(base.mount_runtime(dir, ""), OK)
	assert_true(base.get_expansion_override_table().is_empty(), "no expansion, no table")
	root.clear()
	fresh.clear()
	base.clear()


func test_an_in_place_switch_loads_the_loose_bin_only_under_d() -> void:
	var dir := _make_install()
	_write_loose_bin(dir, "Loose title")
	var root := ResourceRoot.new()
	assert_eq(root.mount_runtime(dir, ""), OK)
	assert_eq(root.mount_runtime(dir, EXPANSION), OK)
	assert_eq(root.get_expansion(), EXPANSION)
	assert_true(root.get_expansion_override_table().is_empty(),
		"a switch with the old archives open reaches no loose file without /d")

	var debug := ResourceRoot.new()
	assert_eq(debug.mount_runtime(dir, "", true), OK)
	assert_eq(debug.mount_runtime(dir, EXPANSION, true), OK)
	assert_eq(_title_of(debug.get_expansion_override_table()), "Loose title",
		"under /d the switch walks the loose file first")
	root.clear()
	debug.clear()


func test_strings_installs_the_table_and_follows_every_remount() -> void:
	var dir := _make_install()
	_write_loose_bin(dir, "Loose title")
	var root := ResourceRoot.new()
	assert_eq(root.mount_runtime(dir, EXPANSION), OK)
	Strings.register_table(Strings.TABLE_MENUTXT, _table("Base title"))
	Strings.track_expansion_override(root)
	assert_eq(Strings.lookup(Strings.TABLE_MENUTXT, "Menu", "TITLE"), "Loose title",
		"the tracked root's table is consulted before the named tables")

	assert_eq(root.mount_runtime(dir, ""), OK)
	assert_null(Strings.get_override_table(), "a remount to the base game clears it")
	assert_eq(Strings.lookup(Strings.TABLE_MENUTXT, "Menu", "TITLE"), "Base title")

	assert_eq(root.mount_runtime(dir, EXPANSION), OK)
	assert_null(Strings.get_override_table(),
		"the switch back, in place without /d, installs none")

	Strings.clear()
	assert_eq(root.mount_runtime(dir, EXPANSION, true), OK)
	assert_null(Strings.get_override_table(), "clear() stops following the root")
	root.clear()


# A packed install: resource.pff (the runtime mount's boot table) and the
# expansion pair, whose L archive carries <n>.bin as the stock install does.
func _make_install() -> String:
	var dir := OS.get_cache_dir().path_join("opennova_strings_override").path_join(
		"install_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(dir.path_join("expansion/" + EXPANSION)), OK)
	WorldFixture.write_pff(self, dir.path_join("resource.pff"), [{"name": "basetag.txt", "bytes": "BASE"}])
	WorldFixture.write_pff(self, dir.path_join("expansion/%s/%s.pff" % [EXPANSION, EXPANSION]),
		[{"name": "exptag.txt", "bytes": "EXP"}])
	WorldFixture.write_pff(self, dir.path_join("expansion/%s/%sL.pff" % [EXPANSION, EXPANSION]),
		[{"name": EXPANSION + ".bin", "bytes": _table("Archived title").to_byte_array()}])
	_dirs.append(dir)
	return dir


func _write_loose_bin(dir: String, title: String) -> void:
	var file := FileAccess.open(dir.path_join("expansion/%s/%s.bin" % [EXPANSION, EXPANSION]),
		FileAccess.WRITE)
	assert_not_null(file, "the loose <n>.bin is writable")
	file.store_buffer(_table(title).to_byte_array())
	file.close()


func _table(title: String) -> RtxtStringFile:
	var table := RtxtStringFile.new()
	table.add_section("Menu")
	table.add_entry("TITLE", title, 0, Vector2i())
	return table


func _title_of(bytes: PackedByteArray) -> String:
	if bytes.is_empty():
		return "<none>"
	var table := RtxtStringFile.new()
	if table.load_from_byte_array(bytes) != OK:
		return "<unparsed>"
	return table.get_string_in_section("Menu", "TITLE")


func _remove_install(dir: String) -> void:
	var exp_dir := "expansion/" + EXPANSION
	for sub in ["resource.pff", exp_dir + "/%s.pff" % EXPANSION, exp_dir + "/%sL.pff" % EXPANSION,
			exp_dir + "/%s.bin" % EXPANSION, exp_dir, "expansion"]:
		DirAccess.remove_absolute(dir.path_join(sub))
	DirAccess.remove_absolute(dir)
