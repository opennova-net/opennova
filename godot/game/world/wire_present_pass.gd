extends RefCounted

# THE joiner present pass: renders a co-op JOINER's remote entities WIRE-DIRECT.
#
# A production joiner has no authored placed-node identity table. Its native sim
# separately materializes streamed pools 1-3 at the HOST's exact packed handles for
# world-side gameplay, while remote pool-0 organics remain decoded client state. Visual
# pose still belongs to that decoded stream, so this pass renders every remote row from
# the load batches plus live S2C 0x0A instead of resolving a local .bms placement
# (docs/net §5.23/§5.25/§5.38b).
#
# It is the wire analog of MissionObjectPlacer + MissionPresentPass and the sole
# presenter for a decoded ClientState (the live Simulation joiner's
# ClientReplicaPipeline, ADR 0026). It keeps one
# ObjectModel per wire handle, resolved by the wire type id, and updates each
# transform + visibility from the source's shared PF_* snapshot contract.
#
# The joiner's OWN player (wire handle H) is already self-filtered out of the snapshot in
# present_snapshot_from_client_replicas (its row carries PF_TYPE_ID 0), so it is never built
# here — it is drawn by LocalPlayerPresenter as the smooth, motor-driven local avatar L. That
# is the live §5.38b two-handle (L = local sim, H = wire identity) reconciliation.
#
# Shell-agnostic, RefCounted, preload-referenced (same convention as MissionPresentPass).
#
# This is the COLD-path facade: it owns spawn/defer/unresolved bookkeeping, the
# liveness prune, the held-weapon model builds, spawn callbacks and stats, and
# pushes the finished row plan into PresentApplier, whose wire walk
# (nova_present_applier_wire.cpp) owns plan validity and the per-frame per-row
# hot path. The per-leg behavioral semantics and their [orig] witnesses are
# documented at the native walk — this facade keeps only the cold-path anchors.


var _sim: Simulation = null       # snapshot source
var _placer: MissionObjectPlacer = null
var _container: Node3D     # parent for spawned wire avatars
var _defer_index: EntityIndex = null  # host only: rows resolving to a PLACED node are
                           # left to MissionPresentPass; null on the joiner (render every wire row)
var _synthetic_origin_only := false
var _camera: Camera3D
var _camera_framed := false
var _nodes := {}           # wire_handle -> ObjectModel
var _unresolved := {}      # wire_handle -> runtime type_id (don't retry same failed type each tick)
var _stats: Dictionary = { "spawned": 0, "unresolved": 0, "live": 0, "pending": 0 }
var _node_spawned_callback := Callable()
# The native wire walk owns the plan keys/row validity and per-row hot path (one
# instance per pass; the mission pass facade owns its own separately). This
# facade additionally owns cold-plan completeness while materialization drains.
var _applier := PresentApplier.new()
const MAX_REMOTE_BODY_CATCHUP_TICKS := 31 # world::TickAccumulator::kMaxCatchupTicks (S14)
# Building one streamed model can synchronously load/assemble enough Godot
# resources to take a substantial part of a frame — a platform resource-assembly
# cost with no retail counterpart (retail materializes its world stream under
# the loading/DEATH hold). Cap this facade at a small batch per presentation
# call so a large cold topology cannot occupy the SceneTree thread until the
# host's reliable window and timeout expire. Four preserves atomic
# materialization for ordinary tiny dynamic cohorts while yielding hundreds-row
# streamed mission loads promptly. NetSessionDrive holds the join-admission
# edge until pending_spawn_count() drains to zero, so this pacing stays behind
# the hold and the revealed world is fully materialized, like retail's.
const DEFAULT_COLD_SPAWN_BUDGET := 4
var _cold_spawn_budget := DEFAULT_COLD_SPAWN_BUDGET
var _pending_spawn_count := 0
var _diagnostic_trace_path := ""
# Remote primary-channel blends are fixed-tick state, while this pass also runs
# on zero-tick render frames and once after a multi-tick catch-up batch. Consume
# the sim clock once per presented snapshot so every row advances by the exact
# logic-tick delta rather than by the number of render submissions.
var _last_present_logic_tick := -1
# The third-person held weapon per wire handle: {handle: Node3D} and the gfx3 each live
# node was built from, so a weapon switch rebuilds and an unarmed row frees. Kept beside
# _nodes rather than parented under the body: ObjectModel.rebuild() frees all of its
# children, so a child weapon would vanish on any body rebuild. The applier detects the
# ADM edge and calls _rebuild_held_weapon; these maps stay here for muzzle_world_for
# and test consumers.
var _weapon_nodes := {}    # wire_handle -> ObjectModel
var _weapon_graphics := {}


