class_name NovaDebugSession
extends RefCounted
## UI-free catalog and execution module shared by F3 and runtime automation.
##
## Callers bind re-resolving targets and register typed control definitions.
## This module validates writes, enforces edit/authority policy, reads back the
## public owner every time, and suspends expensive visualizers while no debug
## presentation is open.

signal catalog_changed
signal control_changed(id: StringName, state: NovaDebugControlState)
## Emitted after a public setter/action was accepted. Kept intentionally
## presentation-neutral.
signal control_invoked(id: StringName, value: Variant)
signal edit_unlock_changed(unlocked: bool)
signal presented_changed(presented: bool)

var _definitions: Dictionary = {}
var _definition_order: Array[StringName] = []
var _targets: Dictionary = {}
var _desired_values: Dictionary = {}
var _explicit_values: Dictionary = {}
var _suspended: Dictionary = {}
var _pending_replays: Dictionary = {}
var _last_target_ids: Dictionary = {}
var _edit_unlocked := false
var _presented := false
var _presentation_sources: Dictionary = {}
var _presentation_leases: Dictionary = {}
var _authority_source := Callable()
var _status_source := Callable()


func bind_target(target: NovaDebugTarget) -> void:
	if target == null or target.id == &"":
		return
	_targets[target.id] = target


func set_target_source(
		id: StringName,
		source: Callable,
		unavailable_reason: String = "") -> void:
	bind_target(NovaDebugTarget.new(id, String(id), source, unavailable_reason))


func register_control(definition: NovaDebugControlDef) -> bool:
	if definition == null or definition.id == &"" or _definitions.has(definition.id):
		return false
	_definitions[definition.id] = definition
	_definition_order.append(definition.id)
	if definition.kind != NovaDebugControlDef.Kind.ACTION:
		_desired_values[definition.id] = definition.default_value
	catalog_changed.emit()
	return true


func has_control(id: StringName) -> bool:
	return _definitions.has(id)


func definition(id: StringName) -> NovaDebugControlDef:
	return _definitions.get(id) as NovaDebugControlDef


## JSON-safe definitions paired with a live state, suitable for MCP.
## `allow_authority` mirrors the write path's per-call confirmation: rows
## report writability for THAT caller, so an MCP client holding
## confirm_authority is not told its own successful writes are locked.
func list_controls(
		page_id: StringName = &"",
		filter_text: String = "",
		allow_authority: bool = false) -> Array[Dictionary]:
	sync()
	var output: Array[Dictionary] = []
	var needle := filter_text.strip_edges().to_lower()
	for id in _definition_order:
		var control := definition(id)
		if page_id != &"" and control.page_id != page_id:
			continue
		if not needle.is_empty():
			var haystack := ("%s %s %s %s" % [
				control.id, control.page_id, control.label,
				control.description]).to_lower()
			if not haystack.contains(needle):
				continue
		var row: Dictionary = control.to_json_value()
		row["state"] = read_control_state(id, allow_authority).to_json_value()
		output.append(row)
	return output


func read_control_state(
		id: StringName,
		allow_authority: bool = false) -> NovaDebugControlState:
	var state := NovaDebugControlState.new()
	state.id = id
	var control := definition(id)
	if control == null:
		state.reason = "Unknown debug control."
		return state
	state.kind = control.kind
	state.desired_value = _desired_values.get(id, control.default_value)
	state.suspended = _suspended.has(id)

	var target := _resolve_target(control.target_id)
	if target == null:
		state.value = state.desired_value
		if control.allow_unresolved_intent:
			state.available = true
			state.writable = _write_allowed(control, allow_authority)
			state.reason = "The game applies this control."
		else:
			state.reason = _target_reason(control.target_id)
		return state

	state.available = _operation_exists(control, target)
	if not state.available:
		state.value = state.desired_value
		state.reason = _missing_operation_reason(control, target)
		return state

	state.writable = _write_allowed(control, allow_authority)
	if not state.writable:
		state.reason = _policy_reason(control)

	if control.kind == NovaDebugControlDef.Kind.ACTION:
		return state
	if state.suspended:
		state.value = state.desired_value
		return state
	var read := _read_value(control, target)
	if bool(read["ok"]):
		state.value = read["value"]
		state.authoritative = true
		_desired_values[id] = state.value
		state.desired_value = state.value
	else:
		state.value = state.desired_value
		if state.reason.is_empty():
			state.reason = String(read["reason"])
	return state


func get_control_state(
		id: StringName,
		allow_authority: bool = false) -> NovaDebugControlState:
	sync()
	return read_control_state(id, allow_authority)


