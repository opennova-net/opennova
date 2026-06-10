extends GutTest

# The inline structured expression editor (ui/expr_row.gd): seeds STRUCTURED
# from the C++ AST's expression trees, serializes byte-stable canonical text
# through MusExpr, and validates through the native compiler. The contract that
# matters: seed(tree) -> get_expr_text() reproduces the stored canonical text
# EXACTLY for every tree the shipped scripts expose, so opening an expression
# for editing can never change it.

const ExprRowClass = preload("res://modtools/music/ui/expr_row.gd")
const MusExpr = preload("res://modtools/music/mus_expr.gd")
const SCRIPT_FIXTURE := "res://../fixtures/mus/jo_gamemus.bin"
const MENU_FIXTURE := "res://../fixtures/mus/jo_menumus.bin"


func _vars() -> Array:
	var out := []
	for i in range(17):
		out.append({"token": "Var%02d" % i, "label": "Var%02d" % i})
	return out


func _row() -> Control:
	var r: Control = ExprRowClass.new()
	add_child_autofree(r)
	r.setup(_vars(), null)
	return r


func _load_script(path: String) -> NovaMusicScript:
	var bytes := FileAccess.get_file_as_bytes(path)
	assert_gt(bytes.size(), 0, "fixture readable")
	var ms := NovaMusicScript.new()
	ms.load_from_decrypted_bytes(bytes, path)
	return ms


func _collect_trees(stmts: Array, out: Array) -> void:
	for s in stmts:
		if s.has("rhs_tree"):
			out.append([s.get("rhs_tree"), String(s.get("rhs", ""))])
		if s.has("expr_tree"):
			out.append([s.get("expr_tree"), String(s.get("expr", ""))])
		_collect_trees(s.get("then", []), out)
		_collect_trees(s.get("else", []), out)


func test_seeding_every_shipped_tree_is_byte_stable():
	# Both shipped scripts, every exposed tree: opening it in the editor and
	# reading the text back must be a no-op.
	for path in [SCRIPT_FIXTURE, MENU_FIXTURE]:
		var ms := _load_script(path)
		var pairs: Array = []
		for sec in ms.get_program_ast(ms.get_default_script_name()):
			_collect_trees(sec.get("statements", []), pairs)
		assert_gt(pairs.size(), 0, "%s exposes trees" % path)
		var r := _row()
		for p in pairs:
			r.set_expr(p[0])
			assert_eq(r.get_expr_text(), String(p[1]),
				"seed -> serialize is byte-stable for %s" % String(p[1]))


func test_structured_seed_opens_structured_not_raw():
	# (Var01 != 0) must open as an Expression cell with a Variable lhs -- the
	# whole point over the old builder's type-it-only reopening.
	var ms := _load_script(SCRIPT_FIXTURE)
	var tree := {}
	for sec in ms.get_program_ast(ms.get_default_script_name()):
		for s in sec.get("statements", []):
			if String(s.get("kind", "")) == "if" and String(s.get("expr", "")).contains("Var01"):
				tree = s.get("expr_tree", {})
	assert_false(tree.is_empty(), "gamemus Var01 condition has a tree")
	var r := _row()
	r.set_expr(tree)
	var d: Dictionary = r.get_expr_dict()
	assert_eq(int(d.get("kind", -1)), MusExpr.BINOP, "opens as a structured binop, not raw text")
	assert_eq(String(d.get("op", "")), "!=")
	assert_eq(MusExpr.serialize(d.get("left", {})), "Var01", "lhs landed on the variable picker")


func test_mutating_an_operand_serializes_canonically():
	var r := _row()
	r.set_expr(MusExpr.binop("==", MusExpr.varref("Var", 2), MusExpr.literal(5)))
	assert_eq(r.get_expr_text(), "(Var02 == 5)")
	# Flip the literal operand via the embedded SpinBox (right cell of the root).
	var root = r._root
	root._right._num.value = 9
	assert_eq(r.get_expr_text(), "(Var02 == 9)", "operand edits re-serialize canonically")


func test_function_call_round_trip():
	var r := _row()
	r.set_expr(MusExpr.call_node("GSV", MusExpr.literal(65536)))
	assert_eq(r.get_expr_text(), "SV(65536)", "stored name serializes in surface form")
	var d: Dictionary = r.get_expr_dict()
	assert_eq(int(d.get("kind", -1)), MusExpr.CALL, "opens as a structured call")
	r.set_expr(MusExpr.call_node("FSet", null))
	assert_eq(r.get_expr_text(), "F.Set()", "no-arg call keeps empty parentheses")


func test_too_deep_nesting_falls_back_to_canonical_text():
	# Depth past the cap opens as type-it but the TEXT stays exact.
	var deep := MusExpr.binop("+",
		MusExpr.binop("*",
			MusExpr.binop("-",
				MusExpr.binop("/",
					MusExpr.binop("%", MusExpr.literal(9), MusExpr.literal(5)),
					MusExpr.literal(2)),
				MusExpr.literal(1)),
			MusExpr.literal(3)),
		MusExpr.literal(4))
	var want := MusExpr.serialize(deep)
	var r := _row()
	r.set_expr(deep)
	assert_eq(r.get_expr_text(), want, "beyond-cap subtrees survive as exact text")


func test_unop_seeds_structurally_and_round_trips():
	# "not X" is a structured mode now, not a type-it fallback.
	var r := _row()
	r.set_expr(MusExpr.unop("!", MusExpr.varref("Var", 1)))
	assert_eq(r.get_expr_text(), "!Var01", "unary text stays canonical")
	var d: Dictionary = r.get_expr_dict()
	assert_eq(int(d.get("kind", -1)), MusExpr.UNOP, "opens as a structured unary, not raw text")
	assert_eq(String(d.get("op", "")), "!")
	assert_eq(MusExpr.serialize(d.get("operand", {})), "Var01", "operand landed structurally")


func test_caller_input_token_seeds_onto_variable_picker():
	# A var list that carries input entries (the graph supplies them for states
	# with caller inputs) makes an l_N reference structural, not type-it.
	var vars := _vars()
	vars.append({"token": "l_32", "label": "Input 1 (l_32)"})
	var r: Control = ExprRowClass.new()
	add_child_autofree(r)
	r.setup(vars, null)
	r.set_expr(MusExpr.binop("==", MusExpr.varref("l", 32), MusExpr.literal(2)))
	assert_eq(r.get_expr_text(), "(l_32 == 2)", "the canonical token is unchanged")
	var d: Dictionary = r.get_expr_dict()
	assert_eq(int(d.get("kind", -1)), MusExpr.BINOP, "seeded structurally")
	assert_eq(MusExpr.serialize(d.get("left", {})), "l_32", "lhs landed on the Input picker entry")


func test_changed_signal_fires_on_edit():
	var r := _row()
	r.set_expr(MusExpr.literal(1))
	var seen: Array = []
	r.expr_changed.connect(func(t): seen.append(t))
	r._root._num.value = 7
	assert_true(seen.has("7"), "operand edits emit expr_changed with the new text")
