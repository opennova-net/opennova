extends Node

# Renders in-engine part animations onto the placed models. The mission runtime (NovaSimulation)
# applies the PLAYPARTANIM action to the AI brain IN-ENGINE (faithful to Entity_ApplyCommand
# @0x43ab60 case 0x22: it writes the part channel's direction + rate, and the AI integrates the
# phase). This host reads each entity's per-channel part-anim phase from the sim every tick and
# poses the rendered model's PANM control register to it -- the original mutates engine state and
# renders from it; the host never re-interprets the action. Targets are resolved by SSN through a
# MissionEntityRegistry (only animatable NovaObjectModel nodes are indexed).
#
# [orig: Entity_ApplyCommand @0x43ab60 case 0x22 (PLAYPARTANIM).] See
# notes/mission/anim-ai-grill-2026-06-07.md.
#
# Referenced via preload() (no class_name), same convention as MissionObjectPlacer / MissionEntityRegistry.

var _registry  # MissionEntityRegistry: SSN -> animatable node
var _sim        # NovaSimulation: the live mission runtime
var _stats: Dictionary = { "posed": 0, "unresolved": 0 }


func setup(registry, sim) -> void:
	_registry = registry
	_sim = sim


func get_stats() -> Dictionary:
	return _stats.duplicate()


## Pose every animated entity's part channels to their engine-computed phase. Called once per logic
## tick by NovaWorld (after the sim advances). Channels the sim reports inactive are left untouched
## so a part that was never commanded keeps its default pose.
func render() -> void:
	if _registry == null or _sim == null:
		return
	var count: int = _sim.get_entity_count()
	for i in range(count):
		# Resolve by the file entity id (bms_id), the same key MissionEntityRegistry indexes nodes
		# by -- NOT the runtime SSN (net_id), which is a separate id space.
		var bms_id: int = _sim.get_entity_bms_id(i)
		if bms_id == 0:
			continue
		var node = _registry.resolve_single(bms_id)
		if node == null or not is_instance_valid(node) or not node.has_method("set_part_phase"):
			continue
		for channel in [1, 2]:
			if _sim.get_entity_part_anim_active(i, channel):
				node.set_part_phase(channel, _sim.get_entity_part_anim_phase(i, channel))
				_stats.posed += 1