# Runtime handles encode the original entity pool in their high nibble. That
# pool, not PF_KIND's BMS-origin value, drives the retail item-effect gates; a
# joiner intentionally has no authoritative BMS origin and therefore receives
# PF_KIND=-1. [orig: pools 0/1/2/3 = organic/item/building/marker].
static func _mission_kind_for_wire_handle(handle: int) -> int:
	match WireHandle.pool(handle):
		0:
			return MissionData.KIND_ORGANIC
		1:
			return MissionData.KIND_ITEM
		2:
			return MissionData.KIND_BUILDING
		3:
			return MissionData.KIND_MARKER
		_:
			return -1


# defer_index: the EntityIndex — any wire row that resolves to an authored placed
# node is rendered by MissionPresentPass instead. On the HOST that leaves admitted joiners.
# A production header-only JOINER passes no defer index and draws every remote row; an
# explicit complete-BMS/debug join can still defer its authored nodes.
func setup(sim: Simulation, placer: MissionObjectPlacer, container: Node3D,
		defer_index: EntityIndex = null, options: Dictionary = {}) -> void:
	_sim = sim
	_placer = placer
	_container = container
	_defer_index = defer_index
	_synthetic_origin_only = bool(options.get("synthetic_origin_only", false))
	_cold_spawn_budget = maxi(1, int(options.get(
			"cold_spawn_budget", DEFAULT_COLD_SPAWN_BUDGET)))
	_pending_spawn_count = 0
	_diagnostic_trace_path = OS.get_environment("OPENNOVA_TRACE_WIRE_BUILDS")
	_camera = options.get("camera", null) as Camera3D
	_camera_framed = false
	_last_present_logic_tick = -1
	_applier.setup_wire(_rebuild_held_weapon)


func get_stats() -> Dictionary:
	return _stats.duplicate()


## Cold rows the budget deferred on the last presented frame. NetSessionDrive
## holds the join-admission edge until this drains to zero so the reveal never
## races the budgeted materialization.
func pending_spawn_count() -> int:
	return _pending_spawn_count


func get_stats_record() -> WirePresentStats:
	return WirePresentStats.new(
			int(_stats.get("live", 0)),
			int(_stats.get("spawned", 0)),
			int(_stats.get("unresolved", 0)))


## Resolve the live model owned by this wire presenter. Runtime-only entities
## have no authored BMS identity, so consumers such as destruction must use the
## same packed pool/slot handle that keys this pass. The map holds only
## NovaObjectModels; is_instance_valid guards LIVENESS (the world teardown frees
## the container's children before this pass tears down, so freed entries here
## are an ordinary case, not a bug).
func resolve_wire_handle(wire_handle: int) -> ObjectModel:
	var node_v: Variant = _nodes.get(wire_handle)
	return node_v if is_instance_valid(node_v) else null


func _free_wire_node(wire_handle: int) -> void:
	var node_v: Variant = _nodes.get(wire_handle)
	if is_instance_valid(node_v):
		(node_v as Node).queue_free()
	_nodes.erase(wire_handle)
	_free_held_weapon(wire_handle)
	_applier.release_wire_handle(wire_handle)


