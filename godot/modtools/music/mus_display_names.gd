extends RefCounted

# Single source of truth for the artist-facing names in the music workspace:
# statement titles/colours/tooltips, friendly intrinsic-function names, and the
# display-only prettifier for canonical expression text. Everything the user
# READS routes through here; everything the compiler EATS stays canonical
# (MusStmtText / MusExpr emit the names-less form, untouched by this file).
# Original engine mnemonics live in tooltips only.
#
# No class_name (preload as a const), matching mus_stmt_text.gd / mus_expr.gd.

# Statement kinds (NovaMusicScript.get_program_ast `kind` strings) -> how they
# read on a program row. Glyphs stay: they make the canvas scannable and the
# map badges use the same family.
const STMT := {
	"play": {
		"title": "♪ Play track",
		"color": Color(0.60, 0.90, 0.60),
		"tip": "Play a track from the sound bank.",
	},
	"transition": {
		"title": "→ Go to state",
		"color": Color(0.55, 0.80, 1.00),
		"tip": "Leave this state and start another one. (engine op: enter / setstate 0x3B)",
	},
	"goto": {
		"title": "↪ Jump to label",
		"color": Color(0.55, 0.80, 1.00),
		"tip": "Jump straight to a label and keep running there. (engine op: goto 0x30)",
	},
	"call": {
		"title": "ƒ Run state and return",
		"color": Color(0.80, 0.65, 1.00),
		"tip": "Run another state, then come back here and continue. (engine op: callv / callvl)",
	},
	"return": {
		"title": "⏎ Return to caller",
		"color": Color(0.70, 0.70, 0.70),
		"tip": "Go back to wherever this state was run from. (engine op: return 0x39)",
	},
	"yield": {
		"title": "⏸ Wait",
		"color": Color(0.70, 0.70, 0.70),
		"tip": "Pause here until the engine's next music tick. (engine op: yield 0x3A)",
	},
	"nop": {
		"title": "· Do nothing",
		"color": Color(0.50, 0.50, 0.50),
		"tip": "Does nothing. (engine op: nop)",
	},
	"done": {
		"title": "▪ End",
		"color": Color(0.50, 0.50, 0.50),
		"tip": "The music program ends here. (engine op: done 0x3F)",
	},
	"assign": {
		"title": "✎ Set variable",
		"color": Color(1.00, 0.78, 0.40),
		"tip": "Store a value in a variable.",
	},
	"incdec": {
		"title": "± Adjust variable",
		"color": Color(1.00, 0.78, 0.40),
		"tip": "Add or subtract 1 from a variable. (engine op: inc/dec)",
	},
	"expr": {
		"title": "ƒ Do action",
		"color": Color(0.80, 0.65, 1.00),
		"tip": "Run a function (set the volume, set a flag, ...). (engine op: method 0x40)",
	},
	"if": {
		"title": "◇ If",
		"color": Color(1.00, 0.85, 0.45),
		"tip": "Run the then-branch when the condition is true, else the else-branch.",
	},
	"switch": {
		"title": "⋔ Choose by value",
		"color": Color(0.50, 0.85, 0.90),
		"tip": "Pick one target by a value: 0 picks the first, 1 the second, ... (engine op: tablexec 0x35)",
	},
	"branch_comment": {
		"title": "⌥ Branch (auto)",
		"color": Color(0.55, 0.55, 0.55),
		"tip": "A branch the decompiler couldn't shape into if/else; shown as-is.",
	},
}

# The 11 intrinsic functions (kIntrinsicNames in libs/mus mus_compile.cpp),
# keyed by stored name. `label` is what the user reads; the stored/surface
# mnemonic goes in the tooltip. Behaviour notes match the VM handlers
# (libs/mus/src/mus_vm.cpp init_intrinsics).
const INTRINSICS := {
	"GSV": {"label": "Set volume", "tip": "Set the music volume for both speakers. (engine name: GSV, surface: SV)"},
	"GSDV": {"label": "Set right-speaker volume", "tip": "Set the music volume for the right speaker only. (engine name: GSDV, surface: SDV)"},
	"GFB": {"label": "Feedback (no effect)", "tip": "Does nothing in Joint Ops; kept for script compatibility. (engine name: GFB, surface: FB)"},
	"GEcho": {"label": "Send debug message", "tip": "Print a number to the event log; has no effect on the music. (engine name: GEcho, surface: Echo)"},
	"GGRnd": {"label": "Random number", "tip": "A random number between two values. (engine name: GGRnd, surface: GRnd)"},
	"FSet": {"label": "Set flag", "tip": "Turn one of the 64 on/off flags on. (engine name: FSet, surface: F.Set)"},
	"FClear": {"label": "Clear flag", "tip": "Turn one of the 64 on/off flags off. (engine name: FClear, surface: F.Clear)"},
	"FIsSet": {"label": "Is flag on?", "tip": "True when that flag is on. (engine name: FIsSet, surface: F.IsSet)"},
	"FIsClear": {"label": "Is flag off?", "tip": "True when that flag is off. (engine name: FIsClear, surface: F.IsClear)"},
	"TStart": {"label": "Start timer (unused)", "tip": "Not wired up in Joint Ops; does nothing. (engine name: TStart, surface: T.Start)"},
	"TStop": {"label": "Stop timer (unused)", "tip": "Not wired up in Joint Ops; does nothing. (engine name: TStop, surface: T.Stop)"},
}

