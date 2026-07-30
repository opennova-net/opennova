class_name MusicSectionGraph
extends RefCounted

# Section transition map, built from the C++ structural model
# (NovaMusicScript.get_section_model), which reads OPCODES -- so a real state
# transition (setstate 0x3B) is distinguished from the frame-setup `enter`
# (0x38), and tablexec switch fan-out is captured.
#
# This replaces the previous approach of string-parsing the decompiled text for
# a "setstate " token: the decompiler emits "enter" for that op (never
# "setstate"), so the old parser produced an EMPTY graph for every shipped
# script. The model is the single source of truth shared with the editor map.

const KIND_TRANSITION := 0   # setstate: unconditional move to a section
const KIND_SWITCH := 1       # tablexec entry: one branch of a switch
const KIND_BRANCH := 2       # goto/brfalse/brtrue whose target is a section entry


# Returns the structural model: an Array of section dicts, each
#   { name, index, is_entry, is_idle_loop,
#     edges:[{to:int, to_name:String, kind:int}], plays:[{track:int, wait:bool}] }
# Empty Array when the script can't be modelled (no script).
static func build(script_resource: NovaMusicScript, script_name: StringName) -> Array:
	if script_resource == null:
		return []
	return script_resource.get_section_model(script_name)


# Arrow glyph for an edge kind: a plain transition, a switch branch, or a
# conditional branch. Used by the section map rows.
static func edge_glyph(kind: int) -> String:
	match kind:
		KIND_SWITCH:
			return "⇒"
		KIND_BRANCH:
			return "↪"
		_:
			return "→"


# Wire colour for an edge kind, so the blueprint map reads a switch fan-out
# (cyan) apart from a plain transition (blue) or a conditional branch (gold) at a
# glance. Matches the inspector's per-kind icon palette (switch ⋔ cyan, if ◇ gold,
# transition → blue). The GraphEdit draws each connection in its source pin colour.
static func edge_color(kind: int) -> Color:
	match kind:
		KIND_SWITCH:
			return Color(0.50, 0.85, 0.90)
		KIND_BRANCH:
			return Color(1.00, 0.85, 0.45)
		_:
			return Color(0.55, 0.80, 1.00)
