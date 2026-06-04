class_name EditorPffTool
extends RefCounted

## The OpenNova Editor's PFF archive tool: open one or more .pff archives, browse their
## contents, extract files (decoded or raw), add files from disk, delete files, and write the
## result to a NEW archive (Save As — the source files are never modified). Launched from a
## button in the Settings popover, not as a workspace.
##
## Built as a child of the host shell (so it inherits the editor theme), with the capabilities it
## can't own injected as Callables in setup(), mirroring EditorResourceBrowser:
##   - open_files(title, filters, on_pick, dir)            multi-file picker
##   - save_file(title, filters, default_name, on_pick, dir)  Save As destination picker
##   - open_dir(title, on_pick, dir)                       output-folder picker
##   - show_status(text)                                   mirror a message into the status bar
##
## Each opened archive is a NovaPffArchive (the C++ shim). Loaded archives + pending edits live in
## this object, which the shell keeps for the whole session, so closing the dialog just hides it;
## nothing is discarded until the editor exits, and originals are never touched.

var _host: Control
var _open_files: Callable
var _save_file: Callable
var _open_dir: Callable
var _show_status: Callable
var _preferred_dir: String = ""

var _dialog: AcceptDialog
var _toolbar: HBoxContainer
var _open_button: Button
var _add_button: Button
var _extract_selected_button: Button
var _extract_all_button: Button
var _remove_button: Button
var _save_button: Button
var _game_option: OptionButton
var _archive_list: ItemList
var _search: LineEdit
var _tree: Tree
var _decode_check: CheckBox
var _status_label: Label
var _confirm_dialog: ConfirmationDialog
var _confirm_callback: Callable = Callable()

# One NovaPffArchive per opened .pff; _active indexes the one shown on the right.
var _archives: Array = []
var _active: int = -1


func setup(host: Control, open_files: Callable, save_file: Callable, open_dir: Callable, show_status: Callable) -> void:
	_host = host
	_open_files = open_files
	_save_file = save_file
	_open_dir = open_dir
	_show_status = show_status


func open(preferred_dir: String = "") -> void:
	_preferred_dir = preferred_dir
	_ensure_dialog()
	_refresh_all()
	_dialog.popup_centered(Vector2i(900, 600))


# ---------------------------------------------------------------------------
# Dialog construction
# ---------------------------------------------------------------------------

