extends RefCounted

# Shell-agnostic placement of a mission's entities into a 3D scene.
#
# Given a parsed mission (NovaMissionData), a resource root, and an item database
# (items.def), this resolves each placed entity to its visual model and instances
# it under a "MissionObjects" container parented to the caller's world root. It is
# the one genuinely shared piece between the runtime (GameWorld) and the editor
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
#     anim_def) and portal-carrying buildings get an individual NovaObjectModel;
#     MultiMesh cannot carry per-instance skeleton/PANM state or section masks.
#     The same rule applies to any graphic whose .3di carries live PANM tracks
#     (free-running wave/spin
#     decorations like pump jacks, and SET/register-posed parts): the original
#     engine re-poses those from the global clock every rendered frame, so they
#     must stay live models even when items.def calls them plain decorations
#     (see _graphic_needs_live_panm).
#
# Models are resolved exactly as veg_assets.gd does: items.def `graphic` -> first
# top-level "<graphic>.3di" through the VFS via NovaObjectData.open_from_resource_root.
# Static batches reuse the full object-editor fidelity path (NovaObjectModel +
# NovaObjectShaderCache materials) by building one template model off-tree and
# harvesting its rest-pose meshes + materials.
#
# Not caller-specific and intentionally free of editor/runtime types so both callers
# can share it. Reference via preload(), not class_name, so it resolves without an
# editor re-import (same convention as veg_assets.gd).

const NovaObjectModelScript := preload("res://engine/object/nova_object_model.gd")
const ITEM_ATTRIB_NO_SHADOW := 0x04000000
const ITEM_ATTRIB2_DYNAMIC_SHADOW := 0x10
const ITEM_ATTRIB2_STATIC_SHADOW := 0x20
const ENTITY_ATTRIB_NO_SHADOW := 0x01000000
const EnvStamperScript := preload("res://engine/mission/mission_batch_env_stamper.gd")
const CollisionHull := preload("res://engine/object/collision_hull.gd")

const CONTAINER_NAME := "MissionObjects"
const RENDER_LOD := 0
const PLAYER_RUNTIME_TYPE_ID := 0x14B9
const PLAYER_VISUAL_ITEM_ID := 105310

var resource_root: NovaResourceRoot
var item_db: NovaItemDatabase
var _panm_clock

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

# Grouped source snapshot for the F3 user-point view. Static MultiMesh entities
# have no per-entity Node3D to discover later, so retain object data and BASE
# entity transforms only after a graphic successfully reaches a batch.
var _static_user_point_sources: Array = []

# Value-only source snapshot for mission-start ITEMS.DEF particlefx. Static
# MultiMesh entities have no Node3D owner after batching, so preserve each
# successfully rendered entity's item identity, object data, and BASE world
# transform. GameWorld consumes this through get_static_item_effect_sources()
# and submits world-bound groups; no renderer or emitter nodes cross this seam.
var _static_item_effect_sources: Array = []


# graphic -> NovaObjectData (or null when unresolvable).
var _object_data_cache: Dictionary = {}
# adm name -> NovaSkeletalAnim (or null when it failed to load). Shared read-only across
# every entity using the same anim_def (eval_pose is const, so sharing one instance is safe).
var _skeletal_cache: Dictionary = {}
# graphic -> Array[{ mesh, material, offset, submesh }] harvested from a template.
var _static_batch_cache: Dictionary = {}
# Per-graphic verdict of _graphic_needs_live_panm (graphic -> bool); epoch-cleared
# with the other caches.
var _graphic_panm_cache: Dictionary = {}
# Every unique harvested batch ShaderMaterial (the throwaway template model's
# materials outlive it on the MultiMesh batches) — update_environment()
# re-stamps these from the live env so static objects relight with TOD.
var _batch_materials: Array = []
var _last_batch_env_gen: int = -1
var _last_batch_env_values: NovaObjectModelScript.EnvLightValues = null
# graphic -> Vector3 ground anchor (model-space point that sits at the entity
# position). Computed once per graphic; see _ground_anchor_for.
var _anchor_cache: Dictionary = {}
# graphic -> Array[ConvexPolygonShape3D] collision hulls in model-local space (the
# editor's pickable bodies; see collision_shapes_for). Computed once per graphic.
var _collision_shapes_cache: Dictionary = {}
# item_id -> bool: the item's graphic carries occlusion/portal records (the
# de-batch predicate; see _has_occlusion_records). Computed once per item.
var _occlusion_cache: Dictionary = {}

# The global cache epoch (NovaResourceRoot.cache_epoch) the caches above were built
# under. Any root mount/rescan/clear bumps the epoch; the next cache access then
# self-clears, so a rescanned resource dir is never served stale models/anchors/hulls.
var _built_epoch: int = 0


func _init(p_resource_root: NovaResourceRoot = null, p_item_db: NovaItemDatabase = null) -> void:
	resource_root = p_resource_root
	item_db = p_item_db


func set_panm_clock(value) -> void:
	_panm_clock = value


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
	_graphic_panm_cache.clear()
	_batch_materials.clear()
	_last_batch_env_gen = -1
	_last_batch_env_values = null
	_anchor_cache.clear()
	_collision_shapes_cache.clear()
	_occlusion_cache.clear()


