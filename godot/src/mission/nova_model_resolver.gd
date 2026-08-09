extends RefCounted

# Resolves an items.def item id to a renderable ObjectModel — a thin layer over
# the bound engine classes (ItemDatabase -> graphic -> ObjectData (.3di) ->
# ObjectModel, plus the .adm skeletal set for animated entities). This is the
# SAME resolution chain MissionObjectPlacer's animated path runs; it lives here so
# the net replay runtime can spawn entities by type without depending on the
# placer's .bms-record-shaped internals.
#
# (Follow-up: fold MissionObjectPlacer's private _load_object_data /
# _needs_individual_node / _apply_skeletal_anim onto this resolver so the .bms
# and net paths share one copy.)

const MissionObjectPlacer := preload("res://src/mission/mission_object_placer.gd")

var _root: ResourceRoot
var _item_db: ItemDatabase
var _data_cache := {}      # graphic -> ObjectData (null = unresolved)
var _adm_cache := {}       # adm_name -> SkeletalAnim (null = unresolved)


func setup(root: ResourceRoot, item_db: ItemDatabase) -> void:
	_root = root
	_item_db = item_db


func has_item_db() -> bool:
	return _item_db != null


# Build a configured ObjectModel for `item_id` (a raw items.def id) under
# `parent`, or null if the item has no graphic / the .3di can't resolve. The model
# is added to the tree BEFORE set_object_data so the mesh/skeleton build can touch
# global transforms (the placer's order); .adm is attached first (no double build).
func make_model(item_id: int, parent: Node, env_state: EnvLightState = null) -> Node3D:
	if _item_db == null or parent == null:
		return null
	var graphic := _item_db.get_graphic(item_id)
	if graphic.is_empty():
		return null
	var data := _load_object_data(graphic)
	if data == null:
		return null
	var model := ObjectModel.new()
	parent.add_child(model)
	if env_state != null:
		model.set_environment_state(env_state)
	model.set_shadow_caster_enabled(MissionObjectPlacer.item_casts_dynamic_shadow(
			_item_db.get_item_type(item_id),
			_item_db.get_attrib(item_id),
			_item_db.get_attrib2(item_id)))
	_apply_skeletal_anim(model, item_id, data.get_bone_origins())
	model.set_object_data(data)
	return model


func _load_object_data(graphic: String) -> ObjectData:
	if _data_cache.has(graphic):
		return _data_cache[graphic]
	var data: ObjectData = null
	var basename := graphic.get_file().get_basename()
	if not basename.is_empty() and _root != null:
		var d := ObjectData.new()
		if d.open_from_resource_root(_root, basename + ".3di") == OK:
			data = d
	_data_cache[graphic] = data
	return data


# The model's bone origins override the .bad's lossy BadBone.position (the placer's
# body path does the same) — the .3di carries the real pivots. The cache key folds the
# table so two graphics sharing one .adm with different pivots don't alias.
func _apply_skeletal_anim(model: Node3D, item_id: int, model_bone_origins := PackedVector3Array()) -> void:
	if model == null or _root == null or _item_db == null:
		return
	var anim_def := _item_db.get_anim_def(item_id)
	if anim_def.is_empty():
		return
	var adm_name := anim_def if anim_def.to_lower().ends_with(".adm") else anim_def + ".adm"
	var cache_key := adm_name + "#" + str(hash(model_bone_origins))
	var skeletal
	if _adm_cache.has(cache_key):
		skeletal = _adm_cache[cache_key]
	else:
		skeletal = SkeletalAnim.new()
		if not skeletal.load_from_resource_root(_root, adm_name, model_bone_origins):
			skeletal = null
		_adm_cache[cache_key] = skeletal
	if skeletal != null and model.has_method("set_skeletal_anim"):
		model.set_skeletal_anim(skeletal)
