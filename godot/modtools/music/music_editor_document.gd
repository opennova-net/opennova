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
	# into the dict instead.
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
	if _compiled_file_bytes.size() > 0:
		mus_script.set_compiled_file_bytes(_compiled_file_bytes)
	elif _compiled_bytecode.size() > 0:
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


# --- Create a project from scratch ---------------------------------------
#
# The editor could only ever EDIT an existing .sbf/.bin pair; there was no way to
# author a music program from nothing. new_project mints a fresh, editable
# script + empty bank so authoring can begin immediately.
#
# The script is minted from a minimal template through the SAME proven path the
# structured edits use: compile_text -> file_bytes -> set_compiled_file_bytes ->
# load_from_decrypted_bytes. The resulting in-memory state is byte-identical to
# opening a real .bin (and get_raw_file_bytes() is non-empty, so the saver can
# write it). The bank is a fresh empty SBF (NovaSbfBank.create_empty); the user
# imports tracks via the Tracks dock (＋ Add).

# A script with one empty section. The empty body compiles to a single `done`,
# and that first section is the entry/start (mus_compile pins entry index 0).
const _NEW_SCRIPT_TEMPLATE := "script %s\nsection Begin\n{\n}\n"


func new_script(script_name: String = "gamescript") -> int:
	var ms := NovaMusicScript.new()
	var d: Dictionary = ms.compile_text(_NEW_SCRIPT_TEMPLATE % script_name)
	if int(d.get("rc", -1)) != 0:
		return ERR_CANT_CREATE
	var fb: PackedByteArray = d.get("file_bytes", PackedByteArray())
	if fb.is_empty():
		return ERR_CANT_CREATE
	# Loads the freshly-encoded bytes into the resource (sets source bytes the
	# saver passes through). Now script_loaded() and can_author() are both true.
	ms.set_compiled_file_bytes(fb)
	mus_script = ms
	script_path = ""
	_compiled_script_text = ""
	_compiled_bytecode = PackedByteArray()
	_compiled_file_bytes = PackedByteArray()
	_set_script_dirty(true)
	return OK


func new_bank() -> int:
	# create_empty is a static factory on the NovaSbfBank GDExtension class (the
	# default constructor leaves a bank unconfigured). Guard so an older binary
	# without the factory degrades to a script-only project instead of crashing.
	if not ClassDB.class_has_method("NovaSbfBank", "create_empty", true):
		return ERR_UNAVAILABLE
	var b = NovaSbfBank.create_empty()
	if b == null:
		return ERR_CANT_CREATE
	bank = b
	bank_path = ""
	_set_bank_dirty(true)
	return OK


# New script + empty bank, unsaved (no paths). A brand-new project starts a fresh
# undo timeline so Ctrl+Z can't reach back into the discarded project. Returns the
# script error if the script couldn't be minted; a bank failure is a soft degrade
# (the script is still authorable/saveable) and does not abort the project.
func new_project() -> int:
	_history = MusicEditHistoryClass.new()
	bank = null
	bank_path = ""
	mus_script = null
	script_path = ""
	_compiled_script_text = ""
	_compiled_bytecode = PackedByteArray()
	_compiled_file_bytes = PackedByteArray()
	_set_bank_dirty(false)
	_set_script_dirty(false)
	var serr := new_script()
	if serr != OK:
		changed.emit()
		return serr
	new_bank()  # best-effort; Start needs a bank, but authoring/save don't
	changed.emit()
	return OK


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
	# without touching disk; the transport label flashes the diagnostic via the
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
	if not script_loaded():
		return false
	# Structured edits operate on a single chunk (the default script) and
	# re-encode a one-chunk file; a multi-chunk .bin would lose its other chunks
	# on the first edit, so keep those read-only.
	if int(mus_script.get_script_count()) != 1:
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


