class_name MusicEditorDocument
extends RefCounted

const MusicEditHistoryClass = preload("res://modtools/music/music_edit_history.gd")

# Loaded resources (either may be null if not yet opened)
var bank: NovaSbfBank
var mus_script: NovaMusicScript  # named mus_script to avoid shadowing Object.script

# Source paths for save-back
var bank_path: String = ""
var script_path: String = ""

# Dirty tracking, per side
var _bank_dirty: bool = false
var _script_dirty: bool = false

# Cached compiled output for Script mode (Live VM runs against this)
var _compiled_script_text: String = ""
var _compiled_bytecode: PackedByteArray = PackedByteArray()
var _compiled_file_bytes: PackedByteArray = PackedByteArray()

# Per-mode undo stacks. Bank-side commands (reorder, rename) push do/undo
# pairs onto _bank_history; Script-side commands will land in Phase F.
# Declared as RefCounted so the parser doesn't need MusicEditHistory's
# class_name registered before this file resolves; the preloaded class is
# the actual concrete type.
var _bank_history: RefCounted
var _script_history: RefCounted

signal changed
signal bank_dirty_changed(dirty: bool)
signal script_dirty_changed(dirty: bool)
signal saved
signal compile_finished(success: bool, errors: Array)


func _init() -> void:
	_bank_history = MusicEditHistoryClass.new()
	_script_history = MusicEditHistoryClass.new()


func is_dirty() -> bool:
	return _bank_dirty or _script_dirty


func bank_loaded() -> bool:
	return bank != null


func script_loaded() -> bool:
	return mus_script != null


func _set_bank_dirty(d: bool) -> void:
	if _bank_dirty != d:
		_bank_dirty = d
		bank_dirty_changed.emit(d)


func _set_script_dirty(d: bool) -> void:
	if _script_dirty != d:
		_script_dirty = d
		script_dirty_changed.emit(d)


func set_script_text(_script_name: StringName, text: String) -> void:
	# Track the in-flight text without re-decompiling. The compiled bytecode
	# cache invalidates on every edit so a subsequent Compile press always
	# produces fresh bytes. _script_name is currently informational; the v1
	# editor only exposes the default script.
	if not script_loaded():
		return
	if _compiled_script_text == text:
		return
	_compiled_script_text = text
	_compiled_bytecode = PackedByteArray()
	_compiled_file_bytes = PackedByteArray()
	_set_script_dirty(true)


# --- Phase F3: compile -------------------------------------------------
#
# Compiles the cached text via the C++ NovaMusicScript::compile_text bridge
# (Phase F4). Returns the diagnostics array; empty on success. Always emits
# compile_finished.
func compile_script() -> Array:
	if not script_loaded():
		var no_script: Array = [{"line": 0, "col": 0, "message": "no script loaded"}]
		compile_finished.emit(false, no_script)
		return no_script
	# First-press fix: when the user hits Compile right after opening a project
	# without first editing the CodeEdit, _compiled_script_text is still empty
	# even though the script is loaded. Seed from the decompiler so the bytes
	# round-trip (text -> bytecode -> bytes that match what was loaded).
	if _compiled_script_text == "":
		_seed_compiled_text_from_script()
	if _compiled_script_text == "":
		var no_text: Array = [{"line": 0, "col": 0, "message": "no script text"}]
		compile_finished.emit(false, no_text)
		return no_text
	# NovaMusicScript.compile_text returns a Dictionary (see header). Output
	# references aren't viable through GDExtension; the bridge packs everything
	# into the dict instead. Older builds without the bridge return null when
	# probed via has_method, so we treat that as a no-op success (matches the
	# pre-F4 stub state).
	if not mus_script.has_method("compile_text"):
		_compiled_bytecode = PackedByteArray()
		_compiled_file_bytes = PackedByteArray()
		compile_finished.emit(true, [])
		return []
	var d: Dictionary = mus_script.compile_text(_compiled_script_text)
	var rc: int = int(d.get("rc", -1))
	if rc != 0:
		var errs: Array = [{
			"line": int(d.get("err_line", 0)),
			"col": int(d.get("err_col", 0)),
			"message": String(d.get("err_msg", "compile error")),
		}]
		compile_finished.emit(false, errs)
		return errs
	_compiled_bytecode = d.get("bytecode", PackedByteArray())
	_compiled_file_bytes = d.get("file_bytes", PackedByteArray())
	if _compiled_file_bytes.size() > 0 and mus_script.has_method("set_compiled_file_bytes"):
		mus_script.set_compiled_file_bytes(_compiled_file_bytes)
	elif _compiled_bytecode.size() > 0 and mus_script.has_method("set_compiled_bytecode"):
		mus_script.set_compiled_bytecode(_compiled_bytecode)
	compile_finished.emit(true, [])
	return []


