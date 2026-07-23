extends RefCounted

# THE joiner present pass: renders a co-op JOINER's remote entities WIRE-DIRECT.
#
# A non-authority client does not own the host's entities, so it cannot resolve them
# through the local MissionEntityRegistry the way the host listen-server's
# MissionPresentPass does (the wire handles live in the HOST's handle space). Instead it
# renders every replicated entity straight from the decoded wire stream — the faithful
# original-client model (the real client builds all dynamic entities from the S2C 0x0C
# spawn + 0x0A motion stream, never from a local .bms placement; docs/net §5.23/§5.25/§5.38b).
#
# It is the wire analog of MissionObjectPlacer + MissionPresentPass and the ClientState
# sibling of NetWorldView (which does the same over the older NovaNetClient model): it
# keeps one NovaObjectModel per wire handle, resolved by the wire type id, and updates
# each one's transform + visibility every tick from NovaSimulation.get_present_snapshot()
# (the SAME flat PF_* buffer the host present reads — the joiner just keys on PF_TYPE_ID /
# PF_WIRE_HANDLE instead of the registry-resolved PF_BMS_ID/KIND/INDEX).
#
# The joiner's OWN player (wire handle H) is already self-filtered out of the snapshot in
# present_snapshot_from_client_view (its row carries PF_TYPE_ID 0), so it is never built
# here — it is drawn by LocalPlayerHost as the smooth, motor-driven local avatar L. That
# is the live §5.38b two-handle (L = local sim, H = wire identity) reconciliation.
#
# Host-agnostic, RefCounted, preload-referenced (same convention as MissionPresentPass).
# Replicated infantry anim state and phase drive the same primary body clip as the host pass;
# the packed aim overlay is then applied to that clip in the same snapshot row.

const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")
const PresentAimOverlay := preload("res://engine/world/present_aim_overlay.gd")
const PresentEmplacedWeapon := preload(
		"res://engine/world/present_emplaced_weapon.gd")

var _sim                   # NovaSimulation (snapshot source)
var _placer                # MissionObjectPlacer (build_player_animated_model -> NovaObjectModel)
var _container: Node3D     # parent for spawned wire avatars
var _env_node              # optional NovaEnvironment node for model lighting globals
var _defer_index           # MissionEntityRegistry (host only): rows resolving to a PLACED node are
                           # left to MissionPresentPass; null on the joiner (render every wire row)
var _synthetic_origin_only := false
var _nodes := {}           # wire_handle -> Node3D
var _unresolved := {}      # wire_handle -> runtime type_id (don't retry same failed type each tick)
var _respawn_revisions := {} # wire_handle -> last presented dead->alive epoch
var _stats: Dictionary = { "spawned": 0, "unresolved": 0, "live": 0 }
var _node_spawned_callback := Callable()
var _row_plan_revision := -1
var _row_plan_stride := 0
var _row_plan_snapshot_size := -1
var _row_plan_index_generation := -1
var _row_plan_local_handle := -1
var _row_bases := PackedInt32Array()
var _row_handles := PackedInt32Array()
var _row_types := PackedInt32Array()
var _row_bms_ids := PackedInt32Array()
var _row_kinds := PackedInt32Array()
var _row_indices := PackedInt32Array()
var _row_nodes: Array = []
var _deferred_nodes: Array = []
# Capability bits + last-applied edge caches, hoisted into the row plan exactly like
# MissionPresentPass's #302 gating: the per-tick loop pays no has_method(), no aim
# clear on already-clear rows, and no body-anim re-dispatch (with its per-call
# state->key String) while the wire state is unchanged.
const CAP_AIM := 1
const CAP_CTRL := 2
const CAP_PART := 4
const CAP_REMOTE_BODY := 8
const CAP_BODY_CLIP := 16
const CAP_BODY_CLIP_AT := 32
const CAP_BODY_SLOT_AT := 64
const CAP_BODY_SLOT := 128
const CAP_RHC := 256
const CAP_WPN := 512
var _row_caps := PackedInt32Array()
var _row_aim_valid := PackedInt32Array()
var _row_rhc := PackedInt32Array()
# state id -> "anim_<name>" String, memoized once per process (the same cache
# MissionPresentPass carries): the native call allocates a fresh String per
# invocation, which on a joiner ran twice per row per frame.
static var _infantry_key_cache := {}