func _ensure_dialog() -> void:
	if _dialog != null and is_instance_valid(_dialog):
		return
	_dialog = AcceptDialog.new()
	_dialog.name = "PffToolDialog"
	_dialog.title = "PFF Archive Tool"
	_dialog.min_size = Vector2i(900, 600)
	_dialog.exclusive = true
	if _host != null and _host.theme != null:
		_dialog.theme = _host.theme
	_dialog.get_ok_button().text = "Close"
	_host.add_child(_dialog)

	var box := VBoxContainer.new()
	box.name = "PffToolBox"
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.size_flags_vertical = Control.SIZE_EXPAND_FILL
	box.add_theme_constant_override("separation", 10)
	_dialog.add_child(box)

	# Toolbar.
	_toolbar = HBoxContainer.new()
	_toolbar.name = "PffToolToolbar"
	_toolbar.add_theme_constant_override("separation", 6)
	box.add_child(_toolbar)

	_open_button = _make_tool_button("Open Archive…", "Open one or more .pff archives.", _on_open_pressed)
	_toolbar.add_child(VSeparator.new())
	_add_button = _make_tool_button("Add Files…", "Add files from disk into the selected archive.", _on_add_pressed)
	_extract_selected_button = _make_tool_button("Extract Selected…", "Save the highlighted files to a folder.", _on_extract_selected_pressed)
	_extract_all_button = _make_tool_button("Extract All…", "Save every file in the archive to a folder.", _on_extract_all_pressed)
	_remove_button = _make_tool_button("Remove Selected", "Remove the highlighted files from the working copy.", _on_remove_pressed)
	var spacer := Control.new()
	spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_toolbar.add_child(spacer)
	_save_button = _make_tool_button("Save As…", "Write a new archive file. Your original is never changed.", _on_save_as_pressed)

	# Game row.
	var game_row := HBoxContainer.new()
	game_row.name = "PffToolGameRow"
	game_row.add_theme_constant_override("separation", 8)
	box.add_child(game_row)
	var game_label := Label.new()
	game_label.text = "Game:"
	game_row.add_child(game_label)
	_game_option = OptionButton.new()
	_game_option.name = "PffToolGameOption"
	_game_option.tooltip_text = "Which game these files come from. This controls how the archive is unscrambled."
	_game_option.item_selected.connect(_on_game_selected)
	game_row.add_child(_game_option)
	_populate_games()

	# Split: archive list (left) | contents (right).
	var split := HSplitContainer.new()
	split.name = "PffToolSplit"
	split.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	split.size_flags_vertical = Control.SIZE_EXPAND_FILL
	box.add_child(split)

	_archive_list = ItemList.new()
	_archive_list.name = "PffToolArchiveList"
	_archive_list.custom_minimum_size = Vector2(220, 0)
	_archive_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_archive_list.item_selected.connect(_on_archive_selected)
	split.add_child(_archive_list)

	var right := VBoxContainer.new()
	right.name = "PffToolRightPane"
	right.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	right.size_flags_vertical = Control.SIZE_EXPAND_FILL
	right.add_theme_constant_override("separation", 8)
	split.add_child(right)

	_search = LineEdit.new()
	_search.name = "PffToolSearch"
	_search.placeholder_text = "Filter files"
	_search.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_search.text_changed.connect(func(_t: String) -> void: _refresh_tree())
	right.add_child(_search)

	_tree = Tree.new()
	_tree.name = "PffToolTree"
	_tree.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_tree.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_tree.columns = 3
	_tree.column_titles_visible = true
	_tree.set_column_title(0, "Name")
	_tree.set_column_title(1, "Size")
	_tree.set_column_title(2, "Protected")
	_tree.set_column_expand(0, true)
	_tree.set_column_expand(1, false)
	_tree.set_column_expand(2, false)
	_tree.set_column_custom_minimum_width(1, 96)
	_tree.set_column_custom_minimum_width(2, 96)
	_tree.hide_root = true
	_tree.select_mode = Tree.SELECT_MULTI
	_tree.multi_selected.connect(func(_item: TreeItem, _col: int, _sel: bool) -> void: _update_buttons())
	right.add_child(_tree)

	var decode_row := HBoxContainer.new()
	right.add_child(decode_row)
	_decode_check = CheckBox.new()
	_decode_check.name = "PffToolDecodeCheck"
	_decode_check.text = "Save readable copies"
	_decode_check.button_pressed = true
	_decode_check.tooltip_text = "Unscramble and unpack files on extract so they open in normal tools. Turn off to save the exact bytes stored in the archive."
	decode_row.add_child(_decode_check)

	_status_label = Label.new()
	_status_label.name = "PffToolStatus"
	_status_label.theme_type_variation = &"Muted"
	_status_label.clip_text = true
	box.add_child(_status_label)


func _make_tool_button(text: String, tooltip: String, on_pressed: Callable) -> Button:
	var b := Button.new()
	b.text = text
	b.tooltip_text = tooltip
	b.focus_mode = Control.FOCUS_NONE
	b.pressed.connect(on_pressed)
	_toolbar.add_child(b)
	return b


func _populate_games() -> void:
	_game_option.clear()
	for game in NovaPffArchive.list_games():
		var entry := game as Dictionary
		var idx := _game_option.item_count
		_game_option.add_item(String(entry.get("name", "Game")))
		_game_option.set_item_metadata(idx, int(entry.get("id", 0)))


# ---------------------------------------------------------------------------
# Refresh
# ---------------------------------------------------------------------------

func _active_archive() -> NovaPffArchive:
	if _active >= 0 and _active < _archives.size():
		return _archives[_active]
	return null


func _refresh_all() -> void:
	_refresh_archive_list()
	_refresh_game_option()
	_refresh_tree()
	_update_buttons()
	if _archives.is_empty():
		_set_status("Open one or more .pff archives to begin.")