func set_control_value(
		id: StringName,
		value: Variant,
		allow_authority: bool = false) -> Error:
	sync()
	var control := definition(id)
	if control == null:
		return ERR_DOES_NOT_EXIST
	if control.kind == NovaDebugControlDef.Kind.ACTION:
		return ERR_INVALID_PARAMETER
	var normalized := _normalize_value(control, value)
	if not bool(normalized["ok"]):
		return ERR_INVALID_PARAMETER
	if not _write_allowed(control, allow_authority):
		return ERR_UNAUTHORIZED
	var current := read_control_state(id)
	if not current.suspended and current.desired_value == normalized["value"] \
			and (not current.authoritative or current.value == normalized["value"]):
		return OK
	if control.expensive and not _presented \
			and _is_expensive_active(control, normalized["value"]):
		_desired_values[id] = normalized["value"]
		_explicit_values[id] = true
		_pending_replays.erase(id)
		_suspended[id] = true
		control_changed.emit(id, read_control_state(id))
		return OK
	_suspended.erase(id)
	var applied := _apply(control, normalized["value"])
	if applied != OK:
		return applied
	_desired_values[id] = normalized["value"]
	_explicit_values[id] = true
	_pending_replays.erase(id)
	control_invoked.emit(id, normalized["value"])
	control_changed.emit(id, read_control_state(id))
	return OK


## Actions accept null (no arguments), an Array (callv arguments), or one
## scalar argument. Returns a JSON-safe result record for automation.
func invoke_control(
		id: StringName,
		args: Variant = null,
		allow_authority: bool = false) -> Dictionary:
	sync()
	var control := definition(id)
	if control == null:
		return _invoke_result(ERR_DOES_NOT_EXIST, null, id, allow_authority)
	if control.kind != NovaDebugControlDef.Kind.ACTION:
		var error := set_control_value(id, args, allow_authority)
		return _invoke_result(error, null, id, allow_authority)
	if not _write_allowed(control, allow_authority):
		return _invoke_result(ERR_UNAUTHORIZED, null, id, allow_authority)
	var target := _resolve_target(control.target_id)
	if target == null:
		return _invoke_result(ERR_UNAVAILABLE, null, id, allow_authority)
	if control.action == &"" or not target.has_method(control.action):
		return _invoke_result(ERR_UNAVAILABLE, null, id, allow_authority)
	var call_args: Array = []
	if args is Array:
		call_args = args
	elif args != null:
		call_args = [args]
	if control.action_validator.is_valid() \
			and not bool(control.action_validator.call(call_args)):
		return _invoke_result(ERR_INVALID_PARAMETER, null, id, allow_authority)
	var result: Variant = target.callv(control.action, call_args)
	if control.action_returns_error and typeof(result) == TYPE_INT \
			and int(result) != OK:
		var action_error: Error = int(result)
		return _invoke_result(action_error, null, id, allow_authority)
	control_invoked.emit(id, args)
	control_changed.emit(id, read_control_state(id))
	return _invoke_result(OK, result, id, allow_authority)


func set_edit_unlocked(unlocked: bool) -> void:
	if _edit_unlocked == unlocked:
		return
	_edit_unlocked = unlocked
	if unlocked:
		sync()
	edit_unlock_changed.emit(unlocked)
	for id in _definition_order:
		var control := definition(id)
		if control.requires_unlock:
			control_changed.emit(id, read_control_state(id))


func is_edit_unlocked() -> bool:
	return _edit_unlocked


## Presentation lifecycle. Expensive non-default controls are physically reset
## on hide but keep their desired session value, then restore against the
## freshly resolved target on show.
func set_presented(presented: bool) -> void:
	set_presentation_source(&"overlay", presented)


## Presentation sources compose: F3 and a transient automation capture can
## overlap without one source hiding expensive views out from under the other.
func set_presentation_source(source: StringName, presented: bool) -> void:
	if source == &"":
		return
	if presented:
		_presentation_sources[source] = true
	else:
		_presentation_sources.erase(source)
	_update_presented_aggregate()


## Ref-counted counterpart for overlapping asynchronous work. A timed-out MCP
## handler may finish after a newer capture has acquired the same source; one
## release must not hide expensive views out from under the newer capture.
func acquire_presentation_source(source: StringName) -> void:
	if source == &"":
		return
	_presentation_leases[source] = int(_presentation_leases.get(source, 0)) + 1
	_update_presented_aggregate()


func release_presentation_source(source: StringName) -> void:
	if source == &"" or not _presentation_leases.has(source):
		return
	var remaining := int(_presentation_leases[source]) - 1
	if remaining > 0:
		_presentation_leases[source] = remaining
	else:
		_presentation_leases.erase(source)
	_update_presented_aggregate()


