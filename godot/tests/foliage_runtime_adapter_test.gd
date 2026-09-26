extends GutTest

# Contract tests for the fresh Godot adapter. Literal PRNG, placement, ground-
# fit, fade, alpha-reference, and :fd vectors live in the portable core tests;
# these checks pin which authored map drives each render tier.

var _dispatcher: FoliageDispatcher
var _foliage_index := 1
var _detail_sampler_calls := 0
var _model_sampler_calls := 0


func before_each() -> void:
	_foliage_index = 1
	_detail_sampler_calls = 0
	_model_sampler_calls = 0
	_dispatcher = FoliageDispatcher.new()
	add_child_autofree(_dispatcher)

	var def := TerrainFoliageDef.new()
	def.graphic = "adapter_test"
	def.match = PackedInt32Array([1])

	var mesh := BoxMesh.new()
	mesh.size = Vector3(2.0, 6.0, 2.0)
	_dispatcher.configure_slots([def], [mesh], [])
	_dispatcher.height_sampler = Callable(self, "_sample_height")
	_dispatcher.foliage_sampler = Callable(self, "_sample_foliage")


func _sample_height(world_x: float, world_z: float) -> float:
	return 0.08 * world_x - 0.05 * world_z + 0.004 * world_x * world_z


func _sample_flat_height(_world_x: float, _world_z: float) -> float:
	return 10.0


func _sample_foliage(_world_x: float, _world_z: float) -> int:
	return _foliage_index


func _sample_detail_only(_world_x: float, _world_z: float) -> int:
	_detail_sampler_calls += 1
	return 1


func _sample_model_only(_world_x: float, _world_z: float) -> int:
	_model_sampler_calls += 1
	return 1


func _camera_xform() -> Transform3D:
	# Identity basis looks down Godot -Z.
	return Transform3D(Basis(), Vector3(0.0, 10.0, 0.0))


func _backend_draws(tier := "") -> Array:
	var report: Dictionary = _dispatcher.get_backend_report()
	var rows: Array = []
	for row_value in report.get("draws", []):
		var row := row_value as Dictionary
		if not bool(row.get("visible", false)):
			continue
		if not tier.is_empty() and String(row.get("tier", "")) != tier:
			continue
		rows.append(row)
	return rows


func test_draw_backend_uses_retained_rendering_server_instances() -> void:
	_dispatcher.render_preview(_camera_xform(), 1000)
	_dispatcher.render_preview(_camera_xform(), 1000)

	var report: Dictionary = _dispatcher.get_backend_report()
	assert_eq(String(report.get("backend", "")), "rendering_server_rid")
	assert_gt(int(report.get("pool_size", 0)), 0,
		"Visible draw-list rows must own retained scenario instances.")
	assert_gt(int(report.get("visible_draws", 0)), 0)
	assert_eq(report.get("visible_draws", -1), report.get("active_draws", -2))
	assert_gt((report.get("draws", []) as Array).size(), 0)

	var draw_nodes := 0
	for child in _dispatcher.get_children():
		if child is MeshInstance3D:
			draw_nodes += 1
	assert_eq(draw_nodes, 0,
		"The replacement backend must not retain a MeshInstance3D fallback.")

	var pool_size := int(report.get("pool_size", 0))
	_dispatcher.render_preview(_camera_xform(), 1001)
	var steady := _dispatcher.get_backend_report()
	assert_eq(int(steady.get("pool_size", -1)), pool_size,
		"A steady draw list must retain the same scenario instances.")
	assert_eq(int(steady.get("instance_creates", -1)), 0)
	assert_eq(int(steady.get("scenario_writes", -1)), 0)
	assert_eq(int(steady.get("configuration_writes", -1)), 0)
	assert_eq(int(steady.get("base_writes", -1)), 0)
	assert_eq(int(steady.get("material_writes", -1)), 0)
	assert_eq(int(steady.get("material_parameter_writes", -1)), 0)
	assert_eq(int(steady.get("visibility_writes", -1)), 0)
	assert_gt(int(steady.get("uniform_writes", 0)), 0,
		"Only retail's advancing wind clock should touch a stable visible draw.")


