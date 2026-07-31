extends RefCounted

# Shared MUS authoring context: the "＋ Add" construct palette metadata, the
# picker builders (section / variable / track) every editing surface shares,
# and the canonical default lines ＋Add inserts for every kind -- creation
# never needs a dialog; everything edits in place on the program view's rows.
#
# Every producer routes through MusStmtText so the emitted .mus lines are
# canonical. The forms NEVER hand-assemble bytecode -- they only emit text the
# compile gate already accepts.
#
# No class_name (preload as a const) to avoid GDScript class-registration
# ordering surprises, matching mus_stmt_text.gd / mus_expr.gd.

const MusStmtText = preload("res://modtools/music/mus_stmt_text.gd")

# The "＋ Add" palette: [label, kind]. nop is intentionally absent -- the
# decompiler renders nop as nothing, so an authored nop has no text line and
# would vanish on the next re-decompile (it couldn't then be deleted/reordered).
const ADD_ITEMS := [
	["♪  play a track", "play"],
	["→  go to a state", "transition"],
	["↪  jump to a label", "goto"],
	["ƒ  run a state and return", "call"],
	["⋔  choose by value …", "switch"],
	["◇  if / else", "if"],
	["✎  set a variable", "assign"],
	["±  adjust a variable (+1 / -1)", "incdec"],
	["ƒ  do an action (volume, flags, …)", "expr"],
	["⏎  return to caller", "return"],
	["⏸  wait", "yield"],
]

var _section_names: PackedStringArray = PackedStringArray()
var _var_list: Array = []        # [{token:String, label:String}]
var _mus = null                  # NovaMusicScript for expr validation
var _bank_names: Array = []


# Supply the context every form/picker needs: the section-name list, the
# variable picker list, the NovaMusicScript used to validate expressions, and
# the bank track names. Safe to call repeatedly; owners refresh it on every
# document change so pickers see current state.
func configure(section_names: PackedStringArray, var_list: Array, mus, bank_names: Array) -> void:
	_section_names = section_names
	_var_list = var_list
	_mus = mus
	_bank_names = bank_names


func section_names() -> PackedStringArray:
	return _section_names


func var_list() -> Array:
	return _var_list


func mus_script():
	return _mus


# True for the kinds that take no inputs (return/yield): the mount inserts their
# canonical lines directly, no editor.
func is_inputless(kind: String) -> bool:
	return kind == "return" or kind == "yield"


# Canonical lines for the inputless kinds; empty for anything that needs input.
func simple_lines(kind: String) -> PackedStringArray:
	match kind:
		"return":
			return MusStmtText.ret()
		"yield":
			return MusStmtText.yield_stmt()
	return PackedStringArray()


# Sensible, always-compilable default lines for EVERY kind ＋Add offers, so an
# add inserts immediately and the user edits the new row in place -- creation
# never needs a dialog. The if ships with a then-action (an empty body
# wouldn't survive the decompiler's if detection); if/switch seed Var00 so
# they are inert until edited.
func default_lines(kind: String) -> PackedStringArray:
	match kind:
		"play":
			return MusStmtText.play(0)
		"assign":
			return MusStmtText.assign("Var00", "0")
		"incdec":
			return MusStmtText.incdec("Var00", true)
		"expr":
			# A debug echo: visible in the event log, no effect on the music.
			return MusStmtText.expr_stmt("Echo(0)")
		"transition":
			if _section_names.is_empty():
				return PackedStringArray()
			return MusStmtText.enter(_section_names[0])
		"goto":
			if _section_names.is_empty():
				return PackedStringArray()
			return MusStmtText.goto_section(_section_names[0])
		"call":
			if _section_names.is_empty():
				return PackedStringArray()
			return MusStmtText.call_section(_section_names[0])
		"if":
			var then_body := PackedStringArray(["Echo(0)"])
			if not _section_names.is_empty():
				then_body = PackedStringArray(["enter %s" % _section_names[0]])
			return MusStmtText.if_block("(Var00 == 0)", then_body, false, PackedStringArray())
		"switch":
			if _section_names.is_empty():
				return PackedStringArray()
			return MusStmtText.switch_stmt("Var00", "enter", PackedStringArray([_section_names[0]]))
	return PackedStringArray()


# ---- shared picker builders (every editing surface uses these) -------------

func make_section_option(selected_name: String) -> OptionButton:
	var ob := OptionButton.new()
	for i in range(_section_names.size()):
		ob.add_item(_section_names[i])
		if _section_names[i] == selected_name:
			ob.select(ob.item_count - 1)
	return ob


func make_var_option(selected_token: String, selected_offset: int = -1) -> OptionButton:
	var ob := OptionButton.new()
	var picked := -1
	for i in range(_var_list.size()):
		ob.add_item(String(_var_list[i].get("label", _var_list[i].get("token", "Var00"))))
		if String(_var_list[i].get("token", "")) == selected_token:
			picked = i
	if _var_list.is_empty():
		ob.add_item("Var00")
	# Fall back to byte-offset matching when the resolved name didn't match a token:
	# a script with editor-named globals reports its custom name (e.g. "Intensity"),
	# not "VarNN", so a name-only match would silently default to Var00 on edit.
	# 64 is the last 4-byte-aligned global offset (Var16, MUS_GLOBALS_BYTES 68).
	if picked < 0 and selected_offset >= 0 and selected_offset <= 64:
		var idx := selected_offset / 4
		if idx < _var_list.size():
			picked = idx
	if picked >= 0:
		ob.select(picked)
	return ob


func make_track_option(selected_track: int) -> OptionButton:
	var ob := OptionButton.new()
	# Cap at 256: the play opcode operand is a single byte, so tracks >= 256 are
	# unplayable by the original engine and must not be offered.
	for i in range(mini(_bank_names.size(), 256)):
		ob.add_item("%d: %s" % [i, _track_name(i)])
	if _bank_names.is_empty():
		# No bank loaded: still let the user pick a slot index by number.
		for i in range(8):
			ob.add_item("track %d" % i)
	if selected_track >= 0 and selected_track < ob.item_count:
		ob.select(selected_track)
	return ob


# Resolve the picked variable's token against the list the editor OPENED with
# (passed in, not read from the instance) so a background configure() can't
# shift the index->token mapping out from under an open editor.
func var_token_at(ob: OptionButton, var_list_snapshot: Array) -> String:
	var idx := ob.selected
	if idx >= 0 and idx < var_list_snapshot.size():
		return String(var_list_snapshot[idx].get("token", "Var00"))
	return "Var00"


# Display fallback for an in-bank entry with no name; the picker rows already
# show the index, so "(unnamed)" reads better than the raw sound_N token.
func _track_name(track: int) -> String:
	if track >= 0 and track < _bank_names.size() and String(_bank_names[track]) != "":
		return String(_bank_names[track])
	return "(unnamed)"
