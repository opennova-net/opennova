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
const CollisionHull := preload("res://engine/object/collision_hull.gd")

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
# adm name -> NovaSkeletalAnim (or null when it failed to load). Shared read-only across
# every entity using the same anim_def (eval_pose is const, so sharing one instance is safe).
var _skeletal_cache: Dictionary = {}
# graphic -> Array[{ mesh, material, offset, submesh }] harvested from a template.
var _static_batch_cache: Dictionary = {}
# graphic -> Vector3 ground anchor (model-space point that sits at the entity
# position). Computed once per graphic; see _ground_anchor_for.
var _anchor_cache: Dictionary = {}
# graphic -> Array[ConvexPolygonShape3D] collision hulls in model-local space (the
# editor's pickable bodies; see collision_shapes_for). Computed once per graphic.
var _collision_shapes_cache: Dictionary = {}

# The global cache epoch (NovaResourceRoot.cache_epoch) the caches above were built
# under. Any root mount/rescan/clear bumps the epoch; the next cache access then
# self-clears, so a rescanned resource dir is never served stale models/anchors/hulls.
var _built_epoch: int = 0


func _init(p_resource_root: NovaResourceRoot = null, p_item_db: NovaItemDatabase = null) -> void:
	resource_root = p_resource_root
	item_db = p_item_db


# Drop every derived cache when the resource-root epoch has moved since they were
# filled. Called at the top of the cache-reading entry points (place / place_single /
# ground_anchor_godot / collision_shapes_for); cheap when the epoch is unchanged.
func _check_epoch() -> void:
	var epoch := NovaResourceRoot.cache_epoch()
	if epoch == _built_epoch:
		return
	_built_epoch = epoch
	_object_data_cache.clear()
	_skeletal_cache.clear()
	_static_batch_cache.clear()
	_anchor_cache.clear()
	_collision_shapes_cache.clear()


# --- Coordinate conversion (BMS is Z-up; Godot is Y-up) -----------------------
# Pure static helpers (unit-testable without assets). Position is a -90 deg rotation
# about X; orientation is a structural port of the engine's matrix builder conjugated
# into Godot's basis (see bms_to_godot_basis).

static func bms_to_godot_position(p: Vector3) -> Vector3:
	# A -90 deg rotation about X maps (x, y, z) -> (x, z, -y).
	return Vector3(p.x, p.z, -p.y)


# Godot orientation basis for an entity authored as (pitch, yaw, roll) in degrees.
#
# [orig: Entity_SpawnFromBMSRecord @0x40eb66 + Math_BuildFixedPointMatrixFromEulerAngles @0x613f40,
#  called via Entity_UpdateOrientationMatrix @0x43b440 (Jointops.exe)] The engine builds the world
#  matrix as Rz(90-yaw) * Ry(pitch) * Rx(roll) in its Z-up, right-handed world: yaw drives the Z/up
#  axis (euler[3] = 90 - yaw), pitch the Y axis (euler[4], positive), roll the X axis (euler[5],
#  positive). Conjugating by the position basis M:(x,y,z)->(x,z,-y) -- which sends engine +Z->godot
#  +Y, +Y->godot -Z, +X->godot +X -- gives the faithful Godot world rotation
#      R_godot = RotY(90 - yaw) * RotZ(-pitch) * RotX(roll).
#  The .3di model imports Y-up / +Z-forward, so a constant model-forward correction C = RotY(90)
#  turns the model's +Z nose onto the engine's +X canonical heading. For yaw-only this collapses to
#  RotY(180 - yaw) -- identical to the long-standing (visually-correct) heading -- while correcting
#  pitch, which the old euler form (Rx(-pitch) in a YXZ basis) tipped the wrong way (nose up instead
#  of down). Roll was already equivalent. See godot/tests/mission_object_placer_test.gd.
static func bms_to_godot_basis(rot_deg: Vector3) -> Basis:
	var pitch := deg_to_rad(rot_deg.x)
	var yaw := deg_to_rad(rot_deg.y)
	var roll := deg_to_rad(rot_deg.z)
	return Basis(Vector3.UP, deg_to_rad(90.0) - yaw) \
		* Basis(Vector3.BACK, -pitch) \
		* Basis(Vector3.RIGHT, roll) \
		* Basis(Vector3.UP, deg_to_rad(90.0))