func _free_held_weapon(wire_handle: int) -> void:
	var weapon_v: Variant = _weapon_nodes.get(wire_handle)
	if is_instance_valid(weapon_v):
		(weapon_v as Node).queue_free()
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
	_applier.reset_wire_runtime_state()
	_pending_spawn_count = 0
	_last_present_logic_tick = -1
	_camera_framed = false
	_stats.live = 0
	_stats.pending = 0


func teardown() -> void:
	reset_runtime_state()


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
	if stride < Simulation.PF_STRIDE:
		return
	var snap: PackedFloat32Array = _sim.get_present_snapshot()
	present_snapshot(snap, stride, int(_sim.get_present_layout_revision()))


## Present a snapshot already fetched by MissionRuntime.
func present_snapshot(
		snap: PackedFloat32Array, stride: int, layout_revision: int) -> void:
	if (_sim == null or _placer == null or _container == null
			or not is_instance_valid(_container)
			or stride < Simulation.PF_STRIDE):
		return
	var tick_delta := _consume_present_logic_tick_delta()
	# Packed handle zero is a valid pool-0 identity, so the numeric getter cannot
	# also carry presence. Fold the sim's explicit validity seam into a -1
	# sentinel: the row filter needs no separate flag, and the row-plan key then
	# distinguishes "no local player yet" from a genuine slot-0 local handle.
	var local_handle := int(_sim.get_local_player_wire_handle()) \
			if bool(_sim.has_local_player()) else -1
	var index_generation := _current_index_generation()
	if _pending_spawn_count == 0 and _applier.wire_plan_is_current(
			snap.size(), stride, layout_revision,
			index_generation, local_handle):
		_applier.present_wire_rows(snap, stride, tick_delta)
		_frame_spectator_camera()
		return
	_applier.begin_wire_plan(layout_revision, stride, snap.size(),
			index_generation, local_handle)
	var count: int = snap.size() / stride
	var live := {}
	var spawned_rows: Array = []  # [node, runtime_kind, visual_item_id] per spawn
	var spawn_attempts := 0
	var pending_spawns := 0
	for i in range(count):
		var base := i * stride
		var type_id := int(snap[base + Simulation.PF_TYPE_ID])
		var handle := int(snap[base + Simulation.PF_WIRE_HANDLE])
		# A zero type row is the joiner's self-filtered echo (H) or an unresolved record;
		# the local player handle is drawn by LocalPlayerPresenter. Packed handle zero is a
		# valid pool-0 slot, so local_handle is -1 (never a wire value) with no local player.
		if type_id == 0 or handle == local_handle:
			continue
		if _synthetic_origin_only and not (
				int(snap[base + Simulation.PF_KIND]) == 255
				and int(snap[base + Simulation.PF_INDEX]) == 0xFFFFFF):
			continue
		var runtime_kind := _mission_kind_for_wire_handle(handle)
		var visual_item_id := _placer.resolve_player_visual_item_id(type_id)
		# Defer any row that carries a PLACED .bms identity: the placed representation —
		# an individual node (animated entities, driven by MissionPresentPass) or a
		# static MultiMesh batch instance (which deliberately has NO per-entity node) —
		# owns the rendering, so this pass must not spawn a wire duplicate. The node
		# resolve is bookkeeping for row-plan validity, not the defer condition; a
		# batched static resolves to null and still defers. Rows without placed
		# identity (kind -1 wire-only rows, 255 runtime synthetics) render here.
		if _defer_index != null:
			var d_kind := int(snap[base + Simulation.PF_KIND])
			var d_index := int(snap[base + Simulation.PF_INDEX])
			if d_kind >= 0 and d_kind <= 3 and d_index >= 0 and d_index != 0xFFFFFF:
				var placed := _defer_index.resolve(
					int(snap[base + Simulation.PF_BMS_ID]), d_kind, d_index)
				if placed != null:
					_applier.append_wire_deferred(placed)
				continue
		live[handle] = true
		if _unresolved.has(handle):
			if int(_unresolved[handle]) == type_id:
				continue
			_unresolved.erase(handle)
		var node: ObjectModel = _nodes.get(handle) \
				if is_instance_valid(_nodes.get(handle)) else null
		if node != null and not _wire_node_matches_row(node, snap, base, type_id):
			_free_wire_node(handle)
			node = null
		var spawned_now := false
		if node == null:
			# Continue the cheap scan after exhausting the budget: later live nodes
			# still need this frame's transform, and mismatched/retired nodes still
			# need prompt teardown. The omitted rows force another cold plan below.
			if spawn_attempts >= _cold_spawn_budget:
				pending_spawns += 1
				continue
			spawn_attempts += 1
			_trace_cold_build("begin", handle, type_id, visual_item_id)
			# build_player_animated_model maps the player runtime type (0x14B9) to its visual
			# item and passes other organics through to build_animated_model — the SAME chain
			# the host uses for the local avatar and placed NPCs.
			node = _placer.build_player_animated_model(type_id, _container)
			_trace_cold_build("end", handle, type_id, visual_item_id)
			if node == null:
				_unresolved[handle] = type_id
				_stats.unresolved += 1
				continue
			node.name = "Wire_%04x" % handle
			node.set_meta("entity_ref", {
				"kind": runtime_kind,
				"origin_kind": int(snap[base + Simulation.PF_KIND]),
				"index": int(snap[base + Simulation.PF_INDEX]),
				"bms_id": int(snap[base + Simulation.PF_BMS_ID]),
				"wire_handle": handle,
				"item_id": visual_item_id,
				"runtime_type_id": type_id,
			})
			_nodes[handle] = node
			_stats.spawned += 1
			spawned_now = true
		_applier.append_wire_row(node, base, handle, spawned_now)
		if spawned_now:
			spawned_rows.append([node, runtime_kind, visual_item_id])
	_applier.present_wire_rows(snap, stride, tick_delta)
	_pending_spawn_count = pending_spawns
	_stats.pending = pending_spawns
	# Spawn registration runs after the production transform is applied, exactly
	# as the inline cold walk ordered it.
	if _node_spawned_callback.is_valid():
		for row_v in spawned_rows:
			_node_spawned_callback.call(row_v[0], int(row_v[1]), int(row_v[2]))
	_stats.live = live.size()
	for handle_v in _nodes.keys():
		var handle := int(handle_v)
		if not live.has(handle):
			_free_wire_node(handle)
	for handle_v in _unresolved.keys():
		if not live.has(int(handle_v)):
			_unresolved.erase(handle_v)
	_frame_spectator_camera()


