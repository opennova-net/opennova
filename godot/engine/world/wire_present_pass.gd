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


# defer_index: on the HOST, the MissionEntityRegistry — any wire row that resolves to a placed
# node is rendered by MissionPresentPass instead, so this pass only draws the un-placed remote
# players (admitted joiners). Pass null on the JOINER, where nothing is placed (render all).
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


## Resolve the live node owned by this wire presenter. Runtime-only entities
## have no authored BMS identity, so consumers such as destruction must use the
## same packed pool/slot handle that keys this pass.
func resolve_wire_handle(wire_handle: int) -> Node3D:
	var node_v: Variant = _nodes.get(wire_handle)
	return node_v as Node3D if node_v is Node3D and is_instance_valid(node_v) else null


func _free_wire_node(wire_handle: int) -> void:
	var node_v: Variant = _nodes.get(wire_handle)
	if node_v is Node3D and is_instance_valid(node_v):
		(node_v as Node3D).queue_free()
	_nodes.erase(wire_handle)
	_respawn_revisions.erase(wire_handle)


func reset_runtime_state() -> void:
	for handle_v in _nodes.keys():
		_free_wire_node(int(handle_v))
	_nodes.clear()
	_unresolved.clear()
	_respawn_revisions.clear()
	_clear_row_plan()
	_stats.live = 0


func teardown() -> void:
	reset_runtime_state()


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
					int(_row_handles[row]), false, 0, 0)
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
		# Host: defer any wire row that resolves to a PLACED mission node to MissionPresentPass,
		# so a placed NPC isn't drawn twice. The joiner passes no index and renders every row.
		if _defer_index != null:
			var placed = _defer_index.resolve(
				int(snap[base + NovaSimulation.PF_BMS_ID]),
				int(snap[base + NovaSimulation.PF_KIND]),
				int(snap[base + NovaSimulation.PF_INDEX]))
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
				runtime_kind, visual_item_id)
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
		visual_item_id: int) -> void:
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
			if node.has_method("set_aim_overlay") else entity_basis)
	var next_transform := Transform3D(root_basis, pos)
	if node.transform != next_transform:
		node.transform = next_transform
	PresentAimOverlay.apply(node, snap, base, false)
	PresentEmplacedWeapon.clear(node)
	_apply_procedural_part(node, snap, base)
	PresentEmplacedWeapon.apply(node, snap, base, false)
	if respawned_since_present and node.has_method("reset_remote_body_state"):
		node.reset_remote_body_state()
	_apply_body_anim(node, snap, base)
	_respawn_revisions[handle] = respawn_revision
	var next_visible := (
			int(snap[base + NovaSimulation.PF_HIDDEN]) == 0
			and int(snap[base +
					NovaSimulation.PF_LOCAL_VIEW_SUPPRESSED]) == 0)
	if node.visible != next_visible:
		node.visible = next_visible
	if spawned_now and _node_spawned_callback.is_valid():
		_node_spawned_callback.call(node, runtime_kind, visual_item_id)


func entity_count() -> int:
	return _nodes.size()