static func entity_transform(position: Vector3, rotation_deg: Vector3) -> Transform3D:
	return Transform3D(bms_to_godot_basis(rotation_deg), bms_to_godot_position(position))


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
	_check_epoch()
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
	# Optional load-stage attribution (the editor's mission open passes one).
	var timeline: PerfTimeline = options.get("timeline", null) as PerfTimeline
	var container := _ensure_container(parent)

	# Bucket entities by graphic, split static vs animated.
	PerfTimeline.span_on(timeline, "bucket_entities")
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
			# Always capture identity (not just edit_mode): the runtime needs it to tag the node so
			# MissionEntityRegistry can resolve SSN/group/zone host-action targets to this live model.
			animated.append({
				"graphic": graphic,
				"item_id": item_id,
				"xform": xform,
				"kind": int(entity.get("kind", -1)),
				"index": int(entity.get("index", -1)),
				"bms_id": int(entity.get("bms_id", 0)),
				"group": int(entity.get("group", -1)),
				"team": int(entity.get("team", -1)),
				"position": entity.get("position", Vector3.ZERO),
			})
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

	PerfTimeline.end_on(timeline)

	# Static: one MultiMeshInstance3D per (graphic, submesh).
	PerfTimeline.span_on(timeline, "static_batches")
	var resolved_graphics: Array = []
	for graphic in static_by_graphic.keys():
		var xforms: Array = static_by_graphic[graphic]
		var batches := _get_static_batches(graphic, env_node, container)
		if batches.is_empty():
			stats.unresolved += xforms.size()
			continue
		resolved_graphics.append(graphic)
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
	PerfTimeline.end_on(timeline)

	# One pick collider per static entity (not per submesh): collision is
	# whole-model. A separate pass (rather than inline with each graphic's
	# batches) so the timeline can attribute collider cost on its own; the pick
	# bodies are addressed by name ("Pick_<kind>_<index>"), never by child order.
	if edit_mode:
		PerfTimeline.span_on(timeline, "pick_colliders")
		for graphic in resolved_graphics:
			var xforms: Array = static_by_graphic[graphic]
			var prefs: Array = static_refs_by_graphic.get(graphic, [])
			for i in range(xforms.size()):
				var pref: Dictionary = prefs[i] if i < prefs.size() else {}
				add_pick_collider(container, int(pref.get("kind", -1)), int(pref.get("index", -1)), graphic, xforms[i])
		PerfTimeline.end_on(timeline)

	# Animated: an individual NovaObjectModel per entity.
	PerfTimeline.span_on(timeline, "animated_models")
	for a in animated:
		var data := _load_object_data(a["graphic"])
		if data == null:
			stats.unresolved += 1
			continue
		var model: Node3D = NovaObjectModelScript.new()
		model.name = "Anim_%s_%d" % [a["graphic"], stats.animated]
		# Render the model origin at the entity's stored position directly. The engine bakes the
		# Ground userpoint into the stored position once, at author-time (place / terrain-drag), not
		# at render -- so a loaded .bms renders at its stored coords verbatim. [orig: sub_401A90, dfx2med.exe]
		model.transform = a["xform"] as Transform3D
		container.add_child(model)
		if env_node != null and model.has_method("set_environment_node"):
			model.set_environment_node(env_node)
		# Load the entity's body-animation set (.adm) BEFORE the data: setting it
		# first is a no-op rebuild (no data yet), so set_object_data below does
		# the ONE skeletal-keyed mesh build - the old order built a static-keyed
		# set first and threw it away, doubling every animated entity's cost.
		# Rigid weapon parts fake-skin; no anim_def -> stays static.
		_apply_skeletal_anim(model, int(a.get("item_id", 0)))
		# Drive the build explicitly (not via _ready) so it is independent of when
		# place() runs relative to the main loop; matches the static template path.
		model.set_object_data(data)
		# Tag identity on the node in BOTH runtime + editor so MissionEntityRegistry can resolve
		# SSN/group/zone host-action targets (e.g. PLAYPARTANIM) back to this live model. Picking +
		# colliders stay editor-only.
		var ref := {
			"kind": int(a.get("kind", -1)),
			"index": int(a.get("index", -1)),
			"bms_id": int(a.get("bms_id", 0)),
			"group": int(a.get("group", -1)),
			"team": int(a.get("team", -1)),
			"position": a.get("position", Vector3.ZERO),
		}
		model.set_meta("entity_ref", ref)
		if edit_mode:
			pickable_records.append({
				"kind": ref["kind"],
				"index": ref["index"],
				"graphic": a["graphic"],
				"node": model,
				"offset": Transform3D.IDENTITY,
				"animated": true,
			})
			add_pick_collider(container, int(ref["kind"]), int(ref["index"]), a["graphic"], a["xform"])
		stats.animated += 1
		stats.placed += 1
	PerfTimeline.end_on(timeline)

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
	_check_epoch()
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
		model.transform = xform
		container.add_child(model)
		if env_node != null and model.has_method("set_environment_node"):
			model.set_environment_node(env_node)
		# Skeletal set first = no-op rebuild; set_object_data does the one
		# skeletal-keyed build (same ordering rationale as place()).
		_apply_skeletal_anim(model, item_id)
		model.set_object_data(data)
		var ref := {
			"kind": kind,
			"index": index,
			"bms_id": int(entity.get("bms_id", 0)),
			"group": int(entity.get("group", -1)),
			"team": int(entity.get("team", -1)),
			"position": entity.get("position", Vector3.ZERO),
		}
		model.set_meta("entity_ref", ref)
		pickable_records.append({
			"kind": kind,
			"index": index,
			"graphic": graphic,
			"node": model,
			"offset": Transform3D.IDENTITY,
			"animated": true,
		})
		add_pick_collider(container, kind, index, graphic, xform)
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
	add_pick_collider(container, kind, index, graphic, xform)
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