func test_probe_draw_control_targets_retained_draws_by_public_pass_identity() -> void:
	_dispatcher.render_preview(_camera_xform(), GameWorld.current_frame_clock_ms())
	_dispatcher.render_preview(_camera_xform(), GameWorld.current_frame_clock_ms())
	var before := _dispatcher.get_backend_report()
	assert_gt(int(before.get("visible_draws", 0)), 0)
	var before_high_fades := {}
	var expected_fade_min := INF
	var expected_fade_max := -INF
	for row_value in before.get("draws", []):
		var row := row_value as Dictionary
		if bool(row.get("visible", false)) \
				and String(row.get("tier", "")) == "detail" \
				and String(row.get("pass", "")) == "high":
			var fade := float(row.get("fade", 0.0))
			before_high_fades[int(row.get("order", -1))] = fade
			expected_fade_min = minf(expected_fade_min, fade)
			expected_fade_max = maxf(expected_fade_max, fade)
	assert_gt(before_high_fades.size(), 0)

	var control: Dictionary = _dispatcher.apply_probe_draw_control(
			FoliageDispatcher.PROBE_DRAW_DETAIL_HIGH,
			true, false, 0.0, -0.125)
	assert_gt(int(control.get("kept", 0)), 0)
	assert_almost_eq(float(control.get("fade_min", 0.0)),
			expected_fade_min, 0.000001)
	assert_almost_eq(float(control.get("fade_max", 0.0)),
			expected_fade_max, 0.000001)

	var visible := _backend_draws()
	assert_eq(visible.size(), int(control.get("kept", -1)))
	for row_value in visible:
		var row := row_value as Dictionary
		assert_eq(String(row.get("tier", "")), "detail")
		assert_eq(String(row.get("pass", "")), "high")
		assert_almost_eq(float(row.get("wind_phase", -1.0)), 0.0, 0.000001)
		var before_fade := float(before_high_fades.get(
				int(row.get("order", -1)), -1.0))
		assert_almost_eq(float(row.get("fade", 0.0)),
				maxf(before_fade - 0.125, 0.0), 0.000001)

	# Pinning all retained draws must not isolate or otherwise change admission.
	_dispatcher.render_preview(_camera_xform(), GameWorld.current_frame_clock_ms())
	var restored := _dispatcher.get_backend_report()
	_dispatcher.apply_probe_draw_control(
			FoliageDispatcher.PROBE_DRAW_ALL,
			false, false, 0.0, 0.0)
	var pinned := _dispatcher.get_backend_report()
	assert_eq(pinned.get("visible_draws", -1), restored.get("visible_draws", -2))
	for row_value in pinned.get("draws", []):
		var row := row_value as Dictionary
		if bool(row.get("visible", false)):
			assert_almost_eq(float(row.get("wind_phase", -1.0)), 0.0, 0.000001)


func test_backend_visibility_tracks_dispatcher_without_dropping_bindings() -> void:
	_dispatcher.render_preview(_camera_xform(), GameWorld.current_frame_clock_ms())
	_dispatcher.render_preview(_camera_xform(), GameWorld.current_frame_clock_ms())
	var shown := _dispatcher.get_backend_report()
	assert_gt(int(shown.active_draws), 0)

	_dispatcher.visible = false
	var hidden := _dispatcher.get_backend_report()
	assert_eq(int(hidden.visible_draws), 0)
	assert_eq(hidden.active_draws, shown.active_draws,
		"Hiding foliage must retain its draw-list bindings and placement caches.")

	_dispatcher.visible = true
	var restored := _dispatcher.get_backend_report()
	assert_eq(restored.visible_draws, restored.active_draws)


func test_backend_recreates_retained_instances_after_world_exit() -> void:
	_dispatcher.render_preview(_camera_xform(), GameWorld.current_frame_clock_ms())
	_dispatcher.render_preview(_camera_xform(), GameWorld.current_frame_clock_ms())
	assert_gt(int(_dispatcher.get_backend_report().pool_size), 0)

	var parent := _dispatcher.get_parent()
	parent.remove_child(_dispatcher)
	assert_eq(int(_dispatcher.get_backend_report().pool_size), 0,
		"Leaving a World3D must release every scenario instance.")
	parent.add_child(_dispatcher)
	_dispatcher.render_preview(_camera_xform(), GameWorld.current_frame_clock_ms())
	var rebound := _dispatcher.get_backend_report()
	assert_true(bool(rebound.scenario_bound))
	assert_gt(int(rebound.pool_size), 0)
	assert_gt(int(rebound.instance_creates), 0)


