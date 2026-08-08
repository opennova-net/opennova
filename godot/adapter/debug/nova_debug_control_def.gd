class_name NovaDebugControlDef
extends RefCounted
## Typed, UI-free description of one public debug knob.
##
## The definition contains only presentation metadata and the public method or
## property names needed to read and operate the target. NovaDebugSession owns
## target resolution, policy, validation, suspension and live readback.

enum Kind {
	CHECK,
	SLIDER,
	ENUM,
	ACTION,
}

enum Authority {
	ANY,
	HOST_ONLY,
}

var id: StringName
var page_id: StringName
var label: String
var description: String
var kind := Kind.CHECK
var target_id: StringName
var getter: StringName
var setter: StringName
var property_name: StringName
var action: StringName
## Setters normally return void. Set this when the public setter returns Error
## and a rejected write must propagate through F3/MCP instead of looking saved.
var setter_returns_error := false
## Optional UI-free action argument predicate. It receives the final Array
## passed to callv() and must return true before the public action is invoked.
var action_validator := Callable()
## Public actions normally return diagnostic payloads. Set this when an
## Error-valued return is the operation result and must be propagated.
var action_returns_error := false
var default_value: Variant
var minimum := 0.0
var maximum := 1.0
var step := 0.1
var choices: Array[String] = []
var expensive := false
var requires_unlock := false
var authority := Authority.ANY
## Transitional seam for controls whose current host still owns the target.
## The session records/emits the intent even when no target has been bound.
var allow_unresolved_intent := false


static func check(
		control_id: StringName,
		control_page: StringName,
		control_label: String,
		control_description: String,
		control_target: StringName,
		control_getter: StringName,
		control_setter: StringName,
		control_default := false) -> NovaDebugControlDef:
	var definition := NovaDebugControlDef.new()
	definition.id = control_id
	definition.page_id = control_page
	definition.label = control_label
	definition.description = control_description
	definition.kind = Kind.CHECK
	definition.target_id = control_target
	definition.getter = control_getter
	definition.setter = control_setter
	definition.default_value = control_default
	return definition


static func slider(
		control_id: StringName,
		control_page: StringName,
		control_label: String,
		control_description: String,
		control_target: StringName,
		control_getter: StringName,
		control_setter: StringName,
		control_default: float,
		control_minimum: float,
		control_maximum: float,
		control_step: float) -> NovaDebugControlDef:
	var definition := NovaDebugControlDef.new()
	definition.id = control_id
	definition.page_id = control_page
	definition.label = control_label
	definition.description = control_description
	definition.kind = Kind.SLIDER
	definition.target_id = control_target
	definition.getter = control_getter
	definition.setter = control_setter
	definition.default_value = control_default
	definition.minimum = control_minimum
	definition.maximum = control_maximum
	definition.step = control_step
	return definition


static func enum_control(
		control_id: StringName,
		control_page: StringName,
		control_label: String,
		control_description: String,
		control_target: StringName,
		control_getter: StringName,
		control_setter: StringName,
		control_default: int,
		control_choices: Array[String]) -> NovaDebugControlDef:
	var definition := NovaDebugControlDef.new()
	definition.id = control_id
	definition.page_id = control_page
	definition.label = control_label
	definition.description = control_description
	definition.kind = Kind.ENUM
	definition.target_id = control_target
	definition.getter = control_getter
	definition.setter = control_setter
	definition.default_value = control_default
	definition.choices = control_choices.duplicate()
	return definition


static func action_control(
		control_id: StringName,
		control_page: StringName,
		control_label: String,
		control_description: String,
		control_target: StringName,
		control_action: StringName) -> NovaDebugControlDef:
	var definition := NovaDebugControlDef.new()
	definition.id = control_id
	definition.page_id = control_page
	definition.label = control_label
	definition.description = control_description
	definition.kind = Kind.ACTION
	definition.target_id = control_target
	definition.action = control_action
	return definition


## JSON-facing representation used by MCP catalog responses.
func to_json_value() -> Variant:
	return {
		"id": String(id),
		"page": String(page_id),
		"label": label,
		"description": description,
		"kind": kind_name(),
		"target": String(target_id),
		"minimum": minimum,
		"maximum": maximum,
		"step": step,
		"choices": choices.duplicate(),
		"expensive": expensive,
		"requires_unlock": requires_unlock,
		"authority": "host" if authority == Authority.HOST_ONLY else "any",
	}


func kind_name() -> String:
	match kind:
		Kind.CHECK:
			return "check"
		Kind.SLIDER:
			return "slider"
		Kind.ENUM:
			return "enum"
		Kind.ACTION:
			return "action"
	return "unknown"
