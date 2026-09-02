class_name ItemEffectDirector
extends RefCounted

# The per-item ITEMS.DEF particle-effect director, extracted from GameWorld:
# the mission-lifetime owner of the entity-attached / static / controller-gated
# item emitters, the owner-registered effect-anchor resolvers, and the retail
# master particle switch. Plain RefCounted on the internal-member pattern
# (DebugViewSet/NetSessionDrive, minus tree presence: it owns no Nodes and runs
# no coroutines — the item-FX emitters attach to game nodes through the effect
# world). Owned by GameWorld as _item_fx, constructed in the world's _init and
# wired once through setup(). GameWorld keeps one-line public delegates
# (set_particles_hidden, register_effect_anchor/unregister_effect_anchor) so
# the owner-facing names never moved.
#
# Shared state is reached through the world's PUBLIC surface —
# get_effect_world() / get_runtime() / get_node_or_null — with TWO lent
# private seams arriving as setup() Callables, null-guarded by the world (the
# DebugViewSet two-Callable precedent): the placer's static item-effect
# sources, and the placer's ItemDatabase (typed at the resolve boundary,
# ADR 0034 — harnesses hand real fixture databases).

const MissionPresentation := preload("res://game/world/mission_presentation.gd")

# The attach scan reads only a model's first 16 userpoints — the witness
# ([orig: ItemDef_GetBoneMaskByName @ 0x49ea40]) lives at engine
# formats/threedi threedi_3di3.h THREEDI_USER_POINT_SCAN_LIMIT.
const ITEM_EFFECT_USER_POINT_SCAN_LIMIT := Simulation.ITEM_USER_POINT_SCAN_LIMIT

# The GameWorld whose entities carry the effects (public surface only; see
# above).
var _world: GameWorld
var _static_sources := Callable()  # () -> Array (the placer's static item-effect sources)
var _item_db_source := Callable()  # () -> ItemDatabase or null (the placer's db, lent by the world)

# Debug: hide every particle effect (the dev tools' "Hide particles" — the retail
# master particle switch, mimicked). Off by default; survives mission reloads.
var _particles_hidden := false

# owner key -> Callable returning the live anchor Transform3D (or null once
# stale) for registered owner-bound effect groups; consulted before the
# item-fx/SSN legs by _effect_owner_transform.
var _effect_anchor_resolvers: Dictionary = {}

# owner key (String) -> presented Node3D, for the per-item attached effect groups.
var _item_fx_nodes: Dictionary = {}
# owner key -> copied entity_ref value identity (bms/origin or wire handle).
var _item_fx_owner_refs: Dictionary = {}
var _item_fx_registered_nodes: Dictionary = {}
var _item_fx_pending_nodes: Dictionary = {}
var _item_fx_registered_static: Dictionary = {}
var _item_fx_pending_static: Dictionary = {}
# Controller/Driver-only PlayerControl item effects are dormant at mission
# startup. Portable lifecycle events activate them without polling.
# identity alias -> true while the vehicle has any controlling occupant
var _item_fx_control_active: Dictionary = {}
# Node instance id -> {node, kind, item_id, aliases}
var _item_fx_control_nodes: Dictionary = {}
# Node instance id -> {group_ids, owner_keys}
var _item_fx_control_instances: Dictionary = {}


## One-time wiring from the owning GameWorld: the world whose public surface
## the director resolves through, and the two lent private seams (the placer's
## get_static_item_effect_sources and get_item_db, null-guarded by the world).
func setup(world: GameWorld, static_sources: Callable,
		item_db_source: Callable) -> void:
	_world = world
	_static_sources = static_sources
	_item_db_source = item_db_source


# The placer's item database through the lent seam (null before a mission /
# with no placer).
func _resolve_item_db() -> ItemDatabase:
	return _item_db_source.call() if _item_db_source.is_valid() else null


## The retail master particle switch (the dev tools' "Hide particles"), delegated
## from the world: flips the effect world's spawn facade and, on re-enable,
## retries the deferred persistent item effects exactly once.
func set_particles_hidden(hidden: bool) -> void:
	var was_hidden := _particles_hidden
	_particles_hidden = hidden
	var effect_world: EffectWorld = _world.get_effect_world()
	if effect_world != null:
		effect_world.set_particles_hidden(hidden)
	if was_hidden and not hidden:
		_retry_pending_item_effects()


## The persisted switch state: the world re-asserts it onto each freshly built
## EffectWorld (the preference survives mission reloads).
func particles_hidden() -> bool:
	return _particles_hidden


