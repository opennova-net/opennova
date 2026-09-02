## Viewing-client presentation for throwables: item-modeled flying rounds
## (grenades, satchels, claymores in the air) and known placed devices — the
## render half of engine/runtime/world's ThrowableSim/RoundSim state (world-wac-ai-re §27).
## Joiners currently learn flying rounds from S2C tag-2; placed-device 0x59/0x12
## replication remains deferred and this pass only renders state the sim has.
## [orig: the round renders as its TrcrID item model via Entity_InitFromItemDef
## @ 0x49e550 with the motor-integrated angles; a placed device is a pool-1
## item entity drawn like any other. The sim stays render-free — this pass
## reconciles model nodes against get_throwable_visuals() each frame.]
extends RefCounted


var _sim: Simulation                  # null in data-driven tests
var _container: Node3D
var _placer: MissionObjectPlacer
var _item_db: ItemDatabase
var _fx_provider: Callable
# The owner-anchor registry (GameWorld's ItemEffectDirector); null when the
# owner runs without an effect world.
var _anchors: ItemEffectDirector

## One reconciled throwable model; `node` stays null for an unresolved
## graphic so the miss is remembered instead of re-tried every frame.
class ModelSlot:
	extends RefCounted
	var node: Node3D
	var item_id: int

	func _init(p_node: Node3D, p_item_id: int) -> void:
		node = p_node
		item_id = p_item_id


## One live round-bound "move" effect group and its effect-world identity.
class MoveEffect:
	extends RefCounted
	var effect: String
	var owner_key: String
	var group_id: int

	func _init(p_effect: String, p_owner_key: String, p_group_id: int) -> void:
		effect = p_effect
		owner_key = p_owner_key
		group_id = p_group_id


# key (int64) -> ModelSlot; flying-round keys combine the pool slot with its
# monotonic lifetime generation, while placed devices occupy a disjoint
# high-bit namespace.
var _models: Dictionary = {}
# Live round key -> full Godot-space transform, polled by the effect-world
# owner resolver.
var _move_effect_transforms: Dictionary = {}
# Live round key -> MoveEffect.
var _move_effects: Dictionary = {}


## Typed diagnostic snapshot (ADR 0017).
class Stats:
	extends RefCounted
	var live: int = 0
	var move_effects: int = 0
	var move_effect_transforms: int = 0


func setup(sim: Simulation, container: Node3D, placer: MissionObjectPlacer,
		item_db: ItemDatabase, fx_provider: Callable = Callable(),
		anchors: ItemEffectDirector = null) -> void:
	_sim = sim
	_container = container
	_placer = placer
	_item_db = item_db
	_fx_provider = fx_provider
	_anchors = anchors


func get_stats() -> Stats:
	var stats := Stats.new()
	stats.live = _models.size()
	stats.move_effects = _move_effects.size()
	stats.move_effect_transforms = _move_effect_transforms.size()
	return stats


func present() -> void:
	if _sim == null:
		return
	present_visuals(_sim.get_throwable_visuals())


## The pure-data presentation leg (the present_snapshot precedent): production
## present() feeds the typed sim's rows; tests feed the same rows directly.
func present_visuals(visuals: Array) -> void:
	_sync_move_effects(visuals)
	if _container == null or not is_instance_valid(_container):
		return
	var seen := {}
	for entry_v in visuals:
		var entry := entry_v as ThrowableVisualRow
		var key := entry.key
		if key < 0:
			continue
		seen[key] = true
		var item_id := entry.item_id
		var pos := entry.pos
		var rot := entry.rotation_deg
		# The model and the continuously attached ammo "move" particle read the
		# same authoritative round transform. [orig: tag-1 -> AmmoDef+0x70 at
		# @0x409fc2; spawn/update @0x4e9f58/@0x4ea8ae/@0x5f7410.]
		var next_transform := Transform3D(
				MissionObjectPlacer.bms_to_godot_basis(rot), pos)
		var rec: ModelSlot = _models.get(key)
		if rec == null or rec.item_id != item_id:
			if rec != null:
				_free_model(rec)
			rec = _build_model(key, item_id)
			if rec == null:
				# unresolved graphic: remember the miss so we do not re-try
				# the build every frame
				_models[key] = ModelSlot.new(null, item_id)
				continue
			_models[key] = rec
		var node := rec.node
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
	if _sim == null:
		return
	_sync_move_effects(_sim.get_throwable_visuals())


