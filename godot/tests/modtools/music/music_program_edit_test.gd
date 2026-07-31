extends GutTest

# Editing on the block-stack program view: instant picker commits, row tools,
# lane (if-body) mutations regenerating the whole if, switch case editing on
# the block, gap inserts, movability against locked neighbours, and the
# document e2e (the intents really mutate a gamemus pair and undo restores).
# Topology/read-side lives in music_program_view_test.gd; the popover itself
# in music_expr_popover_test.gd.

const ProgramViewClass = preload("res://modtools/music/ui/section_program_view.gd")
const StmtRowClass = preload("res://modtools/music/ui/stmt_row.gd")
const IfBlockClass = preload("res://modtools/music/ui/stmt_if_block.gd")
const SwitchBlockClass = preload("res://modtools/music/ui/stmt_switch_block.gd")
const MusForms = preload("res://modtools/music/mus_forms.gd")
const MusicEditorDocument = preload("res://modtools/music/music_editor_document.gd")
const BANK_FIXTURE := "res://../fixtures/sbf/jo_gamemus.sbf"
const SCRIPT_FIXTURE := "res://../fixtures/mus/jo_gamemus.bin"
const PAIR_BANK := "user://music_progedit_pair.sbf"
const PAIR_SCRIPT := "user://music_progedit_pair.bin"


func after_each() -> void:
	_rm(PAIR_BANK)
	_rm(PAIR_SCRIPT)


func _view(editable := true, snames := PackedStringArray(["A", "B", "C"])) -> MusicSectionProgramView:
	var v: MusicSectionProgramView = ProgramViewClass.new()
	v.size = Vector2(700, 500)
	add_child_autofree(v)
	v.configure_authoring(snames, [{"token": "Var00", "label": "Var00"}, {"token": "Var01", "label": "Var01"}],
		null, [], editable)
	return v


func _sec(stmts: Array, index := 0, name := "S") -> Dictionary:
	return {"name": name, "index": index, "statements": stmts}


func _play(off: int, track: int) -> Dictionary:
	return {"kind": "play", "code_offset": off, "track": track, "text": "play sound_%d" % track}


func _enter(off: int, target: String) -> Dictionary:
	return {"kind": "transition", "code_offset": off, "target_name": target,
		"target_section": 1, "text": "enter %s" % target}


func _find_button(node: Node, text: String) -> Button:
	if node is Button and not (node is MenuButton) and (node as Button).text == text:
		return node
	for c in node.get_children():
		var b := _find_button(c, text)
		if b != null:
			return b
	return null


func _find_menu(node: Node, text: String) -> MenuButton:
	if node is MenuButton and (node as MenuButton).text == text:
		return node
	for c in node.get_children():
		var m := _find_menu(c, text)
		if m != null:
			return m
	return null


func _pick(ob: OptionButton, idx: int) -> void:
	ob.select(idx)
	ob.item_selected.emit(idx)


# ---- read-only gating ----

func test_read_only_rows_carry_no_controls():
	var v := _view(false)
	v.show_section(_sec([_play(0, 0), _enter(2, "A")]), [])
	for r in v.top_level_rows():
		assert_null(_find_button(r, "✕"), "no delete on a read-only row")
		assert_null(_find_menu(r, "⋮"), "no move menu either")
		assert_eq((r as StmtRowClass)._track_opt, null, "no live pickers")


func test_dispatch_tail_rows_stay_read_only_even_when_editable():
	var v := _view(true)
	v.show_section(_sec([
		_enter(0, "A"),
		{"kind": "done", "code_offset": 2, "text": "}"},
		{"kind": "expr", "code_offset": 4, "expr": "FB(0)", "text": "FB(0)", "has_call": true, "call_name": "GFB"},
	]), [])
	var rows := v.top_level_rows()
	assert_not_null(v.dispatch_divider())
	var tail: StmtRowClass = rows[rows.size() - 1]
	assert_null(_find_button(tail, "✕"), "engine-event rows refuse edits")
	assert_eq(tail._expr_chip, null, "no chips in the tail")


# ---- instant picker commits ----

func test_track_pick_commits_the_canonical_line():
	var v := _view()
	v.show_section(_sec([_play(0, 0)], 2), ["Drums", "Bass", "Lead"])
	watch_signals(v)
	var row: StmtRowClass = v.top_level_rows()[0]
	assert_not_null(row._track_opt, "editable play row carries the track picker")
	_pick(row._track_opt, 2)
	assert_signal_emitted_with_parameters(v, "replace_statement_requested",
		[2, 0, PackedStringArray(["play sound_2"])])


