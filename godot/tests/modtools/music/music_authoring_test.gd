extends GutTest

# Phase 2 visual authoring: the document's structured statement + section edits
# (span-located, parity-gated, undoable), the annotated-decompile bridge, and the
# canonical text/expression serializers. Mirrors the C++ proof in
# tests/mus/mus_structured_section_edit_test.cpp on the GDScript side, plus the
# gate-rollback + undo behaviour the C++ side can't see.

const MusicEditorDocument = preload("res://modtools/music/music_editor_document.gd")
const MusStmtText = preload("res://modtools/music/mus_stmt_text.gd")
const MusExpr = preload("res://modtools/music/mus_expr.gd")
const BANK_FIXTURE := "res://../fixtures/sbf/jo_gamemus.sbf"
const SCRIPT_FIXTURE := "res://../fixtures/mus/jo_gamemus.bin"
const PAIR_BANK := "user://music_auth_pair.sbf"
const PAIR_SCRIPT := "user://music_auth_pair.bin"


func after_each() -> void:
	_rm(PAIR_BANK)
	_rm(PAIR_SCRIPT)


func _doc() -> MusicEditorDocument:
	var doc = MusicEditorDocument.new()
	_copy(BANK_FIXTURE, PAIR_BANK)
	_copy(SCRIPT_FIXTURE, PAIR_SCRIPT)
	doc.open_pair(PAIR_BANK)
	return doc


func _sname(doc) -> StringName:
	return StringName(doc.mus_script.get_default_script_name())


func _ast(doc) -> Array:
	return doc.mus_script.get_program_ast(_sname(doc))


func _section(doc, name: String) -> Dictionary:
	for s in _ast(doc):
		if String(s.get("name", "")) == name:
			return s
	return {}


func _section_index(doc, name: String) -> int:
	return int(_section(doc, name).get("index", -1))


func _stmt_count(doc, name: String) -> int:
	return (_section(doc, name).get("statements", []) as Array).size()


# Find a section with at least one play, return its name.
func _section_with_play(doc) -> String:
	for s in _ast(doc):
		for st in s.get("statements", []):
			if String(st.get("kind", "")) == "play":
				return String(s.get("name", ""))
	return ""


# ---- annotated bridge ----

func test_annotated_decompile_matches_and_has_rows():
	var doc := _doc()
	var sn := _sname(doc)
	var ann: Dictionary = doc.mus_script.get_annotated_decompile(sn)
	assert_eq(String(ann.get("text", "")), doc.mus_script.get_decompiled_text(sn),
		"annotated text is byte-identical to the names-less decompile")
	var rows: Array = ann.get("rows", [])
	assert_gt(rows.size(), 0, "annotated rows present")
	for r in rows:
		assert_true(r.has("line_start") and r.has("line_end") and r.has("ordinal"),
			"each row carries its span + ordinal")


# ---- insert / undo ----

func test_insert_assignment_then_undo():
	var doc := _doc()
	assert_true(doc.can_author(), "gamemus is authorable")
	var name := _section_with_play(doc)
	assert_ne(name, "", "found a section with a play")
	var sidx := _section_index(doc, name)
	var before := _stmt_count(doc, name)
	assert_true(doc.insert_statement(sidx, MusStmtText.assign("Var05", "(Var05 + 1)")),
		"insert assignment succeeds")
	assert_eq(_stmt_count(doc, name), before + 1, "one statement added")
	# the new statement is the assignment we authored
	var found := false
	for st in _section(doc, name).get("statements", []):
		if String(st.get("kind", "")) == "assign" and String(st.get("var_name", "")) == "Var05":
			found = true
	assert_true(found, "the authored assignment is present in the AST")
	doc.undo()
	assert_eq(_stmt_count(doc, name), before, "undo removes the inserted statement")


func test_insert_transition_adds_edge():
	var doc := _doc()
	# Insert "enter <some other section>" into a section and confirm the model edge.
	var names := Array(doc.mus_script.get_section_names(_sname(doc)))
	assert_gt(names.size(), 1, "multiple sections")
	var src := _section_with_play(doc)
	var sidx := _section_index(doc, src)
	# pick a target distinct from src
	var target := ""
	for n in names:
		if String(n) != src:
			target = String(n)
			break
	assert_true(doc.insert_statement(sidx, MusStmtText.enter(target)), "insert enter succeeds")
	# the source section now has a transition statement to the target
	var has_edge := false
	for st in _section(doc, src).get("statements", []):
		if String(st.get("kind", "")) == "transition" and String(st.get("target_name", "")) == target:
			has_edge = true
	assert_true(has_edge, "the authored transition is in the AST")