## An owner registers a live pose resolver for an owner-bound effect group it
## spawned (e.g. the local muzzle flash riding the viewmodel userpoint). The
## resolver is polled by the effect world's owner-pose sync while any group
## bound to owner_key is alive; re-registering the same key overwrites.
func register_effect_anchor(owner_key: Variant, resolver: Callable) -> void:
	_effect_anchor_resolvers[owner_key] = resolver


func unregister_effect_anchor(owner_key: Variant) -> void:
	_effect_anchor_resolvers.erase(owner_key)


## The world just built a fresh EffectWorld for a mission (_start_effect_world):
## wire the owner-pose provider, attach the persistent per-item effects, and
## hook the wire-spawn callback so late net spawns get their authored emitters.
func on_effect_world_started() -> void:
	var effect_world: EffectWorld = _world.get_effect_world()
	if effect_world == null:
		return
	# One provider for every owned/attached group: int keys are WAC fx2ssn SSNs
	# (resolved through the runtime), String keys are the per-item effect attaches
	# (resolved to the placed node's live transform).
	effect_world.set_owner_position_provider(_effect_owner_transform)
	reattach()
	# The wire-spawn callback is single-subscriber; GameWorld registers one
	# router that fans out to this director AND the effect-light director.


## Mission unload: forget every owner key / pending record. Per-item
## attached-effect owner keys reference nodes in the freed MissionObjects
## container — never let a reload's provider resolve against freed instances.
## The anchor resolvers are deliberately NOT cleared: their owners
## (LocalPlayerPresenter, the present passes) unregister their own keys, exactly as
## before the extraction; _particles_hidden survives reloads by design.
func reset() -> void:
	_item_fx_nodes.clear()
	_item_fx_owner_refs.clear()
	_item_fx_registered_nodes.clear()
	_item_fx_pending_nodes.clear()
	_item_fx_registered_static.clear()
	_item_fx_pending_static.clear()
	_item_fx_control_active.clear()
	_item_fx_control_nodes.clear()
	_item_fx_control_instances.clear()


# Owner-transform provider for the effect world's owned/attached groups. Int keys are
# WAC fx2ssn SSNs (the runtime resolves the live entity transform; null = entity gone,
# the group detaches [orig: CEffect_UpdateEmitterTransform @ 0x5f7410]); String keys
# are the per-item effect attaches registered by reattach (the placed
# entity's current value snapshot. The Node remains only as a pre-first-tick
# seed and lifetime fallback for non-sim-owned callers.
func _effect_owner_transform(owner_key: Variant) -> Variant:
	# Registered live anchors first (the local weapon flash follows its viewmodel
	# userpoint for the emitter group's whole life [orig: the actionEffectHandle
	# per-tick tracker in WeaponAction_ProcessFrame @ 0x540edf]).
	var anchor: Variant = _effect_anchor_resolvers.get(owner_key)
	if anchor is Callable:
		var resolver := anchor as Callable
		if resolver.is_valid():
			return resolver.call()
		_effect_anchor_resolvers.erase(owner_key)
		return null
	if owner_key is String:
		# Untyped on purpose: assigning a FREED instance to a typed Node3D var raises
		# before any is_instance_valid guard could run.
		var node: Variant = _item_fx_nodes.get(owner_key)
		if node is Node3D and is_instance_valid(node) and node.is_inside_tree():
			var entity_ref: EntityRef = _item_fx_owner_refs.get(owner_key)
			var pose_runtime: MissionPresentation = _world.get_runtime()
			if entity_ref != null and pose_runtime != null \
					and pose_runtime.has_current_present_effect_snapshot():
				# Null here means the identity left THIS tick's replica set. Do
				# not fall back to the one-frame-old Node or the group would emit
				# once more from stale state before the batched present frees it.
				return pose_runtime.presented_entity_effect_transform(entity_ref)
			return (node as Node3D).global_transform
		_item_fx_nodes.erase(owner_key)
		_item_fx_owner_refs.erase(owner_key)
		return null
	var ssn_runtime: MissionPresentation = _world.get_runtime()
	if ssn_runtime != null:
		return ssn_runtime.entity_effect_transform_for_ssn(owner_key)
	return null


