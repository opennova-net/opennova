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
# [orig: EntityPool_FindByNetId @ 0x4f0a20]
const WIRE_HANDLE_POOL_SHIFT := 12
const WIRE_HANDLE_POOL_MASK := 0xF

var _sim                   # NovaSimulation (snapshot source)
var _placer                # MissionObjectPlacer (build_player_animated_model -> NovaObjectModel)
var _container: Node3D     # parent for spawned wire avatars
var _env_node              # optional NovaEnvironment node for model lighting globals
var _defer_index           # MissionEntityRegistry (host only): rows resolving to a PLACED node are
                           # left to MissionPresentPass; null on the joiner (render every wire row)
var _nodes := {}           # wire_handle -> Node3D
var _unresolved := {}      # wire_handle -> true (type didn't resolve; don't retry each tick)
var _respawn_revisions := {} # wire_handle -> last presented dead->alive epoch
var _stats: Dictionary = { "spawned": 0, "unresolved": 0, "live": 0 }
var _node_spawned_callback := Callable()


# Runtime handles encode the original entity pool in their high nibble. That
# pool, not PF_KIND's BMS-origin value, drives the retail item-effect gates; a
# joiner intentionally has no authoritative BMS origin and therefore receives
# PF_KIND=-1. [orig: pools 0/1/2/3 = organic/item/building/marker].
static func _mission_kind_for_wire_handle(handle: int) -> int:
	match (handle >> WIRE_HANDLE_POOL_SHIFT) & WIRE_HANDLE_POOL_MASK:
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


# defer_index: on the HOST, the MissionEntityRegistry — any wire row that resolves to a placed
# node is rendered by MissionPresentPass instead, so this pass only draws the un-placed remote
# players (admitted joiners). Pass null on the JOINER, where nothing is placed (render all).
func setup(sim, placer, container: Node3D, env_node = null, defer_index = null) -> void:
	_sim = sim
	_placer = placer
	_container = container
	_env_node = env_node
	_defer_index = defer_index


func get_stats() -> Dictionary:
	return _stats.duplicate()