func test_target_pick_commits_enter_goto_call():
	# Flow-leaving statements are each the last of their own flow (anything
	# after one is the read-only dispatch tail), so test one per section.
	var v := _view()
	watch_signals(v)
	v.show_section(_sec([_enter(0, "A")]), [])
	_pick((v.top_level_rows()[0] as StmtRowClass)._section_opt, 1)
	assert_signal_emitted_with_parameters(v, "replace_statement_requested",
		[0, 0, PackedStringArray(["enter B"])])
	v.show_section(_sec([
		{"kind": "goto", "code_offset": 0, "target_name": "A", "target_section": 0, "text": "goto A"},
	]), [])
	_pick((v.top_level_rows()[0] as StmtRowClass)._section_opt, 2)
	assert_signal_emitted_with_parameters(v, "replace_statement_requested",
		[0, 0, PackedStringArray(["goto C"])])
	# call returns, so flow continues past it.
	v.show_section(_sec([
		{"kind": "call", "code_offset": 0, "target_name": "A", "target_section": 0, "text": "call A"},
		_enter(2, "B"),
	]), [])
	_pick((v.top_level_rows()[0] as StmtRowClass)._section_opt, 1)
	assert_signal_emitted_with_parameters(v, "replace_statement_requested",
		[0, 0, PackedStringArray(["call B"])])


func test_var_and_direction_picks_commit():
	var v := _view()
	v.show_section(_sec([
		{"kind": "assign", "code_offset": 0, "var_name": "Var00", "var_offset": 0,
			"is_local": false, "rhs": "(Var01 + 1)", "text": "Var00 = (Var01 + 1)"},
		{"kind": "incdec", "code_offset": 4, "var_name": "Var00", "var_offset": 0,
			"is_local": false, "is_inc": true, "text": "Var00++"},
	]), [])
	watch_signals(v)
	var rows := v.top_level_rows()
	_pick((rows[0] as StmtRowClass)._var_opt, 1)
	# Retargeting the variable keeps the value expression.
	assert_signal_emitted_with_parameters(v, "replace_statement_requested",
		[0, 0, PackedStringArray(["Var01 = (Var01 + 1)"])])
	_pick((rows[1] as StmtRowClass)._dir_opt, 1)
	assert_signal_emitted_with_parameters(v, "replace_statement_requested",
		[0, 1, PackedStringArray(["Var00--"])])


func test_picker_popup_pins_follow_live():
	var v := _view()
	v.show_section(_sec([_play(0, 0)]), [])
	watch_signals(v)
	var row: StmtRowClass = v.top_level_rows()[0]
	row._track_opt.get_popup().about_to_popup.emit()
	assert_signal_emitted(v, "inline_edit_started")


# ---- tools: delete, move, gap insert ----

func test_delete_and_move_intents():
	var v := _view()
	v.show_section(_sec([_play(0, 0), _play(2, 1), _enter(4, "A")], 1), [])
	watch_signals(v)
	var rows := v.top_level_rows()
	_find_button(rows[1], "✕").pressed.emit()
	assert_signal_emitted_with_parameters(v, "delete_statement_requested", [1, 1])
	var menu := _find_menu(rows[1], "⋮")
	menu.get_popup().id_pressed.emit(0)
	assert_signal_emitted_with_parameters(v, "reorder_statement_requested", [1, 1, -1])
	menu.get_popup().id_pressed.emit(1)
	assert_signal_emitted_with_parameters(v, "reorder_statement_requested", [1, 1, 1])


func test_movability_respects_locked_neighbours():
	var v := _view()
	v.show_section(_sec([
		{"kind": "frame_enter", "code_offset": 0, "locals_count": 1, "text": "enter S"},
		_play(2, 0),
		_play(4, 1),
		{"kind": "done", "code_offset": 6, "text": "}"},
	]), [])
	var rows := v.top_level_rows()
	var first_pop := _find_menu(rows[0], "⋮").get_popup()
	assert_true(first_pop.is_item_disabled(0), "nothing moves above the hidden frame setup")
	assert_false(first_pop.is_item_disabled(1), "down past a sibling is fine")
	var last_pop := _find_menu(rows[1], "⋮").get_popup()
	assert_true(last_pop.is_item_disabled(1), "nothing moves past the section close")


