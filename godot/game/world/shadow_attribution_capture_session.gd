class_name ShadowAttributionCaptureSession
extends RefCounted

## Transactional public seam for the opt-in directional-shadow attribution
## profile. It changes only viewport debug mode, the dynamic directional
## light's shadow switch, the terrain page-shadow provider controls, and
## explicitly named ObjectModel dynamic-caster bits. Visible batch geometry is
## never edited. The original state is restored between variants and at
## finish().

const CaptureVariant := preload("res://game/world/render_capture_variant.gd")
const CasterDiagnostic := preload(
		"res://game/world/shadow_caster_diagnostic.gd")

var _world: GameWorld
var _viewport: Viewport
var _dynamic_shadow: DirectionalLight3D
var _terrain: Terrain
var _original_debug_draw := Viewport.DEBUG_DRAW_DISABLED
var _original_dynamic_shadow_enabled := false
var _original_static_terrain_shadow_enabled := true
var _original_suppressed_static_bms_ids := PackedInt32Array()
var _original_dynamic_caster_states: Dictionary = {}
var _current_variant: RenderCaptureVariant
var _active := false


func begin(world: GameWorld, viewport: Viewport) -> Error:
	if _active:
		finish()
	if world == null or not is_instance_valid(world) \
			or viewport == null or not is_instance_valid(viewport):
		return ERR_INVALID_PARAMETER
	var dynamic := world.get_node_or_null("SunShadow") as DirectionalLight3D
	var terrain := world.get_terrain_node()
	if dynamic == null or terrain == null:
		return ERR_UNCONFIGURED
	_world = world
	_viewport = viewport
	_dynamic_shadow = dynamic
	_terrain = terrain
	_original_debug_draw = viewport.debug_draw
	_original_dynamic_shadow_enabled = dynamic.shadow_enabled
	_original_static_terrain_shadow_enabled = \
			terrain.is_static_terrain_shadow_enabled()
	_original_suppressed_static_bms_ids = \
			terrain.get_suppressed_static_shadow_bms_ids()
	_original_dynamic_caster_states.clear()
	_current_variant = null
	_active = true
	return OK


func apply_variant(variant) -> Error:
	if not _active or _world == null or not is_instance_valid(_world) \
			or _viewport == null or not is_instance_valid(_viewport):
		return ERR_UNCONFIGURED
	if variant == null or not (variant is CaptureVariant):
		return ERR_INVALID_PARAMETER
	_restore_suppressed_dynamic_casters()
	_restore_suppressed_static_casters()

	var targets: Array[ObjectModel] = []
	for bms_id: int in variant.suppressed_dynamic_caster_bms_ids:
		var matches := _models_with_bms_id(bms_id)
		if matches.size() != 1:
			return ERR_DOES_NOT_EXIST if matches.is_empty() else ERR_ALREADY_EXISTS
		var model := matches[0] as ObjectModel
		if not model.is_shadow_caster_enabled():
			return ERR_UNAVAILABLE
		targets.append(model)
	for bms_id: int in variant.suppressed_static_caster_bms_ids:
		if not _has_static_caster_bms_id(bms_id):
			return ERR_DOES_NOT_EXIST

	_viewport.debug_draw = variant.debug_draw
	_dynamic_shadow.shadow_enabled = variant.dynamic_shadow_enabled
	_terrain.set_static_terrain_shadow_enabled(
			variant.static_terrain_shadow_enabled)
	for model: ObjectModel in targets:
		var key := model.get_instance_id()
		_original_dynamic_caster_states[key] = {
			"model": weakref(model),
			"dynamic_enabled": model.is_shadow_caster_enabled(),
		}
		model.set_shadow_caster_enabled(false)
	var static_error := _suppress_static_casters(
			variant.suppressed_static_caster_bms_ids)
	if static_error != OK:
		_restore_suppressed_dynamic_casters()
		_restore_suppressed_static_casters()
		return static_error
	_current_variant = variant
	return OK


func get_variant_diagnostics() -> RenderCaptureVariant:
	if not _active or _current_variant == null \
			or _viewport == null or not is_instance_valid(_viewport) \
			or _dynamic_shadow == null or not is_instance_valid(_dynamic_shadow) \
			or _terrain == null or not is_instance_valid(_terrain):
		return null
	var dynamic_ids := PackedInt32Array(
			_realized_suppressed_dynamic_bms_ids())
	var static_ids := PackedInt32Array(
			_terrain.get_suppressed_static_shadow_bms_ids())
	dynamic_ids.sort()
	static_ids.sort()
	return CaptureVariant.new(
			String(_current_variant.id),
			int(_viewport.debug_draw),
			bool(_dynamic_shadow.shadow_enabled),
			bool(_terrain.is_static_terrain_shadow_enabled()),
			dynamic_ids,
			static_ids)


