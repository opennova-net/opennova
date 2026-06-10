extends GutTest

# The program view's positional document ops: insert_statement_at (gap carets),
# move_statement (drag-reorder), set_run_count (the xN badge). All are single
# text transforms through the same compile gate as the Phase-2 ops, so each is
# exactly ONE undo entry and rolls back whole on a refused edit.

const MusicEditorDocument = preload("res://modtools/music/music_editor_document.gd")
const MusStmtText = preload("res://modtools/music/mus_stmt_text.gd")
const BANK_FIXTURE := "res://../fixtures/sbf/jo_gamemus.sbf"
const SCRIPT_FIXTURE := "res://../fixtures/mus/jo_gamemus.bin"
const PAIR_BANK := "user://music_move_pair.sbf"
const PAIR_SCRIPT := "user://music_move_pair.bin"


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


func _section(doc, name: String) -> Dictionary:
	for s in doc.mus_script.get_program_ast(_sname(doc)):
		if String(s.get("name", "")) == name:
			return s
	return {}


func _text(doc) -> String:
	return doc.mus_script.get_decompiled_text(_sname(doc))


# Ordinals (raw AST indices) of every statement of `kind` in the section.
func _ordinals(doc, section: String, kind: String) -> Array:
	var out: Array = []
	var stmts: Array = _section(doc, section).get("statements", [])
	for i in stmts.size():
		if String(stmts[i].get("kind", "")) == kind:
			out.append(i)
	return out


# The section's play tracks in statement order.
func _play_tracks(doc, section: String) -> Array:
	var out: Array = []
	for st in _section(doc, section).get("statements", []):
		if String(st.get("kind", "")) == "play":
			out.append(int(st.get("track", -1)))
	return out


# ---- insert_statement_at ----

func test_insert_at_lands_in_the_named_gap():
	var doc := _doc()
	var sidx := int(_section(doc, "Win000").get("index", -1))
	var plays := _ordinals(doc, "Win000", "play")
	assert_gt(plays.size(), 2, "Win000 has a play run")
	# Drop a new play into the gap above the SECOND play.
	assert_true(doc.insert_statement_at(sidx, plays[1], MusStmtText.play(9)),
		"insert into a specific gap succeeds")
	var tracks := _play_tracks(doc, "Win000")
	assert_eq(tracks[1], 9, "the new play sits exactly where the caret was")
	doc.undo()
	assert_false(tracks == _play_tracks(doc, "Win000"), "undo removed it")


func test_insert_at_minus_one_is_the_canonical_anchor():
	var doc := _doc()
	var before := _text(doc)
	var sidx := int(_section(doc, "Win000").get("index", -1))
	assert_true(doc.insert_statement_at(sidx, -1, MusStmtText.play(9)), "fallback insert succeeds")
	var via_at := _text(doc)
	doc.undo()
	assert_true(doc.insert_statement(sidx, MusStmtText.play(9)), "plain insert for comparison")
	assert_eq(_text(doc), via_at, "-1 routes to the same anchor as insert_statement")
	doc.undo()
	assert_eq(_text(doc), before, "undo is byte-clean")


func test_insert_at_refuses_past_the_terminator():
	var doc := _doc()
	var sidx := int(_section(doc, "Win000").get("index", -1))
	var dones := _ordinals(doc, "Win000", "done")
	assert_eq(dones.size(), 1, "one section close")
	# The gap between `enter Missionnull` and `}` is dead code: refused.
	assert_false(doc.insert_statement_at(sidx, dones[0], MusStmtText.play(9)),
		"no inserting after the flow has left the state")


func test_insert_at_refuses_the_dispatch_tail():
	var doc := _doc()
	var sidx := int(_section(doc, "Begin").get("index", -1))
	var switches := _ordinals(doc, "Begin", "switch")
	assert_eq(switches.size(), 1, "Begin carries the event switch in its tail")
	assert_false(doc.insert_statement_at(sidx, switches[0], MusStmtText.play(9)),
		"the engine-dispatch tail accepts no inserts")


# ---- move_statement ----

