extends RefCounted

# Base for MusicLiveMode's extracted operation sections (quality slice
# W4-6a, the F5 / #178 inspector-split shape -- see
# modtools/mission/controller/controller_section.gd for the precedent). A
# section owns a cluster of Live-mode operations; ALL document/director/UI
# state, constants and signals stay on the composing MusicLiveMode and are
# reached through `_lm`. Referenced via preload (no class_name), the
# workspace convention.
#
# Unlike the controller precedent (whose composer is RefCounted and needs a
# weakref to break the composer <-> section cycle), the mount here is a
# Control -- a Node, manually managed -- so a plain back-reference cannot
# cycle: the mount owns the sections; the sections point back at the Node.

var _lm


func _init(live_mode) -> void:
	_lm = live_mode
