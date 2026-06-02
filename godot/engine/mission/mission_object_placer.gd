extends RefCounted

# Host-agnostic placement of a mission's entities into a 3D scene.
#
# Given a parsed mission (NovaMissionData), a resource root, and an item database
# (items.def), this resolves each placed entity to its visual model and instances
# it under a "MissionObjects" container parented to the caller's world root. It is
# the one genuinely shared piece between the runtime (NovaWorld) and the editor
# Mission workspace: the terrain editor / runtime each own the terrain + camera;
# this only knows how to turn entities into renderables.
#
# Batching strategy (hybrid):
#   - Static models are merged into MultiMeshInstance3D, one per unique
#     (graphic, submesh). Real maps place hundreds of identical props/buildings;
#     batching collapses them into a handful of draw calls. Per-instance frustum
#     culling is traded away for that batching (the whole batch shares one AABB),
#     which is the right call for dense, always-on-screen scenery.
#   - Animated / skinned models (items.def type == Person, or carrying a skeletal
#     anim_def) get an individual NovaObjectModel so each animates independently;
#     MultiMesh cannot carry per-instance skeleton/PANM state.
#
# Models are resolved exactly as veg_assets.gd does: items.def `graphic` -> first
# top-level "<graphic>.3di" through the VFS via NovaObjectData.open_from_resource_root.
# Static batches reuse the full object-editor fidelity path (NovaObjectModel +
# NovaObjectShaderCache materials) by building one template model off-tree and
# harvesting its rest-pose meshes + materials.
#
# Not host-specific and intentionally free of editor/runtime types so both callers
# can share it. Reference via preload(), not class_name, so it resolves without an
# editor re-import (same convention as veg_assets.gd).

const NovaObjectModelScript := preload("res://engine/object/nova_object_model.gd")

const CONTAINER_NAME := "MissionObjects"
const RENDER_LOD := 0

var resource_root: NovaResourceRoot
var item_db: NovaItemDatabase

# When true, place() also records a per-entity pickable index in pickable_records
# (used by the editor Mission workspace to select / move entities). Off for the
# runtime, which never picks; the index work is then skipped entirely.
var edit_mode: bool = false

# Populated by place() when edit_mode. One record per (entity, static submesh batch)
# or per animated entity. Static records carry { kind, index, graphic, slot, mm, mmi,
# offset, mesh_aabb, animated=false } so the editor can ray-pick (mesh_aabb under the
# instance transform) and move (rewrite mm instance `slot`). Animated records carry
# { kind, index, graphic, node, animated=true } and move the node directly.
var pickable_records: Array = []

# graphic -> NovaObjectData (or null when unresolvable).
var _object_data_cache: Dictionary = {}
# graphic -> Array[{ mesh, material, offset, submesh }] harvested from a template.
var _static_batch_cache: Dictionary = {}
# graphic -> Vector3 ground anchor (model-space point that sits at the entity
# position). Computed once per graphic; see _ground_anchor_for.
var _anchor_cache: Dictionary = {}


func _init(p_resource_root: NovaResourceRoot = null, p_item_db: NovaItemDatabase = null) -> void:
	resource_root = p_resource_root
	item_db = p_item_db


# --- Coordinate conversion (BMS is Z-up; Godot is Y-up) -----------------------
# Ported from the reference mission importer and kept as pure static helpers so
# they are unit-testable without any assets. Position is rotated -90 deg about X;
# rotation negates pitch/yaw and adds a half-turn of yaw, matching the authored
# heading convention.

static func bms_to_godot_position(p: Vector3) -> Vector3:
	# A -90 deg rotation about X maps (x, y, z) -> (x, z, -y).
	return Vector3(p.x, p.z, -p.y)


static func bms_to_godot_rotation(rot_deg: Vector3) -> Vector3:
	# rot_deg = (pitch, yaw, roll) in degrees -> Godot euler (YXZ order).
	return Vector3(
		deg_to_rad(-rot_deg.x),
		deg_to_rad(-rot_deg.y) + PI,
		deg_to_rad(rot_deg.z))


static func entity_transform(position: Vector3, rotation_deg: Vector3) -> Transform3D:
	var euler := bms_to_godot_rotation(rotation_deg)
	return Transform3D(Basis.from_euler(euler), bms_to_godot_position(position))


# Inverse of bms_to_godot_position: a Godot-space point back to mission (BMS) space.
# (x, z, -y) <- (x, y, z) inverts to (gx, -gz, gy). Used by the editor to write a
# dragged object's new ground position back into the mission record.
static func godot_to_bms_position(p: Vector3) -> Vector3:
	return Vector3(p.x, -p.z, p.y)


# --- Placement ----------------------------------------------------------------