func test_move_down_and_back_is_one_undo_each():
	var doc := _doc()
	var original := _text(doc)
	var sidx := int(_section(doc, "Win000").get("index", -1))
	var plays := _ordinals(doc, "Win000", "play")
	var before_tracks := _play_tracks(doc, "Win000")
	# Drag the first play into the gap above the fourth.
	assert_true(doc.move_statement(sidx, plays[0], plays[3]), "move succeeds")
	var after_tracks := _play_tracks(doc, "Win000")
	assert_eq(after_tracks[2], before_tracks[0], "the row landed above the old fourth play")
	assert_eq(after_tracks[0], before_tracks[1], "the rest shifted up")
	doc.undo()
	assert_eq(_text(doc), original, "ONE undo restores the move byte-identically")
	assert_false(doc.can_undo(), "the move was exactly one history entry")


func test_move_to_minus_one_appends_at_the_anchor():
	var doc := _doc()
	var sidx := int(_section(doc, "Win000").get("index", -1))
	var plays := _ordinals(doc, "Win000", "play")
	var first_track: int = _play_tracks(doc, "Win000")[0]
	assert_true(doc.move_statement(sidx, plays[0], -1), "move-to-end succeeds")
	var tracks := _play_tracks(doc, "Win000")
	assert_eq(tracks[tracks.size() - 1], first_track, "the row now plays last")


func test_move_refusals():
	var doc := _doc()
	var win := int(_section(doc, "Win000").get("index", -1))
	var begin := int(_section(doc, "Begin").get("index", -1))
	var plays := _ordinals(doc, "Win000", "play")
	var dones := _ordinals(doc, "Win000", "done")
	assert_false(doc.move_statement(win, dones[0], plays[0]), "the section close never moves")
	assert_false(doc.move_statement(win, plays[0], dones[0]), "nothing moves past the terminator")
	assert_false(doc.move_statement(win, plays[0], plays[0]), "self-move is a no-op")
	assert_false(doc.move_statement(win, plays[0], plays[1]),
		"moving into the gap it already fills is a no-op")
	var tail_exprs := _ordinals(doc, "Begin", "expr")
	assert_gt(tail_exprs.size(), 1, "Begin has the FB() tail call")
	assert_false(doc.move_statement(begin, tail_exprs[-1], _ordinals(doc, "Begin", "expr")[0]),
		"dispatch-tail rows don't move")
	assert_false(doc.can_undo(), "every refusal left history untouched")


# ---- set_run_count ----

func test_run_shrink_and_grow():
	var doc := _doc()
	var original := _text(doc)
	var sidx := int(_section(doc, "Multiplayerstart").get("index", -1))
	var plays := _ordinals(doc, "Multiplayerstart", "play")
	var tracks := _play_tracks(doc, "Multiplayerstart")
	# The shipped script loops the same track many times before the closer.
	var run_len := 0
	for t in tracks:
		if t == tracks[0]:
			run_len += 1
		else:
			break
	assert_gt(run_len, 10, "Multiplayerstart opens with a long identical run")
	assert_true(doc.set_run_count(sidx, plays[0], run_len, 3), "shrink lands in one op")
	assert_eq(_play_tracks(doc, "Multiplayerstart").size(), tracks.size() - run_len + 3,
		"the run is now x3")
	doc.undo()
	assert_eq(_text(doc), original, "ONE undo restores the whole shrink")
	assert_false(doc.can_undo(), "the shrink was exactly one history entry")
	assert_true(doc.set_run_count(sidx, plays[0], run_len, run_len + 4), "grow lands in one op")
	assert_eq(_play_tracks(doc, "Multiplayerstart").size(), tracks.size() + 4, "the run grew by 4")


func test_run_count_refusals():
	var doc := _doc()
	var sidx := int(_section(doc, "Multiplayerstart").get("index", -1))
	var plays := _ordinals(doc, "Multiplayerstart", "play")
	var tracks := _play_tracks(doc, "Multiplayerstart")
	assert_false(doc.set_run_count(sidx, plays[0], 5, 5), "same count is a no-op")
	assert_false(doc.set_run_count(sidx, plays[0], 5, 0), "a run can't drop below one row")
	# Starting on the LAST identical row and claiming 2 spans into a different
	# track's play: not an identical run.
	var run_len := 0
	for t in tracks:
		if t == tracks[0]:
			run_len += 1
		else:
			break
	assert_lt(run_len, plays.size(), "the run is followed by a different play")
	assert_false(doc.set_run_count(sidx, plays[run_len - 1], 2, 5),
		"a mixed span is not a resizable run")
	assert_false(doc.can_undo(), "every refusal left history untouched")


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