var _row_anim_state := PackedInt32Array()
var _row_anim_request := PackedInt32Array()
# The third-person held weapon per wire handle: {handle: Node3D} and the gfx3 each live
# node was built from, so a weapon switch rebuilds and an unarmed row frees. Kept beside
# _nodes rather than parented under the body: NovaObjectModel.rebuild() frees all of its
# children, so a child weapon would vanish on any body rebuild.
var _weapon_nodes := {}
var _weapon_graphics := {}


# Runtime handles encode the original entity pool in their high nibble. That
# pool, not PF_KIND's BMS-origin value, drives the retail item-effect gates; a
# joiner intentionally has no authoritative BMS origin and therefore receives
# PF_KIND=-1. [orig: pools 0/1/2/3 = organic/item/building/marker].
static func _mission_kind_for_wire_handle(handle: int) -> int:
	match WireHandle.pool(handle):
		0:
			return NovaMissionData.KIND_ORGANIC
		1:
			return NovaMissionData.KIND_ITEM
		2:
			return NovaMissionData.KIND_BUILDING
		3:
			return NovaMissionData.KIND_MARKER
		_:
			return -1


# defer_index: the MissionEntityRegistry — any wire row that resolves to a placed node is
# rendered by MissionPresentPass instead, so this pass only draws the un-placed rows. On the
# HOST that means admitted joiners; on the JOINER (which places the mission minus organics and
# stamps rows with the local defer identity for pools 1-3) it means players, streamed AI, and
# anything without a placed node.
func setup(sim, placer, container: Node3D, env_node = null, defer_index = null,
		options: Dictionary = {}) -> void:
	_sim = sim
	_placer = placer
	_container = container
	_env_node = env_node
	_defer_index = defer_index
	_synthetic_origin_only = bool(options.get("synthetic_origin_only", false))


func get_stats() -> Dictionary:
	return _stats.duplicate()


func get_stats_record() -> WirePresentStats:
	return WirePresentStats.new(
			int(_stats.get("live", 0)),
			int(_stats.get("spawned", 0)),
			int(_stats.get("unresolved", 0)))


## Resolve the live node owned by this wire presenter. Runtime-only entities
## have no authored BMS identity, so consumers such as destruction must use the
## same packed pool/slot handle that keys this pass.
func resolve_wire_handle(wire_handle: int) -> Node3D:
	var node_v: Variant = _nodes.get(wire_handle)
	# is_instance_valid FIRST: an `is` type check on an already-freed instance is a
	# script error (the world teardown frees the container's children before this
	# pass tears down, so freed entries here are an ordinary case, not a bug).
	return node_v as Node3D if is_instance_valid(node_v) and node_v is Node3D else null


func _free_wire_node(wire_handle: int) -> void:
	var node_v: Variant = _nodes.get(wire_handle)
	if is_instance_valid(node_v) and node_v is Node3D:
		(node_v as Node3D).queue_free()
	_nodes.erase(wire_handle)
	_respawn_revisions.erase(wire_handle)
	_free_held_weapon(wire_handle)


func _free_held_weapon(wire_handle: int) -> void:
	var weapon_v: Variant = _weapon_nodes.get(wire_handle)
	if is_instance_valid(weapon_v) and weapon_v is Node3D:
		(weapon_v as Node3D).queue_free()
	_weapon_nodes.erase(wire_handle)
	_weapon_graphics.erase(wire_handle)


func reset_runtime_state() -> void:
	for handle_v in _nodes.keys():
		_free_wire_node(int(handle_v))
	for handle_v in _weapon_nodes.keys():
		_free_held_weapon(int(handle_v))
	_weapon_nodes.clear()
	_weapon_graphics.clear()
	_nodes.clear()
	_unresolved.clear()
	_respawn_revisions.clear()
	_clear_row_plan()
	_stats.live = 0