func _update_presented_aggregate() -> void:
	var aggregate := not _presentation_sources.is_empty() \
			or not _presentation_leases.is_empty()
	if aggregate == _presented:
		return
	_set_presented_state(aggregate)


func _set_presented_state(presented: bool) -> void:
	if _presented == presented:
		return
	sync()
	_presented = presented
	for id in _definition_order:
		var control := definition(id)
		if not control.expensive or control.kind == NovaDebugControlDef.Kind.ACTION:
			continue
		var desired: Variant = _desired_values.get(id, control.default_value)
		if not presented and _is_expensive_active(control, desired):
			_suspended[id] = true
			if _apply(control, control.default_value) == OK:
				control_invoked.emit(id, control.default_value)
			elif control.allow_unresolved_intent:
				control_invoked.emit(id, control.default_value)
			control_changed.emit(id, read_control_state(id))
		elif presented and _suspended.has(id):
			_suspended.erase(id)
			if _is_expensive_active(control, desired):
				if _apply(control, desired) == OK:
					control_invoked.emit(id, desired)
				elif control.allow_unresolved_intent:
					control_invoked.emit(id, desired)
			control_changed.emit(id, read_control_state(id))
	presented_changed.emit(presented)


func _is_expensive_active(
		control: NovaDebugControlDef,
		value: Variant) -> bool:
	return value != control.default_value


func is_presented() -> bool:
	return _presented


func set_authority_source(source: Callable) -> void:
	_authority_source = source


## Whether this observer may mutate host-authoritative game state right now.
## UI shells use the same policy source as control invocation, so presentation
## cannot claim edit access while the write path will reject it.
func has_host_authority() -> bool:
	return _has_host_authority()


func set_status_source(source: Callable) -> void:
	_status_source = source


## JSON-facing runtime status for snapshots and MCP.
func runtime_status() -> Variant:
	if not _status_source.is_valid():
		return {}
	var value: Variant = _status_source.call()
	if value is Dictionary:
		return NovaDebugControlState._json_value(value)
	if value is String:
		return {"label": value}
	return {}


## JSON-facing snapshot; typed controls are serialized only at this boundary.
func capture_snapshot(
		filter_text: String = "",
		allow_authority: bool = false) -> Variant:
	return {
		"runtime": runtime_status(),
		"edit_unlocked": _edit_unlocked,
		"presented": _presented,
		"controls": list_controls(&"", filter_text, allow_authority),
	}


## Replays desired values once when a source starts resolving to a different
## target instance. This repairs debug state after a world reload without any
## host-specific reload hook.
func sync() -> void:
	for target_id in _targets:
		var target := _resolve_target(target_id)
		var instance_id := target.get_instance_id() if target != null else 0
		var previous_id := int(_last_target_ids.get(target_id, 0))
		if instance_id == previous_id:
			continue
		_last_target_ids[target_id] = instance_id
		if target == null:
			continue
		for id in _definition_order:
			var control := definition(id)
			if control.target_id != target_id \
					or control.kind == NovaDebugControlDef.Kind.ACTION \
					or _suspended.has(id):
				continue
			if bool(_explicit_values.get(id, false)):
				_replay_or_defer(control)
			else:
				var initial := _read_value(control, target)
				if bool(initial["ok"]):
					_desired_values[id] = initial["value"]
				_pending_replays.erase(id)
	for id_v in _pending_replays.keys():
		var id := StringName(id_v)
		var control := definition(id)
		if control != null and not _suspended.has(id):
			_replay_or_defer(control)


func _replay_or_defer(control: NovaDebugControlDef) -> void:
	if control == null or not _desired_values.has(control.id):
		return
	if not _write_allowed(control, false):
		_pending_replays[control.id] = true
		return
	if _apply(control, _desired_values[control.id]) == OK:
		_pending_replays.erase(control.id)
	else:
		_pending_replays[control.id] = true


func _resolve_target(id: StringName) -> Object:
	var target := _targets.get(id) as NovaDebugTarget
	return target.resolve() if target != null else null


func _target_reason(id: StringName) -> String:
	var target := _targets.get(id) as NovaDebugTarget
	if target != null and not target.unavailable_reason.is_empty():
		return target.unavailable_reason
	return "The %s target is not available." % String(id).replace("_", " ")


func _operation_exists(control: NovaDebugControlDef, target: Object) -> bool:
	if control.kind == NovaDebugControlDef.Kind.ACTION:
		return control.action != &"" and target.has_method(control.action)
	if control.property_name != &"":
		return _has_property(target, control.property_name)
	return control.setter != &"" and target.has_method(control.setter)