# User-facing validation for Add/Rename state flows. Returns "" when the name can
# be submitted, otherwise a short reason the UI can show inline.
func validate_section_name(candidate: StringName, current_name: StringName = &"") -> String:
	var name := String(candidate).strip_edges()
	var current := String(current_name)
	if current != "" and name == current:
		return "Type a different state name."
	var shape_reason := _section_name_shape_error(name)
	if shape_reason != "":
		return shape_reason
	if name != current and _section_name_exists(name):
		return "A state named '%s' already exists." % name
	return ""


# A section name must lex as a single bare identifier ([A-Za-z_][A-Za-z0-9_]*)
# that isn't a reserved keyword -- the only form the compiler's `section <ident>`
# accepts.
func _is_valid_section_name(name: String) -> bool:
	return _section_name_shape_error(name) == ""


func _section_name_shape_error(name: String) -> String:
	if name.is_empty():
		return "Enter a state name."
	if name.length() > 31:
		return "Use 31 characters or fewer."
	if _RESERVED.has(name):
		return "That name is reserved by MUS."
	var c0 := name.unicode_at(0)
	var is_alpha0 := (c0 >= 65 and c0 <= 90) or (c0 >= 97 and c0 <= 122) or c0 == 95
	if not is_alpha0:
		return "Use letters, numbers, and underscores; start with a letter or underscore."
	for i in range(1, name.length()):
		var c := name.unicode_at(i)
		var ok := (c >= 65 and c <= 90) or (c >= 97 and c <= 122) or (c >= 48 and c <= 57) or c == 95
		if not ok:
			return "Use letters, numbers, and underscores; start with a letter or underscore."
	return ""


func _section_name_exists(name: String) -> bool:
	if not script_loaded():
		return false
	var text := _current_script_text()
	if text == "":
		return false
	return _section_header_line(text.split("\n"), name) >= 0


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
	# write path. Reading the committed mus_script (re-decompiled fresh) rather than
	# the in-flight _compiled_script_text keeps every structured edit based on the
	# canonical names-less text and always compilable.
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


# --- Phase 2: structured statement + section authoring -------------------
#
# Generalizes the play-edit write path to every construct. An edit is still a
# TEXT transform over the names-less decompile of the committed script, recompiled
# through _apply_script_text (rolls back on failure), pushed onto the one undo
# timeline. The new precision comes from get_annotated_decompile (NovaMusicScript),
# which hands back the names-less text PLUS a per-top-level-statement line span
# (computed by the SAME emitter the AST uses), so a single statement can be
# spliced/deleted/replaced/reordered by line range -- robust to the brace-leak and
# compound if/on blocks that a brace scanner mishandles. The C++ twin of these
# transforms is proven byte-stable in tests/mus/mus_structured_section_edit_test.cpp.

# MusAstStmtKind mirror (libs/mus/include/mus/ast.h). The annotated rows carry the
# kind so the anchor logic can find a section's terminator.
const _K_PLAY := 0
const _K_TRANSITION := 1
const _K_GOTO := 2
const _K_CALL := 3
const _K_RETURN := 4
const _K_YIELD := 5
const _K_NOP := 6
const _K_DONE := 7
const _K_ASSIGN := 8
const _K_INCDEC := 9
const _K_EXPR := 10
const _K_IF := 11
const _K_SWITCH := 12
const _K_BRANCH_COMMENT := 13
# enter (0x38) frame setup: a read-only annotation, never mutable. Its operand is
# a locals dword count, not a section index; rewriting it as a transition (the
# decompiler/compiler collapse "enter" to setstate 0x3B) would corrupt the frame.
const _K_FRAME_ENTER := 14

# Statements that end (or redirect) a section's straight-line flow. A new
# statement inserts before the first of these so it actually runs. (frame_enter is
# NOT a terminator -- 0x38 does not move the IP -- so it is excluded.)
const _TERMINATOR_KINDS := [
	_K_TRANSITION, _K_GOTO, _K_CALL, _K_RETURN, _K_YIELD, _K_DONE, _K_SWITCH,
]

