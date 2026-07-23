## Host presentation for throwables: item-modeled flying rounds (grenades,
## satchels, claymores in the air) and placed devices — the render half of
## libs/world's ThrowableSim/RoundSim state (world-wac-ai-re §27).
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

# key (int) -> {node: Node3D, item_id: int}; key = round slot, or
# 0x10000 + entity handle for placed devices.
var _models := {}


func setup(sim, container: Node3D, placer, item_db, env_node) -> void:
	_sim = sim
	_container = container
	_placer = placer
	_item_db = item_db
	_env_node = env_node


func present() -> void:
	if _sim == null or _container == null or not is_instance_valid(_container):
		return
	if not _sim.has_method("get_throwable_visuals"):
		return
	var seen := {}
	for entry in _sim.get_throwable_visuals():
		var key := int(entry.get("key", -1))
		if key < 0:
			continue
		seen[key] = true
		var item_id := int(entry.get("item_id", 0))
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
		var pos: Vector3 = entry.get("pos", Vector3.ZERO)
		var rot: Vector3 = entry.get("rotation_deg", Vector3.ZERO)
		# the placer euler convention: rotation_deg = (pitch, MISSION yaw, roll)
		var next_transform := Transform3D(
				MissionObjectPlacer.bms_to_godot_basis(rot), pos)
		if node.transform != next_transform:
			node.transform = next_transform
	# release models whose sim state is gone (detonated, converted, removed)
	for key in _models.keys():
		if not seen.has(key):
			_free_model(_models[key])
			_models.erase(key)


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


func teardown() -> void:
	reset_runtime_state()
