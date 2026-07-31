extends GutTest

# The anchored expression popover: seeding from C++ trees, apply/cancel
# contract, the view-level singleton (retarget, re-anchor across re-renders,
# close-with-notice when the statement is gone), and the stale-apply guard
# (apply after an intervening document change uses the FRESH bodies).

const ProgramViewClass = preload("res://modtools/music/ui/section_program_view.gd")
const PopoverClass = preload("res://modtools/music/ui/expr_popover.gd")
const StmtRowClass = preload("res://modtools/music/ui/stmt_row.gd")
const IfBlockClass = preload("res://modtools/music/ui/stmt_if_block.gd")
const MusExpr = preload("res://modtools/music/mus_expr.gd")
const MusicEditorDocument = preload("res://modtools/music/music_editor_document.gd")
const BANK_FIXTURE := "res://../fixtures/sbf/jo_gamemus.sbf"
const SCRIPT_FIXTURE := "res://../fixtures/mus/jo_gamemus.bin"
const PAIR_BANK := "user://music_popover_pair.sbf"
const PAIR_SCRIPT := "user://music_popover_pair.bin"


func after_each() -> void:
	_rm(PAIR_BANK)
	_rm(PAIR_SCRIPT)


func _view(snames := PackedStringArray(["A", "B"])) -> MusicSectionProgramView:
	var v: MusicSectionProgramView = ProgramViewClass.new()
	v.size = Vector2(700, 500)
	add_child_autofree(v)
	v.configure_authoring(snames,
		[{"token": "Var00", "label": "Var00"}, {"token": "Var01", "label": "Var01"}], null, [], true)
	return v


func _assign_sec(index := 0) -> Dictionary:
	return {"name": "S", "index": index, "statements": [
		{"kind": "assign", "code_offset": 0, "var_name": "Var00", "var_offset": 0,
			"is_local": false, "rhs": "(Var01 + 1)",
			"rhs_tree": MusExpr.binop("+", MusExpr.varref("named", -1, "Var01"), MusExpr.literal(1)),
			"text": "Var00 = (Var01 + 1)"},
	]}


# ---- the popover itself ----

func test_open_seeds_structurally_and_applies_canonical_text():
	var v := _view()
	v.show_section(_assign_sec(), [])
	watch_signals(v)
	var row: StmtRowClass = v.top_level_rows()[0]
	row._expr_chip.pressed.emit()
	assert_true(v._popover.is_open(), "the chip opens the popover")
	assert_signal_emitted(v, "inline_edit_started", "opening pins follow-live")
	assert_eq(v._popover.current_text(), "(Var01 + 1)",
		"the Build tab seeds from the tree, byte-stable")
	v._popover._on_apply()
	# Apply commits through the row's canonical line.
	assert_signal_emitted_with_parameters(v, "replace_statement_requested",
		[0, 0, PackedStringArray(["Var00 = (Var01 + 1)"])])
	assert_false(v._popover.is_open(), "apply closes it")


func test_cancel_is_quiet():
	var v := _view()
	v.show_section(_assign_sec(), [])
	var row: StmtRowClass = v.top_level_rows()[0]
	row._expr_chip.pressed.emit()
	watch_signals(v)
	v._popover.cancel()
	assert_false(v._popover.is_open())
	assert_signal_not_emitted(v, "replace_statement_requested", "cancel commits nothing")
	assert_signal_not_emitted(v, "author_failed", "cancel raises no notice")


func test_type_it_tab_applies_raw_text():
	var v := _view()
	v.show_section(_assign_sec(), [])
	watch_signals(v)
	var row: StmtRowClass = v.top_level_rows()[0]
	row._expr_chip.pressed.emit()
	v._popover._tabs.current_tab = 1
	v._popover._raw.text = "(Var00 * 2)"
	v._popover._on_apply()
	assert_signal_emitted_with_parameters(v, "replace_statement_requested",
		[0, 0, PackedStringArray(["Var00 = (Var00 * 2)"])])


func test_opening_elsewhere_retargets_the_singleton():
	var v := _view()
	var sec := _assign_sec()
	sec["statements"].append({"kind": "expr", "code_offset": 4, "expr": "SV(200)",
		"expr_tree": MusExpr.call_node("GSV", MusExpr.literal(200)),
		"text": "SV(200)", "has_call": true, "call_name": "GSV"})
	v.show_section(sec, [])
	var rows := v.top_level_rows()
	(rows[0] as StmtRowClass)._expr_chip.pressed.emit()
	assert_eq(int(v._popover.key().get("ordinal", -1)), 0)
	(rows[1] as StmtRowClass)._expr_chip.pressed.emit()
	assert_eq(int(v._popover.key().get("ordinal", -1)), 1, "one popover, re-targeted")
	assert_eq(v._popover.current_text(), "SV(200)")
	watch_signals(v)
	v._popover._on_apply()
	# Apply lands on the LAST opened slot only.
	assert_signal_emitted_with_parameters(v, "replace_statement_requested",
		[0, 1, PackedStringArray(["SV(200)"])])