func _trace_cold_build(stage: String, handle: int, type_id: int, visual_item_id: int) -> void:
	if _diagnostic_trace_path.is_empty():
		return
	var mode := FileAccess.READ_WRITE if FileAccess.file_exists(_diagnostic_trace_path) \
			else FileAccess.WRITE_READ
	var file := FileAccess.open(_diagnostic_trace_path, mode)
	if file == null:
		return
	file.seek_end()
	file.store_line("%d %s handle=0x%04x type=0x%04x visual=%d nodes=%d pending_prev=%d" % [
		Time.get_ticks_msec(), stage, handle, type_id, visual_item_id,
		_nodes.size(), _pending_spawn_count])
	file.flush()


## Spectator-only one-shot overview. Live presenters omit the camera option and
## never enter this path.
func _frame_spectator_camera() -> void:
	if (_camera == null or _camera_framed or _nodes.is_empty()
			or _pending_spawn_count > 0):
		return
	var centroid := Vector3.ZERO
	var count := 0
	for node_v in _nodes.values():
		var node := node_v as Node3D
		if node == null or not is_instance_valid(node):
			continue
		centroid += node.global_position
		count += 1
	if count == 0:
		return
	_camera_framed = true
	centroid /= count
	_camera.global_position = centroid + Vector3(0.0, 90.0, 110.0)
	_camera.look_at(centroid, Vector3.UP)


