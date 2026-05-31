extends GutTest


func test_runtime_and_modtools_do_not_ship_resource_roots() -> void:
	var forbidden_dirs := [
		"res://" + "assets",
		"res://modtools/" + "assets",
	]
	for dir_path in forbidden_dirs:
		assert_false(
			DirAccess.dir_exists_absolute(ProjectSettings.globalize_path(dir_path)),
			"%s must not exist; game data belongs in the configured resource root." % dir_path
		)


func test_resource_root_resolves_only_top_level_files() -> void:
	var root := _make_flat_root("flat_resolve")
	_write_file(root.path_join("Alpha.TRN"), "trn")
	DirAccess.make_dir_recursive_absolute(root.path_join("terrains"))
	_write_file(root.path_join("terrains/Dvxi5.trn"), "nested")

	var resources := NovaResourceRoot.new()
	assert_eq(resources.set_root_dir(root), OK)

	assert_eq(_norm(resources.resolve_file("alpha.trn")), _norm(root.path_join("Alpha.TRN")))
	assert_eq(resources.resolve_file("terrains/alpha.trn"), "", "Resource names must be flat basenames, not nested paths.")
	assert_string_contains(resources.get_last_error(), "flat filename", "Pathful lookups should explain the flat resource-root contract.")
	assert_eq(resources.resolve_file("Dvxi5.trn"), "", "Nested files are not part of the flat resource root.")

	var trns := resources.list_files(".trn")
	assert_eq(trns.size(), 1)
	assert_eq(String(trns[0]).get_file(), "Alpha.TRN")


func test_resource_root_reads_top_level_pff_entries() -> void:
	var root := _make_flat_root("pff_entries")
	_write_file(root.path_join("Alpha.TRN"), "loose trn")
	_write_pff(root.path_join("aa_base.pff"), [
		{"name": "Alpha.TRN", "bytes": "archived trn"},
		{"name": "Bravo.env", "bytes": "archived env"},
	])

	var resources := NovaResourceRoot.new()
	assert_eq(resources.set_root_dir(root), OK)

	assert_true(resources.has_file("bravo.env"), "PFF entries should be mounted by flat filename.")
	assert_eq(resources.read_file("Alpha.trn").get_string_from_utf8(), "loose trn", "Loose files should shadow archived entries.")
	assert_eq(resources.read_file("Bravo.env").get_string_from_utf8(), "archived env")

	var entries := resources.list_file_entries(".env")
	assert_eq(entries.size(), 1)
	assert_eq(String(entries[0].logical_name), "Bravo.env")
	assert_eq(String(entries[0].source_type), "pff")
	assert_eq(String(entries[0].archive_path).get_file(), "aa_base.pff")


func test_resource_root_mount_game_expansion_override() -> void:
	var root := _make_flat_root("expansion")
	DirAccess.make_dir_recursive_absolute(root.path_join("expansion/jox01"))
	_write_pff(root.path_join("resource.pff"), [
		{"name": "shared.env", "bytes": "base env"},
		{"name": "baseonly.trn", "bytes": "base trn"},
	])
	_write_pff(root.path_join("expansion/jox01/jox01.pff"), [
		{"name": "shared.env", "bytes": "main env"},
		{"name": "exponly.3di", "bytes": "exp model"},
	])
	_write_pff(root.path_join("expansion/jox01/jox01L.pff"), [
		{"name": "shared.env", "bytes": "local env"},
	])
	_write_file(root.path_join("expansion/jox01/shared.env"), "loose env")

	var resources := NovaResourceRoot.new()
	assert_eq(resources.mount_game(root, "jox01"), OK)
	# Override chain (high -> low): loose expansion > {name}L.pff > {name}.pff > base archives.
	assert_eq(resources.read_file("shared.env").get_string_from_utf8(), "loose env", "Loose expansion file wins over all archives.")
	assert_eq(resources.read_file("exponly.3di").get_string_from_utf8(), "exp model", "Expansion archive beats base.")
	assert_eq(resources.read_file("baseonly.trn").get_string_from_utf8(), "base trn", "Base archive still reachable.")
	# A missing expansion falls back to base-game mounting (no error).
	assert_eq(resources.mount_game(root, "doesnotexist"), OK)
	assert_eq(resources.read_file("baseonly.trn").get_string_from_utf8(), "base trn")


