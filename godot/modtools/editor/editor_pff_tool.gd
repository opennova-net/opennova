class_name EditorPffTool
extends RefCounted

## The OpenNova Editor's PFF archive tool: open one or more .pff archives, browse their
## contents, and extract files (decoded or raw) to disk. Read-only — it never writes archives.
## Launched from a button in the Settings popover, not as a workspace.
##
## Built as a child of the mount shell (so it inherits the editor theme), with the capabilities it
## can't own injected as Callables in setup(), mirroring EditorResourceBrowser:
##   - open_files(title, filters, on_pick, dir)   multi-file picker (choose .pff archives)
##   - open_dir(title, on_pick, dir)              output-folder picker (extract destination)
##   - show_status(text)                          mirror a message into the status bar
##   - on_extracted(dir)                          files landed in `dir` (shell may reindex)
##
## Each opened archive is a NovaPffArchive (the C++ shim). Loaded archives live in this object,
## which the shell keeps for the whole session, so closing the dialog just hides it. The .pff
## files on disk are only ever read.

var _mount: Control
var _open_files: Callable
var _open_dir: Callable
var _show_status: Callable
var _on_extracted: Callable = Callable()
var _preferred_dir: String = ""

var _dialog: AcceptDialog
var _toolbar: HBoxContainer
var _open_button: Button
var _extract_selected_button: Button
var _extract_all_button: Button
var _game_option: OptionButton
var _archive_list: ItemList
var _search: LineEdit
var _tree: Tree
var _decode_check: CheckBox
var _status_label: Label

# Progress row, shown only while an extraction runs.
var _progress_row: HBoxContainer
var _progress_bar: ProgressBar
var _progress_label: Label
var _cancel_button: Button
var _busy: bool = false
var _cancelled: bool = false
# Same-named files across archives that clobbered each other in the last Extract-All run.
var _last_collisions: int = 0

# One NovaPffArchive per opened .pff; _active indexes the one shown on the right.
var _archives: Array = []
var _active: int = -1


func setup(mount: Control, open_files: Callable, open_dir: Callable, show_status: Callable,
		on_extracted: Callable = Callable()) -> void:
	_mount = mount
	_open_files = open_files
	_open_dir = open_dir
	_show_status = show_status
	_on_extracted = on_extracted


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
	if _mount != null and _mount.theme != null:
		_dialog.theme = _mount.theme
	_dialog.get_ok_button().text = "Close"
	_dialog.close_requested.connect(_on_dialog_close_requested)
	_mount.add_child(_dialog)

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
	_extract_selected_button = _make_tool_button("Extract Selected…", "Save the highlighted files to a folder.", _on_extract_selected_pressed)
	_extract_all_button = _make_tool_button("Extract All…", "Save every file from all open archives to a folder.", _on_extract_all_pressed)

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

	_search = SearchField.new("Filter files")
	_search.name = "PffToolSearch"
	_search.search_changed.connect(func(_t: String) -> void: _refresh_tree())
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

	# Progress row spanning the dialog, hidden until a long op runs.
	_progress_row = HBoxContainer.new()
	_progress_row.name = "PffToolProgressRow"
	_progress_row.add_theme_constant_override("separation", 8)
	_progress_row.visible = false
	box.add_child(_progress_row)
	_progress_bar = ProgressBar.new()
	_progress_bar.name = "PffToolProgressBar"
	_progress_bar.show_percentage = false
	_progress_bar.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_progress_row.add_child(_progress_bar)
	_progress_label = Label.new()
	_progress_label.name = "PffToolProgressLabel"
	_progress_label.theme_type_variation = &"Muted"
	_progress_label.custom_minimum_size = Vector2(120, 0)
	_progress_row.add_child(_progress_label)
	_cancel_button = Button.new()
	_cancel_button.name = "PffToolCancel"
	_cancel_button.text = "Stop"
	_cancel_button.focus_mode = Control.FOCUS_NONE
	_cancel_button.pressed.connect(_on_cancel_pressed)
	_progress_row.add_child(_cancel_button)

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
			base = "(archive)"
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
	# The busy lock (_set_busy) owns disabling during a run; this is the idle-state authority.
	if _busy:
		return
	var arc := _active_archive()
	var has_active := arc != null
	var has_selection := _selected_names().size() > 0
	if _extract_all_button != null:
		# Extract All covers every open archive, so it only needs at least one open.
		_extract_all_button.disabled = _archives.is_empty()
	if _game_option != null:
		_game_option.disabled = not has_active
	if _extract_selected_button != null:
		_extract_selected_button.disabled = not (has_active and has_selection)


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
	# Don't switch the active archive while an extraction is running: the worker reads the model and
	# the list selection drives which archive that is. The list reverts to the running archive.
	if _busy:
		if _active >= 0 and _active < _archive_list.item_count:
			_archive_list.select(_active)
		return
	_active = index
	_refresh_game_option()
	_refresh_tree()
	_update_buttons()


func _on_game_selected(index: int) -> void:
	var arc := _active_archive()
	if arc != null:
		arc.set_game(int(_game_option.get_item_metadata(index)))


func _on_extract_selected_pressed() -> void:
	var arc := _active_archive()
	if arc == null or _busy:
		return
	var names := _selected_names()
	if names.is_empty():
		_set_status("Select one or more files first.")
		return
	_open_dir.call("Extract selected to folder", func(dir: String) -> void: _do_extract(names, dir), _preferred_dir)