# Re-stamp every harvested static-batch material from the live environment.
# The batch materials are snapshots harvested from a throwaway model at build
# time; without this the static world would keep its load-time lighting while
# TOD advances — retail relights every entity from the current lighting block
# each frame [orig: setup_entity_lighting_and_shader_constants @ 0x5d98a0].
# Cheap when nothing changed: the env generation (or value-equality) gates the
# push exactly like NovaObjectModel's per-model stamp.
func update_environment(env_node: Node) -> void:
	if _batch_materials.is_empty():
		return
	var gen := -1
	if env_node != null and env_node.has_method("get_env_generation"):
		gen = int(env_node.get_env_generation())
		if gen == _last_batch_env_gen and _last_batch_env_values != null:
			return
	var values: NovaObjectModelScript.EnvLightValues = NovaObjectModelScript.environment_values_from(env_node)
	if values.equals(_last_batch_env_values):
		_last_batch_env_gen = gen
		return
	_last_batch_env_values = values
	_last_batch_env_gen = gen
	for material in _batch_materials:
		NovaObjectModelScript.apply_environment_values(material, values)


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
#  matrix as Rz(90-yaw) * Ry(-pitch) * Rx(roll) in its Z-up, right-handed world: yaw drives the Z/up
#  axis (euler[3] = 90 - yaw), while the builder applies stored positive euler[4] as Ry(-pitch);
#  roll is positive about X. Conjugating by the position basis M:(x,y,z)->(x,z,-y) -- which sends
#  engine +Z->godot
#  +Y, +Y->godot -Z, +X->godot +X -- gives the faithful Godot world rotation
#      R_godot = RotY(90 - yaw) * RotZ(pitch) * RotX(roll).
#  The .3di model imports Y-up / +Z-forward, so a constant model-forward correction C = RotY(90)
#  turns the model's +Z nose onto the engine's +X canonical heading. For yaw-only this collapses to
#  RotY(180 - yaw) -- identical to the long-standing (visually-correct) heading. See
#  godot/tests/mission_object_placer_test.gd.
static func bms_to_godot_basis(rot_deg: Vector3) -> Basis:
	var pitch := deg_to_rad(rot_deg.x)
	var yaw := deg_to_rad(rot_deg.y)
	var roll := deg_to_rad(rot_deg.z)
	return Basis(Vector3.UP, deg_to_rad(90.0) - yaw) \
		* Basis(Vector3.BACK, pitch) \
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
	_destruction_batches = {}
	_destruction_instances = {}
	_hidden_destruction_instances = {}
	_static_user_point_sources = []
	_static_item_effect_sources = []
	if mission == null or parent == null or resource_root == null:
		return stats
	_ensure_item_db()

	var env_node: Node = options.get("environment_node", null)
	# Optional load-stage attribution (the editor's mission open passes one).
	var timeline: PerfTimeline = options.get("timeline", null) as PerfTimeline
	# Optional per-model load-progress pulse (the game shell's loading screen);
	# invoked once per resolved graphic / animated entity, mirroring the
	# original's per-model loading-screen presents [orig: Game_StartMission's
	# in-loop LoadingScreen_UpdateAndPresent calls @ 0x524d9c/0x524f32].
	var progress: Callable = options.get("progress", Callable())
	# Optional entity-kind exclusion (NovaMissionData.KIND_*). The joiner places
	# the mission minus organics: players and streamed AI render wire-direct,
	# while items/buildings/markers become ordinary placed (batched, occludable)
	# nodes the wire present pass defers to by identity.
	var skip_kinds: Array = options.get("skip_kinds", [])
	var container := _ensure_container(parent)

	# Bucket entities by graphic, split static vs animated.
	PerfTimeline.span_on(timeline, "bucket_entities")
	var static_by_graphic: Dictionary = {}  # graphic -> Array[Transform3D]
	# Parallel eligibility flags. Retail composites pool-2 and StaticShadow
	# silhouettes into terrain tiles; keeping one slot per visible transform
	# lets destruction carve the matching shadow slot by the same BMS index.
	var static_shadow_by_graphic: Dictionary = {}  # graphic -> Array[bool]
	# Parallel value records retained only when that graphic resolves to a static
	# batch. Unlike edit-only pick refs, runtime item-effect production needs these
	# for every placed static entity.
	var static_effect_sources_by_graphic: Dictionary = {}  # graphic -> Array[Dictionary]
	# Parallel to static_by_graphic (same slot order); only filled in edit_mode so the
	# pickable index can map a MultiMesh instance back to its mission entity.
	var static_refs_by_graphic: Dictionary = {}  # graphic -> Array[{ kind, index }]
	# Parallel to static_by_graphic (same slot order), ALWAYS filled: the runtime
	# destruction pass carves a destroyed instance out of its batches by bms_id
	# (world-wac-ai-re §24.6 — the husk swap on batched statics).
	var static_ids_by_graphic: Dictionary = {}  # graphic -> Array[int bms_id]
	var animated: Array = []  # [{ graphic, xform, (kind, index in edit_mode) }]
	for e in mission.get_all_entities():
		var entity: Dictionary = e
		if not skip_kinds.is_empty() and int(entity.get("kind", -1)) in skip_kinds:
			continue
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
		if _needs_individual_node(item_id) or _graphic_needs_live_panm(graphic):
			# Always capture identity (not just edit_mode): the runtime needs it to tag the node so
			# MissionEntityRegistry can resolve SSN/group/zone event-action targets to this live model.
			animated.append({
				"graphic": graphic,
				"item_id": item_id,
				"xform": xform,
				"kind": int(entity.get("kind", -1)),
				"index": int(entity.get("index", -1)),
				"bms_id": int(entity.get("bms_id", 0)),
				"group": int(entity.get("group", -1)),
				"team": int(entity.get("team", -1)),
				"ai_flags": int(entity.get("ai_flags", 0)),
				"position": entity.get("position", Vector3.ZERO),
			})
		else:
			if not static_by_graphic.has(graphic):
				static_by_graphic[graphic] = []
				static_shadow_by_graphic[graphic] = []
				static_refs_by_graphic[graphic] = []
				static_effect_sources_by_graphic[graphic] = []
				static_ids_by_graphic[graphic] = []
			static_by_graphic[graphic].append(xform)
			static_shadow_by_graphic[graphic].append(
					item_casts_static_terrain_shadow(
						int(entity.get("kind", -1)),
						int(entity.get("ai_flags", 0)),
						item_db.get_attrib(item_id),
						item_db.get_attrib2(item_id)))
			static_ids_by_graphic[graphic].append(int(entity.get("bms_id", 0)))
			static_effect_sources_by_graphic[graphic].append({
				"kind": int(entity.get("kind", -1)),
				"item_id": item_id,
				"world_transform": xform,
			})
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
		if progress.is_valid():
			progress.call()
		var xforms: Array = static_by_graphic[graphic]
		var shadow_slots: Array = static_shadow_by_graphic.get(graphic, [])
		var has_static_shadow := true in shadow_slots
		var all_static_shadow := has_static_shadow and not (false in shadow_slots)
		var batches := _get_static_batches(graphic, env_node, container)
		if batches.is_empty():
			stats.unresolved += xforms.size()
			continue
		resolved_graphics.append(graphic)
		_record_static_user_point_group(graphic, xforms)
		_record_static_item_effect_group(
				graphic, static_effect_sources_by_graphic.get(graphic, []))
		stats.graphics += 1
		for batch in batches:
			var shadow_mm: MultiMesh = null
			var mm := MultiMesh.new()
			mm.transform_format = MultiMesh.TRANSFORM_3D
			mm.mesh = batch["mesh"]
			mm.instance_count = xforms.size()
			var offset: Transform3D = batch["offset"]
			for i in range(xforms.size()):
				mm.set_instance_transform(i, (xforms[i] as Transform3D) * offset)
			var mmi := MultiMeshInstance3D.new()
			mmi.multimesh = mm
			if all_static_shadow:
				# The reimpl's static directional approximation reaches only the
				# terrain receiver layer. An all-eligible visible batch can therefore
				# carry the static-caster marker without self-shadowing, avoiding
				# a full duplicate MultiMesh per submesh.
				mmi.layers = NovaWater.VISUAL_LAYER_WORLD \
						| NovaWater.VISUAL_LAYER_STATIC_SHADOW_CASTER
				mmi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
			else:
				mmi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
			if batch["material"] != null:
				mmi.material_override = batch["material"]
			mmi.name = "Batch_%s_%d" % [graphic, int(batch.get("submesh", 0))]
			container.add_child(mmi)
			stats.batches += 1
			_destruction_batches.get_or_add(graphic, []).append(mm)
			if has_static_shadow and not all_static_shadow:
				shadow_mm = MultiMesh.new()
				shadow_mm.transform_format = MultiMesh.TRANSFORM_3D
				shadow_mm.mesh = batch["mesh"]
				shadow_mm.instance_count = xforms.size()
				for i in range(xforms.size()):
					var shadow_xform := (xforms[i] as Transform3D) * offset
					if i >= shadow_slots.size() or not bool(shadow_slots[i]):
						shadow_xform.basis = shadow_xform.basis.scaled(Vector3.ZERO)
					shadow_mm.set_instance_transform(i, shadow_xform)
				var shadow_mmi := MultiMeshInstance3D.new()
				shadow_mmi.multimesh = shadow_mm
				shadow_mmi.layers = NovaWater.VISUAL_LAYER_STATIC_SHADOW_CASTER
				shadow_mmi.cast_shadow = \
						GeometryInstance3D.SHADOW_CASTING_SETTING_SHADOWS_ONLY
				if batch["material"] != null:
					shadow_mmi.material_override = batch["material"]
				shadow_mmi.name = "StaticShadow_%s_%d" % [
						graphic, int(batch.get("submesh", 0))]
				container.add_child(shadow_mmi)
				_destruction_batches.get_or_add(graphic).append(shadow_mm)
			if edit_mode:
				_record_static_batch(
						graphic,
						static_refs_by_graphic.get(graphic, []),
						mm,
						mmi,
						offset,
						batch["mesh"],
						shadow_mm,
						shadow_slots)
		stats.batched += xforms.size()
		stats.placed += xforms.size()
		var inst_ids: Array = static_ids_by_graphic.get(graphic, [])
		for i in range(mini(inst_ids.size(), xforms.size())):
			var iid := int(inst_ids[i])
			if iid != 0:
				_destruction_instances[iid] = {
					"graphic": graphic, "index": i,
					"xform": xforms[i] as Transform3D,
					"casts_static_shadow":
						i < shadow_slots.size() and bool(shadow_slots[i]),
				}
	PerfTimeline.end_on(timeline)

	# Align every harvested batch material with the env AS OF placement end —
	# the throwaway-template harvest sees mid-load values (e.g. the modulator
	# before its first iris tick) — and drop the per-frame stamper into the
	# container so the batches keep tracking TOD/weather/iris afterwards.
	_last_batch_env_values = null
	update_environment(env_node)
	_ensure_env_stamper(container, env_node)

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
		if progress.is_valid():
			progress.call()
		var data := _load_object_data(a["graphic"])
		if data == null:
			stats.unresolved += 1
			continue
		var model: Node3D = NovaObjectModelScript.new()
		model.set_panm_clock(_panm_clock)
		model.name = "Anim_%s_%d" % [a["graphic"], stats.animated]
		# Render the model origin at the entity's stored position directly. The engine bakes the
		# Ground userpoint into the stored position once, at author-time (place / terrain-drag), not
		# at render -- so a loaded .bms renders at its stored coords verbatim. [orig: sub_401A90, dfx2med.exe]
		model.transform = a["xform"] as Transform3D
		container.add_child(model)
		if env_node != null and model.has_method("set_environment_node"):
			model.set_environment_node(env_node)
		_configure_item_shadow(
				model,
				int(a.get("item_id", 0)),
				int(a.get("kind", -1)),
				int(a.get("ai_flags", 0)))
		_configure_item_lighting(model, int(a.get("item_id", 0)))
		# Load the entity's body-animation set (.adm) BEFORE the data: setting it
		# first is a no-op rebuild (no data yet), so set_object_data below does
		# the ONE skeletal-keyed mesh build - the old order built a static-keyed
		# set first and threw it away, doubling every animated entity's cost.
		# Rigid weapon parts fake-skin; no anim_def -> stays static.
		_apply_skeletal_anim(model, int(a.get("item_id", 0)),
				data.get_bone_origins(), data.get_bone_parents())
		# Drive the build explicitly (not via _ready) so it is independent of when
		# place() runs relative to the main loop; matches the static template path.
		model.set_object_data(data)
		if item_casts_static_terrain_shadow(
				int(a.get("kind", -1)),
				int(a.get("ai_flags", 0)),
				item_db.get_attrib(int(a.get("item_id", 0))),
				item_db.get_attrib2(int(a.get("item_id", 0)))):
			_add_individual_static_shadow_siblings(
					model,
					String(a.get("graphic", "")),
					Transform3D.IDENTITY,
					env_node,
					"live%d" % stats.animated)
		# Tag identity on the node in BOTH runtime + editor so MissionEntityRegistry can resolve
		# SSN/group/zone event-action targets (e.g. PLAYPARTANIM) back to this live model. Picking +
		# colliders stay editor-only.
		var ref := {
			"kind": int(a.get("kind", -1)),
			"index": int(a.get("index", -1)),
			"bms_id": int(a.get("bms_id", 0)),
			"group": int(a.get("group", -1)),
			"team": int(a.get("team", -1)),
			"position": a.get("position", Vector3.ZERO),
			# The items.def type id, so runtime passes can resolve per-item data
			# (e.g. the particlefx effect attach) without re-deriving it.
			"item_id": int(a.get("item_id", 0)),
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


## Build ONE animated NovaObjectModel for an item type, in its rest pose, parented under
## `parent` -- for an owner-managed entity with no BMS placement (the local-player avatar in
## first/third-person). The caller positions/orients it and toggles visibility; it is NOT
## tagged or registered for the present pass. Returns null when the item type has no
## resolvable graphic (the same resolution path the animated entities in place() use).
func build_animated_model(item_id: int, parent: Node3D, env_node: Node = null) -> Node3D:
	var graphic := _graphic_for(item_id)
	if graphic.is_empty():
		return null
	var data := _load_object_data(graphic)
	if data == null:
		return null
	var model: Node3D = NovaObjectModelScript.new()
	model.set_panm_clock(_panm_clock)
	model.name = "PlayerAvatar_%s" % graphic
	parent.add_child(model)
	if env_node != null and model.has_method("set_environment_node"):
		model.set_environment_node(env_node)
	_configure_item_shadow(model, item_id)
	_apply_skeletal_anim(model, item_id, data.get_bone_origins(), data.get_bone_parents())
	model.set_object_data(data)
	return model


## Resolve runtime-only player type ids to the authored items.def visual item. Keep the runtime
## entity item/type id unchanged; this only chooses graphics/ADM data for presentation.
func resolve_player_visual_item_id(runtime_type_id: int) -> int:
	_ensure_item_db()
	if runtime_type_id == PLAYER_RUNTIME_TYPE_ID and item_db != null and item_db.has_item(PLAYER_VISUAL_ITEM_ID):
		return PLAYER_VISUAL_ITEM_ID
	if item_db != null:
		if item_db.has_item(runtime_type_id):
			return runtime_type_id
		if runtime_type_id > 0 and runtime_type_id < NovaMissionData.ITEM_ID_OFFSET:
			var authored_item_id := runtime_type_id + NovaMissionData.ITEM_ID_OFFSET
			if item_db.has_item(authored_item_id):
				return authored_item_id
	return runtime_type_id


func build_player_animated_model(runtime_type_id: int, parent: Node3D, env_node: Node = null) -> Node3D:
	return build_animated_model(resolve_player_visual_item_id(runtime_type_id), parent, env_node)


## Build ONE animated NovaObjectModel from an EXPLICIT graphic (.3di basename) + an explicit .adm
## name, in rest pose, parented under `parent`. For owner-managed viewmodels that resolve their
## model + animation directly from weapon.def (gfx1/gfx1a + animadm) rather than from an items.def
## item id — the first-person weapon viewmodel. Returns null when the graphic doesn't resolve.
func build_model_from_graphic(graphic: String, adm_name: String, parent: Node3D, clip_key: String = "", env_node: Node = null, rig_graphic: String = "") -> Node3D:
	if graphic.is_empty() or parent == null:
		return null
	var data := _load_object_data(graphic)
	if data == null:
		return null
	var model: Node3D = NovaObjectModelScript.new()
	model.set_panm_clock(_panm_clock)
	model.name = "Viewmodel_%s" % graphic
	parent.add_child(model)
	if env_node != null and model.has_method("set_environment_node"):
		model.set_environment_node(env_node)
	if not adm_name.is_empty():
		# The ADM names the CLIP SET; the rig table belongs to the equipped FP gun. The arms
		# (armsG) and gun both use `rig_graphic`, so their indexed parts ride one shared table --
		# exactly as retail draws the arms with the GUN's bone matrices, not their own
		# [orig: Player_RenderFirstPersonViewModel @0x4ded60 reuses one bone_matrices for both
		# the gfx1 gun and the character-arms submit]. Defaulting to THIS graphic keeps a direct
		# gun build correct; composite callers pass the gun graphic explicitly for both parts.
		# Origins + parents = the model bone table: the rig sizes from the MODEL and its rest
		# positions are RECONSTRUCTED from the model pivots + the reset .bad's bind rotations
		# (NovaSkeletalAnim; the corpus-exact export relation) -- so rigs whose .bad and model
		# disagree in bone count (AKM_1st: 46 vs 45) and rigs with broken BadBone.position
		# (12 of 43 JO viewmodels) both render exactly like a healthy-.bad rig.
		var rig_name := graphic if rig_graphic.is_empty() else rig_graphic
		var skel_model := data if rig_name.nocasecmp_to(graphic) == 0 else _load_object_data(rig_name)
		var skel_origins := skel_model.get_bone_origins() if skel_model != null else PackedVector3Array()
		var skel_parents := skel_model.get_bone_parents() if skel_model != null else PackedInt32Array()
		_apply_skeletal_anim_by_name(model, adm_name, skel_origins, skel_parents)
	model.set_object_data(data)
	# Pose into a starting clip (e.g. the FP weapon idle "anim_wpn_idle" -> mp5_1i) so the model
	# holds that pose rather than its bind/T-pose; the model self-ticks the clip via _process.
	if not clip_key.is_empty() and model.has_method("play_body_clip"):
		model.play_body_clip(clip_key)
	return model


# Attach a skeletal anim set from an EXPLICIT .adm name (vs _apply_skeletal_anim, which resolves it
# from an item def's anim_def). Same per-.adm cache + load path; leaves the model static if the
# .adm fails to load.
func _apply_skeletal_anim_by_name(model: Node3D, adm_name_in: String, model_bone_origins := PackedVector3Array(), model_bone_parents := PackedInt32Array()) -> void:
	if model == null or resource_root == null:
		return
	var adm_name := adm_name_in if adm_name_in.to_lower().ends_with(".adm") else adm_name_in + ".adm"
	_apply_skeletal_from_adm(model, adm_name, model_bone_origins, model_bone_parents)


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

	if _needs_individual_node(item_id) or _graphic_needs_live_panm(graphic):
		var data := _load_object_data(graphic)
		if data == null:
			delta.unresolved = 1
			return delta
		var model: Node3D = NovaObjectModelScript.new()
		model.set_panm_clock(_panm_clock)
		model.name = "Anim_%s_k%d_i%d" % [graphic, kind, index]
		model.transform = xform
		container.add_child(model)
		if env_node != null and model.has_method("set_environment_node"):
			model.set_environment_node(env_node)
		_configure_item_shadow(
				model, item_id, kind, int(entity.get("ai_flags", 0)))
		_configure_item_lighting(model, item_id)
		# Skeletal set first = no-op rebuild; set_object_data does the one
		# skeletal-keyed build (same ordering rationale as place()).
		_apply_skeletal_anim(model, item_id,
				data.get_bone_origins(), data.get_bone_parents())
		model.set_object_data(data)
		if item_casts_static_terrain_shadow(
				kind,
				int(entity.get("ai_flags", 0)),
				item_db.get_attrib(item_id),
				item_db.get_attrib2(item_id)):
			_add_individual_static_shadow_siblings(
					model, graphic, Transform3D.IDENTITY, env_node,
					"k%d_i%d" % [kind, index])
		var ref := {
			"kind": kind,
			"index": index,
			"bms_id": int(entity.get("bms_id", 0)),
			"group": int(entity.get("group", -1)),
			"team": int(entity.get("team", -1)),
			"position": entity.get("position", Vector3.ZERO),
			# Same identity as place()'s animated branch — runtime passes (the
			# particlefx effect attach) resolve per-item data through it.
			"item_id": item_id,
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
	# A first-seen graphic just harvested fresh materials mid-load; align them
	# with the live env like place() does, and make sure the container carries
	# the per-frame stamper (a place_single onto a fresh container).
	_last_batch_env_values = null
	update_environment(env_node)
	_ensure_env_stamper(container, env_node)
	var refs := [{ "kind": kind, "index": index }]
	var casts_static_shadow := item_casts_static_terrain_shadow(
			kind,
			int(entity.get("ai_flags", 0)),
			item_db.get_attrib(item_id),
			item_db.get_attrib2(item_id))
	for batch in batches:
		var mm := MultiMesh.new()
		mm.transform_format = MultiMesh.TRANSFORM_3D
		mm.mesh = batch["mesh"]
		mm.instance_count = 1
		var offset: Transform3D = batch["offset"]
		mm.set_instance_transform(0, xform * offset)
		var mmi := MultiMeshInstance3D.new()
		mmi.multimesh = mm
		if casts_static_shadow:
			# As in an all-eligible pooled batch, the isolated static light can
			# use this visible instance directly: its receiver mask cannot feed
			# the silhouette back onto the model.
			mmi.layers = NovaWater.VISUAL_LAYER_WORLD \
					| NovaWater.VISUAL_LAYER_STATIC_SHADOW_CASTER
			mmi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
		else:
			mmi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		if batch["material"] != null:
			mmi.material_override = batch["material"]
		mmi.name = "Place_%s_k%d_i%d_s%d" % [graphic, kind, index, int(batch.get("submesh", 0))]
		container.add_child(mmi)
		delta.batches += 1
		_record_static_batch(graphic, refs, mm, mmi, offset, batch["mesh"])
	add_pick_collider(container, kind, index, graphic, xform)
	_append_static_user_point_source(graphic, xform)
	_append_static_item_effect_source(kind, item_id, graphic, xform)
	delta.placed = 1
	delta.batched = 1
	return delta


# --- Internals ----------------------------------------------------------------

func _add_individual_static_shadow_siblings(
		model: Node3D, graphic: String, local_xform: Transform3D,
		env_node: Node, suffix: String) -> void:
	# Visible portal/PANM models live below camera-masked ROBJ nodes. Retail's
	# terrain-tile collector ignores those masks and submits every selected-LOD
	# ROBJ, so harvest one independent all-section shadow-only sibling per
	# submesh. The reimpl's static light reaches only the terrain receiver.
	var batches := _get_static_batches(graphic, env_node, model)
	for batch in batches:
		var mm := MultiMesh.new()
		mm.transform_format = MultiMesh.TRANSFORM_3D
		mm.mesh = batch["mesh"]
		mm.instance_count = 1
		var offset: Transform3D = batch["offset"]
		mm.set_instance_transform(0, local_xform * offset)
		var mmi := MultiMeshInstance3D.new()
		mmi.multimesh = mm
		mmi.layers = NovaWater.VISUAL_LAYER_STATIC_SHADOW_CASTER
		mmi.cast_shadow = \
				GeometryInstance3D.SHADOW_CASTING_SETTING_SHADOWS_ONLY
		if batch["material"] != null:
			mmi.material_override = batch["material"]
		mmi.name = "StaticShadow_%s_%s_%d" % [
				graphic, suffix, int(batch.get("submesh", 0))]
		model.add_child(mmi)


func _record_static_user_point_group(graphic: String, transforms: Array) -> void:
	var data := _load_object_data(graphic)
	if data == null or data.get_user_point_count() <= 0:
		return
	_static_user_point_sources.append({
		"graphic": graphic,
		"object_data": data,
		"transforms": transforms.duplicate(),
	})


func _append_static_user_point_source(graphic: String, xform: Transform3D) -> void:
	var data := _load_object_data(graphic)
	if data == null or data.get_user_point_count() <= 0:
		return
	for source_index in range(_static_user_point_sources.size()):
		var source: Dictionary = _static_user_point_sources[source_index]
		if String(source.get("graphic", "")) != graphic:
			continue
		var transforms: Array = source.get("transforms", []).duplicate()
		transforms.append(xform)
		source["transforms"] = transforms
		_static_user_point_sources[source_index] = source
		return
	_static_user_point_sources.append({
		"graphic": graphic,
		"object_data": data,
		"transforms": [xform],
	})


func _record_static_item_effect_group(graphic: String, sources: Array) -> void:
	var data := _load_object_data(graphic)
	if data == null:
		return
	for source_v in sources:
		var source: Dictionary = source_v
		_static_item_effect_sources.append({
			"kind": int(source.get("kind", -1)),
			"item_id": int(source.get("item_id", 0)),
			"graphic": graphic,
			"world_transform": source.get("world_transform", Transform3D.IDENTITY),
			"object_data": data,
		})


func _append_static_item_effect_source(kind: int, item_id: int, graphic: String,
		xform: Transform3D) -> void:
	var data := _load_object_data(graphic)
	if data == null:
		return
	_static_item_effect_sources.append({
		"kind": kind,
		"item_id": item_id,
		"graphic": graphic,
		"world_transform": xform,
		"object_data": data,
	})

# Emit one pickable record per entity slot in a freshly-built static batch. Each
# entity's slot `i` is consistent across every submesh batch of the same graphic
# (instance_count == entity count), so moving entity i means rewriting instance i in
# every batch that shares its graphic. mesh_aabb (under the instance transform) gives
# the editor a tight pick volume without per-instance physics bodies.
func _record_static_batch(
		graphic: String, refs: Array, mm: MultiMesh,
		mmi: MultiMeshInstance3D, offset: Transform3D, mesh: Mesh,
		shadow_mm: MultiMesh = null, shadow_slots: Array = []) -> void:
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
			"shadow_mm": shadow_mm,
			"casts_static_shadow":
				i < shadow_slots.size() and bool(shadow_slots[i]),
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



## Snapshot of successfully rendered static user-point sources for a world debug
## view. Object data is immutable/shared; transform arrays are duplicated so a
## consumer cannot mutate the placer's placement record.
func get_static_user_point_sources() -> Array:
	_check_epoch()
	var out: Array = []
	for row_v in _static_user_point_sources:
		var row: Dictionary = row_v
		var transforms: Array = row.get("transforms", [])
		out.append({
			"graphic": String(row.get("graphic", "")),
			"object_data": row.get("object_data"),
			"transforms": transforms.duplicate(),
		})
	return out


## Snapshot of successfully rendered static entities for mission-start item
## effects. Every row is a value descriptor; object_data is immutable/shared and
## no placed/render Node is exposed. The one-row-per-entity order is placement
## order within the resolved graphic batches and remains stable for the mission.
func get_static_item_effect_sources() -> Array:
	_check_epoch()
	var out: Array = []
	for row_v in _static_item_effect_sources:
		var row: Dictionary = row_v
		out.append({
			"kind": int(row.get("kind", -1)),
			"item_id": int(row.get("item_id", 0)),
			"graphic": String(row.get("graphic", "")),
			"world_transform": row.get("world_transform", Transform3D.IDENTITY),
			"object_data": row.get("object_data"),
		})
	return out


func _graphic_for(item_id: int) -> String:
	if item_db == null:
		return ""
	return item_db.get_graphic(item_id)


# An item that cannot ride the pooled MultiMesh batch and needs its own
# NovaObjectModel: animated items (persons, anim-def carriers), plus
# portal-carrying buildings — the render-occlusion frame drives per-section
# (Robj) visibility masks, and a pooled batch has no per-instance section
# handle. [orig: g_BuildingSectionVisMask consumption,
# Terrain_RenderSectorModels @ 0x5c5d30; docs/render/render-occlusion-re.md §5]
func _needs_individual_node(item_id: int) -> bool:
	if item_db == null:
		return false
	var item_type := item_db.get_item_type(item_id)
	if item_type == NovaItemDatabase.TYPE_PERSON:
		return true
	if item_casts_dynamic_shadow(
			item_type, item_db.get_attrib(item_id), item_db.get_attrib2(item_id)):
		return true
	if not item_db.get_anim_def(item_id).is_empty():
		return true
	return _has_occlusion_records(item_id)


static func item_casts_dynamic_shadow(
		item_type: int, _attrib: int, attrib2: int) -> bool:
	return item_type == NovaItemDatabase.TYPE_PERSON \
			or (attrib2 & ITEM_ATTRIB2_DYNAMIC_SHADOW) != 0


static func item_casts_static_terrain_shadow(
		kind: int, entity_attrib: int, item_attrib: int,
		item_attrib2: int) -> bool:
	if (entity_attrib & ENTITY_ATTRIB_NO_SHADOW) != 0 \
			or (item_attrib & ITEM_ATTRIB_NO_SHADOW) != 0:
		return false
	return kind == NovaMissionData.KIND_BUILDING \
			or (kind == NovaMissionData.KIND_ITEM \
				and (item_attrib2 & ITEM_ATTRIB2_STATIC_SHADOW) != 0)


func _configure_item_shadow(
		model: Node, item_id: int, _kind: int = -1,
		_entity_attrib: int = 0) -> void:
	if model == null or item_db == null:
		return
	model.set_shadow_caster_enabled(item_casts_dynamic_shadow(
			item_db.get_item_type(item_id),
			item_db.get_attrib(item_id),
			item_db.get_attrib2(item_id)))
	# The static tile pass must ignore the visible model's portal/section
	# mask. Eligible mission entities get independent all-section siblings
	# after their visible model is built.
	model.set_static_shadow_caster_enabled(false)


func _configure_item_lighting(model: Node, item_id: int) -> void:
	if model == null or item_db == null:
		return
	# Retail's building collector marks ROBJ 1+ as interior-lighting entries
	# while ROBJ 0 remains the exterior shell. Only portal buildings take this
	# model-section path; people and live-PANM decorations still use ordinary
	# per-entity lighting.
	# [orig: Terrain_RenderSectorModels @0x5C5D30;
	#  collect_render_objects_for_batch @0x5D9156..0x5D9170]
	if item_db.get_item_type(item_id) != NovaItemDatabase.TYPE_BUILDING \
			or not _has_occlusion_records(item_id):
		return
	model.set_interior_section_light_transfer(
			item_db.get_light_transfer(item_id))


func _has_occlusion_records(item_id: int) -> bool:
	if _occlusion_cache.has(item_id):
		return _occlusion_cache[item_id]
	var has_occ := false
	var graphic := _graphic_for(item_id)
	if not graphic.is_empty():
		var data := _load_object_data(graphic)
		if data != null:
			has_occ = data.has_occlusion()
	_occlusion_cache[item_id] = has_occ
	return has_occ


# A graphic whose model carries a live PANM track must not be frozen into a MultiMesh
# batch: the batch harvest captures the rest pose and never evaluates PANM again, while
# the original engine rebuilds PANM node matrices from the global millisecond clock for
# every rendered object, every frame — free-running decorations (pump jacks, radar
# dishes) animate with no mission action involved, and SET/register-driven tracks pose
# parts away from rest. Inert PANM blocks (no family flags or every control idle) keep
# static batching. Mirrors the evaluator's own gates: the entry-level animated check and
# the per-track idle check. [orig: PANM_BuildNodeMatrices track gates + PANM_SampleTrack
# (sub_4354B0) idle gate (control & 0xF0), Render_ShaderTickMs @0x2721A40 — ported in
# libs/threedi/src/threedi_panm_matrices.cpp / threedi_panm_runtime.cpp]
func _graphic_needs_live_panm(graphic: String) -> bool:
	if _graphic_panm_cache.has(graphic):
		return bool(_graphic_panm_cache[graphic])
	var result := false
	var data := _load_object_data(graphic)
	if data != null:
		result = bool(data.has_live_panm())
	_graphic_panm_cache[graphic] = result
	return result


func _model_name_for(graphic: String) -> String:
	var basename := graphic.get_file().get_basename()
	return "" if basename.is_empty() else basename + ".3di"


# Resolve an animated entity's body-animation set from its item def's anim_def and attach it to
# the model so its Skeleton3D builds. Cached per .adm (shared read-only across entities). A model
# with an empty anim_def, or whose .adm fails to load, is left static (unchanged behaviour).
func _apply_skeletal_anim(model: Node3D, item_id: int,
		model_bone_origins := PackedVector3Array(),
		model_bone_parents := PackedInt32Array()) -> void:
	if model == null or resource_root == null or item_db == null:
		return
	var anim_def := item_db.get_anim_def(item_id)
	if anim_def.is_empty():
		return
	var adm_name := anim_def if anim_def.to_lower().ends_with(".adm") else anim_def + ".adm"
	var skeletal = _skeletal_from_adm(adm_name, model_bone_origins, model_bone_parents)
	if skeletal != null and model.has_method("set_skeletal_anim"):
		model.set_skeletal_anim(skeletal)


# Attach a skeletal set from a resolved .adm name, feeding the .3di model's bone table
# (get_bone_origins + get_bone_parents) so the rig builds from the MODEL, never the lossy .bad
# bone records -- with parents supplied the model defines the count/hierarchy and the rest
# positions reconstruct from the model pivots + the reset .bad's bind rotations, the witnessed
# original's rig source [orig: BoneAnim_BuildWorldMatrices @0x40c400 walks modelDef+56]. The
# cache key folds in the table: two models can share one .adm (the FP arms + gun both use
# ak47_1st.adm) yet carry different .3di pivots, so they must not alias. See NovaSkeletalAnim.
func _apply_skeletal_from_adm(model: Node3D, adm_name: String, model_bone_origins: PackedVector3Array, model_bone_parents := PackedInt32Array()) -> void:
	var skeletal = _skeletal_from_adm(adm_name, model_bone_origins, model_bone_parents)
	if skeletal != null and model.has_method("set_skeletal_anim"):
		model.set_skeletal_anim(skeletal)


func _skeletal_from_adm(adm_name: String, model_bone_origins: PackedVector3Array,
		model_bone_parents := PackedInt32Array()):
	var cache_key := adm_name + "#" + str(hash(model_bone_origins)) + "#" + str(hash(model_bone_parents))
	var skeletal
	if _skeletal_cache.has(cache_key):
		skeletal = _skeletal_cache[cache_key]
	else:
		skeletal = NovaSkeletalAnim.new()
		if not skeletal.load_from_resource_root(resource_root, adm_name, model_bone_origins, model_bone_parents):
			skeletal = null
		_skeletal_cache[cache_key] = skeletal
	return skeletal


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
	if data != null:
		anchor = data.get_ground_anchor(RENDER_LOD)
	_anchor_cache[graphic] = anchor
	return anchor


# Public: the Godot model-local ground anchor for `graphic` (resolves + caches the model). The editor
# subtracts this from a terrain-drop position (converted to BMS axes) for the author-time ground bake.
func ground_anchor_godot(graphic: String) -> Vector3:
	_check_epoch()
	return _ground_anchor_for(graphic, _load_object_data(graphic))


func object_data_for(graphic: String) -> NovaObjectData:
	_check_epoch()
	return _load_object_data(graphic)


## Authoritative read-only skeletal set for simulation collision. Uses the same
## ADM + canonical model bone-table cache as the rendered NovaObjectModel, so
## headless per-bone collision cannot drift onto lossy BAD parents/pivots.
func skeletal_anim_for(item_id: int, graphic: String):
	_check_epoch()
	if resource_root == null or item_db == null:
		return null
	var data := _load_object_data(graphic)
	if data == null:
		return null
	var anim_def := item_db.get_anim_def(item_id)
	if anim_def.is_empty():
		return null
	var adm_name := anim_def if anim_def.to_lower().ends_with(".adm") else anim_def + ".adm"
	return _skeletal_from_adm(
			adm_name, data.get_bone_origins(), data.get_bone_parents())


# --- destruction support (world-wac-ai-re §24.6) -------------------------------
# Batched statics have no per-entity node; a destroyed one is carved out of its
# graphic's MultiMesh batches (zero-scale at its own origin — the batch keeps its
# instance count) and the caller grafts the husk model at the returned transform.
var _destruction_batches: Dictionary = {}   # graphic -> Array[MultiMesh]
var _destruction_instances: Dictionary = {} # bms_id -> { graphic, index, xform }
var _hidden_destruction_instances: Dictionary = {} # bms_id -> exact per-batch transforms


## Read-only world transform for a batched static, used by diagnostics that
## compare its visual placement with the simulation collision instance.
## Returns null when the bms_id is unknown.
func get_static_instance_transform(bms_id: int) -> Variant:
	var rec: Variant = _destruction_instances.get(bms_id)
	if not (rec is Dictionary):
		return null
	return (rec as Dictionary).xform


func static_instance_casts_terrain_shadow(bms_id: int) -> bool:
	var rec: Variant = _destruction_instances.get(bms_id)
	return rec is Dictionary \
			and bool((rec as Dictionary).get("casts_static_shadow", false))


## Hide a destroyed batched static in every batch of its graphic. Returns the
## instance's placed transform (for the husk graft), or null when unknown.
func hide_static_instance(bms_id: int) -> Variant:
	var rec: Variant = _destruction_instances.get(bms_id)
	if not (rec is Dictionary):
		return null
	var graphic := String(rec['graphic'])
	var index := int(rec['index'])
	var xform: Transform3D = rec['xform']
	if _hidden_destruction_instances.has(bms_id):
		return xform
	var carved := Transform3D(Basis().scaled(Vector3.ZERO), xform.origin)
	var originals: Array = []
	for mm_v in _destruction_batches.get(graphic, []):
		var mm := mm_v as MultiMesh
		if mm != null and index >= 0 and index < mm.instance_count:
			originals.append({
				'multimesh': mm,
				'transform': mm.get_instance_transform(index),
				'index': index,
			})
			mm.set_instance_transform(index, carved)
	_hidden_destruction_instances[bms_id] = originals
	return xform


## Restore a static carved by hide_static_instance(). The placer owns the
## MultiMeshes, so it also owns the exact per-batch transforms needed to undo a
## destruction presentation reset. Returns false when the instance was not
## hidden (including unknown bms_ids); repeated reset calls are therefore safe.
func show_static_instance(bms_id: int) -> bool:
	var originals_v: Variant = _hidden_destruction_instances.get(bms_id)
	if not (originals_v is Array):
		return false
	for saved_v in originals_v as Array:
		if not (saved_v is Dictionary):
			continue
		var saved: Dictionary = saved_v
		var mm: Variant = saved.get('multimesh')
		var index := int(saved.get('index', -1))
		if mm is MultiMesh and index >= 0 and index < (mm as MultiMesh).instance_count:
			(mm as MultiMesh).set_instance_transform(
					index, saved.get('transform', Transform3D.IDENTITY))
	_hidden_destruction_instances.erase(bms_id)
	return true


## Register an already-resolved object plus its static render batches. This is
## the construction seam for callers that already own parsed geometry (including
## asset-free tests); registrations follow the same resource-root epoch as
## lazily loaded graphics and are copied so caller dictionaries stay isolated.
func register_resolved_static_graphic(graphic: String, data: NovaObjectData,
		batches: Array) -> bool:
	_check_epoch()
	if graphic.is_empty() or data == null or batches.is_empty():
		return false
	var retained_batches: Array = []
	for batch_v in batches:
		if not (batch_v is Dictionary):
			return false
		var batch: Dictionary = batch_v
		if not (batch.get("mesh") is Mesh):
			return false
		retained_batches.append(batch.duplicate())
	_object_data_cache[graphic] = data
	_static_batch_cache[graphic] = retained_batches
	return true


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
	if data != null:
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
	if data == null:
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
					if mi.material_override is ShaderMaterial and not _batch_materials.has(mi.material_override):
						_batch_materials.append(mi.material_override)
					submesh += 1
		tree_parent.remove_child(model)
		model.free()
	_static_batch_cache[graphic] = batches
	return batches


# Keep exactly one per-frame batch-relight driver alive inside the container
# (see mission_batch_env_stamper.gd; the container is rebuilt on re-bake, so
# the stamper's lifetime follows the placed set). No-op without an env node.
func _ensure_env_stamper(container: Node3D, env_node: Node) -> void:
	if container == null or env_node == null:
		return
	var existing := container.get_node_or_null(NodePath("EnvRestamp"))
	if existing != null:
		existing.set("placer", self)
		existing.set("environment_node", env_node)
		return
	var stamper: Node = EnvStamperScript.new()
	stamper.name = "EnvRestamp"
	stamper.placer = self
	stamper.environment_node = env_node
	container.add_child(stamper)


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