# Mission-start attach of the per-item ITEMS.DEF effects — slot A ('particlefx
# <effect> <userpoint>') only: for every presented animated entity whose item def
# authors it, spawn one entity-attached emitter at EVERY model userpoint matching
# the authored name — exact case-insensitive match over the model's first 16
# userpoints, duplicate names all match (a 16-bit mask in the original). Static
# MultiMesh entities use the placer's value descriptors and spawn the same authored
# effects world-bound at their final placement transform; no owner/render node is
# synthesized for them.
# Pool gates are kind-sensitive: pool 0 organics are excluded; pool 1 items skip
# attrib 0x42; pools 2/3 skip attrib 0x2. The fxs/fxw1..4 wake tiers and the
# death/fire/other family are movement/damage-state driven and stay unrouted
# (ptl-format-re.md §8).
# [orig: resolve_item_materials_and_spawn_bone_trails @ 0x522ee0 (mission start,
#  entity pools 1-3) -> ItemDef_GetBoneMaskByName @ 0x49ea40 (first 16, stricmp) ->
#  Entity_SpawnBoneTrailEffect @ 0x43bef0 (one mode-2 attached emitter per masked
#  userpoint: pos = the userpoint, forward = its direction)]
# Public as reattach(): the sim-restart and warm-pass paths re-register the
# persistent effects for the restored entity set.
func reattach() -> void:
	_item_fx_nodes.clear()
	_item_fx_owner_refs.clear()
	_item_fx_registered_nodes.clear()
	_item_fx_pending_nodes.clear()
	_item_fx_registered_static.clear()
	_item_fx_pending_static.clear()
	_item_fx_control_active.clear()
	_item_fx_control_nodes.clear()
	_item_fx_control_instances.clear()
	var effect_world: EffectWorld = _world.get_effect_world()
	if effect_world == null:
		return
	var item_db := _resolve_item_db()
	if item_db == null:
		return
	var attached := 0
	var container: Node = _world.get_node_or_null(NodePath("MissionObjects"))
	if container != null:
		for child in container.get_children():
			# Boundary filter: only placed ObjectModels carry the per-item
			# effect contract (husk grafts and helper nodes skip here).
			var node := child as ObjectModel
			var ref: EntityRef = node.entity_ref if node != null else null
			if ref == null:
				continue
			attached += _attach_item_effect_to_node(node, ref.kind, ref.item_id, item_db)
	var static_sources: Array = _static_sources.call()
	for source_index in range(static_sources.size()):
		var source: StaticEffectSource = static_sources[source_index]
		attached += _attach_item_effect_to_static(source, source_index, item_db)
	if attached > 0:
		print_verbose("GameWorld: item effects — %d emitter(s)" % attached)


func on_wire_node_spawned(node: ObjectModel, kind: int, item_id: int) -> void:
	_attach_item_effect_to_node(node, kind, item_id)


# The original walks entity pools 1-3 with distinct attrib masks. The imported
# mission kind enum is Marker=0, Item=1, Building=2, Organic=3.
func _item_effect_pool_allows(kind: int, attrib: int) -> bool:
	if kind == MissionData.KIND_ITEM:
		return (attrib & (ItemDatabase.ATTRIB_POWERUP | ItemDatabase.ATTRIB_PLAYER_CONTROL)) == 0
	if kind == MissionData.KIND_BUILDING or kind == MissionData.KIND_MARKER:
		return (attrib & ItemDatabase.ATTRIB_POWERUP) == 0
	return false


func _item_effect_controller_allows(kind: int, attrib: int) -> bool:
	# The occupied-controller pass bypasses only PlayerControl. The
	# independent powerup exclusion remains intact.
	return kind == MissionData.KIND_ITEM and \
			(attrib & (ItemDatabase.ATTRIB_POWERUP | ItemDatabase.ATTRIB_PLAYER_CONTROL)) \
			== ItemDatabase.ATTRIB_PLAYER_CONTROL


func _item_fx_identity_aliases(net_id: int, bms_id: int,
		spawn_origin: int, wire_handle: int = -1) -> Array[String]:
	var aliases: Array[String] = []
	var has_wire_identity := wire_handle >= 0 and wire_handle != 0xffff
	if has_wire_identity:
		aliases.append("wire:%d" % wire_handle)
	# Synthetic items.def attachments all carry the same sentinel origin and no
	# authored BMS/net identity. Their packed runtime handle is therefore the only
	# alias that distinguishes siblings on the same carrier.
	if has_wire_identity and bms_id == 0 and (
			spawn_origin == -1 or spawn_origin == SpawnOrigin.NONE):
		return aliases
	if net_id > 0:
		aliases.append("net:%d" % net_id)
	if bms_id > 0:
		aliases.append("bms:%d" % bms_id)
	if spawn_origin > 0:
		aliases.append("origin:%d" % spawn_origin)
	return aliases


