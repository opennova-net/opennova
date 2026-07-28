## Viewing-client presentation for throwables: item-modeled flying rounds
## (grenades, satchels, claymores in the air) and known placed devices — the
## render half of libs/world's ThrowableSim/RoundSim state (world-wac-ai-re §27).
## Joiners currently learn flying rounds from S2C tag-2; placed-device 0x59/0x12
## replication remains deferred and this pass only renders state the sim has.
## [orig: the round renders as its TrcrID item model via Entity_InitFromItemDef
## @ 0x49e550 with the motor-integrated angles; a placed device is a pool-1
## item entity drawn like any other. The sim stays render-free — this pass
## reconciles model nodes against get_throwable_visuals() each frame.]
extends RefCounted

const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")

var _sim  # NovaSimulation
var _container: Node3D
var _placer
var _item_db
var _env_node
var _fx_provider: Callable
var _game_world

# key (int64) -> {node: Node3D, item_id: int}; flying-round keys combine
# the pool slot with its monotonic lifetime generation, while placed devices
# occupy a disjoint high-bit namespace.
var _models := {}
# Live round key -> full Godot-space transform, polled by the effect-world
# owner resolver.
var _move_effect_transforms := {}
# Live round key -> {effect, owner_key, group_id}.
var _move_effects := {}


## Typed diagnostic snapshot (ADR 0017).
class Stats:
	extends RefCounted
	var live: int = 0
	var move_effects: int = 0
	var move_effect_transforms: int = 0


func setup(sim, container: Node3D, placer, item_db, env_node,
		fx_provider: Callable = Callable(), game_world = null) -> void:
	_sim = sim
	_container = container
	_placer = placer
	_item_db = item_db
	_env_node = env_node
	_fx_provider = fx_provider
	_game_world = game_world


func get_stats() -> Stats:
	var stats := Stats.new()
	stats.live = _models.size()
	stats.move_effects = _move_effects.size()
	stats.move_effect_transforms = _move_effect_transforms.size()
	return stats


func present() -> void:
	if _sim == null:
		return
	if not _sim.has_method("get_throwable_visuals"):
		return
	var visuals: Array = _sim.get_throwable_visuals()
	_sync_move_effects(visuals)
	if _container == null or not is_instance_valid(_container):
		return
	var seen := {}
	for entry_v in visuals:
		var entry := entry_v as Dictionary
		var key := int(entry.get("key", -1))
		if key < 0:
			continue
		seen[key] = true
		var item_id := int(entry.get("item_id", 0))
		var pos: Vector3 = entry.get("pos", Vector3.ZERO)
		var rot: Vector3 = entry.get("rotation_deg", Vector3.ZERO)
		# The model and the continuously attached ammo "move" particle read the
		# same authoritative round transform. [orig: tag-1 -> AmmoDef+0x70 at
		# @0x409fc2; spawn/update @0x4e9f58/@0x4ea8ae/@0x5f7410.]
		var next_transform := Transform3D(
				MissionObjectPlacer.bms_to_godot_basis(rot), pos)
		var rec: Dictionary = _models.get(key, {})
		if rec.is_empty() or int(rec.get("item_id", 0)) != item_id:
			if not rec.is_empty():
				_free_model(rec)
			rec = _build_model(key, item_id)
			if rec.is_empty():
				# unresolved graphic: remember the miss so we do not re-try
				# the build every frame
				_models[key] = {"node": null, "item_id": item_id}
				continue
			_models[key] = rec
		var node: Node3D = rec.get("node")
		if node == null or not is_instance_valid(node):
			continue
		# the placer euler convention: rotation_deg = (pitch, MISSION yaw, roll)
		if node.transform != next_transform:
			node.transform = next_transform
	# release models whose sim state is gone (detonated, converted, removed)
	for key in _models.keys():
		if not seen.has(key):
			_free_model(_models[key])
			_models.erase(key)


## Reconcile only the round-bound effects_table "move" groups at the fixed-tick
## seam. Scene nodes remain batched in present(), but particles must see every
## simulated pose and a release before the same tick's EffectWorld advance.
func sync_fixed_tick_effects() -> void:
	if _sim == null or not _sim.has_method("get_throwable_visuals"):
		return
	_sync_move_effects(_sim.get_throwable_visuals())


