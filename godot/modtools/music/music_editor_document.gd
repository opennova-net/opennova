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

# Single unified edit history for the whole document. The Music screen is one
# surface, so bank edits (reorder, rename) and script/play edits share one
# timeline and one Ctrl+Z -- a play edit is a script-semantics change, but the
# user only sees "undo the last thing I did". Declared as RefCounted so the
# parser doesn't need MusicEditHistory's class_name registered before this file
# resolves; the preloaded class is the actual concrete type.
var _history: RefCounted

signal changed
signal bank_dirty_changed(dirty: bool)
signal script_dirty_changed(dirty: bool)
signal saved
signal compile_finished(success: bool, errors: Array)


func _init() -> void:
	_history = MusicEditHistoryClass.new()


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
	# self-bound Callable would form a document -> _history -> Callable ->
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
	_history.push(do_cb, undo_cb)


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
	_history.push(do_cb, undo_cb)


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


# --- Structured play edits (drag-to-add-play) ----------------------------
#
# A play edit is a TEXT transform over the faithful decompiled text: splice or
# remove one `play <bind>` line in a section's body, then recompile. It never
# hand-assembles bytecode and never touches the decompiler emitter, so it can
# only ever produce scripts the round-trip already supports. Gated by
# can_edit_plays() (a script whose current text doesn't compile stays
# read-only). The byte-stability of this path is proven in
# tests/mus/mus_structured_play_edit_test.cpp and the GUT parity test; do not
# loosen the gate without a passing parity test.

func can_edit_plays() -> bool:
	if not script_loaded() or not mus_script.has_method("compile_text"):
		return false
	# Structured edits operate on a single chunk (the default script) and
	# re-encode a one-chunk file; a multi-chunk .bin would lose its other chunks
	# on the first edit, so keep those read-only.
	if mus_script.has_method("get_script_count") and int(mus_script.get_script_count()) != 1:
		return false
	var text := _current_script_text()
	if text == "":
		return false
	var d: Dictionary = mus_script.compile_text(text)
	return int(d.get("rc", -1)) == 0


func insert_play(section_name: StringName, track: int) -> bool:
	if not can_edit_plays():
		return false
	# The play opcode operand is one byte; a track that can't be represented
	# would silently wrap, so reject it rather than write the wrong sound.
	if track < 0 or track > 255:
		return false
	var prev := _current_script_text()
	if prev == "":
		return false
	var next := _splice_play(prev, String(section_name), _track_bind_id(track))
	if next == prev:
		return false  # section not found / nothing changed
	if not _apply_script_text(next):
		_apply_script_text(prev)  # compile failed: roll back, edit nothing
		return false
	_push_text_edit(prev, next)
	return true


# Add a new (empty) section/state. The minimal authoring slice of the visual-
# first mandate: the loudest missing affordance was "there is no visual way to
# add a section". A text transform over the names-less decompile -- append
# `section <name> { }` (an empty body the compiler emits as a single `done`) --
# then recompile through the same parity gate as play edits, so it can only ever
# produce a script the round-trip supports. Reachability/contents are authored
# afterwards (Phase 2). Returns false if the name is invalid, already taken, or
# the result doesn't compile.
func add_section(section_name: StringName) -> bool:
	if not can_edit_plays():
		return false
	var name := String(section_name).strip_edges()
	if not _is_valid_section_name(name):
		return false
	var prev := _current_script_text()
	if prev == "":
		return false
	# A duplicate name would re-point the existing section on recompile; reject it.
	if _section_header_line(prev.split("\n"), name) >= 0:
		return false
	var next := _append_section(prev, name)
	if next == prev:
		return false
	if not _apply_script_text(next):
		_apply_script_text(prev)  # compile failed: roll back, change nothing
		return false
	_push_text_edit(prev, next)
	return true


# Compiler keywords (mus_compile.cpp lexer): a section can't be named any of
# these, since `section <kw>` lexes <kw> as the keyword token, not an identifier,
# and the recompile would fail. Auto-named States never collide, but a Phase-2
# rename UI will route through this validator too.
const _RESERVED := {
	"script": true, "bind": true, "section": true, "declsection": true,
	"global": true, "if": true, "else": true, "return": true, "yield": true,
	"nop": true, "goto": true, "enter": true, "play": true, "on": true,
	"call": true, "done": true, "Me": true,
}


# A section name must lex as a single bare identifier ([A-Za-z_][A-Za-z0-9_]*)
# that isn't a reserved keyword -- the only form the compiler's `section <ident>`
# accepts.
func _is_valid_section_name(name: String) -> bool:
	if name.is_empty() or name.length() > 31:
		return false
	if _RESERVED.has(name):
		return false
	var c0 := name.unicode_at(0)
	var is_alpha0 := (c0 >= 65 and c0 <= 90) or (c0 >= 97 and c0 <= 122) or c0 == 95
	if not is_alpha0:
		return false
	for i in range(1, name.length()):
		var c := name.unicode_at(i)
		var ok := (c >= 65 and c <= 90) or (c >= 97 and c <= 122) or (c >= 48 and c <= 57) or c == 95
		if not ok:
			return false
	return true


# Append an empty `section <name> { }` block. The decompiler regenerates the
# declsection forward-decl and orders bodies by code offset on reload, so the
# new section lands last and the text stays canonical.
func _append_section(text: String, name: String) -> String:
	var t := text
	if not t.ends_with("\n"):
		t += "\n"
	t += "\nsection %s\n{\n}\n" % name
	return t


func remove_play(section_name: StringName, track: int) -> bool:
	if not can_edit_plays():
		return false
	var prev := _current_script_text()
	if prev == "":
		return false
	var next := _remove_one_play(prev, String(section_name), _track_bind_id(track))
	if next == prev:
		return false
	if not _apply_script_text(next):
		_apply_script_text(prev)
		return false
	_push_text_edit(prev, next)
	return true