func teardown() -> void:
	reset_runtime_state()


# Keep the wire-driven skeletal primary pose on the same projection path as
# MissionPresentPass. Infantry uses its exact state-to-clip key; compatible
# non-infantry nodes retain the coarse body-slot fallback. REMOTE-request rows
# (the joiner's wire players) skip re-dispatch entirely while the wire state is
# unchanged: the model consumes phase only on an accepted transition and already
# early-outs on a same-state request, so the skip is behavior-identical and
# avoids the per-call state->key String for every row every tick. Host-loopback
# rows (remote_request 0) keep per-tick dispatch — their playhead rides
# play_body_clip_at's phase.
static func _infantry_key(state: int) -> String:
	var key = _infantry_key_cache.get(state)
	if key == null:
		key = NovaSimulation.infantry_anim_key(state)
		_infantry_key_cache[state] = key
	return key


func _apply_body_anim_gated(
		node, snap: PackedFloat32Array, base: int, caps: int, row: int) -> void:
	var anim_state := int(snap[base + NovaSimulation.PF_ANIM_STATE])
	var remote_request_i := int(snap[base + NovaSimulation.PF_ANIM_REMOTE_REQUEST])
	var anim_pulse := int(snap[base + NovaSimulation.PF_ANIM_STATE_PULSE])
	if (row >= 0 and remote_request_i != 0 and anim_pulse < 0
			and anim_state == int(_row_anim_state[row])
			and remote_request_i == int(_row_anim_request[row])):
		return
	if row >= 0:
		_row_anim_state[row] = anim_state
		_row_anim_request[row] = remote_request_i
	var anim_phase := int(snap[base + NovaSimulation.PF_ANIM_PHASE_TICKS])
	var remote_request := remote_request_i != 0
	# A transition state that arrived and was overwritten within one decode fold
	# (several 0x0A datagrams can apply per render frame — a tapped prone roll is
	# on the wire for 1-2 ticks). Dispatch it FIRST so the model's arbitration
	# sees retail's per-record order: the locked roll accepts, the follow-up
	# state queues behind it and promotes at clip completion.
	# [orig: per-record remote anim apply @0x4c1153]
	if remote_request and anim_pulse >= 0 and (caps & CAP_REMOTE_BODY) != 0:
		var pulse_key := _infantry_key(anim_pulse)
		if not pulse_key.is_empty():
			node.apply_remote_body_state(anim_pulse, pulse_key,
					NovaSimulation.infantry_anim_flags(anim_pulse),
					int(snap[base + NovaSimulation.PF_ANIM_PULSE_TICKS]))
	if anim_state >= 0:
		var key := _infantry_key(anim_state)
		if not key.is_empty():
			# NovaObjectModel owns current/pending acceptance because it also owns
			# clip time and completion. Forward every raw wire request; the model
			# consumes player phase only on an accepted transition and starts a
			# queued state at tick zero. [orig: @0x4c0859/@0x4c11a6]
			if remote_request and (caps & CAP_REMOTE_BODY) != 0:
				node.apply_remote_body_state(anim_state, key,
						NovaSimulation.infantry_anim_flags(anim_state), anim_phase)
				return
			if remote_request and (caps & CAP_BODY_CLIP) != 0:
				node.play_body_clip(key)
				return
			# Host-loopback rows expose the authority's already-accepted CURRENT
			# state and playhead. Re-arbitrating that result can defer it for an
			# extra loop, so pose it directly as before.
			if (not remote_request and anim_phase >= 0
					and (caps & CAP_BODY_CLIP_AT) != 0):
				node.play_body_clip_at(key, anim_phase)
				return
			if not remote_request and (caps & CAP_BODY_CLIP) != 0:
				node.play_body_clip(key)
				return
	var body_anim_slot := int(snap[base + NovaSimulation.PF_BODY_ANIM_SLOT])
	if body_anim_slot < 0:
		return
	if anim_phase >= 0 and (caps & CAP_BODY_SLOT_AT) != 0:
		node.play_body_anim_at(body_anim_slot, anim_phase)
		return
	if (caps & CAP_BODY_SLOT) != 0:
		node.play_body_anim(body_anim_slot)