func _sync_move_effects(visuals: Array) -> void:
	var seen := {}
	for entry_v in visuals:
		var entry := entry_v as Dictionary
		var key := int(entry.get("key", -1))
		if key < 0:
			continue
		seen[key] = true
		var pos: Vector3 = entry.get("pos", Vector3.ZERO)
		var rot: Vector3 = entry.get("rotation_deg", Vector3.ZERO)
		var transform := Transform3D(
				MissionObjectPlacer.bms_to_godot_basis(rot), pos)
		_present_move_effect(key, String(entry.get("move_effect", "")),
				transform)
	for key in _move_effects.keys():
		if not seen.has(key):
			_retire_move_effect(int(key))


func _move_effect_owner_key(key: int) -> String:
	return "throwable-move:%d" % key


func _present_move_effect(key: int, effect: String,
		transform: Transform3D) -> void:
	var rec: Dictionary = _move_effects.get(key, {})
	if effect.is_empty():
		if not rec.is_empty():
			_retire_move_effect(key)
		else:
			_move_effect_transforms.erase(key)
		return
	if not rec.is_empty() and String(rec.get("effect", "")) != effect:
		_retire_move_effect(key)
		rec = {}
	_move_effect_transforms[key] = transform
	if not rec.is_empty():
		return
	var fx = _fx_provider.call() if _fx_provider.is_valid() else null
	if fx == null or _game_world == null \
			or not _game_world.has_method("register_effect_anchor"):
		_move_effect_transforms.erase(key)
		return
	var owner_key := _move_effect_owner_key(key)
	_game_world.register_effect_anchor(owner_key, func() -> Variant:
		return _move_effect_transforms.get(key) if _move_effects.has(key) else null)
	var receipt: Dictionary
	if fx.has_method("spawn_effect_owned_request"):
		receipt = fx.spawn_effect_owned_request(
				owner_key, effect, transform.origin, transform.basis.z)
	else:
		var handle := int(fx.spawn_effect_owned(
				owner_key, effect, transform.origin, transform.basis.z))
		receipt = {"spawned": handle > 0, "effect_handle": handle, "group_id": 0}
	if not bool(receipt.get("spawned", false)):
		_game_world.unregister_effect_anchor(owner_key)
		_move_effect_transforms.erase(key)
		if fx.has_method("release_effect_binding"):
			fx.release_effect_binding(owner_key)
		return
	_move_effects[key] = {
		"effect": effect,
		"owner_key": owner_key,
		"group_id": int(receipt.get("group_id", 0)),
	}


func _retire_move_effect(key: int) -> void:
	var rec: Dictionary = _move_effects.get(key, {})
	_move_effects.erase(key)
	_move_effect_transforms.erase(key)
	if rec.is_empty():
		return
	var owner_key: Variant = rec.get("owner_key")
	if _game_world != null and is_instance_valid(_game_world) \
			and _game_world.has_method("unregister_effect_anchor"):
		_game_world.unregister_effect_anchor(owner_key)
	var group_id := int(rec.get("group_id", 0))
	var fx = _fx_provider.call() if _fx_provider.is_valid() else null
	if group_id > 0 and fx != null and fx.has_method("stop_group"):
		# Stop emission in the same presenter pass that observes round removal;
		# already-live particles drain naturally. [orig:
		# Projectile_ReleaseEffects @0x4e8280 -> @0x5f75d0.]
		fx.stop_group(group_id)
	if fx != null and fx.has_method("release_effect_binding"):
		fx.release_effect_binding(owner_key)


func _build_model(_key: int, item_id: int) -> Dictionary:
	if _placer == null or _item_db == null:
		return {}
	if not _placer.has_method("build_model_from_graphic"):
		return {}
	var def_id := item_id + 100000  # mission::kItemIdOffset
	var graphic := String(_item_db.get_graphic(def_id))
	if graphic.is_empty():
		return {}
	var model: Node3D = _placer.build_model_from_graphic(
			graphic, "", _container, "", _env_node)
	if model == null:
		return {}
	model.name = "Throwable_%d" % item_id
	return {"node": model, "item_id": item_id}


func _free_model(rec: Dictionary) -> void:
	var node: Node3D = rec.get("node")
	if node != null and is_instance_valid(node):
		node.queue_free()


func reset_runtime_state() -> void:
	for key in _models.keys():
		_free_model(_models[key])
	_models.clear()
	for key in _move_effects.keys():
		_retire_move_effect(int(key))
	_move_effect_transforms.clear()


func teardown() -> void:
	reset_runtime_state()
