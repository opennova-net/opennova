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
#  - Main-body skeletal (.bad via .adm): the primary infantry/view-model animation, selected by AI
#    state. The infantry motor exports both off_8135F0 anim_state and clip_phase in the present
#    snapshot, so this pass poses the model to the same .bad phase that produced root motion.
#    PF_BODY_ANIM_SLOT remains a coarse fallback for non-infantry/compat nodes.
#
# Per-entity visual contract (NovaEntityVisual): the present pass is host-agnostic and drives each
# resolved node through a small duck-typed surface (GDScript) that NovaObjectModel implements:
#   transform (Node3D), set_part_phase(channel, phase), visible (Node3D),
#   play_body_clip_at(key, phase_ticks) / play_body_anim_at(slot, phase_ticks).
# has_method guards keep non-animated/static nodes untouched. See docs/adr/0007.
#
# Targets resolve through ONE shared index (MissionEntityRegistry.resolve: bms_id primary, (kind,index)
# fallback). Host-agnostic, RefCounted, preload-referenced (same convention as MissionObjectPlacer).

const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")

var _sim                    # NovaSimulation (or a compatible snapshot source)
var _index                  # MissionEntityRegistry: resolve(bms_id, kind, index) -> Node
var _drive_transform := true
var _drive_part_anim := true
var _drive_visibility := true
var _stats: Dictionary = { "moved": 0, "posed": 0, "hidden": 0, "muzzles": 0 }


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
			# Death is not disappearance: a dead ORGANIC keeps rendering as a corpse
			# (its death anim holds the last frame) until the sim despawns it via
			# PF_HIDDEN — the corpse timer + the seen-by-the-local-player watch
			# [orig: Entity_UpdateInfantryAI @0x4b9c40 death edge / @0x4b9e4d corpse
			# block; world-wac-ai-re §19]. A dead NON-organic keeps rendering too:
			# the destruction pass swaps its model to the husk (Flags|=6), and a
			# def with no husk keeps its graphic standing — the witnessed render
			# pick [orig: Flags&4 && huskModel ? husk : graphic @0x413086; §24].
			var visible := int(snap[base + NovaSimulation.PF_HIDDEN]) == 0
			node.visible = visible
			if not visible:
				_stats.hidden += 1
		_apply_body_anim(node, snap, base)
		_push_muzzle(node, int(snap[base + NovaSimulation.PF_NET_ID]))


# The D-AI-6 muzzle seam: feed each posed model's gun-flash userpoint world position
# back to the sim so AI rounds/effects leave the GUN, not the chest-lift stand-in.
# Keyed by PF_NET_ID (the authored SSN): the present rows render the client wire
# view, whose row order is NOT the AI index, and whose wire handle is 0-ambiguous
# (pool-0 slot 0 packs to the "none" sentinel). Placed NPCs always carry an SSN;
# players carry net_id 0 and are never AI-fired. The skeleton pose applied this
# frame is read next frame (one-frame staleness, ledgered D-AI-6). [orig: the
# anim-event fire computes this inline — Entity_GetAttachmentWorldPosition
# @0x4b2670; our sim has no skeletal pose, so the present layer pushes it back]
func _push_muzzle(node, net_id: int) -> void:
	if net_id <= 0:
		return
	if not node.has_method("has_muzzle") or not node.has_muzzle():
		return
	_sim.set_ai_muzzle_world(net_id, node.get_muzzle_world_position())
	_stats.muzzles += 1


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


# Main-body skeletal clip (.bad via .adm). Infantry uses the full IDA anim state plus clip phase:
# the skeleton is posed to the exact phase that produced root motion, so idles stay planted instead
# of host-side free-running against a separately advanced root track. PF_BODY_ANIM_SLOT is the
# coarse fallback path for compatible non-infantry nodes.
func _apply_body_anim(node, snap: PackedFloat32Array, base: int) -> void:
	var anim_phase := int(snap[base + NovaSimulation.PF_ANIM_PHASE_TICKS])
	var anim_state := int(snap[base + NovaSimulation.PF_ANIM_STATE])
	if anim_state >= 0 and node.has_method("play_body_clip_at"):
		var key := NovaSimulation.infantry_anim_key(anim_state)
		if not key.is_empty():
			node.play_body_clip_at(key, anim_phase)
			return
	var body_anim_slot := int(snap[base + NovaSimulation.PF_BODY_ANIM_SLOT])
	if body_anim_slot < 0:
		return
	if node.has_method("play_body_anim_at"):
		node.play_body_anim_at(body_anim_slot, anim_phase)
		return
	if node.has_method("play_body_anim"):
		node.play_body_anim(body_anim_slot)