func _consume_present_logic_tick_delta() -> int:
	var now := int(_sim.get_logic_tick())
	if _last_present_logic_tick < 0:
		_last_present_logic_tick = now
		return 0
	if now <= _last_present_logic_tick:
		# Equal means a render-only re-present. A lower value means the world was
		# restarted under the same presenter; rebase without fabricating ticks.
		_last_present_logic_tick = now
		return 0
	var delta := now - _last_present_logic_tick
	_last_present_logic_tick = now
	return mini(delta, MAX_REMOTE_BODY_CATCHUP_TICKS)


func _current_index_generation() -> int:
	return int(_defer_index.get_generation()) if _defer_index != null else 0


func _wire_node_matches_row(
		node: ObjectModel,
		snap: PackedFloat32Array,
		base: int,
		type_id: int) -> bool:
	var existing_ref: Dictionary = node.get_meta("entity_ref", {})
	return (
			int(existing_ref.get("runtime_type_id", 0)) == type_id
			and int(existing_ref.get("origin_kind", -1)) ==
					int(snap[base + Simulation.PF_KIND])
			and int(existing_ref.get("index", -1)) ==
					int(snap[base + Simulation.PF_INDEX])
			and int(existing_ref.get("bms_id", 0)) ==
					int(snap[base + Simulation.PF_BMS_ID]))


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
	var body := resolve_wire_handle(handle)
	var body_origin := body.global_transform.origin if body != null else Vector3.INF
	if userpoint.is_empty():
		return body_origin
	var weapon_v: Variant = _weapon_nodes.get(handle)
	if not is_instance_valid(weapon_v):
		return body_origin
	var weapon: ObjectModel = weapon_v
	if not weapon.visible:
		return body_origin
	var data: ObjectData = weapon.get_object_data()
	if data == null:
		return body_origin
	for i in range(data.get_user_point_count()):
		var info: Dictionary = data.get_user_point_info(i)
		if String(info.get("name", "")).nocasecmp_to(userpoint) == 0:
			return weapon.global_transform * Vector3(info.get("position", Vector3.ZERO))
	return body_origin


func entity_count() -> int:
	return _nodes.size()


## The live held-weapon model for a wire body (null when unarmed/freed) — the
## typed read-back muzzle consumers and tests share.
func held_weapon_node(wire_handle: int) -> ObjectModel:
	var weapon_v: Variant = _weapon_nodes.get(wire_handle)
	return weapon_v if is_instance_valid(weapon_v) else null


## Build (or free) this wire body's third-person gun model when its ADM changes —
## the applier's wire walk detects the edge and calls back here so the placer
## build, node naming, and the maps muzzle_world_for/tests consume stay on the
## facade; the per-frame rigid attach lives in the native walk.
## [orig: the model resolve off the equipped ADM — the sim already folded the
##  draw gate in: a hidden or unarmed body reports ADM 0]
func _rebuild_held_weapon(handle: int, adm: int) -> Node3D:
	var graphic := ""
	if adm > 0 and _sim != null:
		graphic = String(_sim.get_weapon_third_person_model(adm))
	if graphic == String(_weapon_graphics.get(handle, "")):
		var existing: Variant = _weapon_nodes.get(handle)
		return existing if is_instance_valid(existing) else null
	_free_held_weapon(handle)
	if not graphic.is_empty() and _placer != null:
		var built := _placer.build_model_from_graphic(graphic, "", _container, "")
		if built != null:
			built.name = "WireWeapon_%04x" % handle
			built.set_shadow_caster_enabled(true)
			_weapon_nodes[handle] = built
	_weapon_graphics[handle] = graphic
	var v: Variant = _weapon_nodes.get(handle)
	return v if is_instance_valid(v) else null
