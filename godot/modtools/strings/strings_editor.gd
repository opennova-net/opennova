class_name StringsEditor
extends "res://modtools/editor/editor_document.gd"

## Authoring controller for an RTXT localized string table (strings/*.bin).
##
## Holds a single RtxtStringFile for its whole lifetime and always mutates it in
## place (open/new/undo reload the same object) so observers can connect once.
## Undo/redo uses byte snapshots of the table (reusing the byte-exact serializer in
## libs/rtxt).
##
## Two change channels keep the editor snappy:
##   - structure_changed: the set of entries/sections changed (add/remove/section/
##     open/new/undo/redo/import). Observers do a full rebuild.
##   - edited: an editing session committed, or a save changed dirtiness. Observers
##     that only track the title/dirty state listen to this; there is NO rebuild.
## Live text/key/section/position edits are silent (model + dirty only) and are
## bracketed by begin_edit()/commit_edit() so one editing session becomes one undo
## step; the view patches just the edited row.

signal structure_changed
signal edited

const UNDO_LIMIT := 100
const DEFAULT_FILENAME := "strings.bin"

var string_table: RtxtStringFile
var selected_index: int = -1

var _undo_stack: Array[PackedByteArray] = []
var _redo_stack: Array[PackedByteArray] = []
var _pending_snapshot: PackedByteArray = PackedByteArray()
var _editing: bool = false


func _init() -> void:
	string_table = RtxtStringFile.new()
	string_table.reset_empty()


# --- Lifecycle ---

func new_table(mark_dirty_state: bool = true) -> void:
	string_table.reset_empty()
	selected_index = -1
	set_current_path("")
	_clear_history()
	is_dirty = mark_dirty_state
	structure_changed.emit()


func open_strings(path: String) -> Error:
	var err := string_table.load_from_path(path)
	if err != OK:
		return err
	selected_index = -1 if string_table.get_entry_count() == 0 else 0
	set_current_path(path)
	remember_open_path(path)
	_clear_history()
	mark_clean()
	structure_changed.emit()
	return OK


func save_current() -> Error:
	if current_path.is_empty() or current_path.get_extension().to_lower() != "bin":
		return ERR_INVALID_PARAMETER
	_flush_edit()
	var err := string_table.save_to_path(current_path)
	if err == OK:
		mark_clean()
		edited.emit()
	return err


func save_as(dir_path: String) -> Error:
	if dir_path.is_empty():
		return ERR_INVALID_PARAMETER
	_flush_edit()
	var mkdir_err := DirAccess.make_dir_recursive_absolute(dir_path)
	if mkdir_err != OK:
		return mkdir_err
	var path := dir_path.path_join(_export_filename())
	var err := string_table.save_to_path(path)
	if err == OK:
		set_current_path(path)
		remember_save_dir(dir_path)
		mark_clean()
		edited.emit()
	return err


func get_project_title() -> String:
	var name := current_path.get_file()
	if name.is_empty():
		name = "untitled"
	return "%s%s" % [name, "*" if is_dirty else ""]


func get_status_context() -> String:
	if string_table == null:
		return "No string table loaded"
	return "%d strings  %d sections" % [string_table.get_entry_count(), string_table.get_section_count()]


# --- Structural mutations (one undo step each; observers rebuild) ---

func add_entry(key: String, text: String, section_index: int, position: Vector2i = Vector2i()) -> int:
	_flush_edit()
	_push_undo()
	var idx := string_table.add_entry(key, text, section_index, position)
	selected_index = idx
	mark_dirty()
	structure_changed.emit()
	return idx


func remove_entry(index: int) -> void:
	if index < 0 or index >= string_table.get_entry_count():
		return
	_flush_edit()
	_push_undo()
	string_table.remove_entry(index)
	_clamp_selection()
	mark_dirty()
	structure_changed.emit()


