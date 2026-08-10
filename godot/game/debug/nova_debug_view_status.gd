extends RefCounted
## One typed observation of a world-space F3 view. Toggle intent, installed
## lifecycle state, and drawable output are separate facts so callers never
## have to infer one from another.

var id: StringName
var enabled: bool
var installed: bool
var drawable_count: int
var reason: String


func _init(
		p_id: StringName,
		p_enabled: bool,
		p_installed: bool,
		p_drawable_count: int,
		p_reason: String) -> void:
	id = p_id
	enabled = p_enabled
	installed = p_installed
	drawable_count = p_drawable_count
	reason = p_reason