func test_render_tiers_use_distinct_foliage_sampler_callbacks() -> void:
	_dispatcher.detail_foliage_sampler = Callable(self, "_sample_detail_only")
	_dispatcher.foliage_sampler = Callable(self, "_sample_model_only")

	_dispatcher.render_preview(_camera_xform(), GameWorld.current_frame_clock_ms())
	_dispatcher.render_preview(_camera_xform(), GameWorld.current_frame_clock_ms())
	assert_gt(_detail_sampler_calls, 0,
		"DETAIL candidates must use the flat-map callback.")
	assert_eq(_model_sampler_calls, 0,
		"DETAIL preview must not route through the MODEL callback.")

	_dispatcher.reset()
	_detail_sampler_calls = 0
	_model_sampler_calls = 0
	_dispatcher.silhouette_anchors = PackedVector3Array([
		Vector3(0.0, 0.0, -64.0),
	])
	_dispatcher.render_frame(_camera_xform(), GameWorld.current_frame_clock_ms())
	assert_eq(_detail_sampler_calls, 0,
		"MODEL-only rendering must not invoke the DETAIL callback.")
	assert_gt(_model_sampler_calls, 0,
		"MODEL anchors must use the sector-routed callback.")


func test_detail_preview_uses_foliage_map() -> void:
	_foliage_index = 1
	_dispatcher.render_preview(_camera_xform(), GameWorld.current_frame_clock_ms())
	var first := _dispatcher.get_frame_stats()
	# Retail updates the detail cache before the patches draw, so a new cell
	# draws in the frame that generates it.
	assert_gt(int(first.runtime_detail_intents), 0,
		"Retail fills detail cache misses before the current draw.")
	assert_gt(int(first.detail_cache_regenerations), 0)
	assert_gt(int(first.detail_mesh_uploads), 0)
	# Pin the detail sway clock: retail's c24.x = ms x 0.003 + OscRing[0] /
	# 65536 (Foliage_SetupVertexShaderConstants), no weather attached here.
	_dispatcher.set_wind_clock_override_ms(1000)
	_dispatcher.render_preview(_camera_xform(), GameWorld.current_frame_clock_ms())
	var stats := _dispatcher.get_frame_stats()

	assert_true(bool(stats.preview_detail_source))
	assert_false(bool(stats.native_detail_source))
	assert_gt(int(stats.detail_cells), 0)
	assert_gt(int(stats.runtime_detail_intents), 0,
		"Foliagemap matches must emit expanded detail geometry.")
	assert_eq(int(stats.runtime_silhouette_intents), 0,
		"Preview cells do not manufacture distant silhouette anchors.")
	assert_eq(
		_dispatcher.get_total_instances(),
		int(stats.detail_high_instances) + int(stats.detail_low_instances)
	)
	assert_gt(int(stats.detail_mesh_hits), 0,
		"the second frame draws the resident meshes the first frame uploaded")
	assert_eq(int(stats.terrain_scene_counter), 2)

	var draws := _backend_draws("detail")
	for draw_value in draws:
		var draw := draw_value as Dictionary
		assert_almost_eq(float(draw.wind_phase), 3.0, 0.000001,
			"The detail phase is the pinned clock x 0.003 with no oscillator term.")
		var alpha_ref := float(draw.alpha_reference)
		assert_true(
			is_equal_approx(alpha_ref, 180.0 / 255.0) or is_equal_approx(alpha_ref, 8.0 / 255.0),
			"Each resident cell draw carries its current pass alpha reference."
		)
		assert_false(bool(draw.casts_shadows),
			"Fresh retail audit confirms both foliage tiers are absent from shadow passes.")
		assert_eq(int(draw.layer_mask) & Water.VISUAL_LAYER_TERRAIN_SHADOW_RECEIVER, 0,
			"an alpha-blind catcher must not darken whole foliage cards")
		assert_eq(int(draw.layer_mask), Water.VISUAL_LAYER_TERRAIN_FOLIAGE,
			"foliage rides its own bit alone: the beauty camera admits it and " +
			"the water mirror excludes it (the witnessed reflection context " +
			"collects no foliage, env-tod-re.md #30)")
		assert_eq(Water.REFLECTION_CULL_MASK & Water.VISUAL_LAYER_TERRAIN_FOLIAGE, 0,
			"the mirror mask excludes the foliage blanket")
		var material := draw.material as ShaderMaterial
		assert_not_null(material)
		if material != null:
			var shadow_receiver := material.next_pass as ShaderMaterial
			assert_null(shadow_receiver,
				"foliage waits for the alpha-aware retail tile-cache compositor")
	assert_gt(draws.size(), 0)

