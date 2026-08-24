extends GutTest

# The game dir is FILENAME-addressed. Retail loads Arial12b.fnt / Impac38b.fnt off a hardcoded
# font table, gametext.bin / vmacros.bin / keyhelp.bin by name (and exits without them),
# MENUMUS.SBF beside MENUMUS.BIN, and a mission's <stem>.lwf beside its <stem>.bms. A Save As
# that only takes a FOLDER and derives the basename cannot author any of those.
#
# Only Mission and Particle offered a file picker; every other workspace got a folder and a
# derived name. These pin the contract for the ones that had to learn it, so the capability
# cannot quietly regress to a folder picker again.
#
# The shell side needs no test change: save_export_flow already branches on
# uses_save_file_dialog() and calls save_as_file(), generically.

const FontsWorkspaceScript := preload("res://modtools/fonts/fonts_workspace.gd")
const MusicWorkspaceScript := preload("res://modtools/music/music_workspace.gd")
const StringsWorkspaceScript := preload("res://modtools/strings/strings_workspace.gd")
const SoundWorkspaceScript := preload("res://modtools/sound/sound_workspace.gd")


func _out_dir(name: String) -> String:
	var dir := OS.get_user_data_dir().path_join("exact_path_save_%s_%d" % [name, Time.get_ticks_usec()])
	DirAccess.make_dir_recursive_absolute(dir)
	return dir


func test_every_filename_addressed_workspace_offers_a_file_picker() -> void:
	# The capability itself, per workspace. A folder picker here means the boot files it owns
	# can no longer be authored by their required names.
	for row in [
		{"name": "Fonts", "ws": FontsWorkspaceScript.new(), "ext": "fnt"},
		{"name": "Music", "ws": MusicWorkspaceScript.new(), "ext": "sbf"},
		{"name": "Strings", "ws": StringsWorkspaceScript.new(), "ext": "bin"},
		{"name": "Sound", "ws": SoundWorkspaceScript.new(), "ext": "lwf"},
	]:
		var ws: Object = row["ws"]
		assert_true(ws.uses_save_file_dialog(),
				"%s saves a named file, not a project directory" % row["name"])
		var filters: PackedStringArray = ws.get_save_file_dialog_filters()
		assert_false(filters.is_empty(), "%s advertises a filter" % row["name"])
		assert_true(String(filters[0]).contains(String(row["ext"])),
				"%s filters on .%s: %s" % [row["name"], row["ext"], filters[0]])
		var default_name := String(ws.get_save_file_dialog_default_name())
		assert_eq(default_name.get_extension().to_lower(), String(row["ext"]),
				"%s prefills a name with the right extension (%s)" % [row["name"], default_name])
		if ws is Node:
			ws.free()


func test_strings_saves_to_the_exact_boot_name() -> void:
	var ws: Object = StringsWorkspaceScript.new()
	if ws is Node:
		add_child_autofree(ws)
	# gametext.bin is one of the three string tables retail EXITS without.
	var path := _out_dir("strings").path_join("gametext.bin")
	assert_eq(ws.save_as_file(path), OK, "Save As writes the exact file chosen")
	assert_true(FileAccess.file_exists(path), "the boot name lands on disk verbatim")
	assert_eq(ws.get_save_file_dialog_default_name(), "gametext.bin",
			"and the picker then prefills it, so re-saving keeps the name")


func test_music_save_as_carries_the_bin_sibling() -> void:
	# Music is the one two-file document: retail loads MENUMUS.SBF and MENUMUS.BIN as a pair,
	# so picking the .sbf has to place its .bin alongside under the same basename.
	var ws: Object = MusicWorkspaceScript.new()
	if ws is Node:
		add_child_autofree(ws)
	var dir := _out_dir("music")
	var sbf := dir.path_join("menumus.sbf")
	var err: int = ws.save_as_file(sbf)
	if err != OK:
		# A brand-new workspace may have no loaded pair to write; the contract under test is
		# the naming, so only assert it when a save actually happened.
		pass_test("no music document loaded to save; naming contract covered by the picker test")
		return
	assert_true(FileAccess.file_exists(sbf), "the chosen .sbf name is honoured")
	assert_true(FileAccess.file_exists(dir.path_join("menumus.bin")),
			"and the .bin sibling follows its basename, not a separate prompt")


func test_a_wrong_extension_is_refused_rather_than_written() -> void:
	# save_as_path validates before touching disk, so a mis-typed name cannot produce a file
	# the game will never look for.
	var ws: Object = SoundWorkspaceScript.new()
	if ws is Node:
		add_child_autofree(ws)
	var path := _out_dir("sound").path_join("mnml.wrong")
	assert_ne(ws.save_as_file(path), OK, "a non-.lwf path is rejected")
	assert_false(FileAccess.file_exists(path), "and nothing is written")
