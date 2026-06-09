extends GutTest

# Covers NovaMusicScript.get_program_ast: the full structured statement tree the
# visual-first editor renders (every play, transition, assignment, intrinsic
# call, if/else, on-switch -- not just the section topology the model exposes).
# Built from libs/mus mus_parse_to_ast, the structured twin of the decompiler.

const FIXTURE := "res://../fixtures/mus/jo_gamemus.bin"


func _ast() -> Array:
	var ms := load(FIXTURE) as NovaMusicScript
	assert_not_null(ms, "fixture loads as NovaMusicScript")
	if ms == null:
		return []
	return ms.get_program_ast(StringName(ms.get_default_script_name()))


func _by_name(ast: Array, name: String) -> Dictionary:
	for d in ast:
		if String(d.get("name", "")) == name:
			return d
	return {}


func _first_of_kind(stmts: Array, kind: String) -> Dictionary:
	for s in stmts:
		if String(s.get("kind", "")) == kind:
			return s
	return {}


func _count_plays(stmts: Array) -> int:
	var total := 0
	for s in stmts:
		var kind := String(s.get("kind", ""))
		if kind == "play":
			total += 1
		elif kind == "switch" and String(s.get("action", "")) == "play":
			total += (s.get("targets", []) as Array).size()
		total += _count_plays(s.get("then", []))
		total += _count_plays(s.get("else", []))
	return total


func test_all_sections_present() -> void:
	var ast := _ast()
	assert_eq(ast.size(), 8, "gamescript has 8 sections")
	if ast.is_empty():
		return
	var first: Dictionary = ast[0]
	for key in ["name", "index", "is_entry", "code_offset", "statements"]:
		assert_true(first.has(key), "section dict has key '%s'" % key)


func test_every_statement_has_kind_and_offset() -> void:
	var ast := _ast()
	var seen := 0
	for sec in ast:
		for s in sec.get("statements", []):
			assert_true(s.has("kind"), "statement has a kind")
			assert_true(s.has("code_offset"), "statement has a code_offset")
			assert_true(s.has("text"), "statement has rendered text")
			seen += 1
	assert_gt(seen, 0, "saw at least one statement")


func test_begin_decomposes() -> void:
	# Begin: an SV(200) intrinsic call, a transition to Testmission, a done, then
	# the leaked tail (enter Missionwin / FB() / on-switch) still owned by Begin.
	var begin := _by_name(_ast(), "Begin")
	assert_false(begin.is_empty(), "Begin present")
	if begin.is_empty():
		return
	assert_true(bool(begin.get("is_entry", false)), "Begin is the entry section")
	var stmts: Array = begin.get("statements", [])

	var expr := _first_of_kind(stmts, "expr")
	assert_false(expr.is_empty(), "Begin has an expression statement")
	assert_true(bool(expr.get("has_call", false)), "the expr is a function call")
	assert_eq(String(expr.get("call_name", "")), "GSV", "the call is GSV")

	var sw := _first_of_kind(stmts, "switch")
	assert_false(sw.is_empty(), "Begin owns the on-switch (leaked past done)")
	assert_eq(String(sw.get("action", "")), "enter", "switch action is enter")
	assert_eq((sw.get("targets", []) as Array).size(), 3, "switch fans to 3 targets")

	assert_false(_first_of_kind(stmts, "done").is_empty(), "Begin has a done")
	assert_false(_first_of_kind(stmts, "transition").is_empty(), "Begin has a transition")


func test_testmission_if_else() -> void:
	var tm := _by_name(_ast(), "Testmission")
	assert_false(tm.is_empty(), "Testmission present")
	if tm.is_empty():
		return
	var iff := _first_of_kind(tm.get("statements", []), "if")
	assert_false(iff.is_empty(), "Testmission has an if")
	assert_true(String(iff.get("expr", "")).contains("Var01"), "condition mentions Var01")
	assert_true(bool(iff.get("else_present", false)), "if has an else block")
	var then_body: Array = iff.get("then", [])
	var else_body: Array = iff.get("else", [])
	assert_eq(then_body.size(), 1, "then has one statement")
	assert_eq(else_body.size(), 1, "else has one statement")
	if then_body.size() == 1:
		assert_eq(String(then_body[0].get("kind", "")), "transition", "then is a transition")
	if else_body.size() == 1:
		assert_eq(String(else_body[0].get("kind", "")), "transition", "else is a transition")


func test_win000_plays() -> void:
	var w := _by_name(_ast(), "Win000")
	assert_false(w.is_empty(), "Win000 present")
	if w.is_empty():
		return
	var stmts: Array = w.get("statements", [])
	assert_eq(_count_plays(stmts), 6, "Win000 plays 6 tracks")
	var p0 := _first_of_kind(stmts, "play")
	assert_false(p0.is_empty(), "Win000 has a play")
	assert_eq(int(p0.get("track", -1)), 2, "first play targets track 2")


func test_statements_ascend_by_offset() -> void:
	var ast := _ast()
	for sec in ast:
		var last := -1
		for s in sec.get("statements", []):
			var off := int(s.get("code_offset", -1))
			assert_true(off >= last, "statements ascend by code offset in %s" % String(sec.get("name", "")))
			last = off


func test_unknown_script_is_empty() -> void:
	var ms := load(FIXTURE) as NovaMusicScript
	if ms == null:
		return
	assert_eq(ms.get_program_ast(StringName("nope")).size(), 0, "unknown script -> empty AST")