func test_gap_insert_routes_to_insert_at():
	var v := _view()
	v.show_section(_sec([_play(0, 0), _enter(2, "A")], 3), [])
	watch_signals(v)
	# The palette index of "play a track" in MusForms.ADD_ITEMS is 0.
	v._add_kind_at(0, 1)
	assert_signal_emitted_with_parameters(v, "insert_statement_at_requested",
		[3, 1, PackedStringArray(["play sound_0"])])


func test_toolbar_add_appends_at_the_anchor():
	var v := _view()
	v.show_section(_sec([_enter(0, "A")], 3), [])
	watch_signals(v)
	v._add_kind_at(0, -1)
	assert_signal_emitted_with_parameters(v, "add_statement_requested",
		[3, PackedStringArray(["play sound_0"])])


# ---- if lanes: every mutation regenerates the whole block ----

func _if_stmt() -> Dictionary:
	return {"kind": "if", "code_offset": 0, "expr": "(Var01 != 0)",
		"text": "if ((Var01 != 0))", "else_present": true,
		"then": [_enter(4, "A")],
		"else": [_play(8, 0), _play(10, 1)]}


func test_lane_row_retarget_regenerates_the_if():
	var v := _view()
	v.show_section(_sec([_if_stmt()]), [])
	watch_signals(v)
	var blk: MusicStmtIfBlock = v.top_level_rows()[0]
	_pick((blk.then_rows()[0] as StmtRowClass)._section_opt, 2)
	# One canonical whole-if replace; the untouched bodies survive verbatim.
	assert_signal_emitted_with_parameters(v, "replace_statement_requested",
		[0, 0, PackedStringArray([
			"if ((Var01 != 0))", "{", "    enter C", "}",
			"else", "{", "    play sound_0", "    play sound_1", "}"])])


func test_lane_delete_append_and_move():
	var v := _view()
	v.show_section(_sec([_if_stmt()]), [])
	watch_signals(v)
	var blk: MusicStmtIfBlock = v.top_level_rows()[0]
	_find_button(blk.else_rows()[0], "✕").pressed.emit()
	assert_signal_emitted_with_parameters(v, "replace_statement_requested",
		[0, 0, PackedStringArray([
			"if ((Var01 != 0))", "{", "    enter A", "}",
			"else", "{", "    play sound_1", "}"])])
	# Lane ＋: append a play to then (palette index of play within the
	# filtered lane items is 0).
	blk._on_lane_add_id(0, "then")
	assert_signal_emitted_with_parameters(v, "replace_statement_requested",
		[0, 0, PackedStringArray([
			"if ((Var01 != 0))", "{", "    enter A", "    play sound_0", "}",
			"else", "{", "    play sound_0", "    play sound_1", "}"])])
	# Move the second else play up.
	var menu := _find_menu(blk.else_rows()[1], "⋮")
	menu.get_popup().id_pressed.emit(0)
	assert_signal_emitted_with_parameters(v, "replace_statement_requested",
		[0, 0, PackedStringArray([
			"if ((Var01 != 0))", "{", "    enter A", "}",
			"else", "{", "    play sound_1", "    play sound_0", "}"])])


func test_add_and_remove_else():
	var v := _view()
	var no_else := _if_stmt()
	no_else["else_present"] = false
	no_else["else"] = []
	v.show_section(_sec([no_else]), [])
	watch_signals(v)
	var blk: MusicStmtIfBlock = v.top_level_rows()[0]
	blk._add_else()
	# A new else seeds a default step (an empty else can't exist on disk).
	assert_signal_emitted_with_parameters(v, "replace_statement_requested",
		[0, 0, PackedStringArray([
			"if ((Var01 != 0))", "{", "    enter A", "}",
			"else", "{", "    enter A", "}"])])
	v.show_section(_sec([_if_stmt()]), [])
	blk = v.top_level_rows()[0]
	blk._remove_else()
	assert_signal_emitted_with_parameters(v, "replace_statement_requested",
		[0, 0, PackedStringArray(["if ((Var01 != 0))", "{", "    enter A", "}"])])