# Keep dynamically materialized items on the same generic PANM path as placed
# mission objects. Semantic EWEAP controls are overlaid after these model-order
# channels, exactly as MissionPresentPass does.
func _apply_procedural_part(node, snap: PackedFloat32Array, base: int) -> void:
	if not node.has_method("set_part_phase"):
		return
	if int(snap[base + NovaSimulation.PF_ACTIVE1]) == 1:
		node.set_part_phase(1, int(snap[base + NovaSimulation.PF_PHASE1]))
	if int(snap[base + NovaSimulation.PF_ACTIVE2]) == 1:
		node.set_part_phase(2, int(snap[base + NovaSimulation.PF_PHASE2]))


## Register the render-host seam for runtime consumers that follow a dynamically
## materialized wire entity (for example, an ITEMS.DEF attached effect). Existing
## nodes are replayed so registration is safe after the first present pass.
func set_node_spawned_callback(callback: Callable) -> void:
	_node_spawned_callback = callback
	if not _node_spawned_callback.is_valid():
		return
	for node_v in _nodes.values():
		var node := node_v as Node3D
		if node == null or not is_instance_valid(node):
			continue
		var ref: Dictionary = node.get_meta("entity_ref", {})
		_node_spawned_callback.call(node, int(ref.get("kind", -1)),
				int(ref.get("item_id", 0)))


## Spawn newly-seen wire entities, update every live one's transform + visibility, and free
## ones that left the stream. Called once per logic tick by the runtime driver (after the sim
## advances), exactly where MissionPresentPass.present() runs for the host.
func present() -> void:
	if _sim == null or _placer == null or _container == null or not is_instance_valid(_container):
		return
	var stride: int = _sim.get_present_stride()
	if stride <= 0:
		return
	var snap: PackedFloat32Array = _sim.get_present_snapshot()
	var layout_revision := -1
	if _sim.has_method("get_present_layout_revision"):
		layout_revision = int(_sim.get_present_layout_revision())
	present_snapshot(snap, stride, layout_revision)


