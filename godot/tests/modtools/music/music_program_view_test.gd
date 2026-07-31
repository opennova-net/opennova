extends GutTest

# The block-stack program view (MusicSectionProgramView): one state's program
# as a vertical list of sentence rows, if/switch as container blocks with
# lanes, folded runs, the inputs card, the engine-events divider, and the
# live-glow registry (including hidden frame/done offset carry). Read-side
# topology -- the editing behaviours live in music_program_edit_test.gd.

const ProgramViewClass = preload("res://modtools/music/ui/section_program_view.gd")
const StmtRowClass = preload("res://modtools/music/ui/stmt_row.gd")
const IfBlockClass = preload("res://modtools/music/ui/stmt_if_block.gd")
const SwitchBlockClass = preload("res://modtools/music/ui/stmt_switch_block.gd")
const SCRIPT_FIXTURE := "res://../fixtures/mus/jo_gamemus.bin"

const ACTIVE := Color(0.55, 1.00, 0.55)


func _view() -> MusicSectionProgramView:
	var v: MusicSectionProgramView = ProgramViewClass.new()
	v.size = Vector2(600, 400)
	add_child_autofree(v)
	return v


func _sec(stmts: Array, index := 0, name := "S") -> Dictionary:
	return {"name": name, "index": index, "statements": stmts}


func _play(off: int, track: int) -> Dictionary:
	return {"kind": "play", "code_offset": off, "track": track, "text": "play sound_%d" % track}


func _row_kinds(v: MusicSectionProgramView) -> Array:
	var out: Array = []
	for r in v.top_level_rows():
		out.append(String(r.get_meta("kind")))
	return out


func _find_button(node: Node, text: String) -> Button:
	if node is Button and (node as Button).text == text:
		return node
	for c in node.get_children():
		var b := _find_button(c, text)
		if b != null:
			return b
	return null


# ---- plain chains ----

func test_chain_renders_one_row_per_statement_in_order():
	var v := _view()
	v.show_section(_sec([
		_play(0, 0),
		{"kind": "assign", "code_offset": 2, "text": "Var01 = 5", "var_name": "Var01"},
		{"kind": "transition", "code_offset": 4, "target_name": "Next", "target_section": 1,
			"text": "enter Next"},
		{"kind": "done", "code_offset": 6, "text": "}"},
	]), ["Boom"])
	assert_eq(_row_kinds(v), ["play", "assign", "transition"],
		"rows in program order; the section close never renders")
	var rows := v.top_level_rows()
	for i in rows.size():
		assert_eq(int(rows[i].get_meta("ordinal")), i, "rows carry raw AST ordinals")
	assert_null(v.dispatch_divider(), "no tail, no divider")
	assert_null(v.inputs_card(), "no inputs, no card")
	assert_null(v.empty_hint(), "not empty")


func test_track_sentence_uses_bank_names():
	var v := _view()
	v.show_section(_sec([_play(0, 0), _play(2, 7)]), ["Helicopter loop"])
	var rows := v.top_level_rows()
	var s0: String = (rows[0] as StmtRowClass)._sentence.text
	var s1: String = (rows[1] as StmtRowClass)._sentence.text
	assert_string_contains(s0, "Helicopter loop", "named bank entry reads by name")
	assert_string_contains(s1, "track 7", "unnamed track falls back to its number")
	assert_false(s0.contains("sound_0"), "the canonical token stays out of the sentence")


# ---- folding ----

func test_identical_run_folds_to_one_row_and_unfolds():
	var v := _view()
	var stmts := []
	for i in range(5):
		stmts.append(_play(i * 2, 0))
	stmts.append(_play(10, 1))
	v.show_section(_sec(stmts), [])
	var rows := v.top_level_rows()
	assert_eq(rows.size(), 2, "5 identical plays fold into one row (+ the odd one)")
	assert_eq(int(rows[0].get_meta("run")), 5, "the folded row knows its run length")
	var unfold := _find_button(rows[0], "⊞ unfold")
	assert_not_null(unfold, "a folded run offers ⊞")
	unfold.pressed.emit()
	rows = v.top_level_rows()
	assert_eq(rows.size(), 6, "unfolded members render individually")
	assert_eq(int(rows[2].get_meta("ordinal")), 2, "each member owns its raw ordinal")
	assert_not_null(_find_button(rows[0], "⊟ fold"), "the first member offers ⊟")
	# The expansion survives a re-render of the SAME section...
	v.show_section(_sec(stmts), [])
	assert_eq(v.top_level_rows().size(), 6, "unfold persists across a re-render")
	# ...but not a different one.
	v.show_section(_sec(stmts, 3, "Other"), [])
	assert_eq(v.top_level_rows().size(), 2, "a different state starts folded")