# ---- delete / replace / reorder ----

func test_delete_statement():
	var doc := _doc()
	var name := _section_with_play(doc)
	var sidx := _section_index(doc, name)
	# ordinal of the first play
	var statements: Array = _section(doc, name).get("statements", [])
	var ord := -1
	for i in range(statements.size()):
		if String(statements[i].get("kind", "")) == "play":
			ord = i
			break
	assert_true(ord >= 0, "found a play ordinal")
	var before := _stmt_count(doc, name)
	assert_true(doc.delete_statement(sidx, ord), "delete succeeds")
	assert_eq(_stmt_count(doc, name), before - 1, "one statement removed")
	doc.undo()
	assert_eq(_stmt_count(doc, name), before, "undo restores it")


func test_delete_done_terminator_refused():
	var doc := _doc()
	var name := _section_with_play(doc)
	var sidx := _section_index(doc, name)
	var statements: Array = _section(doc, name).get("statements", [])
	var done_ord := -1
	for i in range(statements.size()):
		if String(statements[i].get("kind", "")) == "done":
			done_ord = i
	if done_ord >= 0:
		assert_false(doc.delete_statement(sidx, done_ord),
			"the structural section terminator (done) can't be deleted")


func test_replace_statement_changes_track():
	var doc := _doc()
	var name := _section_with_play(doc)
	var sidx := _section_index(doc, name)
	var statements: Array = _section(doc, name).get("statements", [])
	var ord := -1
	var orig_track := -1
	for i in range(statements.size()):
		if String(statements[i].get("kind", "")) == "play":
			ord = i
			orig_track = int(statements[i].get("track", -1))
			break
	assert_true(ord >= 0, "found a play")
	var new_track := 1 if orig_track != 1 else 2
	assert_true(doc.replace_statement(sidx, ord, MusStmtText.play(new_track)), "replace succeeds")
	var st: Dictionary = (_section(doc, name).get("statements", []) as Array)[ord]
	assert_eq(String(st.get("kind", "")), "play", "still a play")
	assert_eq(int(st.get("track", -1)), new_track, "track changed to the new value")


func test_reorder_statement_roundtrips():
	var doc := _doc()
	var name := _section_with_play(doc)
	var sidx := _section_index(doc, name)
	# find two adjacent plays
	var statements: Array = _section(doc, name).get("statements", [])
	var i := -1
	for k in range(statements.size() - 1):
		if String(statements[k].get("kind", "")) == "play" and String(statements[k + 1].get("kind", "")) == "play":
			i = k
			break
	if i < 0:
		pass_test("section has no two adjacent plays to reorder; skipped")
		return
	var t0 := int(statements[i].get("track", -1))
	var t1 := int(statements[i + 1].get("track", -1))
	assert_true(doc.reorder_statement(sidx, i, 1), "swap down succeeds")
	var after: Array = _section(doc, name).get("statements", [])
	assert_eq(int(after[i].get("track", -1)), t1, "neighbours swapped")
	assert_eq(int(after[i + 1].get("track", -1)), t0, "neighbours swapped")


# ---- section rename / delete ----

func test_rename_section():
	var doc := _doc()
	var name := _section_with_play(doc)
	assert_true(doc.rename_section(StringName(name), StringName("RenamedState")), "rename succeeds")
	var names := Array(doc.mus_script.get_section_names(_sname(doc)))
	assert_true(names.has("RenamedState"), "new name present")
	assert_false(names.has(name), "old name gone")
	assert_true(doc.can_author(), "renamed script still compiles")
	doc.undo()
	assert_true(Array(doc.mus_script.get_section_names(_sname(doc))).has(name), "undo restores the old name")


