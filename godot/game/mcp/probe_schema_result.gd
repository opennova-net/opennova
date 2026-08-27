class_name ProbeSchemaResult
extends RefCounted

## The outcome of ProbeSchema.validate: the coerced argument values with the
## schema's defaults filled in, or the list of what was wrong.

var ok := false
var errors := PackedStringArray()
## The validated arguments (JSON-facing; the probe reads them as-is).
var values: Dictionary = {}