## Present a snapshot already fetched by MissionRuntime. Compatible sources
## without a topology revision rebuild their routing plan every call.
func present_snapshot(
		snap: PackedFloat32Array, stride: int, layout_revision: int = -1) -> void:
	if (_sim == null or _placer == null or _container == null
			or not is_instance_valid(_container) or stride <= 0):
		return
	# Packed handle zero is a valid pool-0 identity, so the numeric getter cannot
	# also carry presence. Fold the sim's explicit validity seam into a -1
	# sentinel: the row filter needs no separate flag, and the row-plan key then
	# distinguishes "no local player yet" from a genuine slot-0 local handle.
	var local_handle := int(_sim.get_local_player_wire_handle()) \
			if bool(_sim.has_local_player()) else -1
	if _row_plan_is_current(snap, stride, layout_revision, local_handle):
		for row in range(_row_nodes.size()):
			_present_wire_row(
					_row_nodes[row], snap, int(_row_bases[row]),
					int(_row_handles[row]), false, 0, 0, row)
		return
	_begin_row_plan(snap, stride, layout_revision, local_handle)
	var count: int = snap.size() / stride
	var live := {}
	for i in range(count):
		var base := i * stride
		var type_id := int(snap[base + NovaSimulation.PF_TYPE_ID])
		var handle := int(snap[base + NovaSimulation.PF_WIRE_HANDLE])
		# A zero type row is the joiner's self-filtered echo (H) or an unresolved record;
		# the local player handle is drawn by LocalPlayerHost. Packed handle zero is a
		# valid pool-0 slot, so local_handle is -1 (never a wire value) with no local player.
		if type_id == 0 or handle == local_handle:
			continue
		if _synthetic_origin_only and not (
				int(snap[base + NovaSimulation.PF_KIND]) == 255
				and int(snap[base + NovaSimulation.PF_INDEX]) == 0xFFFFFF):
			continue
		var runtime_kind := _mission_kind_for_wire_handle(handle)
		var visual_item_id := type_id
		if _placer.has_method("resolve_player_visual_item_id"):
			visual_item_id = int(_placer.resolve_player_visual_item_id(type_id))
		# Defer any row that carries a PLACED .bms identity: the placed representation —
		# an individual node (animated entities, driven by MissionPresentPass) or a
		# static MultiMesh batch instance (which deliberately has NO per-entity node) —
		# owns the rendering, so this pass must not spawn a wire duplicate. The node
		# resolve is bookkeeping for row-plan validity, not the defer condition; a
		# batched static resolves to null and still defers. Rows without placed
		# identity (kind -1 wire-only rows, 255 runtime synthetics) render here.
		if _defer_index != null:
			var d_kind := int(snap[base + NovaSimulation.PF_KIND])
			var d_index := int(snap[base + NovaSimulation.PF_INDEX])
			if d_kind >= 0 and d_kind <= 3 and d_index >= 0 and d_index != 0xFFFFFF:
				var placed = _defer_index.resolve(
					int(snap[base + NovaSimulation.PF_BMS_ID]), d_kind, d_index)
				if placed != null and is_instance_valid(placed):
					_deferred_nodes.append(placed)
				continue
		live[handle] = true
		if _unresolved.has(handle):
			if int(_unresolved[handle]) == type_id:
				continue
			_unresolved.erase(handle)
		var node = _nodes.get(handle)
		if node != null and not _wire_node_matches_row(node, snap, base, type_id):
			_free_wire_node(handle)
			node = null
		var spawned_now := false
		if node == null or not is_instance_valid(node):
			# build_player_animated_model maps the player runtime type (0x14B9) to its visual
			# item and passes other organics through to build_animated_model — the SAME chain
			# the host uses for the local avatar and placed NPCs.
			node = _placer.build_player_animated_model(type_id, _container, _env_node)
			if node == null:
				_unresolved[handle] = type_id
				_stats.unresolved += 1
				continue
			node.name = "Wire_%04x" % handle
			node.set_meta("entity_ref", {
				"kind": runtime_kind,
				"origin_kind": int(snap[base + NovaSimulation.PF_KIND]),
				"index": int(snap[base + NovaSimulation.PF_INDEX]),
				"bms_id": int(snap[base + NovaSimulation.PF_BMS_ID]),
				"wire_handle": handle,
				"item_id": visual_item_id,
				"runtime_type_id": type_id,
			})
			_nodes[handle] = node
			_stats.spawned += 1
			spawned_now = true
		_append_row_plan(node, snap, base, handle, type_id)
		_present_wire_row(node, snap, base, handle, spawned_now,
				runtime_kind, visual_item_id, _row_nodes.size() - 1)
	_stats.live = live.size()
	for handle_v in _nodes.keys():
		var handle := int(handle_v)
		if not live.has(handle):
			_free_wire_node(handle)
	for handle_v in _unresolved.keys():
		if not live.has(int(handle_v)):
			_unresolved.erase(handle_v)


func _current_index_generation() -> int:
	if _defer_index != null and _defer_index.has_method("get_generation"):
		return int(_defer_index.get_generation())
	return 0


func _clear_row_plan() -> void:
	_row_plan_revision = -1
	_row_plan_stride = 0
	_row_plan_snapshot_size = -1
	_row_plan_index_generation = -1
	_row_plan_local_handle = -1
	_row_bases.clear()
	_row_handles.clear()
	_row_types.clear()
	_row_bms_ids.clear()
	_row_kinds.clear()
	_row_indices.clear()
	_row_nodes.clear()
	_deferred_nodes.clear()
	_row_caps.clear()
	_row_aim_valid.clear()
	_row_rhc.clear()
	_row_anim_state.clear()
	_row_anim_request.clear()