func _item_fx_control_event_aliases(effect: MissionEffect) -> Array[String]:
	return _item_fx_identity_aliases(effect.a, effect.b, effect.c, effect.wire_handle)


func _item_fx_control_node_aliases(node: Node3D) -> Array[String]:
	var model := node as ObjectModel
	var ref: EntityRef = model.entity_ref if model != null else null
	if ref == null:
		return []
	# A wire row's origin is the header identity it carries (which may be
	# none); a placed model's origin is its own record.
	var origin_kind := ref.origin_kind if ref.wire_handle >= 0 else ref.kind
	var spawn_origin := 0
	if origin_kind >= 0 and ref.index >= 0:
		spawn_origin = SpawnOrigin.pack(origin_kind, ref.index)
	return _item_fx_identity_aliases(0, ref.bms_id, spawn_origin, ref.wire_handle)


func _item_fx_aliases_intersect(left: Array, right: Array) -> bool:
	for alias_v in left:
		if right.has(alias_v):
			return true
	return false


func _item_fx_control_node_is_active(entry: Dictionary) -> bool:
	for alias_v in entry.get("aliases", []):
		if _item_fx_control_active.has(String(alias_v)):
			return true
	return false


func _register_item_fx_control_node(node: ObjectModel, kind: int,
		item_id: int) -> Dictionary:
	var aliases := _item_fx_control_node_aliases(node)
	if aliases.is_empty():
		return {}
	var entry := {
		"node": node,
		"kind": kind,
		"item_id": item_id,
		"aliases": aliases,
	}
	_item_fx_control_nodes[node.get_instance_id()] = entry
	return entry


func _track_item_fx_control_spawn(node_id: int, owner_key: String,
		receipt: EffectSpawnReceipt) -> void:
	var instance: Dictionary = _item_fx_control_instances.get(node_id, {
		"group_ids": [],
		"owner_keys": [],
	})
	var group_ids: Array = instance.get("group_ids", [])
	var group_id := receipt.group_id
	if group_id > 0 and not group_ids.has(group_id):
		group_ids.append(group_id)
	var owner_keys: Array = instance.get("owner_keys", [])
	if not owner_keys.has(owner_key):
		owner_keys.append(owner_key)
	instance["group_ids"] = group_ids
	instance["owner_keys"] = owner_keys
	_item_fx_control_instances[node_id] = instance


func _stop_item_fx_control_node(node_id: int) -> void:
	var instance: Dictionary = _item_fx_control_instances.get(node_id, {})
	var effect_world: EffectWorld = _world.get_effect_world()
	if effect_world != null:
		for group_id_v in instance.get("group_ids", []):
			var group_id := int(group_id_v)
			if group_id > 0:
				effect_world.stop_group(group_id)
	for owner_key_v in instance.get("owner_keys", []):
		var owner_key := String(owner_key_v)
		_item_fx_nodes.erase(owner_key)
		_item_fx_owner_refs.erase(owner_key)
	_item_fx_control_instances.erase(node_id)
	_item_fx_registered_nodes.erase(node_id)
	_item_fx_pending_nodes.erase(node_id)


func _activate_item_fx_control_nodes(event_aliases: Array) -> void:
	for node_id_v in _item_fx_control_nodes.keys().duplicate():
		var node_id := int(node_id_v)
		var entry: Dictionary = _item_fx_control_nodes.get(node_id, {})
		if not _item_fx_aliases_intersect(entry.get("aliases", []), event_aliases):
			continue
		var node_v: Variant = entry.get("node")
		if not is_instance_valid(node_v) or not (node_v is ObjectModel):
			_stop_item_fx_control_node(node_id)
			_item_fx_control_nodes.erase(node_id)
			continue
		if not _item_fx_control_node_is_active(entry):
			continue
		_attach_item_effect_to_node(
				node_v as ObjectModel,
				int(entry.get("kind", -1)),
				int(entry.get("item_id", 0)),
				null,
				true)


func _deactivate_item_fx_control_nodes(event_aliases: Array) -> void:
	for node_id_v in _item_fx_control_nodes.keys().duplicate():
		var node_id := int(node_id_v)
		var entry: Dictionary = _item_fx_control_nodes.get(node_id, {})
		if not _item_fx_aliases_intersect(entry.get("aliases", []), event_aliases):
			continue
		if not _item_fx_control_node_is_active(entry):
			_stop_item_fx_control_node(node_id)


