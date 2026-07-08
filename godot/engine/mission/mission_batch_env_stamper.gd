extends Node

# Per-frame relight driver for the placer's static MultiMesh batches. The
# batch materials are harvested from a throwaway template model, so no live
# NovaObjectModel owns them; this node (added into the MissionObjects
# container by the placer) re-stamps them from the live environment each
# frame — retail relights every entity from the current lighting block per
# frame [orig: setup_entity_lighting_and_shader_constants @ 0x5d98a0].
# Generation-gated inside update_environment(), so a settled env costs one
# int compare. Dies with the container on re-bake/unload.

var placer  # MissionObjectPlacer (RefCounted, no class_name)
var environment_node: Node


func _process(_delta: float) -> void:
	if placer == null:
		return
	if environment_node != null and not is_instance_valid(environment_node):
		environment_node = null
		return
	placer.update_environment(environment_node)