func _begin_row_plan(
		snap: PackedFloat32Array,
		stride: int,
		layout_revision: int,
		local_handle: int) -> void:
	_clear_row_plan()
	_row_plan_revision = layout_revision
	_row_plan_stride = stride
	_row_plan_snapshot_size = snap.size()
	_row_plan_index_generation = _current_index_generation()
	_row_plan_local_handle = local_handle


static func _node_caps(node: Variant) -> int:
	var caps := 0
	if node.has_method("set_aim_overlay"):
		caps |= CAP_AIM
	if node.has_method("set_ctrl_value") and node.has_method("clear_ctrl_value"):
		caps |= CAP_CTRL
	if node.has_method("set_part_phase"):
		caps |= CAP_PART
	if node.has_method("apply_remote_body_state"):
		caps |= CAP_REMOTE_BODY
	if node.has_method("play_body_clip"):
		caps |= CAP_BODY_CLIP
	if node.has_method("play_body_clip_at"):
		caps |= CAP_BODY_CLIP_AT
	if node.has_method("play_body_anim_at"):
		caps |= CAP_BODY_SLOT_AT
	if node.has_method("play_body_anim"):
		caps |= CAP_BODY_SLOT
	if node.has_method("set_right_hand_collapsed"):
		caps |= CAP_RHC
	if node.has_method("set_weapon_channel"):
		caps |= CAP_WPN
	return caps


func _append_row_plan(
		node: Variant,
		snap: PackedFloat32Array,
		base: int,
		handle: int,
		type_id: int) -> void:
	_row_bases.append(base)
	_row_handles.append(handle)
	_row_types.append(type_id)
	_row_bms_ids.append(int(snap[base + NovaSimulation.PF_BMS_ID]))
	_row_kinds.append(int(snap[base + NovaSimulation.PF_KIND]))
	_row_indices.append(int(snap[base + NovaSimulation.PF_INDEX]))
	_row_nodes.append(node)
	_row_caps.append(_node_caps(node))
	# Last-applied edge state (-1 = unknown, first hot frame always applies).
	_row_aim_valid.append(-1)
	_row_rhc.append(-1)
	_row_anim_state.append(-2)
	_row_anim_request.append(-1)


func _row_plan_is_current(
		snap: PackedFloat32Array,
		stride: int,
		layout_revision: int,
		local_handle: int) -> bool:
	# Without a source revision, preserve compatibility by taking the cold path.
	if layout_revision < 0 \
			or _row_plan_revision != layout_revision \
			or _row_plan_stride != stride \
			or _row_plan_snapshot_size != snap.size() \
			or _row_plan_index_generation != _current_index_generation() \
			or _row_plan_local_handle != local_handle:
		return false
	for row in range(_row_nodes.size()):
		var base := int(_row_bases[row])
		var handle := int(_row_handles[row])
		if base < 0 or base + stride > snap.size():
			return false
		var node: Variant = _row_nodes[row]
		if (node == null or not is_instance_valid(node)
				or _nodes.get(handle) != node):
			return false
		if (int(snap[base + NovaSimulation.PF_WIRE_HANDLE]) != handle
				or int(snap[base + NovaSimulation.PF_TYPE_ID]) != int(_row_types[row])
				or int(snap[base + NovaSimulation.PF_BMS_ID]) != int(_row_bms_ids[row])
				or int(snap[base + NovaSimulation.PF_KIND]) != int(_row_kinds[row])
				or int(snap[base + NovaSimulation.PF_INDEX]) != int(_row_indices[row])):
			return false
	for node_v in _deferred_nodes:
		if node_v == null or not is_instance_valid(node_v):
			return false
	return true


func _wire_node_matches_row(
		node: Variant,
		snap: PackedFloat32Array,
		base: int,
		type_id: int) -> bool:
	if node == null or not is_instance_valid(node):
		return false
	var existing_ref: Dictionary = node.get_meta("entity_ref", {})
	return (
			int(existing_ref.get("runtime_type_id", 0)) == type_id
			and int(existing_ref.get("origin_kind", -1)) ==
					int(snap[base + NovaSimulation.PF_KIND])
			and int(existing_ref.get("index", -1)) ==
					int(snap[base + NovaSimulation.PF_INDEX])
			and int(existing_ref.get("bms_id", 0)) ==
					int(snap[base + NovaSimulation.PF_BMS_ID]))


