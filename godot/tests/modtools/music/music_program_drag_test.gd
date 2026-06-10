extends GutTest

# Drag-to-reorder + positional drops on the block-stack canvas, and the ×N
# run-count stepper. Drops are driven through the rows' _can_drop_data /
# _drop_data (the same path Godot's DnD calls), so this runs headless.

const ProgramViewClass = preload("res://modtools/music/ui/section_program_view.gd")
const StmtRowClass = preload("res://modtools/music/ui/stmt_row.gd")
const IfBlockClass = preload("res://modtools/music/ui/stmt_if_block.gd")


func _view(editable := true) -> MusicSectionProgramView:
	var v: MusicSectionProgramView = ProgramViewClass.new()
	v.size = Vector2(700, 500)
	add_child_autofree(v)
	v.configure_authoring(PackedStringArray(["A", "B"]), [], null, [], editable)
	return v


func _sec(stmts: Array, index := 0) -> Dictionary:
	return {"name": "S", "index": index, "statements": stmts}


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


# ---- drag payloads ----

func test_drag_payload_shapes():
	var v := _view()
	v.show_section(_sec([_play(0, 0), _play(2, 1), _enter(4, "A")], 7), [])
	var rows := v.top_level_rows()
	var data = (rows[1] as StmtRowClass)._get_drag_data(Vector2.ZERO)
	assert_eq(String(data.get("kind", "")), "mus_stmt")
	assert_eq(int(data.get("ordinal", -1)), 1, "the payload carries the raw ordinal")


func test_read_only_and_folded_rows_do_not_drag():
	var v := _view(false)
	v.show_section(_sec([_play(0, 0), _enter(2, "A")]), [])
	assert_null((v.top_level_rows()[0] as StmtRowClass)._get_drag_data(Vector2.ZERO),
		"read-only rows don't drag")
	var v2 := _view(true)
	var stmts := []
	for i in range(4):
		stmts.append(_play(i * 2, 0))
	v2.show_section(_sec(stmts), [])
	assert_null((v2.top_level_rows()[0] as StmtRowClass)._get_drag_data(Vector2.ZERO),
		"a collapsed run moves as members after unfolding, not as a unit")


# ---- drop legality + intents ----

func test_row_drop_moves_above_the_target():
	var v := _view()
	v.show_section(_sec([_play(0, 0), _play(2, 1), _enter(4, "A")], 7), [])
	watch_signals(v)
	var rows := v.top_level_rows()
	var payload = (rows[0] as StmtRowClass)._get_drag_data(Vector2.ZERO)
	assert_false((rows[0] as StmtRowClass)._can_drop_data(Vector2.ZERO, payload),
		"a row is not its own drop target")
	assert_true((rows[2] as StmtRowClass)._can_drop_data(Vector2.ZERO, payload))
	(rows[2] as StmtRowClass)._drop_data(Vector2.ZERO, payload)
	assert_signal_emitted_with_parameters(v, "move_statement_requested", [7, 0, 2])


func test_tail_rows_accept_no_drops():
	var v := _view()
	v.show_section(_sec([
		_enter(0, "A"),
		{"kind": "done", "code_offset": 2, "text": "}"},
		{"kind": "expr", "code_offset": 4, "expr": "FB(0)", "text": "FB(0)",
			"has_call": true, "call_name": "GFB"},
	]), [])
	var rows := v.top_level_rows()
	var tail: StmtRowClass = rows[rows.size() - 1]
	assert_false(tail._can_drop_data(Vector2.ZERO, {"kind": "mus_stmt", "ordinal": 0}),
		"the engine-events tail accepts nothing")
	assert_false(tail._can_drop_data(Vector2.ZERO, {"kind": "mus_track", "index": 0}))


func test_track_drop_on_a_row_inserts_above_it():
	var v := _view()
	v.show_section(_sec([_play(0, 0), _enter(2, "A")], 7), [])
	watch_signals(v)
	var first: StmtRowClass = v.top_level_rows()[0]
	assert_true(first._can_drop_data(Vector2.ZERO, {"kind": "mus_track", "index": 3}))
	first._drop_data(Vector2.ZERO, {"kind": "mus_track", "index": 3})
	assert_signal_emitted_with_parameters(v, "insert_statement_at_requested",
		[7, 0, PackedStringArray(["play sound_3"])])


func test_lane_drag_stays_in_its_lane_and_regenerates():
	var v := _view()
	v.show_section(_sec([{
		"kind": "if", "code_offset": 0, "expr": "(Var01 != 0)", "text": "if ((Var01 != 0))",
		"else_present": true,
		"then": [_play(4, 0), _play(6, 1)],
		"else": [_play(8, 2)],
	}]), [])
	watch_signals(v)
	var blk: MusicStmtIfBlock = v.top_level_rows()[0]
	var then0: StmtRowClass = blk.then_rows()[0]
	var then1: StmtRowClass = blk.then_rows()[1]
	var else0: StmtRowClass = blk.else_rows()[0]
	var payload = then1._get_drag_data(Vector2.ZERO)
	assert_eq(String(payload.get("kind", "")), "mus_branch_stmt")
	assert_false(else0._can_drop_data(Vector2.ZERO, payload), "no cross-lane drops")
	assert_true(then0._can_drop_data(Vector2.ZERO, payload))
	then0._drop_data(Vector2.ZERO, payload)
	assert_signal_emitted_with_parameters(v, "replace_statement_requested",
		[0, 0, PackedStringArray([
			"if ((Var01 != 0))", "{", "    play sound_1", "    play sound_0", "}",
			"else", "{", "    play sound_2", "}"])])


# ---- the ×N run stepper ----

func test_run_stepper_requests_the_resize():
	var v := _view()
	var stmts := []
	for i in range(5):
		stmts.append(_play(i * 2, 0))
	stmts.append(_enter(10, "A"))
	v.show_section(_sec(stmts, 7), [])
	watch_signals(v)
	var folded: StmtRowClass = v.top_level_rows()[0]
	var badge := _find_button(folded, "×5")
	assert_not_null(badge, "the count badge is a button when resizable")
	badge.pressed.emit()
	assert_signal_emitted(v, "inline_edit_started", "opening the stepper pins follow-live")
	folded._run_spin.value = 2
	_find_button(folded, "✓").pressed.emit()
	assert_signal_emitted_with_parameters(v, "set_run_count_requested", [7, 0, 5, 2])


func test_run_stepper_cancel_is_quiet():
	var v := _view()
	var stmts := []
	for i in range(3):
		stmts.append(_play(i * 2, 0))
	v.show_section(_sec(stmts), [])
	var folded: StmtRowClass = v.top_level_rows()[0]
	_find_button(folded, "×3").pressed.emit()
	watch_signals(v)
	folded._run_spin.value = 9
	_find_button(folded, "✕").pressed.emit()
	assert_signal_not_emitted(v, "set_run_count_requested", "cancel keeps the count")
