extends RefCounted

# Base for the Mission inspector's extracted panel sections. A section owns its
# widgets and handlers; shared state (controller, option caches, sync helpers,
# detail-dock plumbing) stays on the composing inspector and is reached through
# `_inspector`. Referenced via preload (no class_name), the workspace convention.

var _inspector  # the composing mission_inspector (untyped)


func _init(inspector) -> void:
	_inspector = inspector