func test_rename_rejects_taken_or_reserved():
	var doc := _doc()
	var names := Array(doc.mus_script.get_section_names(_sname(doc)))
	var a := String(names[0])
	var b := String(names[1])
	assert_false(doc.rename_section(StringName(a), StringName(b)), "rename to an existing section name rejected")
	assert_false(doc.rename_section(StringName(a), StringName("play")), "rename to a reserved keyword rejected")
	assert_false(doc.rename_section(StringName(a), StringName("has space")), "rename to a non-identifier rejected")


func test_section_name_validation_explains_rejections():
	var doc := _doc()
	assert_true(doc.has_method("validate_section_name"), "document exposes state-name validation reasons")
	if not doc.has_method("validate_section_name"):
		return
	var names := Array(doc.mus_script.get_section_names(_sname(doc)))
	var current: StringName = StringName(names[0])
	var taken: StringName = StringName(names[1])
	assert_eq(doc.validate_section_name(&"", current), "Enter a state name.")
	assert_eq(doc.validate_section_name(current, current), "Type a different state name.")
	assert_string_contains(doc.validate_section_name(taken, current), "already exists")
	assert_string_contains(doc.validate_section_name(&"play", current), "reserved")
	assert_string_contains(doc.validate_section_name(&"has space", current), "letters, numbers")
	assert_eq(doc.validate_section_name(&"Fresh_State", current), "")


func test_delete_unreferenced_section():
	var doc := _doc()
	var before := Array(doc.mus_script.get_section_names(_sname(doc))).size()
	assert_true(doc.add_section(StringName("Throwaway")), "add a fresh (unreferenced) section")
	assert_eq(Array(doc.mus_script.get_section_names(_sname(doc))).size(), before + 1, "added one")
	assert_true(doc.delete_section(StringName("Throwaway")), "delete the unreferenced section")
	assert_eq(Array(doc.mus_script.get_section_names(_sname(doc))).size(), before, "section count back to base")


func test_delete_entry_section_refused():
	var doc := _doc()
	# the entry/start section (is_entry) must not be deletable -- deleting it would
	# silently re-point where playback starts.
	var entry := ""
	for s in _ast(doc):
		if bool(s.get("is_entry", false)):
			entry = String(s.get("name", ""))
			break
	assert_ne(entry, "", "found the entry section")
	var before := Array(doc.mus_script.get_section_names(_sname(doc))).size()
	assert_false(doc.delete_section(StringName(entry)), "the entry section can't be deleted")
	assert_eq(Array(doc.mus_script.get_section_names(_sname(doc))).size(), before, "nothing changed")


func test_delete_referenced_section_refused():
	var doc := _doc()
	# Find a section that some other section enters (referenced via the model).
	var referenced := ""
	for s in doc.mus_script.get_section_model(_sname(doc)):
		for e in s.get("edges", []):
			var tn := String(e.get("to_name", ""))
			if tn != "":
				referenced = tn
				break
		if referenced != "":
			break
	if referenced == "":
		pass_test("no referenced section found in fixture; skipped")
		return
	var before := Array(doc.mus_script.get_section_names(_sname(doc))).size()
	assert_false(doc.delete_section(StringName(referenced)), "deleting a referenced section is refused")
	assert_eq(Array(doc.mus_script.get_section_names(_sname(doc))).size(), before, "nothing changed")


# ---- gate rollback ----

func test_gate_rejects_unresolved_goto():
	var doc := _doc()
	var name := _section_with_play(doc)
	var sidx := _section_index(doc, name)
	var before := _stmt_count(doc, name)
	# goto to a section that doesn't exist -> "unresolved branch target" -> the gate
	# rolls back and the op returns false.
	assert_false(doc.insert_statement(sidx, MusStmtText.goto_section("NoSuchSectionXYZ")),
		"an edit that doesn't compile is rejected")
	assert_eq(_stmt_count(doc, name), before, "rejected edit changed nothing")
	assert_true(doc.can_author(), "script is still compilable after a rejected edit")


# ---- serializers (pure) ----