func _missing_operation_reason(
		control: NovaDebugControlDef,
		target: Object) -> String:
	var operation := control.action if control.kind == NovaDebugControlDef.Kind.ACTION \
			else control.property_name if control.property_name != &"" else control.setter
	return "%s does not expose %s." % [target.get_class(), operation]


func _read_value(control: NovaDebugControlDef, target: Object) -> Dictionary:
	if control.property_name != &"" and _has_property(target, control.property_name):
		return {"ok": true, "value": target.get(control.property_name), "reason": ""}
	if control.getter != &"" and target.has_method(control.getter):
		return {"ok": true, "value": target.call(control.getter), "reason": ""}
	return {
		"ok": false,
		"value": control.default_value,
		"reason": "This public knob has no readback method yet.",
	}


func _apply(control: NovaDebugControlDef, value: Variant) -> Error:
	var target := _resolve_target(control.target_id)
	if target == null:
		return OK if control.allow_unresolved_intent else ERR_UNAVAILABLE
	if control.property_name != &"":
		if not _has_property(target, control.property_name):
			return ERR_UNAVAILABLE
		target.set(control.property_name, value)
		return OK
	if control.setter == &"" or not target.has_method(control.setter):
		return OK if control.allow_unresolved_intent else ERR_UNAVAILABLE
	var result: Variant = target.call(control.setter, value)
	if control.setter_returns_error and typeof(result) == TYPE_INT \
			and int(result) != OK:
		var setter_error: Error = int(result)
		return setter_error
	return OK


func _write_allowed(
		control: NovaDebugControlDef,
		allow_authority: bool) -> bool:
	if control.requires_unlock and not (_edit_unlocked or allow_authority):
		return false
	if control.authority == NovaDebugControlDef.Authority.HOST_ONLY \
			and not _has_host_authority():
		return false
	return true


func _has_host_authority() -> bool:
	if not _authority_source.is_valid():
		return true
	var value: Variant = _authority_source.call()
	if value is Dictionary:
		return bool(value.get("can_mutate", value.get("is_host", false)))
	return bool(value)


func _policy_reason(control: NovaDebugControlDef) -> String:
	if control.authority == NovaDebugControlDef.Authority.HOST_ONLY \
			and not _has_host_authority():
		return "Only the session host can change authoritative game state."
	if control.requires_unlock and not _edit_unlocked:
		return "Enable Live edits to use this control."
	return ""


func _normalize_value(
		control: NovaDebugControlDef,
		value: Variant) -> Dictionary:
	match control.kind:
		NovaDebugControlDef.Kind.CHECK:
			if typeof(value) != TYPE_BOOL:
				return {"ok": false}
			return {"ok": true, "value": bool(value)}
		NovaDebugControlDef.Kind.SLIDER:
			if typeof(value) != TYPE_INT and typeof(value) != TYPE_FLOAT:
				return {"ok": false}
			var number := float(value)
			if not is_finite(number):
				return {"ok": false}
			number = clampf(number, control.minimum, control.maximum)
			if control.step > 0.0:
				number = snappedf(number, control.step)
			return {"ok": true, "value": number}
		NovaDebugControlDef.Kind.ENUM:
			if typeof(value) != TYPE_INT:
				return {"ok": false}
			var index := int(value)
			if index < 0 or index >= control.choices.size():
				return {"ok": false}
			return {"ok": true, "value": index}
	return {"ok": false}


func _has_property(target: Object, property_name: StringName) -> bool:
	for property in target.get_property_list():
		if StringName(property.get("name", &"")) == property_name:
			return true
	return false


func _invoke_result(
		error: Error,
		result: Variant,
		id: StringName,
		allow_authority: bool = false) -> Dictionary:
	return {
		"error": int(error),
		"result": NovaDebugControlState._json_value(result),
		"state": read_control_state(id, allow_authority).to_json_value(),
	}


## Turn an invoke_control result into non-empty presentation text. A public
## action can fail after its catalog state was read (for example, an entity is
## despawned between refresh and click), so state.reason alone is not enough.
static func invoke_error_message(
		outcome: Dictionary,
		fallback: String = "Debug action failed.") -> String:
	var code := int(outcome.get("error", ERR_UNAVAILABLE))
	if code == OK:
		return ""
	var state_value: Variant = outcome.get("state", {})
	var state: Dictionary = state_value if state_value is Dictionary else {}
	var reason := String(state.get("reason", "")).strip_edges()
	if not reason.is_empty():
		return reason
	var detail := error_string(code).strip_edges()
	if detail.is_empty():
		return fallback
	return "%s (%s)" % [fallback.trim_suffix("."), detail]
