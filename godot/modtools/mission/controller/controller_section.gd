extends RefCounted

# Base for the MissionController's extracted operation sections (maturity
# slice F5, the #178 inspector-split shape). A section owns a cluster of
# operations; ALL document/selection/overlay state stays on the composing
# controller and is reached through `_c`. Referenced via preload (no
# class_name), the workspace convention.
#
# Unlike the inspector precedent (whose composer is a Node), the controller is
# RefCounted, so a plain back-reference would cycle controller <-> section and
# leak both (with the placer/overlay resources they hold). `_c` is therefore a
# weakref-backed read-only property: section bodies use `_c.<member>` exactly
# as if it were a direct reference, and the controller stays freeable.

var _c_ref: WeakRef

var _c:
	get:
		return _c_ref.get_ref()


func _init(controller) -> void:
	_c_ref = weakref(controller)
