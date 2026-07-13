class_name FocusPayload
extends RefCounted
## What a cross-workspace jump focuses after the target file is open: the
## shell's open_in_workspace forwards one of these to the target workspace's
## focus_reference hook. One typed record covers every workspace's focus
## vocabulary (ADR 0017); each workspace reads only the fields it owns and
## ignores the rest. Unset String fields are ""; unset int fields are -1.
## The MCP open_in_workspace tool decodes its wire object via from_dict —
## that dictionary is the transport encoding, this record is the contract.

## Strings: select this key's entry in the table.
var key := ""
## Menus: select the named screen in the active menu tab.
var screen := ""
## Menu styles: activate the Styles tab and select this variable.
var variable := ""
## Avatars: focus this part (with part_kind; -1 = the workspace's default kind).
var part := ""
var part_kind := -1
## Avatars: focus one nationality/division/combo triple (-1 = unset).
var nat := -1
var div := -1
var combo := -1


static func for_key(p_key: String) -> FocusPayload:
	var focus := FocusPayload.new()
	focus.key = p_key
	return focus


static func for_screen(p_screen: String) -> FocusPayload:
	var focus := FocusPayload.new()
	focus.screen = p_screen
	return focus


static func for_variable(p_variable: String) -> FocusPayload:
	var focus := FocusPayload.new()
	focus.variable = p_variable
	return focus


static func for_part(p_part_kind: int, p_part: String) -> FocusPayload:
	var focus := FocusPayload.new()
	focus.part_kind = p_part_kind
	focus.part = p_part
	return focus


static func for_combo(p_nat: int, p_div: int, p_combo: int) -> FocusPayload:
	var focus := FocusPayload.new()
	focus.nat = p_nat
	focus.div = p_div
	focus.combo = p_combo
	return focus


func is_empty() -> bool:
	return key.is_empty() and screen.is_empty() and variable.is_empty() \
			and part.is_empty() and nat < 0 and div < 0 and combo < 0


## The values the user asked for, for status messages ("not found: X").
func describe() -> String:
	var parts := PackedStringArray()
	for value in [key, screen, variable, part]:
		if not String(value).is_empty():
			parts.append(String(value))
	if nat >= 0 and div >= 0 and combo >= 0:
		parts.append("%d/%d/%d" % [nat, div, combo])
	return ", ".join(parts)


## MCP wire decoding (transport edge only). Accepts the historical "kind"
## spelling for the avatar part kind alongside "part_kind".
static func from_dict(d: Dictionary) -> FocusPayload:
	var focus := FocusPayload.new()
	focus.key = String(d.get("key", ""))
	focus.screen = String(d.get("screen", ""))
	focus.variable = String(d.get("variable", ""))
	focus.part = String(d.get("part", ""))
	focus.part_kind = int(d.get("kind", d.get("part_kind", -1)))
	focus.nat = int(d.get("nat", -1))
	focus.div = int(d.get("div", -1))
	focus.combo = int(d.get("combo", -1))
	return focus