# ---- if / switch blocks ----

func test_if_block_lanes():
	var v := _view()
	v.show_section(_sec([
		{"kind": "if", "code_offset": 0, "expr": "((Var01 != 0))", "text": "if ((Var01 != 0))",
			"else_present": true,
			"then": [{"kind": "transition", "code_offset": 4, "target_name": "A",
				"target_section": 1, "text": "enter A"}],
			"else": [_play(8, 0), _play(10, 0)]},
	]), [])
	var rows := v.top_level_rows()
	assert_eq(rows.size(), 1, "the whole if is one block row")
	var blk: MusicStmtIfBlock = rows[0]
	assert_string_contains(blk._header.text, "Var01 != 0", "condition reads in the header")
	assert_false(blk._header.text.contains("(("), "self-parens are unwrapped for display")
	assert_eq(blk.then_rows().size(), 1, "then lane holds its body")
	assert_eq(blk.else_rows().size(), 2, "else lane holds its body")
	var leaf: Control = blk.then_rows()[0]
	assert_eq(int(leaf.get_meta("ordinal")), 0, "lane rows share the block's ordinal")
	assert_eq(String(leaf.get_meta("branch_key")), "0:then:0", "lane rows carry their branch address")


func test_switch_block_case_rows():
	var v := _view()
	v.show_section(_sec([
		{"kind": "switch", "code_offset": 0, "expr": "(Var00)", "action": "enter",
			"text": "on (Var00) enter A B A",
			"targets": [
				{"name": "A", "section": 1, "track": -1},
				{"name": "B", "section": 2, "track": -1},
				{"name": "A", "section": 1, "track": -1},
			]},
	]), [])
	var rows := v.top_level_rows()
	assert_eq(rows.size(), 1, "the dispatch is one block row")
	var blk: MusicStmtSwitchBlock = rows[0]
	var cases := blk.case_rows()
	assert_eq(cases.size(), 3, "one row per table entry")
	assert_eq(int(cases[2].get_meta("case_index")), 2, "cases are indexed")
	assert_not_null(_find_button(cases[0], "open ▸"), "section cases jump to their state")


func test_play_action_switch_shows_track_names():
	var v := _view()
	v.show_section(_sec([
		{"kind": "switch", "code_offset": 0, "expr": "(Var00)", "action": "play",
			"text": "on (Var00) play sound_0 sound_1",
			"targets": [
				{"name": "", "section": -1, "track": 0},
				{"name": "", "section": -1, "track": 1},
			]},
	]), ["Drums"])
	var blk: MusicStmtSwitchBlock = v.top_level_rows()[0]
	var first_label: Label = null
	for c in (blk.case_rows()[0] as Container).get_children():
		if c is Label and not (c as Label).text.contains("→"):
			first_label = c
	assert_not_null(first_label)
	assert_eq(first_label.text, "Drums", "play cases read by bank name")
	assert_null(_find_button(blk.case_rows()[0], "open ▸"), "track cases have nothing to open")


# ---- hidden rows: frame setup, done, offset carry ----