func _refresh_archive_list() -> void:
	if _archive_list == null:
		return
	_archive_list.clear()
	for i in _archives.size():
		var arc: NovaPffArchive = _archives[i]
		var base := arc.get_source_path().get_file()
		if base.is_empty():
			base = "(unsaved archive)"
		if arc.is_dirty():
			base += "  *"
		_archive_list.add_item(base)
	if _active >= 0 and _active < _archives.size():
		_archive_list.select(_active)


func _refresh_game_option() -> void:
	var arc := _active_archive()
	if arc == null:
		return
	var want := arc.get_game()
	for i in _game_option.item_count:
		if int(_game_option.get_item_metadata(i)) == want:
			_game_option.select(i)
			return


func _refresh_tree() -> void:
	if _tree == null:
		return
	_tree.clear()
	var arc := _active_archive()
	if arc == null:
		return
	var root := _tree.create_item()
	var needle := _search.text.strip_edges().to_lower() if _search != null else ""
	for entry_value in arc.get_entries():
		var entry := entry_value as Dictionary
		var name := String(entry.get("name", ""))
		if not needle.is_empty() and not name.to_lower().contains(needle):
			continue
		var item := _tree.create_item(root)
		item.set_text(0, name)
		item.set_metadata(0, name)
		item.set_text(1, _human_size(int(entry.get("size", 0))))
		item.set_text(2, "Yes" if bool(entry.get("encrypted", false)) else "")


func _update_buttons() -> void:
	var arc := _active_archive()
	var has_active := arc != null
	var has_entries := has_active and arc.get_entry_count() > 0
	var has_selection := _selected_names().size() > 0
	if _add_button != null:
		_add_button.disabled = not has_active
	if _extract_all_button != null:
		_extract_all_button.disabled = not has_entries
	if _save_button != null:
		_save_button.disabled = not has_active
	if _game_option != null:
		_game_option.disabled = not has_active
	if _extract_selected_button != null:
		_extract_selected_button.disabled = not (has_active and has_selection)
	if _remove_button != null:
		_remove_button.disabled = not (has_active and has_selection)


func _selected_names() -> PackedStringArray:
	var names := PackedStringArray()
	if _tree == null:
		return names
	var root := _tree.get_root()
	if root == null:
		return names
	var item := root.get_first_child()
	while item != null:
		if item.is_selected(0):
			names.append(String(item.get_metadata(0)))
		item = item.get_next()
	return names


func _set_status(text: String) -> void:
	if _status_label != null:
		_status_label.text = text
	if _show_status.is_valid() and not text.is_empty():
		_show_status.call(text)


func _human_size(n: int) -> String:
	if n < 1024:
		return "%d B" % n
	if n < 1024 * 1024:
		return "%.1f KB" % (float(n) / 1024.0)
	return "%.1f MB" % (float(n) / (1024.0 * 1024.0))


# ---------------------------------------------------------------------------
# Toolbar handlers
# ---------------------------------------------------------------------------

func _on_open_pressed() -> void:
	_open_files.call("Open Archive(s)", PackedStringArray(["*.pff ; PFF archives"]), _on_archives_picked, _preferred_dir)


func _on_archives_picked(paths: PackedStringArray) -> void:
	var opened := 0
	var failed := 0
	for path in paths:
		var arc := NovaPffArchive.new()
		if arc.open(path) == OK:
			_archives.append(arc)
			opened += 1
		else:
			failed += 1
			_set_status(arc.get_last_error())
	if opened > 0:
		_active = _archives.size() - 1
	_refresh_all()
	if opened > 0:
		_set_status("Opened %d archive(s)%s." % [opened, (" (%d failed)" % failed) if failed > 0 else ""])


func _on_archive_selected(index: int) -> void:
	_active = index
	_refresh_game_option()
	_refresh_tree()
	_update_buttons()


func _on_game_selected(index: int) -> void:
	var arc := _active_archive()
	if arc != null:
		arc.set_game(int(_game_option.get_item_metadata(index)))


func _on_add_pressed() -> void:
	if _active_archive() == null:
		return
	_open_files.call("Add File(s)", PackedStringArray(["*.* ; All files"]), _on_files_to_add, _preferred_dir)


