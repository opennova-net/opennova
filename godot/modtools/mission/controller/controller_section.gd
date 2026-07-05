extends RefCounted

# Base for the MissionController's extracted operation sections (maturity
# slice F5, the #178 inspector-split shape). A section owns a cluster of
# operations; ALL document/selection/overlay state stays on the composing
# controller and is reached through `_c`. Referenced via preload (no
# class_name), the workspace convention.

var _c  # the composing mission_controller (untyped)


func _init(controller) -> void:
	_c = controller
