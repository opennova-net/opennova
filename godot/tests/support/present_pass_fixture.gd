class_name PresentPassFixture
extends RefCounted

## What the present-pass tests share: a REAL EffectWorld wired the production
## way, the read helpers over its public group report (rule 11: assert through
## get_debug_group_report, never a private), the PF-layout phase packing the
## flat snapshot builders use, and the per-entity lighting instance walk.


## A REAL EffectWorld over `catalog`, its owner poses resolved by `anchors`
## (the production wiring ItemEffectDirector.on_effect_world_started performs
## for GameWorld's world): advance_fixed_tick(0.0) is the sync. Autofreed with
## the test.
static func make_fx(test: GutTest, anchors: ItemEffectDirector,
		catalog: ParticleFile) -> EffectWorld:
	var fx := EffectWorld.new()
	test.add_child_autofree(fx)
	fx.load_particle_file(catalog)
	fx.set_owner_position_provider(anchors.resolve_owner_transform)
	return fx


## A bare Node3D in the tree, autofreed with the test.
static func container(test: GutTest) -> Node3D:
	var node := Node3D.new()
	test.add_child_autofree(node)
	return node


## The live (still attached) group report row owned by `key`, or null when none.
static func live_owned_row(fx: EffectWorld, key: String) -> EffectGroupReport:
	for row_v in fx.get_debug_group_report():
		var row := row_v as EffectGroupReport
		if row.owner_key == key and not row.detached:
			return row
	return null


## Every group report row owned by `key`, attached or detached.
static func owned_rows(fx: EffectWorld, key: String) -> Array:
	var out: Array = []
	for row_v in fx.get_debug_group_report():
		var row := row_v as EffectGroupReport
		if row.owner_key == key:
			out.append(row)
	return out


## The group report row with `group_id` (a released owner drops its key from
## the row, so retired groups are found by id), or null once swept.
static func row_by_id(fx: EffectWorld, group_id: int) -> EffectGroupReport:
	for row_v in fx.get_debug_group_report():
		var row := row_v as EffectGroupReport
		if int(row.id) == group_id:
			return row
	return null


## Whether the group with `group_id` is detached; `swept` once the report no
## longer lists it.
static func row_detached(fx: EffectWorld, group_id: int, swept: bool = false) -> bool:
	var row := row_by_id(fx, group_id)
	return swept if row == null else row.detached


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