func _on_files_to_add(paths: PackedStringArray) -> void:
	var arc := _active_archive()
	if arc == null:
		return
	var added := 0
	for path in paths:
		var store_name := String(path).get_file()
		if arc.add_file_from_disk(path, store_name, false) == OK:
			added += 1
		else:
			_set_status(arc.get_last_error())
	_refresh_all()
	if added > 0:
		_set_status("Added %d file(s). Use Save As to write a new archive." % added)


func _on_extract_selected_pressed() -> void:
	var arc := _active_archive()
	if arc == null:
		return
	var names := _selected_names()
	if names.is_empty():
		_set_status("Select one or more files first.")
		return
	_open_dir.call("Extract selected to folder", func(dir: String) -> void: _do_extract(names, dir), _preferred_dir)


func _do_extract(names: PackedStringArray, dir: String) -> void:
	var arc := _active_archive()
	if arc == null:
		return
	var decode := _decode_check.button_pressed
	if arc.extract_selected(names, dir, decode) == OK:
		_set_status("Extracted %d file(s) to %s" % [names.size(), dir])
	else:
		_set_status("Extract failed: %s" % arc.get_last_error())


func _on_extract_all_pressed() -> void:
	var arc := _active_archive()
	if arc == null:
		return
	_open_dir.call("Extract all to folder", func(dir: String) -> void: _do_extract_all(dir), _preferred_dir)


func _do_extract_all(dir: String) -> void:
	var arc := _active_archive()
	if arc == null:
		return
	var decode := _decode_check.button_pressed
	var count := arc.get_entry_count()
	if arc.extract_all(dir, decode) == OK:
		_set_status("Extracted %d file(s) to %s" % [count, dir])
	else:
		_set_status("Extract failed: %s" % arc.get_last_error())


func _on_remove_pressed() -> void:
	var arc := _active_archive()
	if arc == null:
		return
	var names := _selected_names()
	if names.is_empty():
		_set_status("Select one or more files first.")
		return
	var msg := "Remove %d file(s)? This only affects the working copy until you Save As." % names.size()
	_confirm(msg, "Remove", func() -> void: _do_remove(names))


func _do_remove(names: PackedStringArray) -> void:
	var arc := _active_archive()
	if arc == null:
		return
	arc.remove_entries(names)
	_refresh_all()
	_set_status("Removed %d file(s). Use Save As to write a new archive." % names.size())


func _on_save_as_pressed() -> void:
	var arc := _active_archive()
	if arc == null:
		return
	var default_name := arc.get_source_path().get_file()
	if default_name.is_empty():
		default_name = "archive.pff"
	_save_file.call("Save archive as", PackedStringArray(["*.pff ; PFF archive"]), default_name, _do_save, _preferred_dir)


func _do_save(path: String) -> void:
	var arc := _active_archive()
	if arc == null:
		return
	var out := path
	if out.get_extension().to_lower() != "pff":
		out += ".pff"
	if arc.save_as(out) == OK:
		_refresh_all()
		_set_status("Saved to %s" % out)
	else:
		_set_status("Save failed: %s" % arc.get_last_error())


# ---------------------------------------------------------------------------
# Confirm dialog (reused for destructive actions)
# ---------------------------------------------------------------------------

func _ensure_confirm() -> void:
	if _confirm_dialog != null and is_instance_valid(_confirm_dialog):
		return
	_confirm_dialog = ConfirmationDialog.new()
	_confirm_dialog.name = "PffToolConfirm"
	_confirm_dialog.exclusive = true
	if _host != null and _host.theme != null:
		_confirm_dialog.theme = _host.theme
	_host.add_child(_confirm_dialog)
	_confirm_dialog.confirmed.connect(_on_confirm_confirmed)


func _confirm(text: String, ok_text: String, callback: Callable) -> void:
	_ensure_confirm()
	_confirm_dialog.dialog_text = text
	_confirm_dialog.get_ok_button().text = ok_text
	_confirm_callback = callback
	_confirm_dialog.popup_centered()


func _on_confirm_confirmed() -> void:
	if _confirm_callback.is_valid():
		var cb := _confirm_callback
		_confirm_callback = Callable()
		cb.call()