# ---- re-render interplay ----

func test_rerender_reanchors_a_surviving_edit():
	var v := _view()
	v.show_section(_assign_sec(), [])
	(v.top_level_rows()[0] as StmtRowClass)._expr_chip.pressed.emit()
	v._popover._tabs.current_tab = 1
	v._popover._raw.text = "(Var00 * 3)"
	watch_signals(v)
	v.show_section(_assign_sec(), [])
	assert_true(v._popover.is_open(), "the popover survives an external re-render")
	assert_eq(v._popover._raw.text, "(Var00 * 3)", "...with the user's editing intact")
	assert_signal_not_emitted(v, "author_failed", "no notice when the edit survives")
	# And apply still lands -- on the REBUILT row.
	v._popover._on_apply()
	assert_signal_emitted_with_parameters(v, "replace_statement_requested",
		[0, 0, PackedStringArray(["Var00 = (Var00 * 3)"])])


func test_rerender_closes_a_lost_edit_with_a_notice():
	var v := _view()
	v.show_section(_assign_sec(), [])
	(v.top_level_rows()[0] as StmtRowClass)._expr_chip.pressed.emit()
	watch_signals(v)
	v.show_section({"name": "S", "index": 0, "statements": [
		{"kind": "transition", "code_offset": 0, "target_name": "A", "target_section": 1,
			"text": "enter A"},
	]}, [])
	assert_false(v._popover.is_open(), "the statement is gone, so is the edit")
	assert_signal_emitted(v, "author_failed", "...and the user is told why")


# ---- stale-apply guard (real document) ----

func test_apply_after_intervening_change_uses_fresh_bodies():
	var doc := _make_doc()
	var sname := StringName(doc.mus_script.get_default_script_name())
	var snames := PackedStringArray()
	for n in doc.mus_script.get_section_names(sname):
		snames.append(String(n))
	var v := _view(snames)
	v.configure_authoring(snames, [{"token": "Var00", "label": "Var00"},
		{"token": "Var01", "label": "Var01"}], doc.mus_script, [], true)
	v.replace_statement_requested.connect(func(sec, o, lines): doc.replace_statement(sec, o, lines))
	var test_sec := _doc_section(doc, "Testmission")
	var sidx := int(test_sec.get("index", -1))
	v.show_section(test_sec, [])
	var blk: MusicStmtIfBlock = v.top_level_rows()[0]
	# 1) open the CONDITION popover...
	blk._cond_chip.pressed.emit()
	assert_true(v._popover.is_open())
	# 2) ...then a DIFFERENT edit lands (retarget the then branch) and the owner
	# re-renders, as live_mode would on document.changed.
	var win_idx := -1
	for i in range(snames.size()):
		if snames[i] == "Win000":
			win_idx = i
	var then_row: StmtRowClass = blk.then_rows()[0]
	then_row._section_opt.select(win_idx)
	then_row._section_opt.item_selected.emit(win_idx)
	v.show_section(_doc_section(doc, "Testmission"), [])
	assert_true(v._popover.is_open(), "the condition edit survived the re-render")
	# 3) apply a new condition: the regenerated if must carry the FRESH then
	# body (Win000), not the bodies captured when the popover opened.
	v._popover._tabs.current_tab = 1
	v._popover._raw.text = "(Var01 != 5)"
	v._popover._on_apply()
	var after := _doc_section(doc, "Testmission")
	var if_dict: Dictionary = after.get("statements", [])[0]
	assert_string_contains(String(if_dict.get("expr", "")), "Var01 != 5", "the new condition landed")
	var then0: Dictionary = if_dict.get("then", [])[0]
	assert_eq(String(then0.get("target_name", "")), "Win000",
		"the intervening lane edit survived the apply (fresh-AST regeneration)")


func _make_doc() -> MusicEditorDocument:
	var doc = MusicEditorDocument.new()
	_copy(BANK_FIXTURE, PAIR_BANK)
	_copy(SCRIPT_FIXTURE, PAIR_SCRIPT)
	doc.open_pair(PAIR_BANK)
	return doc


func _doc_section(doc, name: String) -> Dictionary:
	var sname := StringName(doc.mus_script.get_default_script_name())
	for s in doc.mus_script.get_program_ast(sname):
		if String(s.get("name", "")) == name:
			return s
	return {}


func _copy(src_path: String, dst_path: String) -> void:
	var src := FileAccess.open(src_path, FileAccess.READ)
	assert_not_null(src, "fixture readable: %s" % src_path)
	if src == null:
		return
	var dst := FileAccess.open(dst_path, FileAccess.WRITE)
	if dst != null:
		dst.store_buffer(src.get_buffer(src.get_length()))
		dst.close()


func _rm(path: String) -> void:
	var abs := ProjectSettings.globalize_path(path)
	if FileAccess.file_exists(abs):
		DirAccess.remove_absolute(abs)