## Place every renderable entity of `mission` under a fresh MissionObjects node
## parented to `parent`. Any previous MissionObjects under `parent` is cleared.
## `options` may carry "environment_node" (a NovaEnvironment) used for lighting.
## Returns a stats Dictionary (placed/batched/animated/unresolved/...).
func place(mission: NovaMissionData, parent: Node3D, options: Dictionary = {}) -> Dictionary:
	var stats := {
		"placed": 0,
		"batched": 0,
		"animated": 0,
		"unresolved": 0,
		"markers": 0,
		"graphics": 0,
		"batches": 0,
	}
	pickable_records = []
	if mission == null or parent == null or resource_root == null:
		return stats
	_ensure_item_db()

	var env_node: Node = options.get("environment_node", null)
	var container := _ensure_container(parent)

	# Bucket entities by graphic, split static vs animated.
	var static_by_graphic: Dictionary = {}  # graphic -> Array[Transform3D]
	# Parallel to static_by_graphic (same slot order); only filled in edit_mode so the
	# pickable index can map a MultiMesh instance back to its mission entity.
	var static_refs_by_graphic: Dictionary = {}  # graphic -> Array[{ kind, index }]
	var animated: Array = []  # [{ graphic, xform, (kind, index in edit_mode) }]
	for e in mission.get_all_entities():
		var entity: Dictionary = e
		if int(entity.get("kind", -1)) == NovaMissionData.KIND_MARKER:
			stats.markers += 1
			continue
		var item_id := int(entity.get("item_id", 0))
		var graphic := _graphic_for(item_id)
		if graphic.is_empty():
			stats.unresolved += 1
			continue
		var xform := entity_transform(
			entity.get("position", Vector3.ZERO),
			entity.get("rotation_deg", Vector3.ZERO))
		if _is_animated(item_id):
			var a := { "graphic": graphic, "xform": xform }
			if edit_mode:
				a["kind"] = int(entity.get("kind", -1))
				a["index"] = int(entity.get("index", -1))
			animated.append(a)
		else:
			if not static_by_graphic.has(graphic):
				static_by_graphic[graphic] = []
				static_refs_by_graphic[graphic] = []
			static_by_graphic[graphic].append(xform)
			if edit_mode:
				static_refs_by_graphic[graphic].append({
					"kind": int(entity.get("kind", -1)),
					"index": int(entity.get("index", -1)),
				})

	# Static: one MultiMeshInstance3D per (graphic, submesh).
	for graphic in static_by_graphic.keys():
		var xforms: Array = static_by_graphic[graphic]
		var batches := _get_static_batches(graphic, env_node, container)
		if batches.is_empty():
			stats.unresolved += xforms.size()
			continue
		stats.graphics += 1
		for batch in batches:
			var mm := MultiMesh.new()
			mm.transform_format = MultiMesh.TRANSFORM_3D
			mm.mesh = batch["mesh"]
			mm.instance_count = xforms.size()
			var offset: Transform3D = batch["offset"]
			for i in range(xforms.size()):
				mm.set_instance_transform(i, (xforms[i] as Transform3D) * offset)
			var mmi := MultiMeshInstance3D.new()
			mmi.multimesh = mm
			if batch["material"] != null:
				mmi.material_override = batch["material"]
			mmi.name = "Batch_%s_%d" % [graphic, int(batch.get("submesh", 0))]
			container.add_child(mmi)
			stats.batches += 1
			if edit_mode:
				_record_static_batch(graphic, static_refs_by_graphic.get(graphic, []), mm, mmi, offset, batch["mesh"])
		stats.batched += xforms.size()
		stats.placed += xforms.size()

	# Animated: an individual NovaObjectModel per entity.
	for a in animated:
		var data := _load_object_data(a["graphic"])
		if data == null:
			stats.unresolved += 1
			continue
		var model: Node3D = NovaObjectModelScript.new()
		model.name = "Anim_%s_%d" % [a["graphic"], stats.animated]
		# Anchor the whole model so its ground point sits at the entity origin (same
		# rule as static; for an animated model the offset rides the root node, parts
		# still animate within it). The recorded offset lets the editor drag re-apply it.
		var anchor_inv := Transform3D(Basis(), -_ground_anchor_for(a["graphic"], data))
		model.transform = (a["xform"] as Transform3D) * anchor_inv
		container.add_child(model)
		if env_node != null and model.has_method("set_environment_node"):
			model.set_environment_node(env_node)
		# Drive the build explicitly (not via _ready) so it is independent of when
		# place() runs relative to the main loop; matches the static template path.
		model.set_object_data(data)
		if edit_mode:
			var ref := { "kind": int(a.get("kind", -1)), "index": int(a.get("index", -1)) }
			model.set_meta("entity_ref", ref)
			pickable_records.append({
				"kind": ref["kind"],
				"index": ref["index"],
				"graphic": a["graphic"],
				"node": model,
				"offset": anchor_inv,
				"animated": true,
			})
		stats.animated += 1
		stats.placed += 1

	return stats