# Push a do/undo pair that swaps the script text (and recompiles). Weakref
# capture (see reorder_track) keeps the history off an uncollectable cycle.
func _push_text_edit(prev: String, next: String) -> void:
	var wself: WeakRef = weakref(self)
	var do_cb := func() -> void:
		var s: MusicEditorDocument = wself.get_ref()
		if s != null:
			s._apply_script_text(next)
	var undo_cb := func() -> void:
		var s: MusicEditorDocument = wself.get_ref()
		if s != null:
			s._apply_script_text(prev)
	_history.push(do_cb, undo_cb)


# Set the script text, recompile, and on success emit changed so the map /
# inspector rebuild off the new model. Returns false if the text doesn't compile
# (the caller rolls back).
func _apply_script_text(text: String) -> bool:
	set_script_text(StringName(""), text)
	var errs: Array = compile_script()
	if errs.is_empty():
		changed.emit()
		return true
	return false


func _current_script_text() -> String:
	# Operate on the NAMES-LESS decompile of the COMMITTED script. The names-less
	# form is the proven round-trip (tests/mus/mus_roundtrip_test); the
	# names-aware form (real bank names as bind identifiers) is a display
	# convenience that does not necessarily recompile, so it must never drive the
	# write path. Reading the committed mus_script (not the in-flight
	# _compiled_script_text, which the raw drawer fills with names-aware text)
	# keeps structured edits self-consistent and always compilable.
	if not script_loaded():
		return ""
	var script_name := StringName(mus_script.get_default_script_name())
	return mus_script.get_decompiled_text(script_name)


# The bind identifier a `play` references. The names-less decompile (which the
# write path operates on) binds every slot as "sound_N"; the inspector resolves
# the friendly bank name for display only.
func _track_bind_id(track: int) -> String:
	return "sound_%d" % track


# Insert "play <bind>" into the section's STRAIGHT-LINE body (brace depth 1),
# never inside a nested if/else/on(...) block (which would make it conditional).
# Anchor on the last top-level play so it appends to the unconditional sequence;
# else place it before the first top-level transition/halt so it still runs;
# else just inside the opening brace. Mirrors splice_play in
# tests/mus/mus_structured_play_edit_test.cpp.
func _splice_play(text: String, section_name: String, bind: String) -> String:
	var lines := text.split("\n")
	var hidx := _section_header_line(lines, section_name)
	if hidx < 0:
		return text
	var end := _section_window_end(lines, hidx)
	var depth := 0
	var open_brace := -1
	var last_top_play := -1
	var first_top_transition := -1
	for i in range(hidx + 1, end):
		var s := lines[i].strip_edges()
		if s == "{":
			depth += 1
			if open_brace < 0:
				open_brace = i
			continue
		if s == "}":
			depth -= 1
			continue
		if depth == 1:
			if s.begins_with("play ") or s.begins_with("playw "):
				last_top_play = i
			elif first_top_transition < 0 and (s.begins_with("enter ") or s == "done" or s.begins_with("on (")):
				first_top_transition = i
	var insert_at := -1
	if last_top_play >= 0:
		insert_at = last_top_play + 1
	elif first_top_transition >= 0:
		insert_at = first_top_transition
	elif open_brace >= 0:
		insert_at = open_brace + 1
	else:
		insert_at = hidx + 1
	lines.insert(insert_at, "play %s" % bind)
	return "\n".join(lines)


# Remove the LAST top-level "play <bind>" so it cancels _splice_play's append
# exactly: insert + remove is a byte-identical no-op even when the track already
# appears earlier in the body. Only depth-1 plays are touched, so a play nested
# in a conditional branch is never silently deleted.
func _remove_one_play(text: String, section_name: String, bind: String) -> String:
	var lines := text.split("\n")
	var hidx := _section_header_line(lines, section_name)
	if hidx < 0:
		return text
	var end := _section_window_end(lines, hidx)
	var depth := 0
	var remove_at := -1
	for i in range(hidx + 1, end):
		var s := lines[i].strip_edges()
		if s == "{":
			depth += 1
			continue
		if s == "}":
			depth -= 1
			continue
		if depth == 1 and s == "play %s" % bind:
			remove_at = i
	if remove_at >= 0:
		lines.remove_at(remove_at)
	return "\n".join(lines)


func _section_header_line(lines: PackedStringArray, section_name: String) -> int:
	for i in range(lines.size()):
		if lines[i].strip_edges() == "section %s" % section_name:
			return i
	return -1


func _section_window_end(lines: PackedStringArray, header_idx: int) -> int:
	for i in range(header_idx + 1, lines.size()):
		var s := lines[i].strip_edges()
		if s.begins_with("section ") or s.begins_with("declsection "):
			return i
	return lines.size()


# --- Unified document undo ----------------------------------------------
# One timeline for every edit (bank reorder/rename + play/script edits), so the
# single screen has a single, predictable Ctrl+Z.

func undo() -> void:
	_history.undo()


func redo() -> void:
	_history.redo()


func can_undo() -> bool:
	return _history.can_undo()


func can_redo() -> bool:
	return _history.can_redo()


# Back-compat aliases: callers/tests that predate the unified history. All route
# to the one timeline so bank and script edits undo in the order they happened.
func undo_bank() -> void: _history.undo()
func redo_bank() -> void: _history.redo()
func undo_script() -> void: _history.undo()
func redo_script() -> void: _history.redo()
func can_undo_bank() -> bool: return _history.can_undo()
func can_redo_bank() -> bool: return _history.can_redo()
func can_undo_script() -> bool: return _history.can_undo()
func can_redo_script() -> bool: return _history.can_redo()


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