# Structural / non-mutable rows the write path must refuse: the section-closing
# `}` (done) and the frame-setup `enter` (0x38). Editing either corrupts the
# section (leak the body / scramble the frame), so delete/replace/reorder reject them.
const _LOCKED_KINDS := [_K_DONE, _K_FRAME_ENTER]


# The Phase-2 gate. Same body as can_edit_plays() (single chunk, compiles); a
# distinct name so authoring call sites read intentionally. Kept as an alias so
# the play-edit callers/tests are untouched.
func can_author() -> bool:
	return can_edit_plays()


# Human-readable reason the structured write path is currently disabled, or "" when
# authoring is available. Mirrors can_edit_plays()'s checks so the editor can explain
# WHY edits are off -- most importantly the multi-chunk case, which compiles fine but
# is intentionally read-only (a structured edit re-encodes a single chunk and would
# drop the others), where a bare "must compile" message would just confuse.
func authoring_blocked_reason() -> String:
	if not script_loaded():
		return "Open a project first"
	if int(mus_script.get_script_count()) != 1:
		return "Multi-chunk script is read-only (editing would drop the other chunks)"
	var text := _current_script_text()
	if text == "":
		return "Script is empty"
	var d: Dictionary = mus_script.compile_text(text)
	if int(d.get("rc", -1)) != 0:
		return "Fix script errors first"
	return ""


# {text, rows} for the committed names-less decompile, via the bridge. rows is an
# Array of { section_index, ordinal, code_offset, kind, line_start, line_end }.
func _annotated() -> Dictionary:
	if not script_loaded():
		return {}
	var script_name := StringName(mus_script.get_default_script_name())
	return mus_script.get_annotated_decompile(script_name)


func _rows_for_section(rows: Array, section_index: int) -> Array:
	var out: Array = []
	for r in rows:
		if int(r.get("section_index", -1)) == section_index:
			out.append(r)
	out.sort_custom(func(a, b): return int(a.get("ordinal", 0)) < int(b.get("ordinal", 0)))
	return out


func _find_row(rows: Array, section_index: int, ordinal: int) -> Dictionary:
	for r in rows:
		if int(r.get("section_index", -1)) == section_index and int(r.get("ordinal", -1)) == ordinal:
			return r
	return {}


# Where a fresh statement lands in a section: just before the first terminator
# (so it runs before the state moves on), else after the last statement. Every
# section ends in a `done`, so the terminator branch always fires for real scripts.
func _anchor_line(srows: Array) -> int:
	for r in srows:
		if int(r.get("kind", -1)) in _TERMINATOR_KINDS:
			return int(r.get("line_start", -1))
	if not srows.is_empty():
		return int(srows[-1].get("line_end", -1))
	return -1


# Insert statement lines into a section at the canonical anchor. lines are the
# already-rendered, already-indented .mus body lines (one entry per text line).
func insert_statement(section_index: int, lines: PackedStringArray) -> bool:
	if not can_author() or lines.is_empty():
		return false
	var ann := _annotated()
	if ann.is_empty():
		return false
	var text := String(ann.get("text", ""))
	var srows := _rows_for_section(ann.get("rows", []), section_index)
	var at := _anchor_line(srows)
	if at < 0:
		return false
	var next := _splice_lines(text, at, lines)
	if next == text:
		return false
	return _commit_edit(text, next)


func delete_statement(section_index: int, ordinal: int) -> bool:
	if not can_author():
		return false
	var ann := _annotated()
	if ann.is_empty():
		return false
	var text := String(ann.get("text", ""))
	var row := _find_row(ann.get("rows", []), section_index, ordinal)
	if row.is_empty():
		return false
	# The section-closing `}` (done) is structural -- deleting it would leak the
	# section into the next; the frame-setup `enter` (0x38) is read-only. Keep both.
	if int(row.get("kind", -1)) in _LOCKED_KINDS:
		return false
	var ls := int(row.get("line_start", -1))
	var le := int(row.get("line_end", -1))
	if ls < 0 or le <= ls:
		return false
	var next := _delete_lines(text, ls, le)
	if next == text:
		return false
	return _commit_edit(text, next)


