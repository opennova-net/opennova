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
const PresentAimOverlay := preload("res://engine/world/present_aim_overlay.gd")
const PresentEmplacedWeapon := preload(
		"res://engine/world/present_emplaced_weapon.gd")

var _sim                    # NovaSimulation (or a compatible snapshot source)
var _index                  # MissionEntityRegistry: resolve(bms_id, kind, index) -> Node
var _drive_transform := true
var _drive_part_anim := true
var _drive_visibility := true
# bms_id -> true for nodes the render-occlusion frame currently hides. Shared
# BY REFERENCE from the host (GameWorld mutates it in place) via the
# occlusion_hidden_ids setup option; consulted only on a hidden->visible write,
# never per steady row. Hosts without render occlusion leave it empty.
var _occlusion_hidden_ids: Dictionary = {}
var _stats: Dictionary = { "moved": 0, "posed": 0, "hidden": 0, "muzzles": 0 }
var _row_plan_revision := -1
var _row_plan_stride := 0
var _row_plan_snapshot_size := -1
var _row_plan_index_generation := -1
var _row_bases := PackedInt32Array()
var _row_handles := PackedInt32Array()
var _row_types := PackedInt32Array()
var _row_bms_ids := PackedInt32Array()
var _row_kinds := PackedInt32Array()
var _row_indices := PackedInt32Array()
var _row_nodes: Array = []

# Per-row node capabilities, resolved once at plan rebuild (has_method is a
# per-frame string lookup otherwise), plus last-applied edge state so calls
# whose node-side setter would no-op are never dispatched at all.
const CAP_BODY_CLIP := 1
const CAP_BODY_SLOT := 2
const CAP_BODY_PLAY := 4
const CAP_AIM := 8
const CAP_RHC := 16
const CAP_CTRL := 32
var _row_caps := PackedInt32Array()
var _row_aim_valid := PackedInt32Array()
var _row_rhc := PackedInt32Array()
# infantry_anim_key(state) allocates its String natively per call; the state->key
# map is global and tiny, so cache it for every pass instance.
static var _infantry_key_cache: Dictionary = {}


## options: { drive_transform, drive_part_anim, drive_visibility } (all default true) +
## occlusion_hidden_ids (the host's shared occlusion-claim set; absent = empty). The editor
## preview drives all three; the game drives all three too (its NPCs were previously static-placed).
func setup(sim, index, options: Dictionary = {}) -> void:
	_sim = sim
	_index = index
	_drive_transform = bool(options.get("drive_transform", true))
	_drive_part_anim = bool(options.get("drive_part_anim", true))
	_drive_visibility = bool(options.get("drive_visibility", true))
	var occlusion_ids: Variant = options.get("occlusion_hidden_ids")
	if occlusion_ids is Dictionary:
		_occlusion_hidden_ids = occlusion_ids


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
	var layout_revision := -1
	if _sim.has_method("get_present_layout_revision"):
		layout_revision = int(_sim.get_present_layout_revision())
	present_snapshot(snap, stride, layout_revision)


