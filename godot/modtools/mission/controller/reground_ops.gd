extends "res://modtools/mission/controller/controller_section.gd"

# Terrain reconcile + the re-ground stack (drift scan, cached
# requests, apply, baseline bookkeeping).
# Moved verbatim from mission_controller.gd (F5); state stays on the
# controller, reached through `_c`.

# If the terrain underneath was swapped out from under a loaded mission (the user
# opened a different terrain in the Terrain workspace), the placed objects no
# longer belong to the mounted world. Drop the mission so its objects / metadata
# stop describing a world that is no longer there; re-opening shows it on its own
# terrain again. Called when the Mission workspace regains focus.
#
# When the SAME terrain is still mounted but its heights were edited since the
# mission loaded (or since drift was last resolved), returns how many entities'
# ground points no longer sit on the surface — the workspace prompts to re-ground
# on a non-zero count. Returns 0 on the swap/clear and no-drift paths.
func reconcile_with_terrain() -> int:
	if _c._mission == null or _c.terrain_editor == null or not _c.terrain_editor.has_method("get_current_trn_path"):
		return 0
	if String(_c.terrain_editor.get_current_trn_path()) != _c._loaded_trn_path:
		_c._io.clear()
		return 0
	# Same terrain file: detect height edits made under the loaded mission. The
	# revision gate keeps the common no-edit activate at zero cost (no request
	# build, no sampling).
	if _c._loaded_height_revision < 0 or not _c.terrain_editor.has_method("get_height_revision"):
		return 0
	if int(_c.terrain_editor.get_height_revision()) == _c._loaded_height_revision:
		return 0
	# Wall-clock attribution for the post-edit drift scan (the workspace-switch
	# hitch): created only past the revision gate, so the common no-edit activate
	# stays unmeasured and free. finish() retains it in the PerfTimeline ring for
	# the debug overlay's perf pane; the scan has no status line of its own.
	var timeline := PerfTimeline.begin("Mission drift scan")
	var count := _count_terrain_drift(timeline)
	if count == 0:
		# The height edits missed every object; settle on the new revision so
		# later activates skip the scan.
		_record_height_revision()
	timeline.finish()
	return count


func _record_height_revision() -> void:
	if _c.terrain_editor != null and _c.terrain_editor.has_method("get_height_revision"):
		_c._loaded_height_revision = int(_c.terrain_editor.get_height_revision())
	else:
		_c._loaded_height_revision = -1


# The validity token for _reground_requests_cache (see the cache comment at the
# declaration). Empty when a build precondition is missing — never cached, every
# call rebuilds, exactly the pre-cache behavior.
func _reground_token() -> Array:
	if _c._mission == null or _c._placer == null or _c.terrain_editor == null \
			or not _c.terrain_editor.has_method("sample_height_world") \
			or not _c.terrain_editor.has_method("get_height_revision"):
		return []
	return [
		_c._mission.get_instance_id(),
		_c._mission.object_records_revision(),
		int(_c.terrain_editor.get_height_revision()),
		NovaResourceRoot.cache_epoch(),
	]


# The full (unfiltered) re-ground request set, built at most once per token.
func _reground_requests_cached() -> Array:
	var token := _reground_token()
	if token.is_empty():
		return _build_reground_requests()
	if token != _c._reground_cache_token:
		_c._reground_requests_cache = _build_reground_requests()
		_c._reground_cache_token = token
	return _c._reground_requests_cache


# Adopt the current terrain + entity layout as the known-grounded reference: the
# height revision plus the surface memo under every entity's ground point.
func _record_ground_state() -> void:
	_record_height_revision()
	_c._ground_baseline = {}
	for r in _reground_requests_cached():
		var request: Dictionary = r
		var hit: Vector3 = request["ground_hit_bms"]
		_c._ground_baseline[_c._ground_key(hit)] = hit.z