# Keep the wire-driven skeletal primary pose on the same projection path as
# MissionPresentPass. Infantry uses its exact state-to-clip key; compatible
# non-infantry nodes retain the coarse body-slot fallback.
func _apply_body_anim(node, snap: PackedFloat32Array, base: int) -> void:
	var anim_phase := int(snap[base + NovaSimulation.PF_ANIM_PHASE_TICKS])
	var anim_state := int(snap[base + NovaSimulation.PF_ANIM_STATE])
	var remote_request := (
			int(snap[base + NovaSimulation.PF_ANIM_REMOTE_REQUEST]) != 0)
	if anim_state >= 0:
		var key := NovaSimulation.infantry_anim_key(anim_state)
		if not key.is_empty():
			# NovaObjectModel owns current/pending acceptance because it also owns
			# clip time and completion. Forward every raw wire request; the model
			# consumes player phase only on an accepted transition and starts a
			# queued state at tick zero. [orig: @0x4c0859/@0x4c11a6]
			if remote_request and node.has_method("apply_remote_body_state"):
				node.apply_remote_body_state(anim_state, key,
						NovaSimulation.infantry_anim_flags(anim_state), anim_phase)
				return
			if remote_request and node.has_method("play_body_clip"):
				node.play_body_clip(key)
				return
			# Host-loopback rows expose the authority's already-accepted CURRENT
			# state and playhead. Re-arbitrating that result can defer it for an
			# extra loop, so pose it directly as before.
			if (not remote_request and anim_phase >= 0
					and node.has_method("play_body_clip_at")):
				node.play_body_clip_at(key, anim_phase)
				return
			if not remote_request and node.has_method("play_body_clip"):
				node.play_body_clip(key)
				return
	var body_anim_slot := int(snap[base + NovaSimulation.PF_BODY_ANIM_SLOT])
	if body_anim_slot < 0:
		return
	if anim_phase >= 0 and node.has_method("play_body_anim_at"):
		node.play_body_anim_at(body_anim_slot, anim_phase)
		return
	if node.has_method("play_body_anim"):
		node.play_body_anim(body_anim_slot)


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
	var count: int = snap.size() / stride
	var local_handle := int(_sim.get_local_player_wire_handle())  # the host's own player (drawn by LocalPlayerHost)
	var live := {}
	for i in range(count):
		var base := i * stride
		var type_id := int(snap[base + NovaSimulation.PF_TYPE_ID])
		var handle := int(snap[base + NovaSimulation.PF_WIRE_HANDLE])
		# A zero type/handle row is the joiner's self-filtered echo (H) or an unresolved record;
		# the local player handle is the host's own pool-0 player (drawn by LocalPlayerHost). Skip
		# both — on the joiner local_handle is L, which never appears in the wire stream (harmless).
		if type_id == 0 or handle == 0 or handle == local_handle:
			continue
		var runtime_kind := _mission_kind_for_wire_handle(handle)
		# Host: defer any wire row that resolves to a PLACED mission node to MissionPresentPass,
		# so a placed NPC isn't drawn twice. The joiner passes no index and renders every row.
		if _defer_index != null:
			var placed = _defer_index.resolve(
				int(snap[base + NovaSimulation.PF_BMS_ID]),
				int(snap[base + NovaSimulation.PF_KIND]),
				int(snap[base + NovaSimulation.PF_INDEX]))
			if placed != null and is_instance_valid(placed):
				continue
		live[handle] = true
		if _unresolved.has(handle):
			continue
		var node = _nodes.get(handle)
		var spawned_now := false
		if node == null or not is_instance_valid(node):
			# build_player_animated_model maps the player runtime type (0x14B9) to its visual
			# item and passes other organics through to build_animated_model — the SAME chain
			# the host uses for the local avatar and placed NPCs.
			node = _placer.build_player_animated_model(type_id, _container, _env_node)
			if node == null:
				_unresolved[handle] = true
				_stats.unresolved += 1
				continue
			node.name = "Wire_%04x" % handle
			node.set_meta("entity_ref", {
				"kind": runtime_kind,
				"origin_kind": int(snap[base + NovaSimulation.PF_KIND]),
				"index": int(snap[base + NovaSimulation.PF_INDEX]),
				"bms_id": int(snap[base + NovaSimulation.PF_BMS_ID]),
				"wire_handle": handle,
				"item_id": type_id,
			})
			_nodes[handle] = node
			_stats.spawned += 1
			spawned_now = true
		var respawn_revision := int(
				snap[base + NovaSimulation.PF_RESPAWN_REVISION])
		var respawned_since_present := (
				not spawned_now
				and _respawn_revisions.has(handle)
				and int(_respawn_revisions[handle]) != respawn_revision)
		# Position is already Godot-space (x, z, -y); yaw is mission-space degrees. Build the
		# basis through the ONE placement convention so a wire entity sits exactly where a
		# placed/host-present entity would. Yaw-only (pitch/roll arrive 0 for infantry).
		var pos := Vector3(
			snap[base + NovaSimulation.PF_POS_X],
			snap[base + NovaSimulation.PF_POS_Y],
			snap[base + NovaSimulation.PF_POS_Z])
		var rot := Vector3(
			snap[base + NovaSimulation.PF_PITCH_DEG],
			snap[base + NovaSimulation.PF_YAW_DEG],
			snap[base + NovaSimulation.PF_ROLL_DEG])
		node.transform = Transform3D(MissionObjectPlacer.bms_to_godot_basis(rot), pos)
		PresentAimOverlay.apply(node, snap, base)
		PresentEmplacedWeapon.apply(node, snap, base)
		# Respawn begins a fresh remote animation epoch. The decoded revision can
		# advance even when death and respawn frames were folded by one network
		# pump, so compare revisions rather than looking for a rendered dead row.
		if respawned_since_present and node.has_method("reset_remote_body_state"):
			node.reset_remote_body_state()
		_apply_body_anim(node, snap, base)
		_respawn_revisions[handle] = respawn_revision
		# Host-side dynamic mount targets reach this pass instead of the placed
		# MissionPresentPass, so consume the same retail local-view cull verdict.
		# Joiner snapshots leave the bit clear. Death alone does not suppress the
		# world model: retail leaves a non-hidden corpse visible; flag bit 0 is the
		# authoritative lifecycle visibility gate.
		node.visible = (
				int(snap[base + NovaSimulation.PF_HIDDEN]) == 0
				and int(snap[base +
						NovaSimulation.PF_LOCAL_VIEW_SUPPRESSED]) == 0)
		if spawned_now and _node_spawned_callback.is_valid():
			_node_spawned_callback.call(node, runtime_kind, type_id)
	_stats.live = live.size()
	for h in _nodes.keys():
		if not live.has(h):
			_nodes[h].queue_free()
			_nodes.erase(h)
			_respawn_revisions.erase(h)


func entity_count() -> int:
	return _nodes.size()
