extends RefCounted

# Manual performance probes temporarily flip private instrumentation switches.
# Preserve the first value observed for each object/property pair so every A/B
# leg and the final teardown restore the runtime exactly as they found it.

var _states: Array[Dictionary] = []


func has_property(target: Object, property_name: StringName) -> bool:
	if not is_instance_valid(target):
		return false
	for property_v in target.get_property_list():
		var property: Dictionary = property_v
		if StringName(property.get("name", "")) == property_name:
			return true
	return false


func set_temporary(
		target: Object,
		property_name: StringName,
		value: Variant,
		setter_method: StringName = StringName()) -> Error:
	if not is_instance_valid(target):
		return ERR_INVALID_PARAMETER
	if not has_property(target, property_name):
		return ERR_DOES_NOT_EXIST
	if not setter_method.is_empty() and not target.has_method(setter_method):
		return ERR_METHOD_NOT_FOUND

	var state_index := _find_state(target, property_name)
	if state_index < 0:
		_states.append({
			target = weakref(target),
			property = property_name,
			original = target.get(property_name),
			setter = setter_method,
		})
		state_index = _states.size() - 1
	elif StringName(_states[state_index].setter) != setter_method:
		return ERR_INVALID_PARAMETER
	return _apply(target, property_name, value, setter_method)


## Guard state exposed by an explicit getter/setter pair rather than a property.
## `state_key` only identifies the snapshot; it is never read from the target.
func set_temporary_method(
		target: Object,
		state_key: StringName,
		getter_method: StringName,
		setter_method: StringName,
		value: Variant) -> Error:
	if not is_instance_valid(target):
		return ERR_INVALID_PARAMETER
	if not target.has_method(getter_method) or not target.has_method(setter_method):
		return ERR_METHOD_NOT_FOUND

	var state_index := _find_state(target, state_key)
	if state_index < 0:
		_states.append({
			target = weakref(target),
			property = state_key,
			original = target.call(getter_method),
			setter = setter_method,
		})
		state_index = _states.size() - 1
	elif StringName(_states[state_index].setter) != setter_method:
		return ERR_INVALID_PARAMETER
	return _apply(target, state_key, value, setter_method)


func restore_property(target: Object, property_name: StringName) -> Error:
	if not is_instance_valid(target):
		return ERR_INVALID_PARAMETER
	var state_index := _find_state(target, property_name)
	if state_index < 0:
		return OK
	var state: Dictionary = _states[state_index]
	var err := _apply(
			target,
			StringName(state.property),
			state.original,
			StringName(state.setter))
	if err == OK:
		_states.remove_at(state_index)
	return err


func restore_all() -> Error:
	var first_error: Error = OK
	for state_index in range(_states.size() - 1, -1, -1):
		var state: Dictionary = _states[state_index]
		var target = (state.target as WeakRef).get_ref()
		if not is_instance_valid(target):
			_states.remove_at(state_index)
			continue
		var err := _apply(
				target,
				StringName(state.property),
				state.original,
				StringName(state.setter))
		if err == OK:
			_states.remove_at(state_index)
		elif first_error == OK:
			first_error = err
	return first_error


func _find_state(target: Object, property_name: StringName) -> int:
	for state_index in _states.size():
		var state: Dictionary = _states[state_index]
		var saved_target = (state.target as WeakRef).get_ref()
		if saved_target == target and StringName(state.property) == property_name:
			return state_index
	return -1


func _apply(
		target: Object,
		property_name: StringName,
		value: Variant,
		setter_method: StringName) -> Error:
	if not setter_method.is_empty():
		if not target.has_method(setter_method):
			return ERR_METHOD_NOT_FOUND
		target.call(setter_method, value)
	else:
		target.set(property_name, value)
	return OK