func replace_statement(section_index: int, ordinal: int, lines: PackedStringArray) -> bool:
	if not can_author() or lines.is_empty():
		return false
	var ann := _annotated()
	if ann.is_empty():
		return false
	var text := String(ann.get("text", ""))
	var row := _find_row(ann.get("rows", []), section_index, ordinal)
	if row.is_empty():
		return false
	if int(row.get("kind", -1)) in _LOCKED_KINDS:
		return false
	var ls := int(row.get("line_start", -1))
	var le := int(row.get("line_end", -1))
	if ls < 0 or le <= ls:
		return false
	var arr := text.split("\n")
	for i in range(le - ls):
		arr.remove_at(ls)
	for i in range(lines.size()):
		arr.insert(ls + i, lines[i])
	var next := "\n".join(arr)
	if next == text:
		return false
	return _commit_edit(text, next)


# Swap a statement with its neighbour (direction -1 up / +1 down) in the same
# section. Both must be real statements (not the section terminator) and adjacent.
func reorder_statement(section_index: int, ordinal: int, direction: int) -> bool:
	if not can_author() or direction == 0:
		return false
	var ann := _annotated()
	if ann.is_empty():
		return false
	var text := String(ann.get("text", ""))
	var srows := _rows_for_section(ann.get("rows", []), section_index)
	var pos := -1
	for i in range(srows.size()):
		if int(srows[i].get("ordinal", -1)) == ordinal:
			pos = i
			break
	if pos < 0:
		return false
	# Skip text-less (zero-width, e.g. nop) neighbours so a real statement can step
	# over them instead of getting stuck against an invisible row.
	var other := pos + direction
	while other >= 0 and other < srows.size():
		var nr: Dictionary = srows[other]
		if int(nr.get("line_start", 0)) != int(nr.get("line_end", 0)):
			break
		other += direction
	if other < 0 or other >= srows.size():
		return false
	var lo: Dictionary = srows[mini(pos, other)]
	var hi: Dictionary = srows[maxi(pos, other)]
	# Don't shuffle across a locked row (the section terminator `}` would leak into
	# the tail; the frame-setup `enter` is read-only).
	if int(lo.get("kind", -1)) in _LOCKED_KINDS or int(hi.get("kind", -1)) in _LOCKED_KINDS:
		return false
	if int(lo.get("line_end", -1)) != int(hi.get("line_start", -2)):
		return false
	var arr := text.split("\n")
	var a_ls := int(lo.get("line_start", 0))
	var a_le := int(lo.get("line_end", 0))
	var b_ls := int(hi.get("line_start", 0))
	var b_le := int(hi.get("line_end", 0))
	var out := PackedStringArray()
	out.append_array(arr.slice(0, a_ls))
	out.append_array(arr.slice(b_ls, b_le))
	out.append_array(arr.slice(a_ls, a_le))
	out.append_array(arr.slice(b_le, arr.size()))
	var next := "\n".join(out)
	if next == text:
		return false
	return _commit_edit(text, next)


# Index into the section's ordered rows for `ordinal`, or -1.
func _row_pos(srows: Array, ordinal: int) -> int:
	for i in range(srows.size()):
		if int(srows[i].get("ordinal", -1)) == ordinal:
			return i
	return -1


# Position of the section's first flow terminator (where the authored region
# ends). Insert/move gaps exist up to and including this row; past it is the
# section close / engine-dispatch tail.
func _first_terminator_pos(srows: Array) -> int:
	for i in range(srows.size()):
		if int(srows[i].get("kind", -1)) in _TERMINATOR_KINDS:
			return i
	return srows.size()