func test_leading_frame_enter_becomes_the_inputs_card():
	var v := _view()
	v.show_section(_sec([
		{"kind": "frame_enter", "code_offset": 0, "locals_count": 2, "text": "enter S"},
		_play(2, 0),
		{"kind": "done", "code_offset": 4, "text": "}"},
	]), [])
	assert_eq(_row_kinds(v), ["play"], "the frame op never renders as a row")
	var card := v.inputs_card()
	assert_not_null(card, "leading frame setup -> Inputs from caller card")
	assert_eq((card as MusicInputsCard).input_rows().size(), 2, "one row per banked value")
	assert_eq(v.section_inputs_count(), 2, "the owner still reads the total")
	# While the pc sits on the hidden frame op, the first real row glows.
	v.set_active_offset(0)
	assert_eq((v.top_level_rows()[0] as Control).modulate, ACTIVE,
		"hidden offsets carry to the next rendered row")


func test_done_offset_keeps_the_last_row_lit():
	var v := _view()
	v.show_section(_sec([
		_play(0, 0),
		_play(2, 1),
		{"kind": "done", "code_offset": 4, "text": "}"},
	]), [])
	v.set_active_offset(4)
	var rows := v.top_level_rows()
	assert_eq((rows[1] as Control).modulate, ACTIVE,
		"the program's end keeps the last step lit")
	v.set_active_offset(-1)
	assert_eq((rows[1] as Control).modulate, Color(1, 1, 1), "clearing unlights it")


func test_glow_brackets_to_greatest_offset_at_or_before_pc():
	var v := _view()
	v.show_section(_sec([_play(0, 0), _play(6, 1)]), [])
	v.set_active_offset(3)
	var rows := v.top_level_rows()
	assert_eq((rows[0] as Control).modulate, ACTIVE, "pc between rows lights the one before it")
	v.set_active_offset(6)
	assert_eq((rows[1] as Control).modulate, ACTIVE)
	assert_eq((rows[0] as Control).modulate, Color(1, 1, 1), "only one row glows")


# ---- the engine-events tail (real gamemus Begin) ----

func _load_gamemus() -> NovaMusicScript:
	var bytes := FileAccess.get_file_as_bytes(SCRIPT_FIXTURE)
	assert_gt(bytes.size(), 0, "fixture readable")
	var ms := NovaMusicScript.new()
	ms.load_from_decrypted_bytes(bytes, "gamemus")
	return ms


func test_begin_tail_renders_behind_the_divider():
	var ms := _load_gamemus()
	var begin: Dictionary = {}
	for sec in ms.get_program_ast(ms.get_default_script_name()):
		if String(sec.get("name", "")) == "Begin":
			begin = sec
	assert_false(begin.is_empty())
	var v := _view()
	v.configure_authoring(PackedStringArray(), [], ms, [], false)
	v.show_section(begin, [])
	var divider := v.dispatch_divider()
	assert_not_null(divider, "Begin's leaked main loop sits behind a divider")
	assert_null(v.inputs_card(), "a TAIL frame op is engine payload, not a caller card")
	assert_eq(v.section_inputs_count(), 2, "the owner still reads Begin's input total")
	# The divider explains the engine-handed values in words.
	var explain := ""
	for c in divider.get_child(0).get_children():
		if c is Label:
			explain += (c as Label).text + "\n"
	assert_string_contains(explain, "Input 1", "the payload slots are named")
	# Rows after the divider are the tail (FB() then the event switch), and the
	# tail switch reads its selector as a named input, not l_32.
	var rows := v.top_level_rows()
	var divider_pos := divider.get_index()
	var tail_kinds: Array = []
	for r in rows:
		if r.get_index() > divider_pos:
			tail_kinds.append(String(r.get_meta("kind")))
	assert_eq(tail_kinds, ["expr", "switch"], "the tail renders FB() and the event dispatch")
	var sw: MusicStmtSwitchBlock = rows[rows.size() - 1]
	var head_text := ""
	for c in (sw.get_child(0) as Container).get_child(0).get_children():
		if c is Label and (c as Label).text.contains("Choose by"):
			head_text = (c as Label).text
	assert_string_contains(head_text, "Input 1", "the selector reads by input name")
	assert_false(head_text.contains("l_32"), "no raw engine token leaks")
	# The whole tail glows like any executed code: the pc on the switch lights it.
	var sw_off := -1
	for st in begin.get("statements", []):
		if String(st.get("kind", "")) == "switch":
			sw_off = int(st.get("code_offset", -1))
	v.set_active_offset(sw_off)
	assert_eq(sw.modulate, ACTIVE, "tail rows stay live-lit")