func _present_wire_row(
		node: Variant,
		snap: PackedFloat32Array,
		base: int,
		handle: int,
		spawned_now: bool,
		runtime_kind: int,
		visual_item_id: int,
		row: int = -1) -> void:
	var caps := int(_row_caps[row]) if row >= 0 else _node_caps(node)
	var respawn_revision := int(
			snap[base + NovaSimulation.PF_RESPAWN_REVISION])
	var respawned_since_present := (
			not spawned_now
			and _respawn_revisions.has(handle)
			and int(_respawn_revisions[handle]) != respawn_revision)
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
			if (caps & CAP_AIM) != 0 else entity_basis)
	var next_transform := Transform3D(root_basis, pos)
	if node.transform != next_transform:
		node.transform = next_transform
	# The mounted right-hand collapse rides its own packed field (the bundled
	# legacy apply() drove it); edge-gated to the value change like MissionPresentPass.
	if caps & CAP_RHC:
		var rhc := int(snap[base + NovaSimulation.PF_RIGHT_HAND_COLLAPSED])
		if row < 0 or rhc != int(_row_rhc[row]):
			node.set_right_hand_collapsed(rhc != 0)
		if row >= 0:
			_row_rhc[row] = rhc
	# Aim overlay, edge-gated like MissionPresentPass: apply while valid, clear only
	# on the valid->invalid edge instead of every tick.
	if caps & CAP_AIM:
		var aim_valid := int(snap[base + NovaSimulation.PF_AIM_OVERLAY_VALID])
		if aim_valid != 0:
			PresentAimOverlay.apply_valid(node, snap, base, false)
		elif row < 0 or int(_row_aim_valid[row]) != 0:
			node.set_aim_overlay([])
		if row >= 0:
			_row_aim_valid[row] = aim_valid
	if caps & CAP_CTRL:
		# Semantic mount ownership must clear before generic model-order channels
		# and re-apply after them — only meaningful on nodes with CTRL channels.
		PresentEmplacedWeapon.clear(node)
		if caps & CAP_PART:
			_apply_procedural_part(node, snap, base)
		PresentEmplacedWeapon.apply(node, snap, base, false)
	elif caps & CAP_PART:
		_apply_procedural_part(node, snap, base)
	if respawned_since_present and node.has_method("reset_remote_body_state"):
		node.reset_remote_body_state()
		if row >= 0:
			_row_anim_state[row] = -2 # force the next body-anim dispatch through
	_apply_body_anim_gated(node, snap, base, caps, row)
	# The upper-body weapon channel: the hold pose this player's held weapon and
	# scope state select. The sim derives the state (there is no anim id on the
	# wire — every observer re-derives it); -1 means this row has no channel this
	# frame, which clears any pose left over from the weapon it was holding before.
	# [orig: the selection Entity_UpdateInfantryPlayerBody @0x4b5dad, which retail
	#  runs for every player body it draws, not just the local one]
	if caps & CAP_WPN:
		var wpn_state := int(snap[base + NovaSimulation.PF_WPN_ANIM_STATE])
		node.set_weapon_channel(
				_infantry_key(wpn_state) if wpn_state >= 0 else "",
				int(snap[base + NovaSimulation.PF_WPN_PHASE_TICKS]))
	_update_held_weapon(handle, node, snap, base)
	_respawn_revisions[handle] = respawn_revision
	var next_visible := (
			int(snap[base + NovaSimulation.PF_HIDDEN]) == 0
			and int(snap[base +
					NovaSimulation.PF_LOCAL_VIEW_SUPPRESSED]) == 0)
	if node.visible != next_visible:
		node.visible = next_visible
	if spawned_now and _node_spawned_callback.is_valid():
		_node_spawned_callback.call(node, runtime_kind, visual_item_id)