func test_aerial_preview_rejects_detail_cells_beyond_retail_3d_distance() -> void:
	var aerial_camera := Transform3D(Basis(), Vector3(0.0, 747.0, 0.0))
	_dispatcher.render_preview(aerial_camera, GameWorld.current_frame_clock_ms())
	_dispatcher.render_preview(aerial_camera, GameWorld.current_frame_clock_ms())
	var stats := _dispatcher.get_frame_stats()

	assert_eq(int(stats.detail_cells), 0,
		"Preview collection must include camera altitude like the runtime terrain collector.")
	assert_eq(int(stats.runtime_detail_intents), 0,
		"An aerial camera must not expand ground foliage as near detail.")
	assert_eq(_dispatcher.get_total_instances(), 0)


func test_preview_altitude_distance_drives_detail_alpha_fade() -> void:
	_dispatcher.height_sampler = Callable(self, "_sample_flat_height")
	var elevated_camera := Transform3D(Basis(), Vector3(8.0, 41.0, 8.0))
	_dispatcher.render_preview(elevated_camera, GameWorld.current_frame_clock_ms())
	_dispatcher.render_preview(elevated_camera, GameWorld.current_frame_clock_ms())

	var half_fade_draws := 0
	var found_secondary_cutoff := false
	for draw_value in _backend_draws("detail"):
		var draw := draw_value as Dictionary
		var fade := float(draw.fade)
		if is_equal_approx(fade, 0.5):
			half_fade_draws += 1
			var cutoff := float(draw.high_pass_cutoff)
			if is_equal_approx(cutoff, 180.0 / 255.0):
				found_secondary_cutoff = true
	assert_gt(half_fade_draws, 1,
		"A 31-unit 3D distance must feed retail's 20-to-42 fade to BOTH near submissions.")
	assert_true(found_secondary_cutoff,
		"The near LOW secondary must carry the strict-LESS high-pass cutoff.")


func test_near_detail_submits_high_then_exact_low_secondary() -> void:
	_dispatcher.render_preview(_camera_xform(), GameWorld.current_frame_clock_ms())
	_dispatcher.render_preview(_camera_xform(), GameWorld.current_frame_clock_ms())
	var stats := _dispatcher.get_frame_stats()
	var visible_draws := _backend_draws("detail")
	assert_eq(visible_draws.size(), int(stats.detail_cache_submissions),
		"Every portable detail submission must remain a distinct reimpl draw.")

	var found_ordered_pair := false
	for index in range(visible_draws.size() - 1):
		var high := visible_draws[index] as Dictionary
		var low := visible_draws[index + 1] as Dictionary
		# The near secondary is the LOW pass carrying the strict-LESS high-pass
		# cutoff; its HIGH twin is the submission right before it.
		if (
			String(high.pass) != "high"
			or String(low.pass) != "low"
			or not is_equal_approx(float(low.high_pass_cutoff), 180.0 / 255.0)
		):
			continue
		found_ordered_pair = true
		assert_same(high.mesh, low.mesh,
			"The LOW secondary must reuse the exact HIGH resident geometry.")
		assert_almost_eq(float(high.alpha_reference),
			180.0 / 255.0, 0.000001)
		assert_almost_eq(float(low.alpha_reference),
			8.0 / 255.0, 0.000001)
		break
	assert_true(found_ordered_pair,
		"A near accepted cell must submit HIGH first and exact LOW second.")



func test_mission_tile_info_blocks_covering_detail_candidates() -> void:
	_dispatcher.render_preview(_camera_xform(), GameWorld.current_frame_clock_ms())
	_dispatcher.render_preview(_camera_xform(), GameWorld.current_frame_clock_ms())
	var unblocked := _dispatcher.get_frame_stats()
	assert_gt(int(unblocked.runtime_detail_intents), 0,
		"The control frame must contain foliage candidates before mission-tile exclusion.")

	var tile_info := TerrainTileInfo.new()
	var entries: Array = []
	# Cover every 16-unit preview cell around the origin. The portable runtime
	# still chooses candidates; this fixture only supplies retail's mission
	# tile AABBs to the recovered radius-2 blocker.
	for cell_z in range(-5, 6):
		for cell_x in range(-5, 6):
			var entry := TerrainTileEntry.new()
			entry.set_cell(cell_x, cell_z)
			entries.append(entry)
	tile_info.entries = entries
	_dispatcher.set_tile_info(tile_info)

	assert_eq(_dispatcher.get_total_instances(), 0,
		"Changing the mission tile array must evict resident unblocked geometry.")
	_dispatcher.render_preview(_camera_xform(), GameWorld.current_frame_clock_ms())
	_dispatcher.render_preview(_camera_xform(), GameWorld.current_frame_clock_ms())
	var blocked := _dispatcher.get_frame_stats()
	assert_true(bool(blocked.path_blocker_available),
		"Frame diagnostics must report that the retail blocker substrate is active.")
	assert_eq(int(blocked.runtime_detail_intents), 0,
		"Covering mission tile AABBs must reject every non-FORCE_ON detail candidate.")
	assert_eq(_dispatcher.get_total_instances(), 0)