## Apply a snapshot already fetched by MissionRuntime. Compatible callers may
## keep using present(); a source without a layout revision simply rebuilds the
## routing plan every call.
func present_snapshot(
		snap: PackedFloat32Array, stride: int, layout_revision: int = -1) -> void:
	if _index == null or stride <= 0:
		return
	if not _row_plan_is_current(snap, stride, layout_revision):
		_rebuild_row_plan(snap, stride, layout_revision)
	for row in range(_row_nodes.size()):
		var base := int(_row_bases[row])
		var node: Variant = _row_nodes[row]
		if node == null or not is_instance_valid(node):
			# A freed cached node cannot be written through. The next call will
			# rebuild because validation rejects it.
			continue
		var caps := int(_row_caps[row])
		if _drive_transform:
			_apply_transform(node, snap, base)
		# Inline of PresentAimOverlay.apply(node, snap, base, false) with the
		# capability lookups hoisted into the row plan and the no-overlay clear
		# gated to the valid->invalid edge (the node-side setters no-op on
		# repeats; these gates skip the dispatch itself).
		if caps & CAP_RHC:
			var rhc := int(snap[base + NovaSimulation.PF_RIGHT_HAND_COLLAPSED])
			if rhc != int(_row_rhc[row]):
				node.set_right_hand_collapsed(rhc != 0)
				_row_rhc[row] = rhc
		if caps & CAP_AIM:
			var aim_valid := int(snap[base + NovaSimulation.PF_AIM_OVERLAY_VALID])
			if aim_valid != 0:
				PresentAimOverlay.apply_valid(node, snap, base, false)
			elif int(_row_aim_valid[row]) != 0:
				node.set_aim_overlay([])
			_row_aim_valid[row] = aim_valid
		if _drive_part_anim:
			if caps & CAP_CTRL:
				# Remove last tick's semantic mount ownership before generic model-order
				# channels run. A generic PLAYPARTANIM can itself address EWEAP_*; it
				# must survive dismount, while live gunner aim still overlays it last.
				PresentEmplacedWeapon.clear(node)
				_apply_procedural_part(node, snap, base)
				_stats.posed += PresentEmplacedWeapon.apply(node, snap, base, false)
			else:
				# No ctrl channels on this node: clear/apply are permanent no-ops.
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
			# Retail skips the local first-person UseGun parent's OWN world-model
			# submit once its embedded MountSlot is live and has an FP model (or
			# flags2 Invisible forces it). This packed bit is presentation-only:
			# Entity.hidden, collision, simulation, and separately-rendered attached
			# actors remain untouched. [orig: Entity_RenderVehicleModel @0x4407d0,
			# cull @0x4407f6..0x44084c, submit @0x440918]
			var visible := (
					int(snap[base + NovaSimulation.PF_HIDDEN]) == 0
					and int(snap[base +
							NovaSimulation.PF_LOCAL_VIEW_SUPPRESSED]) == 0)
			if node.visible != visible:
				# Two-bit visibility ownership: while the render-occlusion frame
				# claims this node (the shared hidden set), a sim-wants-visible
				# node stays hidden — occlusion releases through the same set and
				# lands the node on the sim's current intent, so neither writer
				# fights the other frame over frame.
				if not (visible
						and _occlusion_hidden_ids.has(int(_row_bms_ids[row]))):
					node.visible = visible
			if not visible:
				_stats.hidden += 1
		_apply_body_anim(node, snap, base, caps)
		var net_id := int(snap[base + NovaSimulation.PF_NET_ID])
		if net_id > 0:
			_push_muzzle(node, net_id)


func _current_index_generation() -> int:
	if _index != null and _index.has_method("get_generation"):
		return int(_index.get_generation())
	return 0


func _row_plan_is_current(
		snap: PackedFloat32Array, stride: int, layout_revision: int) -> bool:
	# No revision means a compatible fake/custom source: preserve the original
	# full-resolution behavior rather than trusting an unverifiable row order.
	if layout_revision < 0 \
			or _row_plan_revision != layout_revision \
			or _row_plan_stride != stride \
			or _row_plan_snapshot_size != snap.size() \
			or _row_plan_index_generation != _current_index_generation():
		return false
	for row in range(_row_nodes.size()):
		var base := int(_row_bases[row])
		if base < 0 or base + stride > snap.size():
			return false
		var node: Variant = _row_nodes[row]
		if node == null or not is_instance_valid(node):
			return false
		if (int(snap[base + NovaSimulation.PF_WIRE_HANDLE]) != int(_row_handles[row])
				or int(snap[base + NovaSimulation.PF_TYPE_ID]) != int(_row_types[row])
				or int(snap[base + NovaSimulation.PF_BMS_ID]) != int(_row_bms_ids[row])
				or int(snap[base + NovaSimulation.PF_KIND]) != int(_row_kinds[row])
				or int(snap[base + NovaSimulation.PF_INDEX]) != int(_row_indices[row])):
			return false
	return true