# Resolve an animated entity's body-animation set from its item def's anim_def and attach it to
# the model so its Skeleton3D builds. Cached per .adm (shared read-only across entities). A model
# with an empty anim_def, or whose .adm fails to load, is left static (unchanged behaviour).
func _apply_skeletal_anim(model: Node3D, item_id: int) -> void:
	if model == null or resource_root == null or item_db == null:
		return
	var anim_def := item_db.get_anim_def(item_id)
	if anim_def.is_empty():
		return
	var adm_name := anim_def if anim_def.to_lower().ends_with(".adm") else anim_def + ".adm"
	var skeletal
	if _skeletal_cache.has(adm_name):
		skeletal = _skeletal_cache[adm_name]
	else:
		skeletal = NovaSkeletalAnim.new()
		if not skeletal.load_from_resource_root(resource_root, adm_name):
			skeletal = null
		_skeletal_cache[adm_name] = skeletal
	if skeletal != null and model.has_method("set_skeletal_anim"):
		model.set_skeletal_anim(skeletal)


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


# The Godot model-local ground reference point for `graphic`: the "ground" userpoint if present,
# else part-0 center (NovaObjectData.get_ground_anchor). RENDER_LOD is the LOD the placer draws.
# Cached per graphic; Vector3.ZERO when the model is unresolved or has no anchor. No longer applied
# at render -- the editor subtracts it (in BMS axes) from a terrain-drop position so the model's
# ground point lands at the cursor, mirroring the engine's author-time bake. [orig: sub_401A90]
func _ground_anchor_for(graphic: String, data: NovaObjectData) -> Vector3:
	if _anchor_cache.has(graphic):
		return _anchor_cache[graphic]
	var anchor := Vector3.ZERO
	if data != null and data.has_method("get_ground_anchor"):
		anchor = data.get_ground_anchor(RENDER_LOD)
	_anchor_cache[graphic] = anchor
	return anchor


# Public: the Godot model-local ground anchor for `graphic` (resolves + caches the model). The editor
# subtracts this from a terrain-drop position (converted to BMS axes) for the author-time ground bake.
func ground_anchor_godot(graphic: String) -> Vector3:
	_check_epoch()
	return _ground_anchor_for(graphic, _load_object_data(graphic))