## Consume a vehicle-control lifecycle effect from the runtime's drained batch
## (called by the world's _on_runtime_effects router). Returns true when the
## effect was a control event and was handled here.
func consume_control_effect(effect: MissionEffect) -> bool:
	var kind := effect.kind
	if kind != "vehicle_control_started" and kind != "vehicle_control_stopped":
		return false
	var aliases := _item_fx_control_event_aliases(effect)
	if kind == "vehicle_control_started":
		for alias in aliases:
			_item_fx_control_active[alias] = true
		_activate_item_fx_control_nodes(aliases)
	else:
		for alias in aliases:
			_item_fx_control_active.erase(alias)
		_deactivate_item_fx_control_nodes(aliases)
	return true


func _attach_item_effect_to_node(node: ObjectModel, kind: int, item_id: int,
		item_db_override: ItemDatabase = null,
		controller_active: bool = false) -> int:
	var effect_world: EffectWorld = _world.get_effect_world()
	if effect_world == null or node == null or item_id <= 0:
		return 0
	var node_id := node.get_instance_id()
	var registered: Variant = _item_fx_registered_nodes.get(node_id)
	if registered is Node and is_instance_valid(registered):
		return 0
	var item_db: ItemDatabase = item_db_override
	if item_db == null:
		item_db = _resolve_item_db()
	if item_db == null:
		return 0
	var attrib := int(item_db.get_attrib(item_id))
	if controller_active:
		if not _item_effect_controller_allows(kind, attrib):
			return 0
	else:
		if not _item_effect_pool_allows(kind, attrib):
			if not _item_effect_controller_allows(kind, attrib):
				return 0
			var control_entry := _register_item_fx_control_node(node, kind, item_id)
			if not control_entry.is_empty() and _item_fx_control_node_is_active(control_entry):
				return _attach_item_effect_to_node(node, kind, item_id, item_db, true)
			return 0
	var fx := item_db.get_particle_fx(item_id)
	var effect := fx.effect if fx != null else ""
	var userpoint := fx.userpoint if fx != null else ""
	if effect.is_empty():
		return 0
	var data: ObjectData = node.get_object_data()
	if data == null:
		return 0
	if effect_world.are_particles_hidden():
		# The retail master switch makes every spawn facade a no-op. Remember
		# persistent item attachments so re-enabling after a hidden mission load
		# creates them exactly once instead of losing them for the mission.
		_item_fx_pending_nodes[node_id] = {
			"node": node,
			"kind": kind,
			"item_id": item_id,
			"controller_active": controller_active,
		}
		return 0
	var attached := 0
	var matched := 0
	var entity_ref: EntityRef = node.entity_ref
	if not userpoint.is_empty():
		# The first-16 case-insensitive scan is native — ONE impl in
		# engine/formats/threedi (S12c) [orig: ItemDef_GetBoneMaskByName
		# @ 0x49ea40; duplicate names all set their bit].
		var mask := int(data.get_user_point_bone_mask(userpoint))
		for i in range(ITEM_EFFECT_USER_POINT_SCAN_LIMIT):
			if (mask & (1 << i)) == 0:
				continue
			var info := data.get_user_point_info(i)
			var key := "itemfx:%d:%d" % [node_id, i]
			var receipt := effect_world.spawn_effect_attached_request(
					key, effect, node.global_transform,
					info.position,
					info.rotation)
			if receipt.spawned:
				_item_fx_nodes[key] = node
				_item_fx_owner_refs[key] = entity_ref
				if controller_active:
					_track_item_fx_control_spawn(node_id, key, receipt)
				attached += 1
			matched += 1
	if matched == 0:
		# No matched point (or no authored point name): the original still spawns
		# ONE emitter at the entity origin — the spawn_count==0 leg
		# [orig: Entity_SpawnBoneTrailEffect @ 0x43c097 -> submit_effect_descriptor
		#  @ 0x43c0a4 at entity->Position].
		var key := "itemfx:%d:origin" % node_id
		var receipt := effect_world.spawn_effect_attached_request(
				key, effect, node.global_transform, Vector3.ZERO, Vector3.ZERO)
		if receipt.spawned:
			_item_fx_nodes[key] = node
			_item_fx_owner_refs[key] = entity_ref
			if controller_active:
				_track_item_fx_control_spawn(node_id, key, receipt)
			attached += 1
	if attached > 0:
		_item_fx_registered_nodes[node_id] = node
		_item_fx_pending_nodes.erase(node_id)
	return attached