func test_non_flat_if_is_read_only():
	var v := _view()
	v.show_section(_sec([{
		"kind": "if", "code_offset": 0, "expr": "(Var01 != 0)", "text": "if ((Var01 != 0))",
		"else_present": false,
		"then": [{"kind": "switch", "code_offset": 4, "expr": "Var00", "action": "enter",
			"text": "on (Var00) enter A", "targets": [{"name": "A", "section": 0, "track": -1}]}],
		"else": [],
	}]), [])
	var blk: MusicStmtIfBlock = v.top_level_rows()[0]
	assert_false(blk.flat, "a nested block makes the if non-flat")
	assert_eq(blk._cond_chip, null, "condition is not editable")
	assert_eq(blk.then_rows().size(), 1, "the nested block renders as an annotation row")
	assert_true((blk.then_rows()[0] as StmtRowClass).read_only)


# ---- switch case editing on the block ----

func _switch_stmt() -> Dictionary:
	return {"kind": "switch", "code_offset": 0, "expr": "Var00", "action": "enter",
		"text": "on (Var00) enter A B",
		"targets": [
			{"name": "A", "section": 0, "track": -1},
			{"name": "B", "section": 1, "track": -1},
		]}


func test_case_retarget_add_remove():
	var v := _view()
	v.show_section(_sec([_switch_stmt()]), [])
	watch_signals(v)
	var blk: MusicStmtSwitchBlock = v.top_level_rows()[0]
	blk._on_case_retarget(2, 1)
	assert_signal_emitted_with_parameters(v, "replace_statement_requested",
		[0, 0, PackedStringArray(["on (Var00) enter A C"])])
	blk._on_add_case()
	assert_signal_emitted_with_parameters(v, "replace_statement_requested",
		[0, 0, PackedStringArray(["on (Var00) enter A B A"])])
	blk._on_case_remove(0)
	assert_signal_emitted_with_parameters(v, "replace_statement_requested",
		[0, 0, PackedStringArray(["on (Var00) enter B"])])


func test_action_change_keeps_or_resets_targets():
	var v := _view()
	v.show_section(_sec([_switch_stmt()]), [])
	watch_signals(v)
	var blk: MusicStmtSwitchBlock = v.top_level_rows()[0]
	# enter -> goto keeps the section targets.
	blk._action_opt.select(1)
	blk._on_action_picked(1)
	assert_signal_emitted_with_parameters(v, "replace_statement_requested",
		[0, 0, PackedStringArray(["on (Var00) goto A B"])])
	# enter -> play can't map sections onto tracks: reset to one default.
	blk._action_opt.select(2)
	blk._on_action_picked(2)
	assert_signal_emitted_with_parameters(v, "replace_statement_requested",
		[0, 0, PackedStringArray(["on (Var00) play sound_0"])])


func test_case_cap_disables_add_at_64():
	var v := _view()
	var targets := []
	for i in range(64):
		targets.append({"name": "A", "section": 0, "track": -1})
	v.show_section(_sec([{
		"kind": "switch", "code_offset": 0, "expr": "Var00", "action": "enter",
		"text": "on (Var00) enter ...", "targets": targets,
	}]), [])
	var blk: MusicStmtSwitchBlock = v.top_level_rows()[0]
	assert_true(blk._add_case.disabled, "the engine table caps at 64 cases")


# ---- ＋Add auto-open ----

func test_pending_add_opens_the_new_rows_editor():
	var v := _view()
	v.show_section(_sec([_enter(0, "A")], 0), [])
	watch_signals(v)
	v._add_kind_at(6, -1)  # "set a variable" -> default "Var00 = 0"
	assert_signal_emitted_with_parameters(v, "add_statement_requested",
		[0, PackedStringArray(["Var00 = 0"])])
	# The owner would apply + re-render; simulate the post-add section.
	v.show_section(_sec([
		{"kind": "assign", "code_offset": 0, "var_name": "Var00", "var_offset": 0,
			"is_local": false, "rhs": "0", "text": "Var00 = 0"},
		_enter(2, "A"),
	], 0), [])
	var row: Control = v.top_level_rows()[0]
	assert_true(row.has_meta("auto_opened"), "the new row is marked")
	assert_true(v._popover.is_open(), "an expression kind lands mid-edit in the popover")


# ---- document e2e (real gamemus pair) ----

func _doc() -> MusicEditorDocument:
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