func _rebuild_row_plan(
		snap: PackedFloat32Array, stride: int, layout_revision: int) -> void:
	_row_bases.clear()
	_row_handles.clear()
	_row_types.clear()
	_row_bms_ids.clear()
	_row_kinds.clear()
	_row_indices.clear()
	_row_nodes.clear()
	_row_caps.clear()
	_row_aim_valid.clear()
	_row_rhc.clear()
	_row_plan_revision = layout_revision
	_row_plan_stride = stride
	_row_plan_snapshot_size = snap.size()
	_row_plan_index_generation = _current_index_generation()
	var count: int = snap.size() / stride
	for row in range(count):
		var base := row * stride
		var bms_id := int(snap[base + NovaSimulation.PF_BMS_ID])
		var kind := int(snap[base + NovaSimulation.PF_KIND])
		var index := int(snap[base + NovaSimulation.PF_INDEX])
		var node = _index.resolve(bms_id, kind, index)
		if node == null or not is_instance_valid(node):
			continue
		_row_bases.append(base)
		_row_handles.append(int(snap[base + NovaSimulation.PF_WIRE_HANDLE]))
		_row_types.append(int(snap[base + NovaSimulation.PF_TYPE_ID]))
		_row_bms_ids.append(bms_id)
		_row_kinds.append(kind)
		_row_indices.append(index)
		_row_nodes.append(node)
		# Capability lookups are stable per node script: resolve them once per
		# topology change so the per-frame loop never pays has_method() again.
		var caps := 0
		if node.has_method("play_body_clip_at"):
			caps |= CAP_BODY_CLIP
		if node.has_method("play_body_anim_at"):
			caps |= CAP_BODY_SLOT
		if node.has_method("play_body_anim"):
			caps |= CAP_BODY_PLAY
		if node.has_method("set_aim_overlay"):
			caps |= CAP_AIM
		if node.has_method("set_right_hand_collapsed"):
			caps |= CAP_RHC
		if node.has_method("set_ctrl_value") and node.has_method("clear_ctrl_value"):
			caps |= CAP_CTRL
		_row_caps.append(caps)
		# Last-applied edge state (-1 = unknown, first frame always applies).
		_row_aim_valid.append(-1)
		_row_rhc.append(-1)


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
	var entity_basis := MissionObjectPlacer.bms_to_godot_basis(rot)
	var root_basis := (PresentAimOverlay.root_basis(snap, base, entity_basis)
			if node.has_method("set_aim_overlay") else entity_basis)
	var next_transform := Transform3D(root_basis, pos)
	if node.transform != next_transform:
		node.transform = next_transform
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
func _apply_body_anim(node, snap: PackedFloat32Array, base: int, caps: int) -> void:
	var anim_state := int(snap[base + NovaSimulation.PF_ANIM_STATE])
	if anim_state >= 0 and (caps & CAP_BODY_CLIP):
		var key := _infantry_key(anim_state)
		if not key.is_empty():
			node.play_body_clip_at(
					key, int(snap[base + NovaSimulation.PF_ANIM_PHASE_TICKS]))
			return
	var body_anim_slot := int(snap[base + NovaSimulation.PF_BODY_ANIM_SLOT])
	if body_anim_slot < 0:
		return
	if caps & CAP_BODY_SLOT:
		node.play_body_anim_at(
				body_anim_slot, int(snap[base + NovaSimulation.PF_ANIM_PHASE_TICKS]))
		return
	if caps & CAP_BODY_PLAY:
		node.play_body_anim(body_anim_slot)


static func _infantry_key(state: int) -> String:
	var key = _infantry_key_cache.get(state)
	if key == null:
		key = NovaSimulation.infantry_anim_key(state)
		_infantry_key_cache[state] = key
	return key
