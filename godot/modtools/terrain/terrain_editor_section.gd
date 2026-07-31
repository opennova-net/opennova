extends RefCounted

# Base for TerrainEditor's extracted method-bundle sections (quality slice
# W4-6d, the merged W4-6a/6b/6c shape). A section owns a cluster of
# operations; ALL state stays on the composing editor and is reached through
# `_te`. Referenced via preload (no class_name), the workspace convention.
#
# Unlike the MissionController precedent (a RefCounted composer that needs a
# weakref-backed back-ref), the mount here is a Node3D: a plain back-reference
# from a RefCounted section to a Node cannot cycle, so `_te` is a direct
# untyped var.

var _te


func _init(editor) -> void:
	_te = editor
