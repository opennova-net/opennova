extends GutTest

# mus_display_names: the single source of artist-facing names. Every AST kind
# and every intrinsic must have an entry, the engine mnemonics must survive in
# tooltips (so cross-referencing original scripts stays possible), and the
# prettifier must stay display-only (canonical text untouched elsewhere).

const MusDisplayNames = preload("res://modtools/music/mus_display_names.gd")
const MusExpr = preload("res://modtools/music/mus_expr.gd")

# Every `kind` string nova_music_script.cpp ast_stmt_to_dict can emit, minus
# frame_enter (hidden -- never rendered, so it needs no display name).
const AST_KINDS := [
	"play", "transition", "goto", "call", "return", "yield", "nop", "done",
	"assign", "incdec", "expr", "if", "switch", "branch_comment",
]


func test_every_ast_kind_has_title_color_tooltip():
	for kind in AST_KINDS:
		assert_true(MusDisplayNames.STMT.has(kind), "%s has a display entry" % kind)
		assert_ne(MusDisplayNames.stmt_title(kind), "", "%s has a title" % kind)
		assert_ne(MusDisplayNames.stmt_tooltip(kind), "", "%s has a tooltip" % kind)


func test_every_intrinsic_has_friendly_label_and_mnemonic_tooltip():
	# Keyed off MusExpr.INTRINSICS (the catalog driven by libs/mus) so a new
	# intrinsic there can't silently miss a display name here.
	for it in MusExpr.INTRINSICS:
		var stored := String(it["name"])
		assert_true(MusDisplayNames.INTRINSICS.has(stored), "%s has a display entry" % stored)
		var label := MusDisplayNames.intrinsic_label(stored)
		assert_ne(label, "", "%s has a label" % stored)
		assert_ne(label, stored, "%s label is friendly, not the raw mnemonic" % stored)
		assert_string_contains(MusDisplayNames.intrinsic_tooltip(stored), stored,
			"%s tooltip keeps the engine mnemonic for cross-referencing" % stored)


func test_titles_are_plain_language_not_mnemonics():
	assert_string_contains(MusDisplayNames.stmt_title("expr"), "action",
		"expr reads as an action, not 'Run'")
	assert_string_contains(MusDisplayNames.stmt_title("switch"), "Choose",
		"switch reads as a choice, not 'On'")
	assert_string_contains(MusDisplayNames.stmt_title("incdec"), "variable",
		"incdec names the variable, not '± Var'")


func test_pretty_expr_rewrites_intrinsic_surface_forms():
	assert_eq(MusDisplayNames.pretty_expr("SV(65536)"), "Set volume(65536)")
	assert_eq(MusDisplayNames.pretty_expr("F.Set(3)"), "Set flag(3)")
	assert_eq(MusDisplayNames.pretty_expr("F.IsClear(3)"), "Is flag off?(3)")
	# SDV must not be half-eaten by the shorter SV rewrite.
	assert_eq(MusDisplayNames.pretty_expr("SDV(65536)"), "Set right-speaker volume(65536)")
	assert_eq(MusDisplayNames.pretty_expr("(GRnd(0) % 4)"), "(Random number(0) % 4)")


func test_pretty_expr_swaps_var_tokens_for_short_friendly_names():
	var vars := [
		{"token": "Var01", "label": "MissionActive (Var01)"},
		{"token": "Var02", "label": "Var02"},  # unnamed: untouched
	]
	assert_eq(MusDisplayNames.pretty_expr("(Var01 != 0)", vars), "(MissionActive != 0)",
		"named var reads by its short name inside expressions")
	assert_eq(MusDisplayNames.pretty_expr("(Var02 != 0)", vars), "(Var02 != 0)",
		"unnamed var stays raw")


func test_pretty_expr_handles_statement_lines_too():
	var vars := [{"token": "Var05", "label": "Speed (Var05)"}]
	assert_eq(MusDisplayNames.pretty_expr("Var05 = (Var05 + 1)", vars), "Speed = (Speed + 1)")
