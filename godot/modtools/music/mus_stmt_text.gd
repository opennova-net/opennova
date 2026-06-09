extends RefCounted

# Canonical names-less .mus statement-line generators for Phase-2 authoring.
#
# Every function returns the EXACT text the decompiler would emit for that
# construct, so an authored statement is byte-stable (survives a decompile
# round-trip unchanged): 4-space body indent, binops always parenthesized,
# intrinsics in their split G/F/T surface form. The document splices these lines
# into a section through the compile gate. No class_name (preload as a const) to
# avoid GDScript class-registration ordering surprises.

static func play(track: int) -> PackedStringArray:
	return PackedStringArray(["play sound_%d" % track])


static func enter(section: String) -> PackedStringArray:
	return PackedStringArray(["enter %s" % section])


static func goto_section(section: String) -> PackedStringArray:
	return PackedStringArray(["goto %s" % section])


static func call_section(section: String) -> PackedStringArray:
	return PackedStringArray(["call %s" % section])


static func ret() -> PackedStringArray:
	return PackedStringArray(["return"])


static func yield_stmt() -> PackedStringArray:
	return PackedStringArray(["yield"])


# NOT byte-stable and intentionally NOT wired into the authoring palette: the
# decompiler renders nop as nothing, so an authored "nop" line has no text and
# would vanish on the next re-decompile. Kept only for completeness/tests.
static func nop() -> PackedStringArray:
	return PackedStringArray(["nop"])


static func assign(var_token: String, rhs: String) -> PackedStringArray:
	return PackedStringArray(["%s = %s" % [var_token, rhs]])


static func incdec(var_token: String, is_inc: bool) -> PackedStringArray:
	return PackedStringArray(["%s%s" % [var_token, "++" if is_inc else "--"]])


# A bare expression statement (method/intrinsic call), e.g. "SV(200)".
static func expr_stmt(expr: String) -> PackedStringArray:
	return PackedStringArray([expr])


# on (selector) <action> t1 t2 ...   (tablexec). action is "enter" / "goto" /
# "play"; targets are section names (enter/goto) or "sound_N" tokens (play).
static func switch_stmt(selector: String, action: String, targets: PackedStringArray) -> PackedStringArray:
	var joined := " ".join(targets)
	return PackedStringArray(["on (%s) %s %s" % [selector, action, joined]])


# A whole if / if-else block. then_body / else_body are flat arrays of unindented
# body statement lines; they are indented 4 spaces to match the decompiler. The
# block is one top-level statement (one ordinal), so it deletes/replaces as a unit.
static func if_block(cond: String, then_body: PackedStringArray, with_else: bool, else_body: PackedStringArray) -> PackedStringArray:
	var out := PackedStringArray()
	out.append("if (%s)" % cond)
	out.append("{")
	for l in then_body:
		out.append("    %s" % l)
	out.append("}")
	if with_else:
		out.append("else")
		out.append("{")
		for l in else_body:
			out.append("    %s" % l)
		out.append("}")
	return out