func _realized_suppressed_dynamic_bms_ids() -> Array:
	var by_bms_id: Dictionary = {}
	for state_value: Variant in _original_dynamic_caster_states.values():
		var state := state_value as Dictionary
		var model_ref := state.get("model") as WeakRef
		var model := model_ref.get_ref() as ObjectModel \
				if model_ref != null else null
		if model == null or not is_instance_valid(model) \
				or not bool(state.get("dynamic_enabled", false)) \
				or model.is_shadow_caster_enabled():
			continue
		var ref_value: Variant = model.get_meta("entity_ref", {})
		if ref_value is Dictionary and (ref_value as Dictionary).has("bms_id"):
			by_bms_id[int((ref_value as Dictionary).bms_id)] = true
	var ids: Array = by_bms_id.keys()
	ids.sort()
	return ids


func get_dynamic_caster_inventory() -> Array:
	if not _active or _world == null or not is_instance_valid(_world):
		return []
	var models: Array[ObjectModel] = []
	_collect_object_models(_world, models)
	var item_db: ItemDatabase = _world.get_item_db()
	var rows: Array = []
	for model: ObjectModel in models:
		if not model.is_shadow_caster_enabled():
			continue
		var ref_value: Variant = model.get_meta("entity_ref", {})
		var ref: Dictionary = ref_value if ref_value is Dictionary else {}
		var item_id := int(ref.get("item_id", 0))
		var graphic := String(ref.get("graphic", ""))
		var attrib2 := int(ref.get("attrib2", 0))
		if item_db != null:
			if graphic.is_empty():
				graphic = item_db.get_graphic(item_id)
			if not ref.has("attrib2"):
				attrib2 = item_db.get_attrib2(item_id)
		var bounds: AABB = model.global_transform * model.get_model_bounds()
		rows.append(CasterDiagnostic.new(
				int(ref.get("bms_id", -1)), item_id, graphic, attrib2,
				Water.VISUAL_LAYER_DYNAMIC_SHADOW_CASTER, bounds,
				String(model.get_path())))
	rows.sort_custom(_caster_row_less)
	return rows


func get_static_caster_inventory() -> Array:
	if not _active or _world == null or not is_instance_valid(_world):
		return []
	var item_db: ItemDatabase = _world.get_item_db()
	var by_identity: Dictionary = {}

	# Runtime husks can carry the static layer directly on ObjectModel render
	# children. Placed static/portal populations are represented by the aligned
	# MultiMesh rows below.
	var models: Array[ObjectModel] = []
	_collect_object_models(_world, models)
	for model: ObjectModel in models:
		if not model.is_static_shadow_caster_enabled():
			continue
		var ref := _model_ref(model)
		_append_static_inventory_row(by_identity, item_db,
				int(ref.get("bms_id", -1)), int(ref.get("item_id", 0)),
				String(ref.get("graphic", "")), int(ref.get("attrib2", 0)),
				model.global_transform * model.get_model_bounds(),
				String(model.get_path()))

	var sources: Array[MultiMeshInstance3D] = []
	_collect_static_multimeshes(_world, sources)
	for source: MultiMeshInstance3D in sources:
		var mm := source.multimesh
		if mm == null or mm.mesh == null:
			continue
		var identities := _static_source_identities(source)
		for index in range(mini(mm.instance_count, identities.size())):
			var identity: Dictionary = identities[index]
			if not bool(identity.get("eligible", false)):
				continue
			var world_xform := source.global_transform \
					* mm.get_instance_transform(index)
			var bounds: AABB = world_xform * mm.mesh.get_aabb()
			_append_static_inventory_row(by_identity, item_db,
					int(identity.get("bms_id", -1)),
					int(identity.get("item_id", 0)),
					String(identity.get("graphic", "")),
					int(identity.get("attrib2", 0)), bounds,
					"%s#%d" % [source.get_path(), index])
	var rows: Array = by_identity.values()
	rows.sort_custom(_caster_row_less)
	return rows


func finish() -> void:
	if not _active:
		return
	_restore_suppressed_dynamic_casters()
	_restore_suppressed_static_casters()
	if _viewport != null and is_instance_valid(_viewport):
		_viewport.debug_draw = _original_debug_draw
	if _dynamic_shadow != null and is_instance_valid(_dynamic_shadow):
		_dynamic_shadow.shadow_enabled = _original_dynamic_shadow_enabled
	if _terrain != null and is_instance_valid(_terrain):
		_terrain.set_static_terrain_shadow_enabled(
				_original_static_terrain_shadow_enabled)
	_world = null
	_viewport = null
	_dynamic_shadow = null
	_terrain = null
	_current_variant = null
	_active = false


func _restore_suppressed_dynamic_casters() -> void:
	for state_value: Variant in _original_dynamic_caster_states.values():
		var state := state_value as Dictionary
		var model_ref := state.get("model") as WeakRef
		var model := model_ref.get_ref() as ObjectModel \
				if model_ref != null else null
		if model != null and is_instance_valid(model):
			model.set_shadow_caster_enabled(bool(state.dynamic_enabled))
	_original_dynamic_caster_states.clear()


func _suppress_static_casters(bms_ids: PackedInt32Array) -> Error:
	if _terrain == null or not is_instance_valid(_terrain):
		return ERR_UNCONFIGURED
	_terrain.set_suppressed_static_shadow_bms_ids(bms_ids)
	return OK


