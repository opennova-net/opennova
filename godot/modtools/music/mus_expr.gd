extends RefCounted

# Structured MUS expression model + serializer for the Phase-2 expression builder.
#
# Nodes are plain Dictionaries (the rest of the music editor passes AST dicts
# around the same way). serialize() emits the EXACT text the decompiler produces
# -- binops always fully parenthesized "(a op b)", unary "!x"/"-x"/"~x" with no
# space, intrinsics in split G/F/T surface form -- so an expression built here is
# byte-stable through the compile -> decompile round-trip. There is no GDScript
# PARSER: per the design, the compiler (NovaMusicScript.compile_text) is the only
# grammar authority; validate_expr() probes it. No class_name (preload as a const).

enum { LITERAL, VARREF, ME, BINOP, UNOP, CALL, RAW }


# ---- constructors --------------------------------------------------------

static func literal(value: int) -> Dictionary:
	return {"kind": LITERAL, "value": int(value)}


# form: "Var" (Var00..Var15, index 0..15), "g" (g_N raw offset), "l" (l_N local),
# or "named" (a declared global, serialized as its name).
static func varref(form: String, index: int, name: String = "") -> Dictionary:
	return {"kind": VARREF, "form": form, "index": int(index), "name": name}


static func me() -> Dictionary:
	return {"kind": ME}


static func binop(op: String, left: Dictionary, right: Dictionary) -> Dictionary:
	return {"kind": BINOP, "op": op, "left": left, "right": right}


static func unop(op: String, operand: Dictionary) -> Dictionary:
	return {"kind": UNOP, "op": op, "operand": operand}


# intrinsic = canonical stored name ("GSV", "FSet", ...). arg = null or a node.
# Named call_node (not `call`) so it doesn't shadow Object.call.
static func call_node(intrinsic: String, arg = null) -> Dictionary:
	return {"kind": CALL, "intrinsic": intrinsic, "arg": arg}


static func raw(text: String) -> Dictionary:
	return {"kind": RAW, "text": text}


# ---- serialize -----------------------------------------------------------

static func serialize(node) -> String:
	if typeof(node) != TYPE_DICTIONARY:
		return "0"
	match int(node.get("kind", -1)):
		LITERAL:
			return str(int(node.get("value", 0)))
		ME:
			return "Me"
		VARREF:
			match String(node.get("form", "Var")):
				"Var":
					return "Var%02d" % int(node.get("index", 0))
				"g":
					return "g_%d" % int(node.get("index", 0))
				"l":
					return "l_%d" % int(node.get("index", 0))
				_:
					return String(node.get("name", ""))
		UNOP:
			return "%s%s" % [String(node.get("op", "-")), serialize(node.get("operand", literal(0)))]
		BINOP:
			return "(%s %s %s)" % [
				serialize(node.get("left", literal(0))),
				String(node.get("op", "+")),
				serialize(node.get("right", literal(0))),
			]
		CALL:
			var s := surface(String(node.get("intrinsic", "")))
			var a = node.get("arg", null)
			if a == null:
				return "%s()" % s
			return "%s(%s)" % [s, serialize(a)]
		RAW:
			return String(node.get("text", "0"))
	return "0"  # never-empty invariant: any tree serializes to a compilable token


# Decompiler surface form of a stored intrinsic name: G* drops the G (GSV->SV),
# F*/T* become Obj.method (FSet->F.Set). Matches split_method_name in
# libs/mus/src/mus_decompile_shared.h, and the compiler inverts it.
static func surface(stored: String) -> String:
	if stored.length() > 1 and stored[0] == "G":
		return stored.substr(1)
	if stored.length() > 1 and (stored[0] == "F" or stored[0] == "T"):
		return "%s.%s" % [stored[0], stored.substr(1)]
	return stored


# ---- catalogs (driven by libs/mus) --------------------------------------

# Binary operators, grouped for the menu (text op, friendly label). Straight from
# binop_str (mus_decompile_shared.h) / binop_byte (mus_compile.cpp).
const BINOPS := [
	{"op": "==", "label": "is equal to", "group": "Compare"},
	{"op": "!=", "label": "is not equal to", "group": "Compare"},
	{"op": ">", "label": "greater than", "group": "Compare"},
	{"op": "<", "label": "less than", "group": "Compare"},
	{"op": ">=", "label": "at least", "group": "Compare"},
	{"op": "<=", "label": "at most", "group": "Compare"},
	{"op": "+", "label": "plus", "group": "Math"},
	{"op": "-", "label": "minus", "group": "Math"},
	{"op": "*", "label": "times", "group": "Math"},
	{"op": "/", "label": "divided by", "group": "Math"},
	{"op": "%", "label": "remainder", "group": "Math"},
	{"op": "&&", "label": "and (both)", "group": "Logic"},
	{"op": "||", "label": "or (either)", "group": "Logic"},
	{"op": "&", "label": "bit-and", "group": "Bits"},
	{"op": "|", "label": "bit-or", "group": "Bits"},
	{"op": "^", "label": "bit-xor", "group": "Bits"},
	{"op": "<<", "label": "shift left", "group": "Bits"},
	{"op": ">>", "label": "shift right", "group": "Bits"},
]

const UNOPS := [
	{"op": "-", "label": "negate"},
	{"op": "!", "label": "not (flip true/false)"},
	{"op": "~", "label": "flip bits"},
]

# The 11 intrinsics (kIntrinsicNames in mus_compile.cpp). stored name, friendly
# gloss, default arg count. The compiler accepts 0 or 1 arg for any of them.
const INTRINSICS := [
	{"name": "GSV", "label": "Set music volume", "arity": 1},
	{"name": "GSDV", "label": "Set default volume", "arity": 1},
	{"name": "GFB", "label": "Fade / feedback", "arity": 1},
	{"name": "GEcho", "label": "Echo (debug)", "arity": 1},
	{"name": "GGRnd", "label": "Random number", "arity": 1},
	{"name": "FSet", "label": "Set a flag", "arity": 1},
	{"name": "FClear", "label": "Clear a flag", "arity": 1},
	{"name": "FIsSet", "label": "Is flag set?", "arity": 1},
	{"name": "FIsClear", "label": "Is flag clear?", "arity": 1},
	{"name": "TStart", "label": "Start a timer", "arity": 0},
	{"name": "TStop", "label": "Stop a timer", "arity": 0},
]


# Validate an expression string by compiling a throwaway probe script that uses it
# as a condition. Reuses the native compiler (the single grammar authority) so the
# builder never re-implements parsing. mus may be null (then always ok, for tests
# that have no script handy). Returns { ok: bool, err: String }.
static func validate_expr(text: String, mus) -> Dictionary:
	if mus == null or not mus.has_method("compile_text"):
		return {"ok": true, "err": ""}
	var probe := "script _probe\nsection _s\n{\nif (%s)\n{\nreturn\n}\n}\n" % text
	var r: Dictionary = mus.compile_text(probe)
	return {"ok": int(r.get("rc", -1)) == 0, "err": String(r.get("err_msg", ""))}
