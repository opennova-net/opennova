class_name PresentPassFixture
extends RefCounted

## What the present-pass tests share: the read helpers over an EffectWorld's
## public group report rows (rule 11: assert through get_debug_group_report,
## never a private), the PF-layout phase packing the flat snapshot builders use,
## and the per-entity lighting instance walk.


## A bare Node3D in the tree, autofreed with the test.
static func container(test: GutTest) -> Node3D:
	var node := Node3D.new()
	test.add_child_autofree(node)
	return node


## The first emitter's position of one group report row (the spawn point of a
## transient), Vector3.INF for a missing row.
static func emitter_position(row: EffectGroupReport) -> Vector3:
	if row == null or row.emitters.is_empty():
		return Vector3.INF
	return (row.emitters[0] as EffectEmitterReport).position


## The first emitter's forward axis, Vector3.INF for a missing row.
static func emitter_forward(row: EffectGroupReport) -> Vector3:
	if row == null or row.emitters.is_empty():
		return Vector3.INF
	return (row.emitters[0] as EffectEmitterReport).forward


## Pack one anim channel's phase into a flat PF-layout row the way
## Simulation.get_present_snapshot() emits it: the low word in PF_PHASE<n>,
## the high word + 1 in PF_ACTIVE<n> (0 = inactive).
static func write_phase(out: PackedFloat32Array, base: int,
		channel: int, phase: int, active: bool) -> void:
	var phase_field := Simulation.PF_PHASE1 + (channel - 1) * 2
	var active_field := Simulation.PF_ACTIVE1 + (channel - 1) * 2
	var bits := phase & 0xFFFFFFFF
	out[base + phase_field] = float(bits & 0xFFFF)
	out[base + active_field] = (
			float(((bits >> 16) & 0xFFFF) + 1) if active else 0.0)


## The per-entity lighting factors (x = effectScale, y = interior flag,
## z = interior daylight t) are one instance uniform stamped on the direct
## GeometryInstance3D children of every ROBJ part node and of the model's own
## Skeleton3D (skinned submeshes bind there instead of under a part).
static func entity_light_instances(model: ObjectModel) -> Array[GeometryInstance3D]:
	var parents: Array[Node] = []
	var parts: Dictionary = model.get_render_part_nodes()
	for key in parts.keys():
		parents.append(parts[key] as Node3D)
	if model.has_skeleton():
		parents.append(model.get_skeleton())
	var out: Array[GeometryInstance3D] = []
	for parent in parents:
		for child in parent.get_children():
			var instance := child as GeometryInstance3D
			if instance != null:
				out.append(instance)
	return out