# Insert statement lines into the gap ABOVE the row at `before_ordinal`
# (the program view's insertion carets). -1 falls back to the canonical
# append anchor. Gaps past the first terminator (the dispatch tail) and the
# hidden frame-setup row are not insertion targets.
func insert_statement_at(section_index: int, before_ordinal: int, lines: PackedStringArray) -> bool:
	if not can_author() or lines.is_empty():
		return false
	if before_ordinal < 0:
		return insert_statement(section_index, lines)
	var ann := _annotated()
	if ann.is_empty():
		return false
	var text := String(ann.get("text", ""))
	var srows := _rows_for_section(ann.get("rows", []), section_index)
	var pos := _row_pos(srows, before_ordinal)
	if pos < 0 or pos > _first_terminator_pos(srows):
		return false
	if int(srows[pos].get("kind", -1)) == _K_FRAME_ENTER:
		return false
	var at := int(srows[pos].get("line_start", -1))
	if at < 0:
		return false
	var next := _splice_lines(text, at, lines)
	if next == text:
		return false
	return _commit_edit(text, next)


# Cut one statement and re-insert it before another (the program view's
# drag-reorder). `before_ordinal` -1 = the canonical append anchor. Locked rows
# don't move; nothing moves into the dispatch tail; a no-op move returns false.
func move_statement(section_index: int, ordinal: int, before_ordinal: int) -> bool:
	if not can_author() or ordinal == before_ordinal:
		return false
	var ann := _annotated()
	if ann.is_empty():
		return false
	var text := String(ann.get("text", ""))
	var srows := _rows_for_section(ann.get("rows", []), section_index)
	var pos := _row_pos(srows, ordinal)
	if pos < 0:
		return false
	var row: Dictionary = srows[pos]
	if int(row.get("kind", -1)) in _LOCKED_KINDS:
		return false
	var term := _first_terminator_pos(srows)
	if pos > term:
		return false
	var ls := int(row.get("line_start", -1))
	var le := int(row.get("line_end", -1))
	if ls < 0 or le <= ls:
		return false
	var at := -1
	if before_ordinal < 0:
		at = _anchor_line(srows)
	else:
		var tpos := _row_pos(srows, before_ordinal)
		if tpos < 0 or tpos > term:
			return false
		if int(srows[tpos].get("kind", -1)) == _K_FRAME_ENTER:
			return false
		at = int(srows[tpos].get("line_start", -1))
	if at < 0 or (at > ls and at < le):
		return false
	var arr := text.split("\n")
	var moved := arr.slice(ls, le)
	var rest := PackedStringArray()
	rest.append_array(arr.slice(0, ls))
	rest.append_array(arr.slice(le, arr.size()))
	var dst := at if at <= ls else at - (le - ls)
	var out := PackedStringArray()
	out.append_array(rest.slice(0, dst))
	out.append_array(moved)
	out.append_array(rest.slice(dst, rest.size()))
	var next := "\n".join(out)
	if next == text:
		return false
	return _commit_edit(text, next)