# --- Incremental placement (editor authoring) ---------------------------------

## Render one freshly-added entity into an existing MissionObjects container without
## rebuilding the whole world. Used by the editor when the user places a new object:
## add_entity has already written the record, so this only turns that record into a
## renderable. Reuses the model + batch caches, so repeated placement of the same
## graphic costs no re-harvest. Unlike place(), each new static entity gets its own
## single-instance MultiMesh (one extra draw group) rather than joining the shared
## batch; a later save+reopen re-batches everything normally. Records the pickable
## index for the new entity (this path is editor-only and always picks).
## Returns a small delta stats dict: { placed, batched, animated, batches, unresolved }.
func place_single(mission: NovaMissionData, container: Node3D, kind: int, index: int, env_node: Node = null) -> Dictionary:
	var delta := { "placed": 0, "batched": 0, "animated": 0, "batches": 0, "unresolved": 0 }
	if mission == null or container == null or resource_root == null:
		return delta
	_ensure_item_db()
	var entity: Dictionary = mission.get_entity(kind, index)
	if entity.is_empty():
		return delta
	var item_id := int(entity.get("item_id", 0))
	var graphic := _graphic_for(item_id)
	if graphic.is_empty():
		delta.unresolved = 1
		return delta
	var xform := entity_transform(
		entity.get("position", Vector3.ZERO),
		entity.get("rotation_deg", Vector3.ZERO))

	if _is_animated(item_id):
		var data := _load_object_data(graphic)
		if data == null:
			delta.unresolved = 1
			return delta
		var model: Node3D = NovaObjectModelScript.new()
		model.name = "Anim_%s_k%d_i%d" % [graphic, kind, index]
		var anchor_inv := Transform3D(Basis(), -_ground_anchor_for(graphic, data))
		model.transform = xform * anchor_inv
		container.add_child(model)
		if env_node != null and model.has_method("set_environment_node"):
			model.set_environment_node(env_node)
		model.set_object_data(data)
		var ref := { "kind": kind, "index": index }
		model.set_meta("entity_ref", ref)
		pickable_records.append({
			"kind": kind,
			"index": index,
			"graphic": graphic,
			"node": model,
			"offset": anchor_inv,
			"animated": true,
		})
		delta.placed = 1
		delta.animated = 1
		return delta

	var batches := _get_static_batches(graphic, env_node, container)
	if batches.is_empty():
		delta.unresolved = 1
		return delta
	var refs := [{ "kind": kind, "index": index }]
	for batch in batches:
		var mm := MultiMesh.new()
		mm.transform_format = MultiMesh.TRANSFORM_3D
		mm.mesh = batch["mesh"]
		mm.instance_count = 1
		var offset: Transform3D = batch["offset"]
		mm.set_instance_transform(0, xform * offset)
		var mmi := MultiMeshInstance3D.new()
		mmi.multimesh = mm
		if batch["material"] != null:
			mmi.material_override = batch["material"]
		mmi.name = "Place_%s_k%d_i%d_s%d" % [graphic, kind, index, int(batch.get("submesh", 0))]
		container.add_child(mmi)
		delta.batches += 1
		_record_static_batch(graphic, refs, mm, mmi, offset, batch["mesh"])
	delta.placed = 1
	delta.batched = 1
	return delta


# --- Internals ----------------------------------------------------------------

# Emit one pickable record per entity slot in a freshly-built static batch. Each
# entity's slot `i` is consistent across every submesh batch of the same graphic
# (instance_count == entity count), so moving entity i means rewriting instance i in
# every batch that shares its graphic. mesh_aabb (under the instance transform) gives
# the editor a tight pick volume without per-instance physics bodies.
func _record_static_batch(graphic: String, refs: Array, mm: MultiMesh, mmi: MultiMeshInstance3D, offset: Transform3D, mesh: Mesh) -> void:
	var mesh_aabb: AABB = mesh.get_aabb() if mesh != null else AABB()
	for i in range(mm.instance_count):
		var ref: Dictionary = refs[i] if i < refs.size() else {}
		pickable_records.append({
			"kind": int(ref.get("kind", -1)),
			"index": int(ref.get("index", -1)),
			"graphic": graphic,
			"slot": i,
			"mm": mm,
			"mmi": mmi,
			"offset": offset,
			"mesh_aabb": mesh_aabb,
			"animated": false,
		})