# The re-ground request set restricted to entities whose ground SURFACE moved
# since the baseline. A row whose sampled height still matches its memo is an
# entity sitting at an author-chosen offset over unchanged terrain — excluded, so
# a bulk re-ground can never flatten it. Rows without a memo (placed or dragged
# since the baseline) ride through to the engine's own deviation check, which
# skips them unless they are genuinely off the surface.
func _drifted_requests() -> Array:
	var requests: Array = []
	for r in _reground_requests_cached():
		var request: Dictionary = r
		var hit: Vector3 = request["ground_hit_bms"]
		var key = _c._ground_key(hit)
		if _c._ground_baseline.has(key) and absf(hit.z - float(_c._ground_baseline[key])) <= _c.REGROUND_EPSILON:
			continue
		requests.append(request)
	return requests


# Dry-run count through the engine's re-ground policy: the prompt count and the
# apply share one request set and one epsilon (literally one token-keyed cached
# build, see _reground_requests_cached), so the count can never lie.
func _count_terrain_drift(timeline: PerfTimeline = null) -> int:
	PerfTimeline.span_on(timeline, "requests")
	var requests := _drifted_requests()
	PerfTimeline.end_on(timeline)
	if requests.is_empty():
		return 0
	PerfTimeline.span_on(timeline, "count")
	var count: int = _c._mission.reground_entities(requests, _c.REGROUND_EPSILON, false)
	PerfTimeline.end_on(timeline)
	return count


## Snap every entity whose ground moved back onto the terrain surface as ONE undo
## step, then update the moved entities' placed world in place (a re-ground is a
## pure-z move of existing entities — no membership change — so re-baking every
## placed object would be waste; an unmappable record falls back to the full
## re-bake). Returns how many entities moved. Adopts the new ground state
## afterwards (the drift is resolved). Serves both the activate-time prompt and
## the inspector's manual button; entities authored off the surface on purpose
## are protected by the baseline filter (see _ground_baseline).
func reground_drifted() -> int:
	if _c._mission == null:
		return 0
	var timeline := PerfTimeline.begin("Mission re-ground")
	timeline.span("requests")
	var requests := _drifted_requests()
	timeline.end_span()
	_c._flush_edit()
	_c._mission.begin_edit()
	timeline.span("apply")
	var result: Dictionary = _c._mission.reground_entities_apply(requests, _c.REGROUND_EPSILON)
	var moved := int(result.get("moved", 0))
	timeline.end_span()
	_c._mission.commit_edit() # pushes one step only if something actually moved
	# The apply is pure-z (x/y preserved exactly by the conjugate bake, see
	# _build_reground_requests) and the surface did not change, so the cached rows
	# are byte-valid for the post-apply document; re-key the token so the baseline
	# re-record below reuses them instead of re-marshalling + resampling the world.
	_c._reground_cache_token = _reground_token()
	timeline.span("baseline")
	_record_ground_state()
	timeline.end_span()
	if moved > 0:
		timeline.span("update")
		if not _apply_reground_world_update(requests,
				result.get("rows", PackedInt32Array()),
				result.get("positions", PackedVector3Array())):
			_c._rebake_objects()
		timeline.end_span()
		_c.mark_dirty()
		timeline.finish()
		_c._report("Re-grounded %d object%s (%s)." % [moved, "" if moved == 1 else "s", timeline.brief(3)])
	else:
		timeline.finish()
		_c._report("No objects needed re-grounding.")
	return moved


## Snap EVERY entity onto the current surface as one undo step — the bulk-repair
## seam (MCP reground_mission). Unlike reground_drifted there is NO baseline
## filter: rows whose terrain never moved are re-grounded too, which is exactly
## the mis-grounded-mission repair the drift path is designed to skip (its
## filter protects deliberate off-surface authoring; this seam plants those
## too, so callers must warn). Returns { checked, moved }.
func reground_all() -> Dictionary:
	if _c._mission == null:
		return { "checked": 0, "moved": 0 }
	var requests := _reground_requests_cached()
	_c._flush_edit()
	_c._mission.begin_edit()
	var result: Dictionary = _c._mission.reground_entities_apply(requests, _c.REGROUND_EPSILON)
	var moved := int(result.get("moved", 0))
	_c._mission.commit_edit() # pushes one step only if something actually moved
	# Same pure-z reasoning as reground_drifted: the cached rows stay byte-valid
	# for the post-apply document, so re-key the token before re-recording.
	_c._reground_cache_token = _reground_token()
	_record_ground_state()
	if moved > 0:
		if not _apply_reground_world_update(requests,
				result.get("rows", PackedInt32Array()),
				result.get("positions", PackedVector3Array())):
			_c._rebake_objects()
		_c.mark_dirty()
	_c._report("Re-grounded %d of %d entities." % [moved, requests.size()])
	return { "checked": requests.size(), "moved": moved }