# Resize a folded run: `old_count` consecutive, identical, single-line rows
# starting at `start_ordinal` become `new_count` copies of the same line. One
# splice = one undo entry, whether the run grows or shrinks.
func set_run_count(section_index: int, start_ordinal: int, old_count: int, new_count: int) -> bool:
	if not can_author() or old_count < 1 or new_count < 1 or new_count == old_count:
		return false
	var ann := _annotated()
	if ann.is_empty():
		return false
	var text := String(ann.get("text", ""))
	var srows := _rows_for_section(ann.get("rows", []), section_index)
	var pos := _row_pos(srows, start_ordinal)
	if pos < 0 or pos + old_count > srows.size():
		return false
	var arr := text.split("\n")
	var first: Dictionary = srows[pos]
	var ls := int(first.get("line_start", -1))
	if ls < 0 or int(first.get("line_end", -1)) != ls + 1:
		return false
	var line := arr[ls]
	for i in range(old_count):
		var r: Dictionary = srows[pos + i]
		if int(r.get("ordinal", -1)) != start_ordinal + i:
			return false  # a hidden row interrupts the run
		if int(r.get("kind", -1)) in _LOCKED_KINDS:
			return false
		if int(r.get("line_start", -1)) != ls + i or int(r.get("line_end", -1)) != ls + i + 1:
			return false  # multi-line or non-adjacent: not a foldable run
		if arr[ls + i] != line:
			return false  # not identical
	var out := PackedStringArray()
	out.append_array(arr.slice(0, ls))
	for i in range(new_count):
		out.append(line)
	out.append_array(arr.slice(ls + old_count, arr.size()))
	var next := "\n".join(out)
	if next == text:
		return false
	return _commit_edit(text, next)


# Rename a section everywhere it is a section reference (its section/declsection
# headers + every enter/goto/call/on target). Produces identical runtime bytecode
# (names live only in the editor-debug string table), so this is always safe when
# it compiles. Rejects reserved/taken/invalid names.
func rename_section(old_name: StringName, new_name: StringName) -> bool:
	if not can_author():
		return false
	var nn := String(new_name).strip_edges()
	var on := String(old_name)
	if not _is_valid_section_name(nn) or nn == on:
		return false
	var ann := _annotated()
	if ann.is_empty():
		return false
	var text := String(ann.get("text", ""))
	var lines := text.split("\n")
	if _section_header_line(lines, on) < 0:
		return false  # old section missing
	if _section_header_line(lines, nn) >= 0:
		return false  # new name already a section
	var next := _rename_identifier(text, on, nn)
	if next == text:
		return false
	return _commit_edit(text, next)


# Delete a section's declsection + body. Refused (no-op) when the section is
# referenced, because a removed `enter X` would silently re-intern X at offset 0
# (the compile gate can't catch that). The user must retarget references first.
func delete_section(name: StringName) -> bool:
	if not can_author():
		return false
	# Refuse deleting the entry/start section. The compiler re-pins the entry to the
	# first-declared section, so deleting the entry would silently change where
	# playback begins -- and the result still compiles, so the gate wouldn't catch
	# it. The user must designate a new start state first.
	if _is_entry_section(String(name)):
		return false
	var ann := _annotated()
	if ann.is_empty():
		return false
	var text := String(ann.get("text", ""))
	if _count_section_token(text, String(name)) > 2:
		return false  # referenced -> refuse
	var next := _delete_section_text(text, String(name))
	if next == text:
		return false
	return _commit_edit(text, next)


# Gate + commit shared by every Phase-2 op: recompile through _apply_script_text
# (rolls back to `prev` on failure), then push the do/undo pair.
func _commit_edit(prev: String, next: String) -> bool:
	if not _apply_script_text(next):
		_apply_script_text(prev)  # compile failed: change nothing
		return false
	_push_text_edit(prev, next)
	return true


# --- line-splice primitives (twin of the C++ test's helpers) -------------

func _splice_lines(text: String, at_line: int, lines: PackedStringArray) -> String:
	var arr := text.split("\n")
	if at_line < 0 or at_line > arr.size():
		return text
	for i in range(lines.size()):
		arr.insert(at_line + i, lines[i])
	return "\n".join(arr)


func _delete_lines(text: String, start: int, end_excl: int) -> String:
	var arr := text.split("\n")
	if start < 0 or end_excl > arr.size() or start >= end_excl:
		return text
	for i in range(end_excl - start):
		arr.remove_at(start)
	return "\n".join(arr)


# --- section-token transforms (twin of the C++ test's helpers) -----------