func add_section(name: String) -> int:
	_flush_edit()
	_push_undo()
	var idx := string_table.add_section(name)
	mark_dirty()
	structure_changed.emit()
	return idx


func remove_section(index: int, reassign_to: int = -1) -> void:
	_flush_edit()
	_push_undo()
	string_table.remove_section(index, reassign_to)
	_clamp_selection()
	mark_dirty()
	structure_changed.emit()


func rename_section(index: int, name: String) -> void:
	_flush_edit()
	_push_undo()
	string_table.rename_section(index, name)
	mark_dirty()
	structure_changed.emit()


# --- Editing session (silent: model + dirty only, bracketed by begin/commit) ---

func begin_edit() -> void:
	if not _editing:
		_pending_snapshot = string_table.to_byte_array()
		_editing = true


func commit_edit() -> void:
	if not _editing:
		return
	_editing = false
	var now := string_table.to_byte_array()
	if now != _pending_snapshot:
		_undo_stack.append(_pending_snapshot)
		_trim_undo()
		_redo_stack.clear()
		mark_dirty()
		edited.emit()


func set_entry_text_live(index: int, text: String) -> void:
	string_table.set_entry_text(index, text)
	mark_dirty()


func set_entry_position_live(index: int, position: Vector2i) -> void:
	string_table.set_entry_position(index, position)
	mark_dirty()


func set_entry_key_live(index: int, key: String) -> void:
	string_table.set_entry_key(index, key)
	mark_dirty()


func set_entry_section_index_live(index: int, section_index: int) -> void:
	string_table.set_entry_section_index(index, section_index)
	mark_dirty()


# --- Undo / redo ---

func can_undo() -> bool:
	return not _undo_stack.is_empty()


func can_redo() -> bool:
	return not _redo_stack.is_empty()


func undo() -> void:
	_flush_edit()
	if _undo_stack.is_empty():
		return
	_redo_stack.append(string_table.to_byte_array())
	string_table.load_from_byte_array(_undo_stack.pop_back())
	_clamp_selection()
	mark_dirty()
	structure_changed.emit()


func redo() -> void:
	_flush_edit()
	if _redo_stack.is_empty():
		return
	_undo_stack.append(string_table.to_byte_array())
	string_table.load_from_byte_array(_redo_stack.pop_back())
	_clamp_selection()
	mark_dirty()
	structure_changed.emit()


# --- Validation ---

## Returns { "issues_by_index": { idx: Array[String] }, "summary": String,
## "ok": bool }. Flags case-insensitive duplicate keys, empty keys, and entries
## whose section index is out of range.
func validate() -> Dictionary:
	var issues: Dictionary = {}
	var by_key: Dictionary = {}
	var count := string_table.get_entry_count()
	var section_count := string_table.get_section_count()
	var duplicates := 0
	var empties := 0
	var bad_sections := 0

	for i in count:
		var upper := string_table.get_entry_key(i).strip_edges().to_upper()
		if upper.is_empty():
			_add_issue(issues, i, "Empty key")
			empties += 1
		else:
			if not by_key.has(upper):
				by_key[upper] = []
			(by_key[upper] as Array).append(i)
		var section := string_table.get_entry_section_index(i)
		if section < 0 or section >= section_count:
			_add_issue(issues, i, "Invalid section")
			bad_sections += 1

	for upper in by_key:
		var indices := by_key[upper] as Array
		if indices.size() > 1:
			for i in indices:
				_add_issue(issues, i, "Duplicate key")
				duplicates += 1

	var summary := "No issues"
	if not issues.is_empty():
		var parts: Array[String] = []
		if duplicates > 0:
			parts.append("%d duplicate" % duplicates)
		if empties > 0:
			parts.append("%d empty key" % empties)
		if bad_sections > 0:
			parts.append("%d bad section" % bad_sections)
		summary = ", ".join(PackedStringArray(parts))

	return {"issues_by_index": issues, "summary": summary, "ok": issues.is_empty()}