# Surface form -> friendly label, ordered longest-first so e.g. "SDV(" is
# rewritten before its suffix "SV(" could match. Built from INTRINSICS once.
const _SURFACE_REWRITES := [
	["F.IsClear(", "Is flag off?("],
	["F.IsSet(", "Is flag on?("],
	["F.Clear(", "Clear flag("],
	["F.Set(", "Set flag("],
	["T.Start(", "Start timer("],
	["T.Stop(", "Stop timer("],
	["Echo(", "Send debug message("],
	["GRnd(", "Random number("],
	["SDV(", "Set right-speaker volume("],
	["SV(", "Set volume("],
	["FB(", "Feedback("],
]


static func stmt_title(kind: String) -> String:
	var e: Dictionary = STMT.get(kind, {})
	return String(e.get("title", "• %s" % kind))


static func stmt_color(kind: String) -> Color:
	var e: Dictionary = STMT.get(kind, {})
	return e.get("color", Color(0.7, 0.7, 0.7))


static func stmt_tooltip(kind: String) -> String:
	var e: Dictionary = STMT.get(kind, {})
	return String(e.get("tip", ""))


static func intrinsic_label(stored: String) -> String:
	var e: Dictionary = INTRINSICS.get(stored, {})
	return String(e.get("label", stored))


static func intrinsic_tooltip(stored: String) -> String:
	var e: Dictionary = INTRINSICS.get(stored, {})
	return String(e.get("tip", ""))


# --- caller inputs (l_N locals at/above the frame base) ---------------------

# The default `enter` frame base (byte offset where 0x38 banks the caller's
# arguments); every stock script uses 0x20. NovaMusicScript.get_locals_frame_offset
# supplies the per-script value.
const DEFAULT_LOCALS_BASE := 32

static var _local_re: RegEx = null


# "Input K" for the local token at byte offset `off` when it sits on the frame's
# 4-byte grid, else "" (a raw l_N below the base is not a caller input).
static func input_label_for_offset(off: int, locals_base: int = DEFAULT_LOCALS_BASE) -> String:
	if off < locals_base or (off - locals_base) % 4 != 0:
		return ""
	return "Input %d" % ((off - locals_base) / 4 + 1)


# The canonical token for the state's (k+1)-th caller input (k 0-based).
static func input_token(k: int, locals_base: int = DEFAULT_LOCALS_BASE) -> String:
	return "l_%d" % (locals_base + 4 * k)


# DISPLAY-ONLY prettifier for canonical expression/statement text: swaps the
# intrinsic surface forms for their friendly labels, caller-input locals
# (l_<base+4k>) for "Input K" (or the section's named input from `input_names`,
# {0-based index -> label}), and VarXX tokens for their friendly names (when
# a profile named them). The result is for labels; it must NEVER be fed back to
# the compiler -- the write path keeps the canonical text.
static func pretty_expr(text: String, var_list: Array = [], locals_base: int = DEFAULT_LOCALS_BASE, input_names: Dictionary = {}) -> String:
	var out := text
	for r in _SURFACE_REWRITES:
		out = out.replace(String(r[0]), String(r[1]))
	if out.contains("l_"):
		if _local_re == null:
			_local_re = RegEx.new()
			_local_re.compile("\\bl_(\\d+)\\b")
		var rebuilt := ""
		var pos := 0
		for m in _local_re.search_all(out):
			var off := int(m.get_string(1))
			var label := input_label_for_offset(off, locals_base)
			if label != "":
				var k := (off - locals_base) / 4
				if input_names.has(k):
					label = String(input_names[k])
			rebuilt += out.substr(pos, m.get_start() - pos)
			rebuilt += label if label != "" else m.get_string(0)
			pos = m.get_end()
		out = rebuilt + out.substr(pos)
	for v in var_list:
		var token := String(v.get("token", ""))
		var label := String(v.get("label", ""))
		if token == "" or label == "" or label == token:
			continue
		# MusVarNames labels read "Friendly (VarXX)"; inside an expression only
		# the short name reads well ("(MissionActive != 0)").
		var suffix := " (%s)" % token
		if label.ends_with(suffix):
			label = label.substr(0, label.length() - suffix.length())
		out = out.replace(token, label)
	return out