func prepare_script_for_run() -> Array:
	if not script_loaded():
		var no_script: Array = [{"line": 0, "col": 0, "message": "no script loaded"}]
		compile_finished.emit(false, no_script)
		return no_script
	if _script_dirty:
		return compile_script()
	return []


func open_bank(path: String) -> int:
	if not _path_exists(path):
		return ERR_FILE_NOT_FOUND
	var res = load(path)
	if res == null or not (res is NovaSbfBank):
		return ERR_CANT_OPEN
	bank = res
	bank_path = path
	_set_bank_dirty(false)
	changed.emit()
	return OK


func open_script(path: String) -> int:
	if not _path_exists(path):
		return ERR_FILE_NOT_FOUND
	var res = load(path)
	if res == null or not (res is NovaMusicScript):
		return ERR_CANT_OPEN
	mus_script = res
	script_path = path
	_compiled_script_text = ""
	_compiled_bytecode = PackedByteArray()
	_compiled_file_bytes = PackedByteArray()
	_set_script_dirty(false)
	changed.emit()
	return OK


func close_pair() -> void:
	bank = null
	mus_script = null
	bank_path = ""
	script_path = ""
	_compiled_script_text = ""
	_compiled_bytecode = PackedByteArray()
	_compiled_file_bytes = PackedByteArray()
	_set_bank_dirty(false)
	_set_script_dirty(false)
	changed.emit()


func open_pair(any_path: String) -> int:
	var ext := any_path.get_extension().to_lower()
	var sibling: String = ""
	if ext == "sbf":
		sibling = any_path.get_basename() + ".bin"
		var primary_err = open_bank(any_path)
		if primary_err != OK:
			return primary_err
		if _path_exists(sibling):
			open_script(sibling)  # ignore secondary failure
	elif ext == "bin":
		sibling = any_path.get_basename() + ".sbf"
		var primary_err2 = open_script(any_path)
		if primary_err2 != OK:
			return primary_err2
		if _path_exists(sibling):
			open_bank(sibling)
	else:
		return ERR_INVALID_PARAMETER
	return OK


# Check existence for both res:// (virtual) and user:// / absolute paths.
func _path_exists(path: String) -> bool:
	if path.begins_with("res://"):
		return ResourceLoader.exists(path)
	return FileAccess.file_exists(path)


func save_to_disk() -> int:
	# Compile-on-Save: when the script side has uncommitted edits, run the
	# compiler first so the saver writes fresh bytecode. A failed compile bails
	# without touching disk; the error popout in script_mode listens to the
	# compile_finished signal that compile_script() emits.
	if _script_dirty and script_loaded():
		var errs: Array = prepare_script_for_run()
		if errs.size() > 0:
			return ERR_COMPILATION_FAILED
	return _save_resources()


func get_var_profile_path() -> String:
	if script_path == "":
		return ""
	return script_path.get_basename() + ".music_profile.json"


func _save_resources() -> int:
	var bank_err: int = OK
	var script_err: int = OK
	if bank_loaded() and bank_path != "":
		bank_err = ResourceSaver.save(bank, bank_path)
	if script_loaded() and script_path != "":
		script_err = ResourceSaver.save(mus_script, script_path)
	if bank_err != OK:
		return bank_err
	if script_err != OK:
		return script_err
	_set_bank_dirty(false)
	_set_script_dirty(false)
	saved.emit()
	return OK


# --- Phase E: bank-edit operations ---------------------------------------
#
# reorder_track and rename_track push do/undo callables onto the bank
# history so the editor's undo button reverts them in linear order. The
# _no_history variants do the raw mutation; the wrapped form pushes a
# pair that calls those, which keeps the undo callback from re-pushing
# itself when invoked. replace/add/delete are not undoable in v1; they
# just dirty.

