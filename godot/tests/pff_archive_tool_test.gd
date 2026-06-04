extends GutTest

## Controller tests for EditorPffTool. The file/dir/save pickers are injected as Callables, so the
## stubs capture each on_pick and the test drives the flows directly — no native dialogs needed.

var _cap: Dictionary = {}


func before_each() -> void:
	_cap = {}


func test_dialog_builds_and_lists_games() -> void:
	var t := _new_tool()
	assert_not_null(t._dialog, "ensure builds the dialog.")
	assert_eq(t._game_option.item_count, NovaPffArchive.list_games().size(), "Game dropdown lists every profile.")
	t._refresh_all()
	assert_true(t._add_button.disabled, "Add is disabled with no archive open.")
	assert_true(t._save_button.disabled, "Save As is disabled with no archive open.")
	assert_true(t._extract_all_button.disabled, "Extract All is disabled with no archive open.")
	assert_false(t._open_button.disabled, "Open is always available.")


func test_open_populates_tree_and_enables_buttons() -> void:
	var t := _new_tool()
	var path := _pff_dir().path_join("a.pff")
	_write_pff(path, [{"name": "a.txt", "bytes": "AAA"}, {"name": "b.txt", "bytes": "BB"}])
	t._on_archives_picked(PackedStringArray([path]))
	assert_eq(t._archive_list.item_count, 1, "Opened archive appears in the list.")
	assert_eq(t._tree.get_root().get_child_count(), 2, "Contents tree shows both entries.")
	assert_false(t._add_button.disabled)
	assert_false(t._save_button.disabled)
	assert_false(t._extract_all_button.disabled)
	assert_true(t._extract_selected_button.disabled, "No selection yet.")
	assert_true(t._remove_button.disabled, "No selection yet.")


func test_filter_narrows_tree() -> void:
	var t := _new_tool()
	var path := _pff_dir().path_join("filter.pff")
	_write_pff(path, [{"name": "alpha.txt", "bytes": "x"}, {"name": "beta.dat", "bytes": "y"}])
	t._on_archives_picked(PackedStringArray([path]))
	t._search.text = "alph"
	t._refresh_tree()
	assert_eq(t._tree.get_root().get_child_count(), 1, "Filter narrows to matching names.")


func test_selection_enables_extract_and_remove() -> void:
	var t := _new_tool()
	var path := _pff_dir().path_join("sel.pff")
	_write_pff(path, [{"name": "a.txt", "bytes": "AAA"}, {"name": "b.txt", "bytes": "BB"}])
	t._on_archives_picked(PackedStringArray([path]))
	t._tree.get_root().get_first_child().select(0)
	t._update_buttons()
	assert_false(t._extract_selected_button.disabled)
	assert_false(t._remove_button.disabled)
	assert_eq(t._selected_names().size(), 1, "One row selected.")


func test_add_marks_dirty_and_flags_list() -> void:
	var t := _new_tool()
	var root := _pff_dir()
	var path := root.path_join("add.pff")
	_write_pff(path, [{"name": "a.txt", "bytes": "AAA"}])
	t._on_archives_picked(PackedStringArray([path]))
	var extra := root.path_join("extra.bin")
	_write_file(extra, "new content")
	t._on_files_to_add(PackedStringArray([extra]))
	assert_eq(t._active_archive().get_entry_count(), 2, "Added file joins the model.")
	assert_true(t._active_archive().is_dirty())
	assert_string_contains(t._archive_list.get_item_text(0), "*", "Dirty archive shows a marker in the list.")


func test_remove_through_confirm_callback() -> void:
	var t := _new_tool()
	var path := _pff_dir().path_join("rm.pff")
	_write_pff(path, [{"name": "keep.txt", "bytes": "k"}, {"name": "drop.txt", "bytes": "d"}])
	t._on_archives_picked(PackedStringArray([path]))
	_select_row(t, "drop.txt")
	t._on_remove_pressed()
	assert_true(t._confirm_callback.is_valid(), "Remove stages a confirmation callback.")
	t._on_confirm_confirmed()
	assert_false(t._active_archive().has_file("drop.txt"), "Confirmed remove drops the entry.")
	assert_true(t._active_archive().has_file("keep.txt"))


func test_save_as_writes_a_new_archive() -> void:
	var t := _new_tool()
	var root := _pff_dir()
	var path := root.path_join("src.pff")
	_write_pff(path, [{"name": "a.txt", "bytes": "AAA"}])
	t._on_archives_picked(PackedStringArray([path]))
	t._on_save_as_pressed()
	assert_true(_cap.has("save_file"), "Save As routes through the injected save picker.")
	assert_eq(String(_cap["save_default"]), "src.pff", "Save defaults to the source basename.")
	var out := root.path_join("copy.pff")
	(_cap["save_file"] as Callable).call(out)
	assert_true(FileAccess.file_exists(out), "Save As writes the chosen file.")
	assert_false(t._active_archive().is_dirty(), "A successful save clears dirty.")


func test_extract_selected_through_dir_picker() -> void:
	var t := _new_tool()
	var root := _pff_dir()
	var path := root.path_join("ex.pff")
	_write_pff(path, [{"name": "a.txt", "bytes": "AAA"}])
	t._on_archives_picked(PackedStringArray([path]))
	_select_row(t, "a.txt")
	t._on_extract_selected_pressed()
	assert_true(_cap.has("open_dir"), "Extract Selected routes through the folder picker.")
	(_cap["open_dir"] as Callable).call(root)
	assert_eq(FileAccess.get_file_as_string(root.path_join("a.txt")), "AAA", "Selected file is extracted.")


# ---------------------------------------------------------------------------
# Harness
# ---------------------------------------------------------------------------

func _new_tool() -> EditorPffTool:
	var host := Control.new()
	add_child_autofree(host)
	var t := EditorPffTool.new()
	t.setup(host, _stub_open_files, _stub_save_file, _stub_open_dir, _stub_show_status)
	t._ensure_dialog()
	return t


func _select_row(t: EditorPffTool, name: String) -> void:
	var item := t._tree.get_root().get_first_child()
	while item != null:
		if String(item.get_metadata(0)) == name:
			item.select(0)
			return
		item = item.get_next()
	assert_true(false, "Row not found: %s" % name)


func _stub_open_files(_title, _filters, on_pick, _dir) -> void:
	_cap["open_files"] = on_pick


func _stub_save_file(_title, _filters, default_name, on_pick, _dir) -> void:
	_cap["save_file"] = on_pick
	_cap["save_default"] = default_name


func _stub_open_dir(_title, on_pick, _dir) -> void:
	_cap["open_dir"] = on_pick


func _stub_show_status(_text) -> void:
	pass


# ---------------------------------------------------------------------------
# Fixtures (mirror resource_root_contract_test._write_pff)
# ---------------------------------------------------------------------------

func after_each() -> void:
	_remove_dir_recursive(OS.get_cache_dir().path_join("opennova_pff_tool"))


func _pff_dir() -> String:
	var dir := OS.get_cache_dir().path_join("opennova_pff_tool").path_join(str(Time.get_ticks_usec()))
	assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	return dir


func _write_file(path: String, text: String) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file != null:
		file.store_string(text)
		file.close()


func _write_pff(path: String, entries: Array) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		return
	var header_size := 20
	var entry_size := 36
	var next_payload_offset := header_size + entries.size() * entry_size
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


func _entry_bytes(entry: Dictionary) -> PackedByteArray:
	return entry.bytes if entry.bytes is PackedByteArray else String(entry.bytes).to_utf8_buffer()


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