func test_mission_tile_info_uses_decoded_terrain_plane_z() -> void:
	const BLOCKER_RADIUS := 2.0
	_dispatcher.height_sampler = Callable(self, "_sample_flat_height")
	var spawn_position := Vector3(297.805573, 10.0, 409.123169)
	var spawn_camera := Transform3D(Basis(), spawn_position)
	_dispatcher.render_preview(spawn_camera, GameWorld.current_frame_clock_ms())
	_dispatcher.render_preview(spawn_camera, GameWorld.current_frame_clock_ms())
	var unblocked := _dispatcher.get_frame_stats()
	var unblocked_intents := int(unblocked.runtime_detail_intents)
	assert_gt(unblocked_intents, 0,
		"The exact 00TRa spawn control frame must contain foliage before blocking.")

	var tile_info := TerrainTileInfo.new()
	var entries: Array[TerrainTileEntry] = []
	# Exact 00TRa.til subset responsible for the player-start and armory-truck
	# exclusions. Stored-negated fixed Z decodes once onto terrain/Godot +Z.
	for raw_position in [
		Vector2i(19070976, -26279936), # entry 25
		Vector2i(19005440, -26607616), # entry 27
		Vector2i(18415616, -25755648), # entry 747
		Vector2i(20185088, -24838144), # entry 753
		Vector2i(19202048, -25034752), # entry 764
	]:
		var entry := TerrainTileEntry.new()
		entry.x_fixed = raw_position.x
		entry.z_fixed = raw_position.y
		entries.append(entry)
	tile_info.entries = entries
	assert_true(tile_info.blocks_foliage(
		spawn_position.x, spawn_position.z, BLOCKER_RADIUS),
		"The exact authored player spawn must be excluded on +Godot Z.")
	assert_true(tile_info.blocks_foliage(311.190552, 394.856628, BLOCKER_RADIUS),
		"The armory-truck center must be excluded by the authored tile subset.")
	assert_true(tile_info.blocks_foliage(311.811996, 396.190958, BLOCKER_RADIUS),
		"The truck-adjacent c8 candidate must touch the authored exclusion.")
	assert_false(tile_info.blocks_foliage(313.704367, 398.025046, BLOCKER_RADIUS),
		"The exact c3 candidate must survive just beyond the blocker edge.")
	assert_false(tile_info.blocks_foliage(316.302939, 397.743686, BLOCKER_RADIUS),
		"The exact c4 candidate must remain eligible.")
	assert_false(tile_info.blocks_foliage(318.975696, 397.309534, BLOCKER_RADIUS),
		"The exact c5 candidate must remain eligible.")
	_dispatcher.tile_info = tile_info

	_dispatcher.render_preview(spawn_camera, GameWorld.current_frame_clock_ms())
	_dispatcher.render_preview(spawn_camera, GameWorld.current_frame_clock_ms())
	var blocked := _dispatcher.get_frame_stats()
	var blocked_intents := int(blocked.runtime_detail_intents)
	assert_true(bool(blocked.path_blocker_available))
	assert_lt(blocked_intents, unblocked_intents,
		"The authored tile subset must selectively block candidates at the exact 00TRa spawn.")
	assert_gt(blocked_intents, 0,
		"Authored exclusions must not blanket-suppress the surrounding foliage field.")