# --- CSV import / export ---
# Columns: section_name,key,text,pos_x,pos_y. Newlines inside text are escaped as
# literal \n / \r so each entry stays on one CSV line.

func export_csv(path: String) -> Error:
	_flush_edit()
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		return FileAccess.get_open_error()
	file.store_csv_line(PackedStringArray(["section_name", "key", "text", "pos_x", "pos_y"]))
	var section_count := string_table.get_section_count()
	for i in string_table.get_entry_count():
		var section := string_table.get_entry_section_index(i)
		var section_name := string_table.get_section_name(section) if section >= 0 and section < section_count else ""
		var pos := string_table.get_entry_position(i)
		file.store_csv_line(PackedStringArray([
			section_name,
			string_table.get_entry_key(i),
			_escape_newlines(string_table.get_entry_text(i)),
			str(pos.x),
			str(pos.y),
		]))
	file.close()
	return OK


func import_csv(path: String) -> Error:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return FileAccess.get_open_error()
	_flush_edit()
	_push_undo()
	string_table.reset_empty()
	var section_indices: Dictionary = {}  # section_name -> index
	var first := true
	while not file.eof_reached():
		var row := file.get_csv_line()
		if row.size() == 0 or (row.size() == 1 and row[0].is_empty()):
			continue
		if first:
			first = false
			continue  # header
		var section_name := row[0] if row.size() > 0 else ""
		var key := row[1] if row.size() > 1 else ""
		var text := _unescape_newlines(row[2]) if row.size() > 2 else ""
		var pos_x := int(row[3]) if row.size() > 3 else 0
		var pos_y := int(row[4]) if row.size() > 4 else 0
		var section_index := 0
		if section_indices.has(section_name):
			section_index = section_indices[section_name]
		else:
			section_index = string_table.add_section(section_name)
			section_indices[section_name] = section_index
		string_table.add_entry(key, text, section_index, Vector2i(pos_x, pos_y))
	file.close()
	selected_index = -1 if string_table.get_entry_count() == 0 else 0
	mark_dirty()
	structure_changed.emit()
	return OK


# --- Internal ---

func _flush_edit() -> void:
	# Commit any open editing session so its model changes land as a discrete undo
	# step before a structural mutation snapshots state.
	commit_edit()


func _export_filename() -> String:
	if not current_path.is_empty():
		return current_path.get_file()
	return DEFAULT_FILENAME


func _push_undo() -> void:
	_undo_stack.append(string_table.to_byte_array())
	_trim_undo()
	_redo_stack.clear()


func _trim_undo() -> void:
	while _undo_stack.size() > UNDO_LIMIT:
		_undo_stack.pop_front()


func _clear_history() -> void:
	_undo_stack.clear()
	_redo_stack.clear()
	_editing = false
	_pending_snapshot = PackedByteArray()


func _clamp_selection() -> void:
	var count := string_table.get_entry_count()
	if count == 0:
		selected_index = -1
	elif selected_index >= count:
		selected_index = count - 1


func _add_issue(issues: Dictionary, index: int, message: String) -> void:
	if not issues.has(index):
		issues[index] = []
	var list := issues[index] as Array
	if not list.has(message):
		list.append(message)


func _escape_newlines(text: String) -> String:
	return text.replace("\\", "\\\\").replace("\r", "\\r").replace("\n", "\\n")


func _unescape_newlines(text: String) -> String:
	# Reverse of _escape_newlines. Walk the string so an escaped backslash is not
	# mistaken for the start of a \n / \r escape.
	var out := ""
	var i := 0
	while i < text.length():
		var c := text[i]
		if c == "\\" and i + 1 < text.length():
			var next := text[i + 1]
			match next:
				"n":
					out += "\n"
				"r":
					out += "\r"
				"\\":
					out += "\\"
				_:
					out += next
			i += 2
		else:
			out += c
			i += 1
	return out
