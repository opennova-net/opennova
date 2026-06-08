extends RefCounted

# THE mission present pass: applies the live entity state the runtime computes onto the placed scene
# nodes once per logic tick. Replaces the two divergent paths it consolidates -- the game's
# MissionCommandHost (PANM part-anim only) and the editor's MissionSimDriver._apply (position + yaw
# only) -- with ONE component both hosts go through, so the game gains movement and the editor gains
# part-anims from the same code.
#
# Hybrid split (the engine decides, the host draws): it pulls ONE batched snapshot from the sim
# (NovaSimulation.get_present_snapshot -- a flat PackedFloat32Array, avoiding ~10 Variant-boxed getter
# calls per entity) and writes each animated node's transform + PANM part channels + visibility.
#
# Two animation systems, distinct on purpose:
#  - Procedural part-anim (PANM): the vehicle/emplacement part system (turret/dish), PLAYPARTANIM
#    case 0x22, integrated in-engine; applied here via set_part_phase. IMPLEMENTED.
#  - Main-body skeletal (.bad via .adm): the primary infantry/player/view-model animation, selected by
#    AI state. The skeletal runtime is NOT built yet (libs/bad + libs/adm are parse-only; no
#    Skeleton3D/AnimationPlayer; organics render frozen at rest pose). apply_body_anim is a DEFERRED
#    SEAM. [orig: AnimMap_PlayAnimBySlot @0x40bda0, off_8135F0, Entity_UpdateInfantryAI @0x4b9910.]
#
# Targets resolve through ONE shared index (MissionEntityRegistry.resolve: bms_id primary, (kind,index)
# fallback). Host-agnostic, RefCounted, preload-referenced (same convention as MissionObjectPlacer).

const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")

var _sim                    # NovaSimulation (or a compatible snapshot source)
var _index                  # MissionEntityRegistry: resolve(bms_id, kind, index) -> Node
var _drive_transform := true
var _drive_part_anim := true
var _drive_visibility := true
var _stats: Dictionary = { "moved": 0, "posed": 0, "hidden": 0 }


## options: { drive_transform, drive_part_anim, drive_visibility } (all default true). The editor
## preview drives all three; the game drives all three too (its NPCs were previously static-placed).
func setup(sim, index, options: Dictionary = {}) -> void:
	_sim = sim
	_index = index
	_drive_transform = bool(options.get("drive_transform", true))
	_drive_part_anim = bool(options.get("drive_part_anim", true))
	_drive_visibility = bool(options.get("drive_visibility", true))


func get_stats() -> Dictionary:
	return _stats.duplicate()


## Apply the current sim state onto every resolved animated node. Called once per logic tick by the
## runtime driver (after the sim advances).
func present() -> void:
	if _sim == null or _index == null:
		return
	var stride: int = _sim.get_present_stride()
	if stride <= 0:
		return
	var snap: PackedFloat32Array = _sim.get_present_snapshot()
	var count: int = snap.size() / stride
	for i in range(count):
		var base := i * stride
		var node = _index.resolve(
			int(snap[base + NovaSimulation.PF_BMS_ID]),
			int(snap[base + NovaSimulation.PF_KIND]),
			int(snap[base + NovaSimulation.PF_INDEX]))
		if node == null or not is_instance_valid(node):
			continue
		if _drive_transform:
			_apply_transform(node, snap, base)
		if _drive_part_anim:
			_apply_procedural_part(node, snap, base)
		if _drive_visibility:
			var visible := int(snap[base + NovaSimulation.PF_HIDDEN]) == 0 and int(snap[base + NovaSimulation.PF_ALIVE]) == 1
			node.visible = visible
			if not visible:
				_stats.hidden += 1
		_apply_body_anim(node, int(snap[base + NovaSimulation.PF_ANIM_SLOT]))


# Position is already Godot-space (x, z, -y); rotation is mission-space degrees. Build the basis
# through the ONE placement convention (MissionObjectPlacer.bms_to_godot_basis) so a sim-driven entity
# sits exactly where placement would put it. Yaw-only today (pitch/roll arrive at 0; reserved seam).
func _apply_transform(node, snap: PackedFloat32Array, base: int) -> void:
	var pos := Vector3(
		snap[base + NovaSimulation.PF_POS_X],
		snap[base + NovaSimulation.PF_POS_Y],
		snap[base + NovaSimulation.PF_POS_Z])
	var rot := Vector3(
		snap[base + NovaSimulation.PF_PITCH_DEG],
		snap[base + NovaSimulation.PF_YAW_DEG],
		snap[base + NovaSimulation.PF_ROLL_DEG])
	node.transform = Transform3D(MissionObjectPlacer.bms_to_godot_basis(rot), pos)
	_stats.moved += 1


# PANM: the engine integrates each channel's phase (Entity_ApplyCommand @0x43ab60 case 0x22); the host
# only poses commanded channels, leaving an untouched channel at its default pose.
func _apply_procedural_part(node, snap: PackedFloat32Array, base: int) -> void:
	if not node.has_method("set_part_phase"):
		return
	if int(snap[base + NovaSimulation.PF_ACTIVE1]) == 1:
		node.set_part_phase(1, int(snap[base + NovaSimulation.PF_PHASE1]))
		_stats.posed += 1
	if int(snap[base + NovaSimulation.PF_ACTIVE2]) == 1:
		node.set_part_phase(2, int(snap[base + NovaSimulation.PF_PHASE2]))
		_stats.posed += 1


# DEFERRED SEAM. The main-body skeletal clip (.bad via .adm) selected by AI state/anim_slot. The
# skeletal runtime does not exist yet, so this only records the requested slot for inspection; when the
# runtime is built it will drive AnimMap_PlayAnimBySlot. See the class header for the IDA anchors.
func _apply_body_anim(node, anim_slot: int) -> void:
	if anim_slot < 0:
		return
	if node.has_method("set_meta"):
		node.set_meta("requested_anim_slot", anim_slot)