# ---- empty state ----

func test_caller_inputs_are_pickable_variables():
	# Input slots join the variable list for pickers and expressions, named,
	# with the canonical engine token beside them.
	var v := _view()
	v.configure_authoring(PackedStringArray(), [{"token": "Var00", "label": "Var00"}], null, [], false)
	v.show_section(_sec([
		{"kind": "frame_enter", "code_offset": 0, "locals_count": 2, "text": "enter S"},
		_play(2, 0),
	]), [])
	var labels: Array = []
	for entry in v._display_var_list():
		labels.append(String(entry.get("label", "")))
	assert_has(labels, "Input 1 (l_32)", "the 1st caller input is pickable")
	assert_has(labels, "Input 2 (l_36)", "the 2nd caller input is pickable")


func test_folded_run_carries_no_tools_until_unfolded():
	var v := _view()
	v.configure_authoring(PackedStringArray(["A"]), [], null, [], true)
	var stmts := []
	for i in range(4):
		stmts.append(_play(i * 2, 0))
	v.show_section(_sec(stmts), [])
	var folded: Control = v.top_level_rows()[0]
	assert_null(_find_button_tipped(folded, "Delete this step"),
		"deleting a collapsed run would act on one hidden member: unfold first")
	_find_button(folded, "⊞ unfold").pressed.emit()
	assert_not_null(_find_button_tipped(v.top_level_rows()[1], "Delete this step"),
		"unfolded members carry their own tools")


func _find_button_tipped(node: Node, tip: String) -> Button:
	if node is Button and (node as Button).tooltip_text == tip:
		return node
	for c in node.get_children():
		var b := _find_button_tipped(c, tip)
		if b != null:
			return b
	return null


func test_empty_callable_shows_hint_and_card():
	var v := _view()
	v.show_section(_sec([
		{"kind": "frame_enter", "code_offset": 0, "locals_count": 1, "text": "enter S"},
		{"kind": "done", "code_offset": 2, "text": "}"},
	]), [])
	assert_not_null(v.empty_hint(), "frame+done is an empty callable -> hint")
	assert_not_null(v.inputs_card(), "...that still declares its inputs")
	assert_eq(v.top_level_rows().size(), 0, "nothing else renders")


func test_read_only_add_is_disabled_with_the_reason():
	var v := _view()
	v.configure_authoring(PackedStringArray(), [], null, [], false, "Fix script errors first")
	v.show_section(_sec([]), [])
	assert_true(v.add_menu_button().disabled, "read-only: no adding")
	assert_eq(v.add_menu_button().tooltip_text, "Fix script errors first",
		"the tooltip says WHY")
	var hint_add := _find_button(v.empty_hint(), "＋ Add step")
	assert_not_null(hint_add)
	assert_true(hint_add.disabled)
	assert_eq(hint_add.tooltip_text, "Fix script errors first")


# ---- the inputs story (naming, call chips) ----

const INPUTS_PROFILE := "user://music_progview_profile.json"


class StubMus:
	extends RefCounted

	func get_default_script_name() -> StringName:
		return &"gamescript"

	func get_locals_frame_offset(_n) -> int:
		return 32


func _rm_profile() -> void:
	var abs := ProjectSettings.globalize_path(INPUTS_PROFILE)
	if FileAccess.file_exists(abs):
		DirAccess.remove_absolute(abs)


