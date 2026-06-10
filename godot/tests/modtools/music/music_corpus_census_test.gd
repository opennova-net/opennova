extends GutTest

# Census of the shipped MUS corpus (gamemus + menumus): pins the structural
# invariants the block-stack program view banks on, so a fixture or decompiler
# change that breaks a UI assumption fails HERE with a readable message instead
# of as a rendering glitch. Also prints a per-fixture histogram (kinds, ifs,
# switch widths, caller inputs, dispatch tails) -- the census of record for
# editor design decisions.
#
# Invariants pinned:
#  - every statement kind is one the row renderer knows;
#  - every if is FLAT (no if/switch/branch_comment nested in a body) and its
#    body statements are simple rows with canonical text (whole-if regeneration
#    rebuilds the block from those texts);
#  - frame_enter (hidden frame setup) only ever appears top-level, either
#    LEADING (caller inputs -> the Inputs card) or after the first terminal
#    (engine dispatch tail -> the divider explanation);
#  - switch stays within the engine's one-byte table (<= 64 targets) with a
#    known action, and every target resolves (section name or track index);
#  - caller inputs bank at byte 32 (l_32 = "Input 1") in both stock scripts.

const FIXTURES := [
	["res://../fixtures/mus/jo_gamemus.bin", "gamemus"],
	["res://../fixtures/mus/jo_menumus.bin", "menumus"],
]

const KNOWN_KINDS := ["play", "transition", "goto", "call", "return", "yield",
	"nop", "done", "assign", "incdec", "expr", "if", "switch",
	"branch_comment", "frame_enter"]
# Flow leaves the section here; anything after (besides done/nop) is the
# engine-dispatch tail the view renders behind the divider.
const TERMINAL := ["transition", "goto", "return"]

var _scripts: Array = []


func before_all() -> void:
	for f in FIXTURES:
		var bytes := FileAccess.get_file_as_bytes(f[0])
		assert_gt(bytes.size(), 0, "%s fixture readable" % f[1])
		var ms := NovaMusicScript.new()
		ms.load_from_decrypted_bytes(bytes, f[1])
		_scripts.append([f[1], ms])


func _ast(ms: NovaMusicScript) -> Array:
	return ms.get_program_ast(ms.get_default_script_name())


# The index of the first top-level statement after which flow has left the
# section: the first TERMINAL statement, or the section's `done`, whichever
# comes first. -1 = runs to its end.
func _authored_end(stmts: Array) -> int:
	for i in stmts.size():
		var k := String(stmts[i].get("kind", ""))
		if k in TERMINAL or k == "done":
			return i
	return -1


func test_corpus_census_and_invariants() -> void:
	for entry in _scripts:
		var label: String = entry[0]
		var ms: NovaMusicScript = entry[1]
		var ast := _ast(ms)
		assert_gt(ast.size(), 0, "%s has sections" % label)
		assert_eq(ms.get_locals_frame_offset(ms.get_default_script_name()), 32,
			"%s banks caller inputs at byte 32" % label)

		var kind_counts := {}
		var if_total := 0
		var switch_widths := []
		var tail_sections := []
		var input_sections := {}

		for sec in ast:
			var sname := String(sec.get("name", "?"))
			var stmts: Array = sec.get("statements", [])
			var end := _authored_end(stmts)
			var leading := true
			var inputs := 0
			var tail_kinds := []

			for i in stmts.size():
				var s: Dictionary = stmts[i]
				var k := String(s.get("kind", ""))
				kind_counts[k] = int(kind_counts.get(k, 0)) + 1
				assert_true(k in KNOWN_KINDS,
					"%s/%s stmt %d: kind '%s' is one the renderer knows" % [label, sname, i, k])

				if k == "frame_enter":
					# Leading run = caller inputs; after the authored end =
					# engine dispatch payload. Anywhere else breaks the card/
					# divider split the view renders.
					var in_tail := end >= 0 and i > end
					assert_true(leading or in_tail,
						"%s/%s: frame_enter at %d is leading or in the dispatch tail" % [label, sname, i])
					inputs += int(s.get("locals_count", 0))
				if k != "frame_enter":
					leading = false

				if end >= 0 and i > end and k != "done" and k != "nop":
					tail_kinds.append(k)

				if k == "if":
					if_total += 1
					for branch in ["then", "else"]:
						for st in s.get(branch, []):
							var bk := String(st.get("kind", ""))
							assert_false(bk in ["if", "switch", "branch_comment", "frame_enter", "done"],
								"%s/%s: if body holds only simple rows (got '%s')" % [label, sname, bk])
							assert_ne(String(st.get("text", "")), "",
								"%s/%s: if-body '%s' carries canonical text for regeneration" % [label, sname, bk])

				if k == "switch":
					var targets: Array = s.get("targets", [])
					switch_widths.append(targets.size())
					assert_between(targets.size(), 1, 64,
						"%s/%s: switch table fits the one-byte count" % [label, sname])
					var action := String(s.get("action", ""))
					assert_true(action in ["enter", "goto", "play"],
						"%s/%s: switch action '%s' is renderable" % [label, sname, action])
					for t in targets:
						if action == "play":
							assert_gte(int(t.get("track", -1)), 0,
								"%s/%s: play-switch target is a track" % [label, sname])
						else:
							assert_ne(String(t.get("name", "")), "",
								"%s/%s: %s-switch target resolves to a section" % [label, sname, action])

			if not tail_kinds.is_empty():
				tail_sections.append("%s after #%d: %s" % [sname, end, ", ".join(tail_kinds)])
			if inputs > 0:
				input_sections[sname] = inputs

		var hist := []
		var keys := kind_counts.keys()
		keys.sort()
		for k in keys:
			hist.append("%s=%d" % [k, kind_counts[k]])
		gut.p("[census] %s: %d sections; %s" % [label, ast.size(), ", ".join(hist)])
		gut.p("[census] %s: ifs=%d, switch widths=%s, inputs=%s" % [
			label, if_total, str(switch_widths), str(input_sections)])
		for t in tail_sections:
			gut.p("[census] %s dispatch tail: %s" % [label, t])


func test_gamemus_begin_tail_shape() -> void:
	# The one shipped dispatch tail the divider design was drawn from: Begin's
	# main loop leaks past `enter Testmission` as frame_enter (the engine hands
	# a value -> l_32), an FB() call, and the 3-way event switch.
	var ms: NovaMusicScript = _scripts[0][1]
	var begin: Dictionary = {}
	for sec in _ast(ms):
		if String(sec.get("name", "")) == "Begin":
			begin = sec
	assert_false(begin.is_empty(), "gamemus has Begin")
	var stmts: Array = begin.get("statements", [])
	var end := _authored_end(stmts)
	assert_eq(String(stmts[end].get("kind", "")), "transition",
		"Begin's authored flow ends at enter Testmission")
	var tail := []
	for i in range(end + 1, stmts.size()):
		var k := String(stmts[i].get("kind", ""))
		if k != "done" and k != "nop":
			tail.append(k)
	assert_eq(tail, ["frame_enter", "expr", "switch"],
		"Begin's engine tail = frame setup, FB(), event switch")
	# The tail's frame_enter is what makes Begin "take inputs" -- the divider
	# (not the caller card) must explain it. It banks TWO dwords (l_32 + l_36);
	# the event switch reads the first (Input 1).
	var fe: Dictionary = stmts[end + 2] if stmts.size() > end + 2 else {}
	assert_eq(String(fe.get("kind", "")), "frame_enter", "frame setup right after done")
	assert_eq(int(fe.get("locals_count", 0)), 2, "the game hands Begin two values")