func _sync_move_effects(visuals: Array) -> void:
	var seen := {}
	for entry_v in visuals:
		var entry := entry_v as ThrowableVisualRow
		var key := entry.key
		if key < 0:
			continue
		seen[key] = true
		var transform := Transform3D(
				MissionObjectPlacer.bms_to_godot_basis(entry.rotation_deg), entry.pos)
		# The sim's emitter liveness (the round+0x1CC handle mirror): a row
		# whose emitter is released — a ClipWaterFx round under the water
		# plane — retires its group and forgets the handle, so the same round
		# spawns a FRESH group on surfacing. The release is not latched.
		# [orig: Projectile_UpdatePhysics @0x4ea019..0x4ea03e — the
		#  ammoFlags & 0x20000000 release arm; the lazy spawn @0x4e9f58]
		_present_move_effect(key, entry.move_effect, transform, entry.move_effect_live)
	for key in _move_effects.keys():
		if not seen.has(key):
			_retire_move_effect(int(key))


func _move_effect_owner_key(key: int) -> String:
	return "throwable-move:%d" % key


func _present_move_effect(key: int, effect: String,
		transform: Transform3D, live: bool = true) -> void:
	var rec: MoveEffect = _move_effects.get(key)
	if effect.is_empty() or not live:
		if rec != null:
			_retire_move_effect(key)
		else:
			_move_effect_transforms.erase(key)
		return
	if rec != null and rec.effect != effect:
		_retire_move_effect(key)
		rec = null
	_move_effect_transforms[key] = transform
	if rec != null:
		return
	var fx: EffectWorld = _fx_provider.call() if _fx_provider.is_valid() else null
	if fx == null or _anchors == null:
		_move_effect_transforms.erase(key)
		return
	var owner_key := _move_effect_owner_key(key)
	_anchors.register_effect_anchor(owner_key, func() -> Variant:
		return _move_effect_transforms.get(key) if _move_effects.has(key) else null)
	var receipt := fx.spawn_effect_owned_request(
			owner_key, effect, transform.origin, transform.basis.z)
	if not receipt.spawned:
		_anchors.unregister_effect_anchor(owner_key)
		_move_effect_transforms.erase(key)
		fx.release_effect_binding(owner_key)
		return
	_move_effects[key] = MoveEffect.new(effect, owner_key, receipt.group_id)


func _retire_move_effect(key: int) -> void:
	var rec: MoveEffect = _move_effects.get(key)
	_move_effects.erase(key)
	_move_effect_transforms.erase(key)
	if rec == null:
		return
	var owner_key := rec.owner_key
	if _anchors != null:
		_anchors.unregister_effect_anchor(owner_key)
	var group_id := rec.group_id
	var fx: EffectWorld = _fx_provider.call() if _fx_provider.is_valid() else null
	if fx == null:
		return
	if group_id > 0:
		# Stop emission in the same presenter pass that observes round removal;
		# already-live particles drain naturally. [orig:
		# Projectile_ReleaseEffects @0x4e8280 -> Entity_ReleaseEffectEmitter @0x5f75d0.]
		fx.stop_group(group_id)
	fx.release_effect_binding(owner_key)


func _build_model(_key: int, item_id: int) -> ModelSlot:
	if _placer == null or _item_db == null:
		return null
	var def_id := item_id + 100000  # mission::kItemIdOffset
	var graphic := String(_item_db.get_graphic(def_id))
	if graphic.is_empty():
		return null
	var model: Node3D = _placer.build_model_from_graphic(
			graphic, "", _container, "", "", true)
	if model == null:
		return null
	model.name = "Throwable_%d" % item_id
	return ModelSlot.new(model, item_id)


func _free_model(rec: ModelSlot) -> void:
	var node := rec.node
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