# Post-apply world sync for a bulk re-ground: rewrite only the moved entities'
# MultiMesh slots / animated nodes / pick bodies in place — the same absolute
# writes _apply_selected_xform does for the selection — instead of re-baking
# every placed object. A re-ground changes no membership, so _stats,
# _membership_rev, and the inspector's option caches all stay valid (none hold
# positions); the preview registry's entity_ref positions go stale exactly as
# they do after a drag and refresh on the next re-bake. A moved entity with
# zero pickable records is skipped, not a failure: the placer found it
# unresolved at place time, so nothing is rendered for it (this also keeps
# headless owners on the targeted path). Returns false when a matched record's
# backing node was freed underneath us — the caller falls back to the full
# re-bake, which rebuilds everything from the document.
func _apply_reground_world_update(requests: Array, moved_rows: PackedInt32Array,
		new_positions_bms: PackedVector3Array) -> bool:
	# Container-local transform per moved entity, keyed "kind:index". Rotation is
	# unchanged by a re-ground; the row carried it so nothing is re-marshalled.
	var moved: Dictionary = {}
	var any_marker := false
	for n in moved_rows.size():
		var request: Dictionary = requests[moved_rows[n]]
		var kind := int(request.get("kind", -1))
		if kind == NovaMissionData.KIND_MARKER:
			any_marker = true
		moved["%d:%d" % [kind, int(request.get("index", -1))]] = _c.MissionObjectPlacer.entity_transform(
				new_positions_bms[n], request.get("rotation_deg", Vector3.ZERO))
	for r in _c._pickable:
		var rec: Dictionary = r
		var key := "%d:%d" % [int(rec["kind"]), int(rec["index"])]
		if not moved.has(key):
			continue
		var xform: Transform3D = moved[key]
		if bool(rec.get("animated", false)):
			var node = rec.get("node")
			if node == null or not is_instance_valid(node):
				return false
			node.transform = xform * (rec.get("offset", Transform3D.IDENTITY) as Transform3D)
		else:
			var mmi = rec.get("mmi")
			if mmi == null or not is_instance_valid(mmi):
				return false
			var mm: MultiMesh = rec["mm"]
			mm.set_instance_transform(int(rec["slot"]), xform * (rec["offset"] as Transform3D))
	# Pick bodies sit at the entity transform directly (the drag path's lockstep
	# write); a missing body is normal (markers, shapeless or unresolved models).
	var container = _c._objects_container()
	if container != null:
		for n in moved_rows.size():
			var request: Dictionary = requests[moved_rows[n]]
			var kind := int(request.get("kind", -1))
			if kind == NovaMissionData.KIND_MARKER:
				continue
			var index := int(request.get("index", -1))
			var body := container.get_node_or_null(NodePath("Pick_%d_%d" % [kind, index])) as Node3D
			if body != null:
				body.transform = moved["%d:%d" % [kind, index]]
	# A selected entity re-syncs its box / gizmo / bound collider through the one
	# shared writer (rotation unchanged: pure-z). The selection SURVIVES a
	# targeted re-ground — only the re-bake fallback still drops it.
	if not _c._selected_ref.is_empty():
		var sel_key := "%d:%d" % [int(_c._selected_ref.get("kind", -1)), int(_c._selected_ref.get("index", -1))]
		if moved.has(sel_key):
			_c._viewport._apply_selected_xform(moved[sel_key])
	# Markers are mesh-less; their gizmos live in the active mode's overlay.
	if any_marker:
		_c._refresh_active_overlay()
	if _c._pick_debug:
		_c._viewport._refresh_pick_debug()
	return true