func test_resource_root_list_expansions() -> void:
	var root := _make_flat_root("list_expansions")
	# Two valid expansions (a subdir holding a matching <name>.pff) plus one incomplete
	# subdir (no matching pff) that must be excluded.
	DirAccess.make_dir_recursive_absolute(root.path_join("expansion/jox01"))
	DirAccess.make_dir_recursive_absolute(root.path_join("expansion/jox02"))
	DirAccess.make_dir_recursive_absolute(root.path_join("expansion/incomplete"))
	_write_pff(root.path_join("expansion/jox01/jox01.pff"), [{"name": "a.3di", "bytes": "x"}])
	_write_pff(root.path_join("expansion/jox02/jox02.pff"), [{"name": "b.3di", "bytes": "y"}])
	_write_file(root.path_join("expansion/incomplete/readme.txt"), "no pff here")

	# list_expansions does not require the root to be mounted (the UI lists before mounting).
	var expansions := NovaResourceRoot.new().list_expansions(root)
	assert_eq(expansions.size(), 2, "Only subdirs with a matching <name>.pff are expansions.")
	assert_true(expansions.has("jox01"))
	assert_true(expansions.has("jox02"))
	assert_false(expansions.has("incomplete"), "A subdir without <name>.pff is not an expansion.")

	# A root with no expansion/ dir yields an empty list (the UI hides the control).
	var base_only := _make_flat_root("list_expansions_base")
	assert_eq(NovaResourceRoot.new().list_expansions(base_only).size(), 0)


func test_resource_root_loads_dds_from_pff() -> void:
	# A DDS that exists only inside a .pff has no filesystem path, so it must decode from
	# the archived bytes (Godot 4.6 Image.load_dds_from_buffer). Round-trip a real image
	# through Godot's own DDS encoder for a genuinely decodable fixture.
	var image := Image.create(4, 4, false, Image.FORMAT_RGBA8)
	image.fill(Color(0.2, 0.4, 0.8, 1.0))
	var dds := image.save_dds_to_buffer()
	assert_gt(dds.size(), 4, "save_dds_to_buffer should produce DDS bytes.")

	var root := _make_flat_root("dds_pff")
	_write_pff(root.path_join("textures.pff"), [{"name": "swatch.dds", "bytes": dds}])

	var resources := NovaResourceRoot.new()
	assert_eq(resources.set_root_dir(root), OK)
	var tex: Texture2D = resources.load_texture("swatch.dds")
	assert_not_null(tex, "A DDS resident only inside a .pff should decode to a texture.")
	if tex != null:
		assert_eq(tex.get_width(), 4)
		assert_eq(tex.get_height(), 4)


func after_each() -> void:
	_remove_dir_recursive(OS.get_cache_dir().path_join("opennova_resource_root_contract"))


func _make_flat_root(name: String) -> String:
	var root := OS.get_cache_dir().path_join("opennova_resource_root_contract").path_join("%s_%d" % [name, Time.get_ticks_usec()])
	assert_eq(DirAccess.make_dir_recursive_absolute(root), OK)
	return root


func _write_file(path: String, text: String) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file, "Fixture should be writable: %s" % path)
	if file != null:
		file.store_string(text)
		file.close()


func _write_pff(path: String, entries: Array) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file, "PFF fixture should be writable: %s" % path)
	if file == null:
		return
	var header_size := 20
	var entry_size := 36
	var payload_offset := header_size + entries.size() * entry_size
	var next_payload_offset := payload_offset

	file.store_32(header_size)
	file.store_32(0x33464650)
	file.store_32(entries.size())
	file.store_32(entry_size)
	file.store_32(header_size)

	for entry in entries:
		var bytes := _entry_bytes(entry)
		file.store_32(0)
		file.store_32(next_payload_offset)
		file.store_32(bytes.size())
		file.store_32(0)
		var name_bytes := String(entry.name).to_utf8_buffer()
		for i in range(16):
			file.store_8(name_bytes[i] if i < name_bytes.size() else 0)
		file.store_32(0)
		next_payload_offset += bytes.size()

	for entry in entries:
		file.store_buffer(_entry_bytes(entry))
	file.close()


# A PFF entry payload is either a String (text fixtures) or a raw PackedByteArray (binary
# fixtures such as a DDS); normalize to bytes so both forms work.
func _entry_bytes(entry: Dictionary) -> PackedByteArray:
	return entry.bytes if entry.bytes is PackedByteArray else String(entry.bytes).to_utf8_buffer()


func _norm(path: String) -> String:
	return path.replace("\\", "/").to_lower()


func _remove_dir_recursive(path: String) -> void:
	var dir := DirAccess.open(path)
	if dir == null:
		return
	dir.list_dir_begin()
	var entry := dir.get_next()
	while entry != "":
		var child := path.path_join(entry)
		if dir.current_is_dir():
			_remove_dir_recursive(child)
		else:
			DirAccess.remove_absolute(child)
		entry = dir.get_next()
	dir.list_dir_end()
	DirAccess.remove_absolute(path)