## World position of a named userpoint on this wire body's HELD WEAPON — the anchor
## retail's adm-arm fire effect spawns at. The weapon model is drawn rigid at the
## attach transform, so a model-space userpoint just rides that transform; no bone
## walk is needed, unlike the character's own userpoints.
##
## Returns the body's own origin when the weapon, its model data or the named point
## cannot be resolved: retail's deepest fallback is the entity origin, NOT the wire
## fire position (which is the shooter's eye), so falling back to the caller's origin
## would defeat the point of anchoring at all.
## [orig: the rigid weapon draw @0x4e3d71; the userpoint fallback @0x401867..0x401887]
func muzzle_world_for(handle: int, userpoint: String) -> Vector3:
	var node_v: Variant = _nodes.get(handle)
	var body_origin := Vector3.INF
	if is_instance_valid(node_v) and node_v is Node3D:
		body_origin = (node_v as Node3D).global_transform.origin
	if userpoint.is_empty():
		return body_origin
	var weapon_v: Variant = _weapon_nodes.get(handle)
	if not is_instance_valid(weapon_v) or not (weapon_v is Node3D):
		return body_origin
	var weapon := weapon_v as Node3D
	if not weapon.visible or not weapon.has_method("get_object_data"):
		return body_origin
	var data = weapon.get_object_data()
	if data == null or not data.has_method("get_user_point_count"):
		return body_origin
	for i in range(int(data.get_user_point_count())):
		var info: Dictionary = data.get_user_point_info(i)
		if String(info.get("name", "")).nocasecmp_to(userpoint) == 0:
			return weapon.global_transform * Vector3(info.get("position", Vector3.ZERO))
	return body_origin


func entity_count() -> int:
	return _nodes.size()


## This body's third-person gun — retail's draw 5, for a remote player. The sim already
## folded the draw gate in: a hidden or unarmed body reports ADM 0, which is both our
## weapon table's null row and the original's own `if (entity->equippedAdmIndex)`
## precondition, so there is no separate visibility field to consult.
##
## The model is drawn RIGID (one matrix into every bone slot), so it needs no skeleton and
## no clip of its own; it is posed entirely by bone 16's joint plus the weapon's own attach
## basis, which is neither bone 16's rotation nor any aim-overlay class.
## [orig: BoneCallback_org0_World draw 5 @0x4e3c87..0x4e3d99; matrix @0x4b2180..0x4b22f8;
##  gate Entity_CanFireWeapon @0x4dcb10]
func _update_held_weapon(handle: int, node: Node3D, snap: PackedFloat32Array, base: int) -> void:
	var adm := int(snap[base + NovaSimulation.PF_HELD_WEAPON_ADM])
	var graphic := ""
	if adm > 0 and _sim != null and _sim.has_method("get_weapon_third_person_model"):
		graphic = String(_sim.get_weapon_third_person_model(adm))
	if graphic != String(_weapon_graphics.get(handle, "")):
		_free_held_weapon(handle)
		if not graphic.is_empty() and _placer != null:
			var built: Node3D = _placer.build_model_from_graphic(
					graphic, "", _container, "", _env_node)
			if built != null:
				built.name = "WireWeapon_%04x" % handle
				_weapon_nodes[handle] = built
		_weapon_graphics[handle] = graphic
	var weapon_v: Variant = _weapon_nodes.get(handle)
	if not is_instance_valid(weapon_v) or not (weapon_v is Node3D):
		return
	var weapon := weapon_v as Node3D
	if adm <= 0 or not node.visible:
		weapon.visible = false
		return
	var attach: Variant = PresentHeldWeapon.attach_transform(
			node,
			Vector3(
					snap[base + NovaSimulation.PF_HELD_WEAPON_PITCH_DEG],
					snap[base + NovaSimulation.PF_HELD_WEAPON_YAW_DEG],
					snap[base + NovaSimulation.PF_HELD_WEAPON_ROLL_DEG]),
			snap[base + NovaSimulation.PF_HELD_WEAPON_HAND_FRAME] != 0.0)
	if attach == null:
		weapon.visible = false
		return
	weapon.global_transform = attach as Transform3D
	weapon.visible = true