func test_named_inputs_reach_card_pickers_and_sentences():
	_rm_profile()
	MusInputNames.set_input_label(INPUTS_PROFILE, "gamescript", "S", 0, "Mission event")
	var v := _view()
	v.configure_authoring(PackedStringArray(["S"]), [], StubMus.new(), [], true, "", INPUTS_PROFILE)
	v.show_section(_sec([
		{"kind": "frame_enter", "code_offset": 0, "locals_count": 2, "text": "enter S"},
		{"kind": "switch", "code_offset": 2, "expr": "l_32", "action": "enter",
			"text": "on (l_32) enter S", "targets": [{"name": "S", "section": 0, "track": -1}]},
	], 0, "S"), [])
	# The card edits the name in place.
	var card := v.inputs_card()
	assert_not_null(card)
	var first_edit: LineEdit = null
	for c in (card as MusicInputsCard).input_rows()[0].get_children():
		if c is LineEdit:
			first_edit = c
	assert_not_null(first_edit, "editable card renders name fields")
	assert_eq(first_edit.text, "Mission event", "the sidecar name seeds the field")
	# The selector sentence reads by the name, and the slot stays pickable
	# under it.
	var blk: MusicStmtSwitchBlock = v.top_level_rows()[0]
	assert_string_contains(blk._selector_chip.text, "Mission event",
		"sentences read the named input")
	var labels: Array = []
	for entry in v._display_var_list():
		labels.append(String(entry.get("label", "")))
	assert_has(labels, "Mission event (l_32)", "the named slot is pickable")
	assert_has(labels, "Input 2 (l_36)", "unnamed slots keep the generic name")
	_rm_profile()


func test_card_rename_emits_through_the_view():
	var v := _view()
	v.configure_authoring(PackedStringArray(), [], null, [], true)
	v.show_section(_sec([
		{"kind": "frame_enter", "code_offset": 0, "locals_count": 1, "text": "enter S"},
		_play(2, 0),
	], 0, "Callee"), [])
	watch_signals(v)
	var card := v.inputs_card()
	var edit: LineEdit = null
	for c in (card as MusicInputsCard).input_rows()[0].get_children():
		if c is LineEdit:
			edit = c
	assert_not_null(edit)
	edit.text = "Outcome"
	edit.text_submitted.emit("Outcome")
	assert_signal_emitted_with_parameters(v, "input_renamed", ["Callee", 0, "Outcome"])


func test_call_rows_say_what_the_callee_takes():
	var v := _view()
	v.configure_authoring(PackedStringArray(["Sub"]), [], null, [], true, "", "", {"Sub": 2})
	v.show_section(_sec([
		{"kind": "call", "code_offset": 0, "target_name": "Sub", "target_section": 1,
			"text": "call Sub"},
		_play(2, 0),
	]), [])
	var row: Control = v.top_level_rows()[0]
	var hand: Label = null
	for c in row.get_child(0).get_children():
		if c is Label and (c as Label).text.begins_with("hands it"):
			hand = c
	assert_not_null(hand, "a call to an inputs-taking state carries the chip")
	assert_eq(hand.text, "hands it: Input 1, Input 2")
	# A call to a no-inputs state carries none.
	v.configure_authoring(PackedStringArray(["Sub"]), [], null, [], true, "", "", {})
	v.show_section(_sec([
		{"kind": "call", "code_offset": 0, "target_name": "Sub", "target_section": 1,
			"text": "call Sub"},
		_play(2, 0),
	]), [])
	var none: Label = null
	for c in (v.top_level_rows()[0] as Control).get_child(0).get_children():
		if c is Label and (c as Label).text.begins_with("hands it"):
			none = c
	assert_null(none, "no chip when the callee declares no inputs")


# ---- interaction signals ----

func test_row_click_selects_statement():
	var v := _view()
	v.show_section(_sec([_play(0, 0), _play(2, 1)], 4), [])
	watch_signals(v)
	var ev := InputEventMouseButton.new()
	ev.button_index = MOUSE_BUTTON_LEFT
	ev.pressed = true
	(v.top_level_rows()[1] as StmtRowClass)._on_gui_input(ev)
	assert_signal_emitted_with_parameters(v, "statement_selected", [4, 1])


func test_open_button_requests_the_target_state():
	var v := _view()
	v.show_section(_sec([
		{"kind": "transition", "code_offset": 0, "target_name": "Win000",
			"target_section": 3, "text": "enter Win000"},
	]), [])
	watch_signals(v)
	var open := _find_button(v.top_level_rows()[0], "open ▸")
	assert_not_null(open)
	open.pressed.emit()
	assert_signal_emitted_with_parameters(v, "open_section_requested", [&"Win000"])
