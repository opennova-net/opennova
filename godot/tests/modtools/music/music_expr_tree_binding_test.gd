extends GutTest

# End-to-end closure of the structured-expression path: the C++ AST's
# rhs_tree/expr_tree dictionaries (NovaMusicScript.get_program_ast) must
# serialize through the editor's MusExpr to EXACTLY the canonical flat text the
# decompiler stored beside them. ctest pins C-tree -> C-render equality; this
# pins the binding's dict shape + MusExpr.serialize as the same function, so an
# expression edited structurally in the editor round-trips byte-stable.

const MusExpr = preload("res://modtools/music/mus_expr.gd")
const BANK_FIXTURE := "res://../fixtures/sbf/jo_gamemus.sbf"
const SCRIPT_FIXTURE := "res://../fixtures/mus/jo_gamemus.bin"


func _load_script() -> NovaMusicScript:
	var bytes := FileAccess.get_file_as_bytes(SCRIPT_FIXTURE)
	assert_gt(bytes.size(), 0, "fixture readable")
	var ms := NovaMusicScript.new()
	ms.load_from_decrypted_bytes(bytes, "gamemus")
	return ms


func _walk(stmts: Array, pairs: Array) -> void:
	for s in stmts:
		if s.has("rhs_tree"):
			pairs.append([s.get("rhs_tree"), String(s.get("rhs", ""))])
		if s.has("expr_tree"):
			pairs.append([s.get("expr_tree"), String(s.get("expr", ""))])
		_walk(s.get("then", []), pairs)
		_walk(s.get("else", []), pairs)


func test_locals_frame_offset_exposed():
	# The editor numbers caller inputs from this base ("Input 1" = l_32).
	var ms := _load_script()
	assert_eq(ms.get_locals_frame_offset(ms.get_default_script_name()), 32,
		"stock scripts bank caller inputs at byte 32")


func test_every_exposed_tree_serializes_to_its_canonical_text():
	var ms := _load_script()
	var ast: Array = ms.get_program_ast(ms.get_default_script_name())
	assert_gt(ast.size(), 0, "AST present")
	var pairs: Array = []
	for sec in ast:
		_walk(sec.get("statements", []), pairs)
	assert_gt(pairs.size(), 0, "gamemus exposes expression trees through the binding")
	for p in pairs:
		assert_eq(MusExpr.serialize(p[0]), String(p[1]),
			"tree dict serializes byte-identical to the stored canonical text")


func test_tree_dict_shape_matches_mus_expr_nodes():
	# The known gamemus condition (Var01 != 0): binding dict must be a BINOP whose
	# operands MusExpr understands without translation.
	var ms := _load_script()
	var ast: Array = ms.get_program_ast(ms.get_default_script_name())
	var tree := {}
	for sec in ast:
		for s in sec.get("statements", []):
			if String(s.get("kind", "")) == "if" and String(s.get("expr", "")).contains("Var01"):
				tree = s.get("expr_tree", {})
	assert_false(tree.is_empty(), "the Var01 if-condition carries a tree dict")
	assert_eq(int(tree.get("kind", -1)), MusExpr.BINOP, "condition is a BINOP node")
	assert_eq(String(tree.get("op", "")), "!=")
	var left: Dictionary = tree.get("left", {})
	assert_eq(int(left.get("kind", -1)), MusExpr.VARREF, "lhs is a variable reference")
	assert_eq(MusExpr.serialize(left), "Var01", "lhs reads back as Var01")
	var right: Dictionary = tree.get("right", {})
	assert_eq(int(right.get("kind", -1)), MusExpr.LITERAL, "rhs is a literal")