func test_silhouette_uses_foliage_map_and_view_depth() -> void:
	_foliage_index = 1
	_dispatcher.silhouette_anchors = PackedVector3Array([Vector3(0.0, 0.0, -64.0)])
	_dispatcher.render_frame(_camera_xform(), GameWorld.current_frame_clock_ms())
	var far_stats := _dispatcher.get_frame_stats()
	assert_eq(int(far_stats.runtime_detail_intents), 0)
	assert_eq(int(far_stats.silhouette_anchors_visible), 1)
	assert_gt(int(far_stats.runtime_silhouette_intents), 0,
		"A foliagemap match at view depth >= 38 emits silhouettes.")
	var found_model_draw := false
	for draw_value in _backend_draws("silhouette"):
		var draw := draw_value as Dictionary
		found_model_draw = true
		assert_false(bool(draw.casts_shadows),
			"Retail does not invoke the MODEL pass while rendering shadows.")
		assert_not_null(draw.material as ShaderMaterial)
		break
	assert_true(found_model_draw)

	_dispatcher.silhouette_anchors = PackedVector3Array([Vector3(0.0, 0.0, -20.0)])
	_dispatcher.render_frame(_camera_xform(), GameWorld.current_frame_clock_ms())
	var near_stats := _dispatcher.get_frame_stats()
	assert_eq(int(near_stats.silhouette_anchors_visible), 0)
	assert_eq(int(near_stats.runtime_silhouette_intents), 0,
		"Anchors shall not enter the silhouette tier before view depth 38.")
	assert_eq(_backend_draws("silhouette").size(), 0,
		"Unused draw instances must release meshes when no longer submitted.")


func test_preview_does_not_manufacture_silhouette_anchors() -> void:
	_foliage_index = 1
	_dispatcher.silhouette_anchors = PackedVector3Array()
	_dispatcher.render_preview(_camera_xform(), GameWorld.current_frame_clock_ms())
	var stats := _dispatcher.get_frame_stats()
	assert_eq(int(stats.silhouette_anchors_input), 0)
	assert_eq(int(stats.runtime_silhouette_intents), 0)


func test_reset_clears_render_batches() -> void:
	_dispatcher.render_preview(_camera_xform(), GameWorld.current_frame_clock_ms())
	_dispatcher.render_preview(_camera_xform(), GameWorld.current_frame_clock_ms())
	assert_gt(_dispatcher.get_total_instances(), 0)
	_dispatcher.reset()
	assert_eq(_dispatcher.get_total_instances(), 0)
	assert_eq(int(_dispatcher.get_frame_stats().terrain_scene_counter), 0)
	assert_eq(int(_dispatcher.get_backend_report().pool_size), 0,
		"Reset must release every retained RenderingServer RID.")


func test_detail_mesh_cache_reuses_resident_geometry() -> void:
	# The cache fills before the draw: the first frame uploads and draws.
	_dispatcher.render_preview(_camera_xform(), GameWorld.current_frame_clock_ms())
	var uploaded := _dispatcher.get_frame_stats()
	assert_gt(int(uploaded.detail_mesh_uploads), 0)
	assert_gt(int(uploaded.detail_mesh_hits), 0,
		"The near LOW secondary reuses the mesh uploaded by the preceding HIGH pass.")
	assert_eq(
		int(uploaded.detail_cache_submissions),
		int(uploaded.detail_mesh_uploads) + int(uploaded.detail_mesh_hits),
		"Every first visible submission is accounted as an upload or cache hit."
	)

	_dispatcher.render_preview(_camera_xform(), GameWorld.current_frame_clock_ms())
	var reused := _dispatcher.get_frame_stats()
	assert_eq(int(reused.detail_mesh_uploads), 0,
		"Stable cache revisions must not rebuild ArrayMeshes every render tick.")
	assert_gt(int(reused.detail_mesh_hits), 0)
	assert_eq(int(reused.detail_mesh_hits), int(reused.detail_cache_submissions))


func test_duplicate_silhouette_anchors_submit_twice() -> void:
	_foliage_index = 1
	var anchor := Vector3(0.0, 0.0, -64.0)
	_dispatcher.silhouette_anchors = PackedVector3Array([anchor, anchor])
	_dispatcher.render_frame(_camera_xform(), GameWorld.current_frame_clock_ms())
	var stats := _dispatcher.get_frame_stats()

	assert_gt(int(stats.model_cache_submissions), 0)
	assert_eq(int(stats.render_batches), int(stats.model_cache_submissions),
		"Repeated anchors remain distinct retail draw submissions.")
	var draws := _backend_draws("silhouette")
	assert_eq(draws.size(), int(stats.model_cache_submissions),
		"every MODEL submission is its own instanced draw")
	var per_cell := {}
	for draw_value in draws:
		var draw := draw_value as Dictionary
		assert_gt(int(draw.instance_count), 0,
			"a MODEL draw instances the slot mesh once per placed candidate")
		var cell := int(draw.cell_key)
		per_cell[cell] = int(per_cell.get(cell, 0)) + 1
	for cell in per_cell:
		assert_eq(int(per_cell[cell]), 2,
			"the second identical anchor draws each resident cell again")


