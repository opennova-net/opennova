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
# here — it is drawn by LocalPlayerPresenter as the smooth, motor-driven local avatar L. That
# is the live §5.38b two-handle (L = local sim, H = wire identity) reconciliation.
#
# Shell-agnostic, RefCounted, preload-referenced (same convention as MissionPresentPass).
#
# This is the COLD-path facade: it owns spawn/defer/unresolved bookkeeping, the
# liveness prune, the held-weapon model builds, spawn callbacks and stats, and
# pushes the finished row plan into NovaPresentApplier, whose wire walk
# (nova_present_applier_wire.cpp) owns plan validity and the per-frame per-row
# hot path. The per-leg behavioral semantics and their [orig] witnesses are
# documented at the native walk — this facade keeps only the cold-path anchors.

const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")

var _sim                   # NovaSimulation (snapshot source)
var _placer                # MissionObjectPlacer (build_player_animated_model -> NovaObjectModel)
var _container: Node3D     # parent for spawned wire avatars
var _env_node              # optional NovaEnvironment node for model lighting globals
var _defer_index           # MissionEntityRegistry (host only): rows resolving to a PLACED node are
                           # left to MissionPresentPass; null on the joiner (render every wire row)
var _synthetic_origin_only := false
var _nodes := {}           # wire_handle -> Node3D
var _unresolved := {}      # wire_handle -> runtime type_id (don't retry same failed type each tick)
var _stats: Dictionary = { "spawned": 0, "unresolved": 0, "live": 0 }
var _node_spawned_callback := Callable()
# The native wire walk: plan validity + the per-row hot path live on the applier
# (one instance per pass; the mission pass facade owns its own separately).
var _applier := NovaPresentApplier.new()
const MAX_REMOTE_BODY_CATCHUP_TICKS := 31 # MissionRuntime.MAX_CATCHUP_TICKS
# Remote primary-channel blends are fixed-tick state, while this pass also runs
# on zero-tick render frames and once after a multi-tick catch-up batch. Consume
# the sim clock once per presented snapshot so every row advances by the exact
# logic-tick delta rather than by the number of render submissions.
var _last_present_logic_tick := -1
# The third-person held weapon per wire handle: {handle: Node3D} and the gfx3 each live
# node was built from, so a weapon switch rebuilds and an unarmed row frees. Kept beside
# _nodes rather than parented under the body: NovaObjectModel.rebuild() frees all of its
# children, so a child weapon would vanish on any body rebuild. The applier detects the
# ADM edge and calls _rebuild_held_weapon; these maps stay here for muzzle_world_for
# and test consumers.
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
	_last_present_logic_tick = -1
	_applier.setup_wire(_rebuild_held_weapon)


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
	_free_held_weapon(wire_handle)
	_applier.release_wire_handle(wire_handle)


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
	_applier.reset_wire_runtime_state()
	_last_present_logic_tick = -1
	_stats.live = 0


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
	if stride < NovaSimulation.PF_STRIDE:
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
			or not is_instance_valid(_container)
			or stride < NovaSimulation.PF_STRIDE):
		return
	var tick_delta := _consume_present_logic_tick_delta()
	# Packed handle zero is a valid pool-0 identity, so the numeric getter cannot
	# also carry presence. Fold the sim's explicit validity seam into a -1
	# sentinel: the row filter needs no separate flag, and the row-plan key then
	# distinguishes "no local player yet" from a genuine slot-0 local handle.
	var local_handle := int(_sim.get_local_player_wire_handle()) \
			if bool(_sim.has_local_player()) else -1
	var index_generation := _current_index_generation()
	if _applier.wire_plan_is_current(snap.size(), stride, layout_revision,
			index_generation, local_handle):
		_applier.present_wire_rows(snap, stride, tick_delta)
		return
	_applier.begin_wire_plan(layout_revision, stride, snap.size(),
			index_generation, local_handle)
	var count: int = snap.size() / stride
	var live := {}
	var spawned_rows: Array = []  # [node, runtime_kind, visual_item_id] per spawn
	for i in range(count):
		var base := i * stride
		var type_id := int(snap[base + NovaSimulation.PF_TYPE_ID])
		var handle := int(snap[base + NovaSimulation.PF_WIRE_HANDLE])
		# A zero type row is the joiner's self-filtered echo (H) or an unresolved record;
		# the local player handle is drawn by LocalPlayerPresenter. Packed handle zero is a
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
					_applier.append_wire_deferred(placed)
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
		_applier.append_wire_row(node, base, handle, spawned_now)
		if spawned_now:
			spawned_rows.append([node, runtime_kind, visual_item_id])
	_applier.present_wire_rows(snap, stride, tick_delta)
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


func _consume_present_logic_tick_delta() -> int:
	# Lightweight test/compatibility sources predate the clock seam. They retain
	# the historical one-fixed-tick-per-call contract; production NovaSimulation
	# always exposes its monotonic logic tick.
	if _sim == null or not _sim.has_method("get_logic_tick"):
		return 1
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
	if _defer_index != null and _defer_index.has_method("get_generation"):
		return int(_defer_index.get_generation())
	return 0


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


## Build (or free) this wire body's third-person gun model when its ADM changes —
## the applier's wire walk detects the edge and calls back here so the placer
## build, node naming, and the maps muzzle_world_for/tests consume stay on the
## facade; the per-frame rigid attach lives in the native walk.
## [orig: the model resolve off the equipped ADM — the sim already folded the
##  draw gate in: a hidden or unarmed body reports ADM 0]
func _rebuild_held_weapon(handle: int, adm: int) -> Node3D:
	var graphic := ""
	if adm > 0 and _sim != null and _sim.has_method("get_weapon_third_person_model"):
		graphic = String(_sim.get_weapon_third_person_model(adm))
	if graphic == String(_weapon_graphics.get(handle, "")):
		var existing: Variant = _weapon_nodes.get(handle)
		return existing as Node3D if is_instance_valid(existing) else null
	_free_held_weapon(handle)
	if not graphic.is_empty() and _placer != null:
		var built: Node3D = _placer.build_model_from_graphic(
				graphic, "", _container, "", _env_node)
		if built != null:
			built.name = "WireWeapon_%04x" % handle
			built.set_shadow_caster_enabled(true)
			_weapon_nodes[handle] = built
	_weapon_graphics[handle] = graphic
	var v: Variant = _weapon_nodes.get(handle)
	return v as Node3D if is_instance_valid(v) else null