func _spawn_static_item_effect(effect: String, transform: Transform3D) -> bool:
	var effect_world: EffectWorld = _world.get_effect_world()
	var options := EffectSpawnOptions.new()
	options.admission = EffectScene.ADMISSION_ALWAYS
	options.binding = EffectScene.BINDING_WORLD
	options.render_domain = EffectScene.RENDER_DOMAIN_WORLD
	var receipt := effect_world.spawn_effect_request(effect, transform, options)
	return receipt.spawned


func _attach_item_effect_to_static(source: StaticEffectSource, source_index: int,
		item_db_override: ItemDatabase = null) -> int:
	var effect_world: EffectWorld = _world.get_effect_world()
	if effect_world == null or source == null or source_index < 0:
		return 0
	if _item_fx_registered_static.has(source_index):
		return 0
	var item_id := source.item_id
	var kind := source.kind
	if item_id <= 0:
		return 0
	var item_db: ItemDatabase = item_db_override
	if item_db == null:
		item_db = _resolve_item_db()
	if item_db == null or not _item_effect_pool_allows(kind, item_db.get_attrib(item_id)):
		return 0
	var fx := item_db.get_particle_fx(item_id)
	var effect := fx.effect if fx != null else ""
	var userpoint := fx.userpoint if fx != null else ""
	var data: ObjectData = source.object_data
	if effect.is_empty() or data == null:
		return 0
	if effect_world.are_particles_hidden():
		_item_fx_pending_static[source_index] = source
		return 0
	var entity_transform: Transform3D = source.world_transform
	var attached := 0
	var matched := 0
	if not userpoint.is_empty():
		# The same native first-16 scan as the animated leg (S12c)
		# [orig: ItemDef_GetBoneMaskByName @ 0x49ea40].
		var mask := int(data.get_user_point_bone_mask(userpoint))
		for i in range(ITEM_EFFECT_USER_POINT_SCAN_LIMIT):
			if (mask & (1 << i)) == 0:
				continue
			var info := data.get_user_point_info(i)
			# The same local pose EffectWorld.spawn_effect_attached uses; static
			# sources compose it once with their placement transform.
			var local_pose := EffectWorld.forward_pose(
					info.position,
					info.rotation)
			if _spawn_static_item_effect(effect, entity_transform * local_pose):
				attached += 1
			matched += 1
	if matched == 0:
		# The spawn_count==0 leg uses the entity origin. Preserve the entity basis,
		# matching an attached origin pose at the moment it becomes world-bound.
		if _spawn_static_item_effect(effect, entity_transform):
			attached += 1
	if attached > 0:
		_item_fx_registered_static[source_index] = true
		_item_fx_pending_static.erase(source_index)
	return attached


func _retry_pending_item_effects() -> void:
	var effect_world: EffectWorld = _world.get_effect_world()
	if effect_world == null or effect_world.are_particles_hidden():
		return
	var pending_ids := _item_fx_pending_nodes.keys().duplicate()
	for node_id_v in pending_ids:
		var node_id := int(node_id_v)
		var entry: Dictionary = _item_fx_pending_nodes.get(node_id, {})
		# A wire node may have despawned while particles were disabled. Keep the
		# freed-object Variant untyped until after the validity guard; a typed cast
		# can raise before is_instance_valid gets a chance to reject it.
		var node_v: Variant = entry.get("node")
		if not is_instance_valid(node_v) or not (node_v is ObjectModel):
			_item_fx_pending_nodes.erase(node_id)
			continue
		var node := node_v as ObjectModel
		var controller_active := bool(entry.get("controller_active", false))
		if controller_active:
			var control_entry: Dictionary = _item_fx_control_nodes.get(node_id, {})
			if control_entry.is_empty() or not _item_fx_control_node_is_active(control_entry):
				_item_fx_pending_nodes.erase(node_id)
				continue
		_attach_item_effect_to_node(node, int(entry.get("kind", -1)),
				int(entry.get("item_id", 0)), null, controller_active)
	var static_ids := _item_fx_pending_static.keys().duplicate()
	for source_index_v in static_ids:
		var source_index := int(source_index_v)
		var source: StaticEffectSource = _item_fx_pending_static.get(source_index)
		_attach_item_effect_to_static(source, source_index)