# The MODEL depth masks are immediate draws inside the BySide waves: the far
# wave's lead the far-side alpha rung, the camera wave's lead the scars, and
# a sorting offset beyond any view depth keeps each ahead of its rung.
func test_model_masks_lead_their_side_rung() -> void:
	_foliage_index = 1
	_dispatcher.silhouette_anchors = PackedVector3Array([Vector3(0.0, 0.0, -64.0)])
	# Camera at 10 over water at 0: the anchor's z - 1 is below the water.
	_dispatcher.set_water_height(0.0)
	_dispatcher.render_frame(_camera_xform(), GameWorld.current_frame_clock_ms())
	var far_draws := _backend_draws("silhouette")
	assert_gt(far_draws.size(), 0)
	for draw_value in far_draws:
		var draw := draw_value as Dictionary
		assert_true(bool(draw.far_side))
		assert_eq(int(draw.render_priority), ObjectShaderCache.RENDER_RUNG_ALPHA_FAR_SIDE)
		assert_eq(int((draw.material as ShaderMaterial).render_priority),
			ObjectShaderCache.RENDER_RUNG_ALPHA_FAR_SIDE,
			"the bound material carries the rung")
		assert_lt(float(draw.sorting_offset), -100000.0,
			"the masks sort ahead of every far-side alpha draw")

	# Water at -5: the anchor stands above it, on the camera side.
	_dispatcher.set_water_height(-5.0)
	_dispatcher.render_frame(_camera_xform(), GameWorld.current_frame_clock_ms())
	var camera_draws := _backend_draws("silhouette")
	assert_gt(camera_draws.size(), 0)
	for draw_value in camera_draws:
		var draw := draw_value as Dictionary
		assert_false(bool(draw.far_side))
		assert_eq(int(draw.render_priority), ObjectShaderCache.RENDER_RUNG_SCARS)
		assert_eq(int((draw.material as ShaderMaterial).render_priority),
			ObjectShaderCache.RENDER_RUNG_SCARS)


# The detail passes take their own rungs by the patch side of the water, and
# a near secondary LOW draw sorts a hair nearer than its HIGH twin so the
# equal-depth pair keeps the retail HIGH-then-LOW order.
func test_detail_passes_take_their_side_rungs_and_order_the_secondary() -> void:
	_dispatcher.set_water_height(-1000.0)
	_dispatcher.render_preview(_camera_xform(), GameWorld.current_frame_clock_ms())
	_dispatcher.render_preview(_camera_xform(), GameWorld.current_frame_clock_ms())
	var draws := _backend_draws("detail")
	assert_gt(draws.size(), 0)
	var secondaries := 0
	for draw_value in draws:
		var draw := draw_value as Dictionary
		assert_false(bool(draw.far_side), "every patch is above the far-below water")
		assert_eq(int(draw.render_priority), ObjectShaderCache.RENDER_RUNG_FOLIAGE_CAMERA_SIDE)
		assert_eq(int((draw.material as ShaderMaterial).render_priority),
			ObjectShaderCache.RENDER_RUNG_FOLIAGE_CAMERA_SIDE)
		if is_equal_approx(float(draw.high_pass_cutoff), 180.0 / 255.0):
			secondaries += 1
			assert_gt(float(draw.sorting_offset), 0.0,
				"the near secondary LOW draws after its HIGH twin")
		else:
			assert_eq(float(draw.sorting_offset), 0.0)
	assert_gt(secondaries, 0)

	# Water above every patch with the camera over it: the far pass.
	_dispatcher.set_water_height(20.0)
	var high_camera := Transform3D(Basis(), Vector3(0.0, 30.0, 0.0))
	_dispatcher.height_sampler = Callable(self, "_sample_flat_height")
	_dispatcher.render_preview(high_camera, GameWorld.current_frame_clock_ms())
	_dispatcher.render_preview(high_camera, GameWorld.current_frame_clock_ms())
	var far_draws := _backend_draws("detail")
	assert_gt(far_draws.size(), 0)
	for draw_value in far_draws:
		var draw := draw_value as Dictionary
		assert_true(bool(draw.far_side))
		assert_eq(int(draw.render_priority), ObjectShaderCache.RENDER_RUNG_FOLIAGE_FAR_SIDE)