func reorder_track(from_index: int, to_index: int) -> void:
	if not bank_loaded():
		return
	# Capture a weakref, never self: the history stack outlives the call, so a
	# self-bound Callable would form a document -> _bank_history -> Callable ->
	# document cycle that GDScript refcounting can never collect. That leaks the
	# document and its loaded bank/script (the resources stay "in use" at exit).
	var wself: WeakRef = weakref(self)
	var do_cb := func() -> void:
		var s: MusicEditorDocument = wself.get_ref()
		if s != null:
			s._reorder_track_no_history(from_index, to_index)
	var undo_cb := func() -> void:
		var s: MusicEditorDocument = wself.get_ref()
		if s != null:
			s._reorder_track_no_history(to_index, from_index)
	do_cb.call()
	_bank_history.push(do_cb, undo_cb)


func _reorder_track_no_history(from_index: int, to_index: int) -> void:
	if not bank_loaded():
		return
	bank.reorder_entry(from_index, to_index)
	_set_bank_dirty(true)
	changed.emit()


func rename_track(index: int, new_name: StringName) -> void:
	if not bank_loaded():
		return
	var entries: Array = bank.get_entries()
	var prev_name: String = ""
	if index >= 0 and index < entries.size():
		prev_name = String(entries[index].get("name", ""))
	# Weakref capture (see reorder_track) keeps the undo pair from pinning the
	# document into an uncollectable reference cycle.
	var wself: WeakRef = weakref(self)
	var new_name_str := String(new_name)
	var do_cb := func() -> void:
		var s: MusicEditorDocument = wself.get_ref()
		if s != null:
			s._rename_track_no_history(index, new_name_str)
	var undo_cb := func() -> void:
		var s: MusicEditorDocument = wself.get_ref()
		if s != null:
			s._rename_track_no_history(index, prev_name)
	do_cb.call()
	_bank_history.push(do_cb, undo_cb)


func _rename_track_no_history(index: int, new_name: String) -> void:
	if not bank_loaded():
		return
	bank.rename_entry(index, new_name)
	_set_bank_dirty(true)
	changed.emit()


func replace_track_audio(index: int, samples: PackedFloat32Array) -> int:
	if not bank_loaded():
		return ERR_UNCONFIGURED
	var err: int = bank.set_entry_pcm(index, samples)
	if err == OK:
		_set_bank_dirty(true)
		changed.emit()
	return err


func add_track(name: StringName, samples: PackedFloat32Array) -> int:
	if not bank_loaded():
		return ERR_UNCONFIGURED
	var err: int = bank.add_entry(String(name), samples)
	if err == OK:
		_set_bank_dirty(true)
		changed.emit()
	return err


func delete_track(index: int) -> void:
	if not bank_loaded():
		return
	bank.delete_entry(index)
	_set_bank_dirty(true)
	changed.emit()


# --- Phase E4: per-mode undo --------------------------------------------

func undo_bank() -> void:
	_bank_history.undo()


func redo_bank() -> void:
	_bank_history.redo()


func undo_script() -> void:
	_script_history.undo()


func redo_script() -> void:
	_script_history.redo()


func can_undo_bank() -> bool:
	return _bank_history.can_undo()


func can_redo_bank() -> bool:
	return _bank_history.can_redo()


func can_undo_script() -> bool:
	return _script_history.can_undo()


func can_redo_script() -> bool:
	return _script_history.can_redo()


# Lazy seed: pull the decompiler's text for the default script and cache it.
# Used by compile_script when the user presses Compile without having edited
# the CodeEdit first. Doing this eagerly in open_script would clobber any
# in-flight script_text the user has already typed; lazy keeps both paths
# safe.
func _seed_compiled_text_from_script() -> void:
	if not script_loaded():
		return
	var script_name: StringName = StringName(mus_script.get_default_script_name())
	if bank_loaded() and mus_script.has_method("get_decompiled_text_with_bank"):
		_compiled_script_text = mus_script.get_decompiled_text_with_bank(script_name, bank)
	else:
		_compiled_script_text = mus_script.get_decompiled_text(script_name)