func _wire(v: MusicSectionProgramView, doc) -> void:
	v.replace_statement_requested.connect(func(sec, o, lines): doc.replace_statement(sec, o, lines))
	v.delete_statement_requested.connect(func(sec, o): doc.delete_statement(sec, o))
	v.add_statement_requested.connect(func(sec, lines): doc.insert_statement(sec, lines))
	v.insert_statement_at_requested.connect(func(sec, o, lines): doc.insert_statement_at(sec, o, lines))


func test_e2e_lane_retarget_through_the_document():
	var doc := _doc()
	var sname := StringName(doc.mus_script.get_default_script_name())
	var snames := PackedStringArray()
	for n in doc.mus_script.get_section_names(sname):
		snames.append(String(n))
	var v := _view(true, snames)
	_wire(v, doc)
	var sec := _doc_section(doc, "Testmission")
	v.show_section(sec, [])
	var blk: MusicStmtIfBlock = v.top_level_rows()[0]
	assert_true(blk.flat, "the shipped if is flat-editable")
	var original := String((blk.then_rows()[0] as StmtRowClass)._stmt.get("target_name", ""))
	var win_idx := -1
	for i in range(snames.size()):
		if snames[i] == "Win000":
			win_idx = i
	assert_gt(win_idx, -1)
	_pick((blk.then_rows()[0] as StmtRowClass)._section_opt, win_idx)
	var after := _doc_section(doc, "Testmission")
	var then0: Dictionary = after.get("statements", [])[0].get("then", [])[0]
	assert_eq(String(then0.get("target_name", "")), "Win000",
		"the document really retargeted the then-branch")
	doc.undo()
	var restored := _doc_section(doc, "Testmission")
	var rthen0: Dictionary = restored.get("statements", [])[0].get("then", [])[0]
	assert_eq(String(rthen0.get("target_name", "")), original, "one undo restores it")


func test_lane_add_offers_simple_kinds_only():
	var v := _view()
	v.show_section(_sec([_if_stmt()]), [])
	var blk: MusicStmtIfBlock = v.top_level_rows()[0]
	for item in blk._lane_add_items:
		var k := String(item[1])
		assert_false(k == "if" or k == "switch",
			"bodies stay flat: no nested blocks in the lane palette")
	assert_gt(blk._lane_add_items.size(), 0, "the lane palette still offers the leaf kinds")


func test_e2e_delete_and_undo_through_the_document():
	var doc := _doc()
	var sname := StringName(doc.mus_script.get_default_script_name())
	var snames := PackedStringArray()
	for n in doc.mus_script.get_section_names(sname):
		snames.append(String(n))
	var v := _view(true, snames)
	_wire(v, doc)
	var original := doc.mus_script.get_decompiled_text(sname)
	var sec := _doc_section(doc, "Lose000")
	v.show_section(sec, [])
	# Delete the first deletable row through its own ✕.
	var target: Control = null
	for r in v.top_level_rows():
		var b := _find_button(r, "✕")
		if b != null:
			target = r
			b.pressed.emit()
			break
	assert_not_null(target, "found a deletable row")
	var after := _doc_section(doc, "Lose000")
	assert_lt((after.get("statements", []) as Array).size(),
		(sec.get("statements", []) as Array).size(), "the document lost the statement")
	doc.undo()
	assert_eq(doc.mus_script.get_decompiled_text(sname), original,
		"one undo restores the canonical text byte-identically")


func test_e2e_block_defaults_compile():
	var doc := _doc()
	var forms = MusForms.new()
	var sname := StringName(doc.mus_script.get_default_script_name())
	var snames := PackedStringArray()
	for n in doc.mus_script.get_section_names(sname):
		snames.append(String(n))
	forms.configure(snames, [], doc.mus_script, [])
	var sec := _doc_section(doc, "Win000")
	var sidx := int(sec.get("index", -1))
	var before := (sec.get("statements", []) as Array).size()
	assert_true(doc.insert_statement(sidx, forms.default_lines("if")),
		"the default if block passes the compile gate")
	assert_true(doc.insert_statement(sidx, forms.default_lines("switch")),
		"the default dispatch passes the compile gate")
	var after := _doc_section(doc, "Win000")
	var kinds := []
	for st in after.get("statements", []):
		kinds.append(String(st.get("kind", "")))
	assert_has(kinds, "if", "the if landed structurally")
	assert_has(kinds, "switch", "the dispatch landed structurally")
	doc.undo()
	doc.undo()
	assert_eq((_doc_section(doc, "Win000").get("statements", []) as Array).size(), before,
		"two undos restore the section")


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