## Decline path for the activate-time prompt: adopt the current height revision so
## the prompt stays quiet until the NEXT height edit. The surface memo is kept, so
## the drift stays visible to the manual re-ground button (declining the question
## is not the same as calling the layout grounded). NOTE an undo of an APPLIED
## re-ground re-arms neither path today: the apply adopted the new surface as the
## baseline, so the restored pre-apply positions read as authored offsets over
## unchanged terrain until the next height edit. Re-arming after undo (tagging the
## re-ground's undo step and dropping its baseline rows on restore) is an open
## follow-up.
func acknowledge_terrain_drift() -> void:
	_record_height_revision()


# One re-ground request per entity, sampling the terrain under each entity's ROTATED
# ground anchor. The engine bakes position = hit - R*anchor (authoring.cpp
# bake_ground_transform, with R including the Rz(90) model-forward correction even at
# zero rotation), and bms_to_godot_basis is the exact conjugate of that R (parity
# pinned by mission_object_placer_test.gd) — so adding the rotated anchor back on
# first, mirroring the drop bake in _move_selected_to_world (basis * ground offset),
# makes the apply a pure z re-ground: x/y are preserved exactly and an entity
# already on the surface never counts as drifted. Unresolved items (no items.def
# row -> unknown anchor) and off-terrain samples (NAN) are skipped; markers ground
# their own origin (anchor ZERO, the engine stores the hit directly).
func _build_reground_requests() -> Array:
	var requests: Array = []
	if _c._mission == null or _c._placer == null:
		return requests
	if _c.terrain_editor == null or not _c.terrain_editor.has_method("sample_height_world"):
		return requests
	# Pass 1: walk the entities, resolving each one's rotated ground point (and
	# skipping unresolved graphics); the sample points land in a parallel packed
	# array so the surface query is ONE batched call, not ~6 boundary crossings
	# per entity.
	var rows: Array = []
	var points := PackedVector2Array()
	for e in _c._mission.get_all_entities():
		var entity: Dictionary = e
		var kind := int(entity.get("kind", -1))
		var ground_godot: Vector3 = _c.MissionObjectPlacer.bms_to_godot_position(entity.get("position", Vector3.ZERO))
		var anchor_bms := Vector3.ZERO
		if kind != NovaMissionData.KIND_MARKER:
			var graphic: String = _c._placer.graphic_for(int(entity.get("item_id", 0)))
			if graphic.is_empty():
				continue
			var anchor_godot: Vector3 = _c._placer.ground_anchor_godot(graphic)
			anchor_bms = _c.MissionObjectPlacer.godot_to_bms_position(anchor_godot)
			ground_godot += _c.MissionObjectPlacer.bms_to_godot_basis(entity.get("rotation_deg", Vector3.ZERO)) * anchor_godot
		rows.append({
			"kind": kind,
			"index": int(entity.get("index", -1)),
			"ground_godot": ground_godot,
			"anchor_bms": anchor_bms,
			"rotation_deg": entity.get("rotation_deg", Vector3.ZERO),
		})
		points.append(Vector2(ground_godot.x, ground_godot.z))
	# Pass 2: sample — batched when the editor offers it, else the scalar loop so
	# any duck-typed mount (a headless stub faking the surface) keeps working.
	var heights: PackedFloat32Array
	if _c.terrain_editor.has_method("sample_heights_world"):
		heights = _c.terrain_editor.sample_heights_world(points)
	else:
		heights = PackedFloat32Array()
		heights.resize(points.size())
		for i in points.size():
			heights[i] = _c.terrain_editor.sample_height_world(points[i].x, points[i].y)
	# Pass 3: assemble, dropping off-terrain rows (NAN) exactly as the per-point
	# builder did.
	for i in rows.size():
		var height := heights[i]
		if is_nan(height):
			continue
		var row: Dictionary = rows[i]
		var ground_godot: Vector3 = row["ground_godot"]
		requests.append({
			"kind": row["kind"],
			"index": row["index"],
			"ground_hit_bms": _c.MissionObjectPlacer.godot_to_bms_position(Vector3(ground_godot.x, height, ground_godot.z)),
			"ground_anchor_bms": row["anchor_bms"],
			# Not read by the engine (its parser ignores unknown keys); carried for
			# the targeted world update, which rebuilds the moved entities'
			# container-local transforms without re-marshalling them.
			"rotation_deg": row["rotation_deg"],
		})
	return requests