func _ensure_item_db() -> void:
	if item_db != null or resource_root == null:
		return
	var db := NovaItemDatabase.new()
	if db.load_from_resource_root(resource_root, "items.def") == OK:
		item_db = db


# The items database (items.def), loaded on demand from the resource root. Used by the
# editor to enumerate placeable items for the palette; returns null if items.def cannot
# be resolved. Shares the same instance the placer resolves graphics through.
func get_item_db() -> NovaItemDatabase:
	_ensure_item_db()
	return item_db


func _graphic_for(item_id: int) -> String:
	if item_db == null:
		return ""
	return item_db.get_graphic(item_id)


func _is_animated(item_id: int) -> bool:
	if item_db == null:
		return false
	if item_db.get_item_type(item_id) == NovaItemDatabase.TYPE_PERSON:
		return true
	return not item_db.get_anim_def(item_id).is_empty()


func _model_name_for(graphic: String) -> String:
	var basename := graphic.get_file().get_basename()
	return "" if basename.is_empty() else basename + ".3di"


func _load_object_data(graphic: String) -> NovaObjectData:
	if _object_data_cache.has(graphic):
		return _object_data_cache[graphic]
	var data: NovaObjectData = null
	var model_name := _model_name_for(graphic)
	if not model_name.is_empty() and resource_root != null:
		var d := NovaObjectData.new()
		if d.open_from_resource_root(resource_root, model_name) == OK:
			data = d
	_object_data_cache[graphic] = data
	return data


# The model-space anchor for `graphic`: the point that should sit at the entity's
# placed position. RENDER_LOD is the LOD the placer draws (userpoints are model-global;
# the part-0-center fallback is per-LOD). Cached per graphic; Vector3.ZERO when the
# model is unresolved or has no anchor (degrades to the old model-origin placement).
func _ground_anchor_for(graphic: String, data: NovaObjectData) -> Vector3:
	if _anchor_cache.has(graphic):
		return _anchor_cache[graphic]
	var anchor := Vector3.ZERO
	if data != null and data.has_method("get_ground_anchor"):
		anchor = data.get_ground_anchor(RENDER_LOD)
	_anchor_cache[graphic] = anchor
	return anchor


# Build a template NovaObjectModel, let it assemble the rest-pose meshes and
# fidelity materials, then harvest one batch per submesh: the mesh, its shader
# material, and the part's rest transform baked as a per-batch offset. The model is
# parented into the live tree (so its bounds/global-transform math is valid and
# silent) only for the duration of the harvest, then freed; the harvested
# Mesh/Material refs survive in the returned dictionaries. `tree_parent` must be a
# node already inside the SceneTree.
func _get_static_batches(graphic: String, env_node: Node, tree_parent: Node) -> Array:
	if _static_batch_cache.has(graphic):
		return _static_batch_cache[graphic]
	var batches: Array = []
	var data := _load_object_data(graphic)
	if data != null and tree_parent != null:
		var model: Node3D = NovaObjectModelScript.new()
		# Enter the tree first (so global_transform/bounds math is valid and quiet),
		# then drive rebuild() explicitly: relying on _ready() is unreliable when
		# place() runs before the main loop flushes _ready callbacks. No frame ticks
		# between add_child and free, so _process()/animation never runs.
		tree_parent.add_child(model)
		model.object_data = data
		if env_node != null and model.has_method("set_environment_node"):
			model.set_environment_node(env_node)
		model.rebuild()
		# Anchor the model so its ground reference point (the "ground" userpoint, else
		# part 0's center) lands at the entity origin instead of the model origin. Baking
		# T(-anchor) into every batch offset means every consumer of "offset" (place,
		# place_single, the editor drag via the recorded offset, picking) inherits it.
		var anchor_inv := Transform3D(Basis(), -_ground_anchor_for(graphic, data))
		var submesh := 0
		for robj_node in model.get_render_part_nodes().values():
			var part_node := robj_node as Node3D
			if part_node == null:
				continue
			for child in part_node.get_children():
				if child is MeshInstance3D and child.mesh != null:
					var mi := child as MeshInstance3D
					batches.append({
						"mesh": mi.mesh,
						"material": mi.material_override,
						"offset": anchor_inv * part_node.transform * mi.transform,
						"submesh": submesh,
					})
					submesh += 1
		tree_parent.remove_child(model)
		model.free()
	_static_batch_cache[graphic] = batches
	return batches


func _ensure_container(parent: Node3D) -> Node3D:
	var existing := parent.get_node_or_null(NodePath(CONTAINER_NAME))
	if existing != null:
		for child in existing.get_children():
			existing.remove_child(child)
			child.queue_free()
		return existing
	var container := Node3D.new()
	container.name = CONTAINER_NAME
	parent.add_child(container)
	return container