func test_stmt_text_canonical_forms():
	assert_eq(Array(MusStmtText.assign("Var07", "(Var07 + 1)")), ["Var07 = (Var07 + 1)"])
	assert_eq(Array(MusStmtText.enter("Combat")), ["enter Combat"])
	assert_eq(Array(MusStmtText.incdec("Var03", true)), ["Var03++"])
	assert_eq(Array(MusStmtText.incdec("Var03", false)), ["Var03--"])
	# The selector/condition is wrapped once in (...), exactly like the decompiler:
	# a bare-var selector stays single-parens; a binop (already parenthesized by
	# MusExpr.serialize) becomes double, which is the canonical round-tripping form.
	assert_eq(Array(MusStmtText.switch_stmt("Var01", "enter", PackedStringArray(["A", "B"]))), ["on (Var01) enter A B"])
	# if/else block: 4-space body indent, "}" at column 0.
	var block := Array(MusStmtText.if_block("(Var01 == 0)", PackedStringArray(["enter A"]), true, PackedStringArray(["enter B"])))
	assert_eq(block, ["if ((Var01 == 0))", "{", "    enter A", "}", "else", "{", "    enter B", "}"])


# Authoring an if-block end to end: it compiles, round-trips to a STRUCTURED if
# (not a branch-comment), and deletes cleanly by its single ordinal.
func test_author_if_block_roundtrips():
	var doc := _doc()
	var name := _section_with_play(doc)
	var sidx := _section_index(doc, name)
	var target := ""
	for n in Array(doc.mus_script.get_section_names(_sname(doc))):
		if String(n) != name:
			target = String(n)
			break
	var lines := MusStmtText.if_block("(Var01 == 0)", PackedStringArray([MusStmtText.enter(target)[0]]), false, PackedStringArray())
	var before := _stmt_count(doc, name)
	assert_true(doc.insert_statement(sidx, lines), "insert if-block succeeds")
	# the committed AST shows a structured 'if' in that section
	var has_if := false
	var if_ord := -1
	var statements: Array = _section(doc, name).get("statements", [])
	for i in range(statements.size()):
		if String(statements[i].get("kind", "")) == "if":
			has_if = true
			if_ord = i
	assert_true(has_if, "the authored if re-decompiles to a structured if")
	# deleting the if (one ordinal) removes the whole block
	assert_true(doc.delete_statement(sidx, if_ord), "delete the if-block by its ordinal")
	assert_eq(_stmt_count(doc, name), before, "the whole block was removed")


func test_expr_serialize():
	assert_eq(MusExpr.serialize(MusExpr.literal(200)), "200")
	assert_eq(MusExpr.serialize(MusExpr.varref("Var", 1)), "Var01")
	assert_eq(MusExpr.serialize(MusExpr.me()), "Me")
	# binop always parenthesized with spaces
	assert_eq(MusExpr.serialize(MusExpr.binop("==", MusExpr.varref("Var", 1), MusExpr.literal(2))), "(Var01 == 2)")
	# unary no space
	assert_eq(MusExpr.serialize(MusExpr.unop("!", MusExpr.varref("Var", 0))), "!Var00")
	# intrinsic surface form: GSV -> SV, FSet -> F.Set
	assert_eq(MusExpr.serialize(MusExpr.call_node("GSV", MusExpr.literal(200))), "SV(200)")
	assert_eq(MusExpr.serialize(MusExpr.call_node("FSet", MusExpr.literal(3))), "F.Set(3)")
	assert_eq(MusExpr.serialize(MusExpr.call_node("TStart", null)), "T.Start()")


# An authored expression validates through the native compiler.
func test_expr_validate_via_compiler():
	var doc := _doc()
	var ok: Dictionary = MusExpr.validate_expr("(Var01 == 2)", doc.mus_script)
	assert_true(bool(ok.get("ok", false)), "a well-formed condition validates")
	var bad: Dictionary = MusExpr.validate_expr("(Var01 ==", doc.mus_script)
	assert_false(bool(bad.get("ok", true)), "a malformed condition is caught by the compiler")


func _copy(src_path: String, dst_path: String) -> void:
	var src := FileAccess.open(src_path, FileAccess.READ)
	assert_not_null(src, "fixture readable: %s" % src_path)
	if src == null:
		return
	var dst := FileAccess.open(dst_path, FileAccess.WRITE)
	if dst != null:
		dst.store_buffer(src.get_buffer(src.get_length()))
		dst.close()
	src.close()


func _rm(path: String) -> void:
	var abs := ProjectSettings.globalize_path(path)
	if FileAccess.file_exists(abs):
		DirAccess.remove_absolute(abs)