# The model-local ground anchor mapped to mission (BMS) axes, for the engine's
# authoring facade (NovaMissionData.place_entity_grounded / move_entity_grounded).
# The (x, y, z) -> (x, -z, y) axis map is linear, so it is valid on offset
# vectors like the anchor, not just points.
func ground_anchor_bms(graphic: String) -> Vector3:
	return godot_to_bms_position(ground_anchor_godot(graphic))


# Public: the model graphic for an items.def id (or "" if unresolved), so the editor can resolve a
# fresh placement's anchor without reaching into the private item-db cache.
func graphic_for(item_id: int) -> String:
	_ensure_item_db()
	return _graphic_for(item_id)


# Convex collision hulls for `graphic`, in model-local space, for the editor's
# pickable physics bodies. Built once per graphic (cached) from the model's parsed
# collision volumes via CollisionHull.shapes_for -- the SAME path the Object Editor
# overlay validates, so what the user saw is exactly what picking tests against.
# Models with no collision volumes fall back to a single box hull from the visual
# model AABB so every placed entity stays pickable (never worse than the old AABB pick).
func collision_shapes_for(graphic: String) -> Array:
	_check_epoch()
	if _collision_shapes_cache.has(graphic):
		return _collision_shapes_cache[graphic]
	var shapes: Array = []
	var data := _load_object_data(graphic)
	if data != null and data.has_method("get_collision_volumes"):
		shapes = CollisionHull.shapes_for(data.get_collision_volumes())
	if shapes.is_empty():
		var aabb := _visual_model_aabb(data)
		if aabb.size != Vector3.ZERO:
			var pts := PackedVector3Array()
			for x in [aabb.position.x, aabb.end.x]:
				for y in [aabb.position.y, aabb.end.y]:
					for z in [aabb.position.z, aabb.end.z]:
						pts.push_back(Vector3(x, y, z))
			var box := ConvexPolygonShape3D.new()
			box.points = pts
			shapes = [box]
	_collision_shapes_cache[graphic] = shapes
	return shapes


# Add one StaticBody3D pick collider for entity (kind,index) under the container, with
# this graphic's convex collision hulls and an "entity_ref" meta the editor reads back
# from intersect_ray. Positioned at the entity transform (render is direct now), so the body
# coincides with the drawn model. Editor-only (edit_mode); freed automatically when the
# container is cleared/re-baked -- no manual lifecycle.
func add_pick_collider(container: Node3D, kind: int, index: int, graphic: String, entity_xform: Transform3D) -> StaticBody3D:
	var shapes: Array = collision_shapes_for(graphic)
	if shapes.is_empty():
		return null
	var body := StaticBody3D.new()
	body.name = "Pick_%d_%d" % [kind, index]
	body.set_meta("entity_ref", { "kind": kind, "index": index })
	body.transform = entity_xform
	for shape in shapes:
		var cs := CollisionShape3D.new()
		cs.shape = shape
		body.add_child(cs)
	container.add_child(body)
	return body


# Merged AABB of the render submeshes (model-local), used only as the collision
# fallback for models that carry no collision volumes.
func _visual_model_aabb(data: NovaObjectData) -> AABB:
	if data == null or not data.has_method("build_lod_submeshes"):
		return AABB()
	var aabb := AABB()
	var first := true
	for s in data.build_lod_submeshes(RENDER_LOD):
		var entry: Dictionary = s
		var mesh: ArrayMesh = entry.get("mesh")
		if mesh == null:
			continue
		var m: AABB = mesh.get_aabb()
		m.position += entry.get("abs", Vector3.ZERO) as Vector3
		if first:
			aabb = m
			first = false
		else:
			aabb = aabb.merge(m)
	return aabb


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
		# Each batch's "offset" is the submesh's model-local rest transform (part * mesh) relative to
		# the entity origin -- NO ground-anchor offset. The engine bakes the Ground userpoint into the
		# stored position at author-time, not at render, so a loaded .bms draws at its stored coords
		# verbatim. [orig: sub_401A90, dfx2med.exe]
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
						"offset": part_node.transform * mi.transform,
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