func _restore_suppressed_static_casters() -> void:
	if _terrain != null and is_instance_valid(_terrain):
		_terrain.set_suppressed_static_shadow_bms_ids(
				_original_suppressed_static_bms_ids)


func _has_static_caster_bms_id(bms_id: int) -> bool:
	for row in get_static_caster_inventory():
		if row.bms_id == bms_id:
			return true
	return false


func _models_with_bms_id(bms_id: int) -> Array[ObjectModel]:
	var models: Array[ObjectModel] = []
	_collect_object_models(_world, models)
	var matches: Array[ObjectModel] = []
	for model: ObjectModel in models:
		var ref_value: Variant = model.get_meta("entity_ref", {})
		if ref_value is Dictionary \
				and int((ref_value as Dictionary).get("bms_id", -1)) == bms_id:
			matches.append(model)
	return matches


static func _collect_object_models(node: Node, out: Array[ObjectModel]) -> void:
	for child: Node in node.get_children():
		if child is ObjectModel:
			out.append(child as ObjectModel)
		_collect_object_models(child, out)


static func _collect_static_multimeshes(
		node: Node, out: Array[MultiMeshInstance3D]) -> void:
	for child: Node in node.get_children():
		if child is MultiMeshInstance3D \
				and ((child as MultiMeshInstance3D).layers \
						& Water.VISUAL_LAYER_STATIC_SHADOW_CASTER) != 0:
			out.append(child as MultiMeshInstance3D)
		_collect_static_multimeshes(child, out)


static func _model_ref(model: ObjectModel) -> Dictionary:
	var ref_value: Variant = model.get_meta("entity_ref", {})
	return ref_value if ref_value is Dictionary else {}


static func _static_source_identities(source: MultiMeshInstance3D) -> Array:
	var count := source.multimesh.instance_count \
			if source.multimesh != null else 0
	var bms_ids := Array(source.get_meta("static_shadow_bms_ids", []))
	var item_ids := Array(source.get_meta("static_shadow_item_ids", []))
	var attrib2_values := Array(source.get_meta("static_shadow_attrib2", []))
	var slots := Array(source.get_meta("static_shadow_slots", []))
	var graphic := String(source.get_meta("static_shadow_graphic", ""))
	if bms_ids.is_empty():
		var parent := source.get_parent()
		while parent != null and not (parent is ObjectModel):
			parent = parent.get_parent()
		if parent is ObjectModel:
			var ref := _model_ref(parent as ObjectModel)
			bms_ids = [int(ref.get("bms_id", -1))]
			item_ids = [int(ref.get("item_id", 0))]
			attrib2_values = [int(ref.get("attrib2", 0))]
			slots = [true]
			graphic = String(ref.get("graphic", ""))
	var rows: Array = []
	for index in range(count):
		rows.append({
			"bms_id": bms_ids[index] if index < bms_ids.size() else -1,
			"item_id": item_ids[index] if index < item_ids.size() else 0,
			"attrib2": attrib2_values[index] \
					if index < attrib2_values.size() else 0,
			"eligible": bool(slots[index]) if index < slots.size() else false,
			"graphic": graphic,
		})
	return rows


static func _append_static_inventory_row(
		by_identity: Dictionary,
		item_db: ItemDatabase,
		bms_id: int,
		item_id: int,
		graphic_value: String,
		attrib2_value: int,
		bounds: AABB,
		sort_path: String,
		) -> void:
	var graphic := graphic_value
	var attrib2 := attrib2_value
	if item_db != null:
		if graphic.is_empty():
			graphic = item_db.get_graphic(item_id)
		if attrib2 == 0:
			attrib2 = item_db.get_attrib2(item_id)
	var key := "%d:%d:%s" % [bms_id, item_id, graphic]
	if by_identity.has(key):
		var existing = by_identity[key]
		existing.aabb = existing.aabb.merge(bounds)
		if sort_path.naturalnocasecmp_to(existing.sort_path) < 0:
			existing.sort_path = sort_path
		return
	by_identity[key] = CasterDiagnostic.new(
			bms_id, item_id, graphic, attrib2,
			Water.VISUAL_LAYER_STATIC_SHADOW_CASTER, bounds, sort_path)


static func _caster_row_less(left, right) -> bool:
	if left.bms_id != right.bms_id:
		return left.bms_id < right.bms_id
	if left.item_id != right.item_id:
		return left.item_id < right.item_id
	var graphic_order: int = left.graphic.naturalnocasecmp_to(right.graphic)
	if graphic_order != 0:
		return graphic_order < 0
	if left.aabb.position != right.aabb.position:
		return _vector3_less(left.aabb.position, right.aabb.position)
	if left.aabb.size != right.aabb.size:
		return _vector3_less(left.aabb.size, right.aabb.size)
	return left.sort_path.naturalnocasecmp_to(right.sort_path) < 0


static func _vector3_less(left: Vector3, right: Vector3) -> bool:
	if left.x != right.x:
		return left.x < right.x
	if left.y != right.y:
		return left.y < right.y
	return left.z < right.z