# The thermal view keeps only one faint LOW draw per patch (the engine
# compiler carries the witness); the dispatcher forwards its flag.
func test_thermal_view_draws_one_faint_low_pass_per_patch() -> void:
	_dispatcher.set_thermal_view(true)
	_dispatcher.render_preview(_camera_xform(), GameWorld.current_frame_clock_ms())
	_dispatcher.render_preview(_camera_xform(), GameWorld.current_frame_clock_ms())
	var draws := _backend_draws("detail")
	assert_gt(draws.size(), 0)
	for draw_value in draws:
		var draw := draw_value as Dictionary
		assert_eq(String(draw.pass), "low")
		assert_eq(float(draw.high_pass_cutoff), 0.0, "no near secondary under thermal")
		assert_lte(float(draw.fade), 0.1 + 0.000001)


func test_terrain_change_invalidates_resident_geometry() -> void:
	var data := TerrainData.new()
	_dispatcher.colormap_source = data
	_dispatcher.render_preview(_camera_xform(), GameWorld.current_frame_clock_ms())
	_dispatcher.render_preview(_camera_xform(), GameWorld.current_frame_clock_ms())
	assert_gt(_dispatcher.get_total_instances(), 0)

	data.detail_density += 1
	assert_eq(_dispatcher.get_total_instances(), 0,
		"In-place terrain edits must invalidate foliage render meshes.")
	assert_eq(int(_dispatcher.get_frame_stats().terrain_scene_counter), 0,
		"In-place terrain edits must restart the retail cache cadence.")


func test_non_triangle_array_mesh_disables_slot() -> void:
	var line_mesh := ArrayMesh.new()
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array([
		Vector3.ZERO,
		Vector3.ONE,
	])
	line_mesh.add_surface_from_arrays(Mesh.PRIMITIVE_LINES, arrays)

	var def := TerrainFoliageDef.new()
	def.graphic = "line_mesh"
	def.match = PackedInt32Array([1])
	_dispatcher.configure_slots([def], [line_mesh], [])
	_dispatcher.render_preview(_camera_xform(), GameWorld.current_frame_clock_ms())
	_dispatcher.render_preview(_camera_xform(), GameWorld.current_frame_clock_ms())
	var stats := _dispatcher.get_frame_stats()
	assert_eq(int(stats.detail_cache_regenerations), 0,
		"Non-triangle public meshes must not enter the triangle expansion path.")
	assert_eq(_dispatcher.get_total_instances(), 0)


func test_slot_diagnostics_explain_every_authored_slot_that_cannot_render() -> void:
	var enabled := TerrainFoliageDef.new()
	enabled.graphic = 'enabled_veg'
	enabled.match = PackedInt32Array([1])
	var missing := TerrainFoliageDef.new()
	missing.graphic = 'missing_veg'
	missing.match = PackedInt32Array([2])
	var invalid := TerrainFoliageDef.new()
	invalid.graphic = 'invalid_veg'
	invalid.match = PackedInt32Array([3])

	var line_mesh := ArrayMesh.new()
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array([Vector3.ZERO, Vector3.ONE])
	line_mesh.add_surface_from_arrays(Mesh.PRIMITIVE_LINES, arrays)

	_dispatcher.configure_slots(
		[enabled, missing, invalid, null],
		[BoxMesh.new(), null, line_mesh, null],
		[null, null, null, null])

	var diagnostics: Array = _dispatcher.get_slot_diagnostics()
	assert_eq(diagnostics.size(), 4)
	assert_eq(String(diagnostics[0].status), 'enabled')
	assert_eq(String(diagnostics[1].status), 'missing_mesh')
	assert_eq(String(diagnostics[2].status), 'invalid_mesh')
	assert_eq(String(diagnostics[3].status), 'missing_definition')
	assert_eq(String(diagnostics[1].graphic), 'missing_veg')

	var stats := _dispatcher.get_frame_stats()
	assert_eq(stats.authored_slots, 3)
	assert_eq(stats.enabled_slots, 1)
	assert_eq(stats.disabled_slots, 2)