# Whole-identifier token replace, skipping string literals and // comments.
func _rename_identifier(text: String, old_name: String, new_name: String) -> String:
	var out := ""
	var i := 0
	var n := text.length()
	var in_str := false
	while i < n:
		var c := text[i]
		if c == "\"":
			in_str = not in_str
			out += c
			i += 1
			continue
		if not in_str and c == "/" and i + 1 < n and text[i + 1] == "/":
			while i < n and text[i] != "\n":
				out += text[i]
				i += 1
			continue
		if not in_str and _is_ident_start_char(c):
			var j := i + 1
			while j < n and _is_ident_cont_char(text[j]):
				j += 1
			var tok := text.substr(i, j - i)
			out += new_name if tok == old_name else tok
			i = j
		else:
			out += c
			i += 1
	return out


# Count whole-identifier occurrences of a section name (outside strings/comments).
# A canonical decompile names each section twice when unreferenced (declsection +
# section header), so > 2 means referenced.
func _count_section_token(text: String, name: String) -> int:
	var count := 0
	var i := 0
	var n := text.length()
	var in_str := false
	while i < n:
		var c := text[i]
		if c == "\"":
			in_str = not in_str
			i += 1
			continue
		if not in_str and c == "/" and i + 1 < n and text[i + 1] == "/":
			while i < n and text[i] != "\n":
				i += 1
			continue
		if not in_str and _is_ident_start_char(c):
			var j := i + 1
			while j < n and _is_ident_cont_char(text[j]):
				j += 1
			if text.substr(i, j - i) == name:
				count += 1
			i = j
		else:
			i += 1
	return count


func _delete_section_text(text: String, name: String) -> String:
	var lines := text.split("\n")
	# Drop the declsection forward-decl.
	var dl := -1
	for i in range(lines.size()):
		if lines[i].strip_edges() == "declsection %s" % name:
			dl = i
			break
	if dl >= 0:
		lines.remove_at(dl)
	# Drop the section body window (header -> next section/declsection header / EOF).
	var hidx := _section_header_line(lines, name)
	if hidx < 0:
		return text
	var end := _section_window_end(lines, hidx)
	for i in range(end - hidx):
		lines.remove_at(hidx)
	return "\n".join(lines)


# True when `name` is the script's entry/start section (is_entry on the AST dict).
func _is_entry_section(name: String) -> bool:
	if not script_loaded():
		return false
	var sn := StringName(mus_script.get_default_script_name())
	for s in mus_script.get_program_ast(sn):
		if String(s.get("name", "")) == name:
			return bool(s.get("is_entry", false))
	return false


func _is_ident_start_char(c: String) -> bool:
	if c.is_empty():
		return false
	var u := c.unicode_at(0)
	return (u >= 65 and u <= 90) or (u >= 97 and u <= 122) or u == 95


func _is_ident_cont_char(c: String) -> bool:
	if c.is_empty():
		return false
	var u := c.unicode_at(0)
	return (u >= 65 and u <= 90) or (u >= 97 and u <= 122) or (u >= 48 and u <= 57) or u == 95


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



# Lazy seed: pull the decompiler's text for the default script and cache it.
# Used by compile_script (e.g. on transport Start / Save) when the buffer hasn't
# been filled by a structured edit yet. Doing this eagerly in open_script would
# clobber any in-flight script_text; lazy keeps both paths safe.
#
# ALWAYS seed from the NAMES-LESS decompile, even with a bank loaded. The
# names-aware form (real bank names as bind identifiers) is a display convenience
# that does NOT reliably recompile -- a bank name containing a space or other
# non-identifier character fails to lex -- so seeding the compile buffer from it
# could make the live VM run, or Save write, a script that won't round-trip. The
# structured write path is already names-less (see _current_script_text); seeding
# names-less keeps the whole compile path on the one proven-recompilable form.
func _seed_compiled_text_from_script() -> void:
	if not script_loaded():
		return
	var script_name: StringName = StringName(mus_script.get_default_script_name())
	_compiled_script_text = mus_script.get_decompiled_text(script_name)
