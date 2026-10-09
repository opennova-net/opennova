class_name McpToolArgs
extends RefCounted

## Typed reads of a tool call's JSON arguments, shared by the endpoints'
## handlers. A JSON number arrives as an int or a float, and int()/float() on
## a non-numeric Variant is a script error that would abort a handler mid-call,
## so a handler validates the type first and refuses the call on a null.


## `value` as a finite float, or null when it is no finite number.
static func finite_number(value: Variant) -> Variant:
	if typeof(value) != TYPE_INT and typeof(value) != TYPE_FLOAT:
		return null
	var number := float(value)
	return number if is_finite(number) else null


## `value` as an int, or null when it is no whole finite number.
static func integer_number(value: Variant) -> Variant:
	var number: Variant = finite_number(value)
	if number == null or float(number) != floorf(float(number)):
		return null
	return int(number)