func _do_extract(names: PackedStringArray, dir: String) -> void:
	# Extract Selected is scoped to the active archive (selection lives in its tree).
	var arc := _active_archive()
	if arc == null:
		return
	_last_collisions = 0
	# One step: the active archive extracts just the selected names.
	await _perform_extraction([{"arc": arc, "names": names}], dir, 1)


func _on_extract_all_pressed() -> void:
	# Extract All covers EVERY open archive, not just the active one.
	if _archives.is_empty() or _busy:
		return
	_open_dir.call("Extract all to folder", func(dir: String) -> void: _do_extract_all(dir), _preferred_dir)


func _do_extract_all(dir: String) -> void:
	# Flat: every file from every open archive goes straight into `dir`. Same-named files across
	# archives are last-write-wins — kept intentionally simple, but we count collisions so the
	# report can warn rather than silently dropping files. Each archive is one step with an empty
	# name list, meaning "extract everything in it".
	var plan: Array = []
	var seen := {}
	_last_collisions = 0
	for arc in _archives:
		plan.append({"arc": arc, "names": PackedStringArray()})
		for entry_value in arc.get_entries():
			# Collisions are by output basename, since that is what lands in `dir`.
			var base := String((entry_value as Dictionary).get("name", "")).get_file()
			if seen.has(base):
				_last_collisions += 1
			seen[base] = true
	await _perform_extraction(plan, dir, _archives.size())


# Shared non-blocking extraction. Each plan step extracts one archive on a background C++ worker
# (read + decode + write off the main thread); this coroutine just polls progress, drives the
# Stop button, and keeps the editor responsive. A step's `names` lists the entries to pull, or is
# empty to mean "every entry in that archive".
func _perform_extraction(plan: Array, dir: String, archive_count: int) -> void:
	if _busy:
		return
	# Total up front, so the progress bar spans the whole multi-archive run.
	var total := 0
	for step in plan:
		var names: PackedStringArray = step["names"]
		total += names.size() if names.size() > 0 else int(step["arc"].get_entry_count())
	if total == 0:
		_set_status("Nothing to extract.")
		return
	var decode := _decode_check.button_pressed
	_cancelled = false
	_set_busy(true)
	_cancel_button.visible = true
	_progress_bar.max_value = total
	_progress_bar.value = 0
	var ok := 0
	var raw := 0
	var failed := 0
	var base := 0  # entries fully accounted for by completed steps
	for step in plan:
		if _cancelled:
			break
		var arc = step["arc"]
		var names: PackedStringArray = step["names"]
		var step_total: int = names.size() if names.size() > 0 else int(arc.get_entry_count())
		if arc.extract_async(names, dir, decode) != OK:
			# Could not even start this archive: count the whole step as failed and move on.
			failed += step_total
			base += step_total
			continue
		while arc.is_extract_running():
			await _mount.get_tree().process_frame
			if not is_instance_valid(_dialog):
				# Mount/dialog torn down mid-run: stop the worker and clear the busy lock. The
				# archive's destructor joins the thread, so no work escapes; skip the dead UI.
				arc.request_extract_cancel()
				_busy = false
				return
			if _cancelled:
				arc.request_extract_cancel()
			var done_now: int = base + int(arc.get_extract_progress_done())
			_progress_bar.value = done_now
			_progress_label.text = "%d / %d" % [done_now, total]
		arc.wait_for_extract_completion()
		ok += int(arc.get_extract_ok_count())
		raw += int(arc.get_extract_raw_count())
		failed += int(arc.get_extract_failed_count())
		base += step_total
		_progress_bar.value = base
		_progress_label.text = "%d / %d" % [base, total]
	_set_busy(false)
	_refresh_all()
	_report_extraction(ok + raw + failed, total, ok, raw, failed, dir, archive_count)
	# Files landed on disk (even on a partial/stopped run): let the shell reindex if it
	# cares about this directory, so new files show up in quick open without a restart.
	if ok + raw > 0 and _on_extracted.is_valid():
		_on_extracted.call(dir)


func _report_extraction(done: int, total: int, ok: int, raw: int, failed: int, dir: String, archive_count: int) -> void:
	var written := ok + raw
	var msg := ""
	if _cancelled and done < total:
		msg = "Stopped: %d of %d processed, %d written to %s" % [done, total, written, dir]
	elif archive_count > 1:
		msg = "Extracted %d file(s) from %d archives to %s" % [written, archive_count, dir]
	else:
		msg = "Extracted %d file(s) to %s" % [written, dir]
	if raw > 0:
		msg += "  (%d saved as raw — could not unscramble)" % raw
	if failed > 0:
		msg += "  (%d could not be read)" % failed
	if _last_collisions > 0:
		msg += "  (%d overwritten by a same-named file from another archive)" % _last_collisions
	_set_status(msg)


func _set_busy(active: bool) -> void:
	_busy = active
	if _progress_row != null:
		_progress_row.visible = active
	var dis := active
	for b in [_open_button, _extract_selected_button, _extract_all_button]:
		if b != null:
			b.disabled = dis
	if _game_option != null:
		_game_option.disabled = dis
	if _dialog != null and is_instance_valid(_dialog):
		_dialog.get_ok_button().disabled = dis
	if not active:
		if _progress_bar != null:
			_progress_bar.value = 0
		if _progress_label != null:
			_progress_label.text = ""
		_update_buttons()  # restore selection-dependent idle enablement


func _on_cancel_pressed() -> void:
	if _busy:
		_cancelled = true
		if _progress_label != null:
			_progress_label.text = "Stopping…"


func _on_dialog_close_requested() -> void:
	# Closing mid-extraction cancels cooperatively; files already written are kept.
	if _busy:
		_cancelled = true
