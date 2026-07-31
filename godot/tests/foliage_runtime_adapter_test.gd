extends GutTest

# Contract tests for the fresh Godot adapter. Literal PRNG, placement, ground-
# fit, fade, alpha-reference, and :fd vectors live in the portable core tests;
# these checks pin which authored map drives each render tier.

var _dispatcher: NovaFoliageDispatcher
var _foliage_index := 1
var _detail_sampler_calls := 0
var _model_sampler_calls := 0


func before_each() -> void:
	_foliage_index = 1
	_detail_sampler_calls = 0
	_model_sampler_calls = 0
	_dispatcher = NovaFoliageDispatcher.new()
	add_child_autofree(_dispatcher)

	var def := NovaTerrainFoliageDef.new()
	def.graphic = "adapter_test"
	def.match = 1

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


func test_detail_authoring_brush_wraps_effective_resolution() -> void:
	var foliage_map := NovaTerrainFoliageMap.new()
	foliage_map.set_size(300, 300)
	assert_eq(foliage_map.get_detail_sample_resolution(), 256)
	assert_true(foliage_map.paint_detail_circle_wrap(
		255, 255, 2, 1.0, 1.0, 19))
	for point in [
		Vector2i(255, 255),
		Vector2i(0, 255),
		Vector2i(1, 255),
		Vector2i(255, 0),
		Vector2i(255, 1),
	]:
		assert_eq(int(foliage_map.get_index(point.x, point.y)), 19,
			"DETAIL brush coverage must stay continuous across the wrap seam.")
	assert_eq(int(foliage_map.get_index(256, 255)), 0,
		"Unused non-power-of-two stride columns must remain untouched.")
	assert_eq(int(foliage_map.get_index(255, 256)), 0,
		"Unused non-power-of-two stride rows must remain untouched.")


func test_render_tiers_use_distinct_foliage_sampler_callbacks() -> void:
	_dispatcher.detail_foliage_sampler = Callable(self, "_sample_detail_only")
	_dispatcher.foliage_sampler = Callable(self, "_sample_model_only")

	_dispatcher.render_preview(_camera_xform())
	_dispatcher.render_preview(_camera_xform())
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
	_dispatcher.render_frame(_camera_xform())
	assert_eq(_detail_sampler_calls, 0,
		"MODEL-only rendering must not invoke the DETAIL callback.")
	assert_gt(_model_sampler_calls, 0,
		"MODEL anchors must use the sector-routed callback.")


func test_detail_preview_uses_foliage_map() -> void:
	_foliage_index = 1
	_dispatcher.render_preview(_camera_xform())
	var warmup := _dispatcher.get_frame_stats()
	assert_eq(int(warmup.runtime_detail_intents), 0,
		"Retail fills detail cache misses after the current draw.")
	assert_gt(int(warmup.detail_cache_regenerations), 0)
	_dispatcher.render_preview(_camera_xform())
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
	assert_gt(int(stats.detail_mesh_uploads), 0)
	assert_eq(int(stats.terrain_scene_counter), 2)

	var found_draw := false
	for child in _dispatcher.get_children():
		if not child.name.begins_with("FoliageDetailDraw") or not child.visible:
			continue
		found_draw = true
		assert_almost_eq(float(child.get_instance_shader_parameter("u_wind_phase")), 0.002, 0.000001)
		var alpha_ref := float(child.get_instance_shader_parameter("u_alpha_ref"))
		assert_true(
			is_equal_approx(alpha_ref, 180.0 / 255.0) or is_equal_approx(alpha_ref, 8.0 / 255.0),
			"Each resident cell draw carries its current pass alpha reference."
		)
		assert_eq(child.cast_shadow, GeometryInstance3D.SHADOW_CASTING_SETTING_OFF,
			"Fresh retail audit confirms both foliage tiers are absent from shadow passes.")
		assert_eq(child.layers & NovaWater.VISUAL_LAYER_TERRAIN_SHADOW_RECEIVER, 0,
			"an alpha-blind catcher must not darken whole foliage cards")
		var material := child.material_override as ShaderMaterial
		assert_not_null(material)
		if material != null:
			var shadow_receiver := material.next_pass as ShaderMaterial
			assert_null(shadow_receiver,
				"foliage waits for the alpha-aware retail tile-cache compositor")
	assert_true(found_draw)

func test_editor_surface_input_overrides_reach_detail_materials() -> void:
	var image := Image.create(2, 2, false, Image.FORMAT_RGBA8)
	image.fill(Color(0.5, 0.5, 1.0, 1.0))
	var heightfield_normal := ImageTexture.create_from_image(image)
	var tile_overlay := ImageTexture.create_from_image(image)
	var tint := Vector3(0.25, 0.5, 0.75)

	_dispatcher.set_surface_input_overrides(heightfield_normal, tile_overlay, tint)
	_dispatcher.render_preview(_camera_xform())
	_dispatcher.render_preview(_camera_xform())

	var found_draw := false
	for child in _dispatcher.get_children():
		if not child.name.begins_with("FoliageDetailDraw") or not child.visible:
			continue
		var material := child.material_override as ShaderMaterial
		assert_not_null(material)
		if material == null:
			continue
		found_draw = true
		assert_same(material.get_shader_parameter("u_heightfield_normal"), heightfield_normal)
		assert_true(bool(material.get_shader_parameter("u_has_heightfield_normal")))
		assert_same(material.get_shader_parameter("u_tile_overlay"), tile_overlay)
		assert_true(bool(material.get_shader_parameter("u_has_tile_overlay")))
		assert_eq(material.get_shader_parameter("u_tile_overlay_tint"), tint)
	assert_true(found_draw)

	var diagnostics := _dispatcher.get_frame_stats()
	assert_true(bool(diagnostics.surface_input_overrides))
	assert_true(bool(diagnostics.surface_override_has_heightfield_normal))
	assert_true(bool(diagnostics.surface_override_has_tile_overlay))

	_dispatcher.clear_surface_input_overrides()
	_dispatcher.render_preview(_camera_xform())
	var cleared := _dispatcher.get_frame_stats()
	assert_false(bool(cleared.surface_input_overrides))
	for child in _dispatcher.get_children():
		if not child.name.begins_with("FoliageDetailDraw") or not child.visible:
			continue
		var material := child.material_override as ShaderMaterial
		assert_false(bool(material.get_shader_parameter("u_has_heightfield_normal")))
		assert_false(bool(material.get_shader_parameter("u_has_tile_overlay")))



func test_aerial_preview_rejects_detail_cells_beyond_retail_3d_distance() -> void:
	var aerial_camera := Transform3D(Basis(), Vector3(0.0, 747.0, 0.0))
	_dispatcher.render_preview(aerial_camera)
	_dispatcher.render_preview(aerial_camera)
	var stats := _dispatcher.get_frame_stats()

	assert_eq(int(stats.detail_cells), 0,
		"ONED preview collection must include camera altitude like the runtime terrain collector.")
	assert_eq(int(stats.runtime_detail_intents), 0,
		"An aerial Mission camera must not expand ground foliage as near detail.")
	assert_eq(_dispatcher.get_total_instances(), 0)


func test_preview_altitude_distance_drives_detail_alpha_fade() -> void:
	_dispatcher.height_sampler = Callable(self, "_sample_flat_height")
	var elevated_camera := Transform3D(Basis(), Vector3(8.0, 41.0, 8.0))
	_dispatcher.render_preview(elevated_camera)
	_dispatcher.render_preview(elevated_camera)

	var half_fade_draws := 0
	var found_secondary_cutoff := false
	for child in _dispatcher.get_children():
		if not child.name.begins_with("FoliageDetailDraw") or not child.visible:
			continue
		var fade := float(child.get_instance_shader_parameter("u_fade"))
		if is_equal_approx(fade, 0.5):
			half_fade_draws += 1
			var cutoff := float(child.get_instance_shader_parameter("u_high_pass_cutoff"))
			if is_equal_approx(cutoff, 180.0 / 255.0):
				found_secondary_cutoff = true
	assert_gt(half_fade_draws, 1,
		"A 31-unit 3D distance must feed retail's 20-to-42 fade to BOTH near submissions.")
	assert_true(found_secondary_cutoff,
		"The near LOW secondary must carry the strict-LESS high-pass cutoff.")


func test_near_detail_submits_high_then_exact_low_secondary() -> void:
	_dispatcher.render_preview(_camera_xform())
	_dispatcher.render_preview(_camera_xform())
	var stats := _dispatcher.get_frame_stats()
	var visible_draws: Array[MeshInstance3D] = []
	for child in _dispatcher.get_children():
		if (
			child is MeshInstance3D
			and child.name.begins_with("FoliageDetailDraw")
			and child.visible
		):
			visible_draws.append(child)
	assert_eq(visible_draws.size(), int(stats.detail_cache_submissions),
		"Every portable detail submission must remain a distinct reimpl draw.")

	var found_ordered_pair := false
	for index in range(visible_draws.size() - 1):
		var high := visible_draws[index]
		var low := visible_draws[index + 1]
		var high_material := high.material_override as ShaderMaterial
		var low_material := low.material_override as ShaderMaterial
		if high_material == null or low_material == null:
			continue
		var high_code := high_material.shader.code
		var low_code := low_material.shader.code
		if (
			not high_code.contains("depth_draw_always")
			or not low_code.contains("depth_draw_never")
		):
			continue
		found_ordered_pair = true
		assert_same(high.mesh, low.mesh,
			"The LOW secondary must reuse the exact HIGH resident geometry.")
		assert_almost_eq(float(high.get_instance_shader_parameter("u_alpha_ref")),
			180.0 / 255.0, 0.000001)
		assert_almost_eq(float(low.get_instance_shader_parameter("u_alpha_ref")),
			8.0 / 255.0, 0.000001)
		assert_true(high_code.contains("depth_draw_always"),
			"The first near pass writes depth for alpha-test survivors.")
		assert_true(low_code.contains("depth_draw_never"),
			"The second near pass preserves depth.")
		break
	assert_true(found_ordered_pair,
		"A near accepted cell must submit HIGH first and exact LOW second.")



func test_mission_tile_info_blocks_covering_detail_candidates() -> void:
	_dispatcher.render_preview(_camera_xform())
	_dispatcher.render_preview(_camera_xform())
	var unblocked := _dispatcher.get_frame_stats()
	assert_gt(int(unblocked.runtime_detail_intents), 0,
		"The control frame must contain foliage candidates before mission-tile exclusion.")

	assert_true(_dispatcher.has_method("set_tile_info"),
		"The foliage adapter must expose the retail mission .til blocker.")
	if not _dispatcher.has_method("set_tile_info"):
		return

	var tile_info := NovaTerrainTileInfo.new()
	var entries: Array = []
	# Cover every 16-unit preview cell around the origin. The portable runtime
	# still chooses candidates; this fixture only supplies retail's mission
	# tile AABBs to the recovered radius-2 blocker.
	for cell_z in range(-5, 6):
		for cell_x in range(-5, 6):
			var entry := NovaTerrainTileEntry.new()
			entry.set_cell(cell_x, cell_z)
			entries.append(entry)
	tile_info.entries = entries
	_dispatcher.call("set_tile_info", tile_info)

	assert_eq(_dispatcher.get_total_instances(), 0,
		"Changing the mission tile array must evict resident unblocked geometry.")
	_dispatcher.render_preview(_camera_xform())
	_dispatcher.render_preview(_camera_xform())
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
	_dispatcher.render_preview(spawn_camera)
	_dispatcher.render_preview(spawn_camera)
	var unblocked := _dispatcher.get_frame_stats()
	var unblocked_intents := int(unblocked.runtime_detail_intents)
	assert_gt(unblocked_intents, 0,
		"The exact 00TRa spawn control frame must contain foliage before blocking.")

	var tile_info := NovaTerrainTileInfo.new()
	var entries: Array[NovaTerrainTileEntry] = []
	# Exact 00TRa.til subset responsible for the player-start and armory-truck
	# exclusions. Stored-negated fixed Z decodes once onto terrain/Godot +Z.
	for raw_position in [
		Vector2i(19070976, -26279936), # entry 25
		Vector2i(19005440, -26607616), # entry 27
		Vector2i(18415616, -25755648), # entry 747
		Vector2i(20185088, -24838144), # entry 753
		Vector2i(19202048, -25034752), # entry 764
	]:
		var entry := NovaTerrainTileEntry.new()
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

	_dispatcher.render_preview(spawn_camera)
	_dispatcher.render_preview(spawn_camera)
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
	_dispatcher.render_frame(_camera_xform())
	var far_stats := _dispatcher.get_frame_stats()
	assert_eq(int(far_stats.runtime_detail_intents), 0)
	assert_eq(int(far_stats.silhouette_anchors_visible), 1)
	assert_gt(int(far_stats.runtime_silhouette_intents), 0,
		"A foliagemap match at view depth >= 38 emits silhouettes.")
	var found_model_draw := false
	for child in _dispatcher.get_children():
		if not child.name.begins_with("FoliageModelDraw") or not child.visible:
			continue
		found_model_draw = true
		assert_eq(child.cast_shadow, GeometryInstance3D.SHADOW_CASTING_SETTING_OFF,
			"Retail does not invoke the MODEL pass while rendering shadows.")
		var material := child.material_override as ShaderMaterial
		assert_not_null(material)
		var shader_code := material.shader.code
		assert_true(shader_code.contains("blend_add"),
			"Retail MODEL RGB is black under ONE/ONE blending, so it must preserve destination color.")
		assert_true(shader_code.contains("depth_draw_always"),
			"The color-invisible MODEL pass must retain its alpha-tested depth write.")
		assert_true(shader_code.contains("fog_disabled"),
			"Retail disables fog for the color-invisible MODEL pass.")
		assert_false(shader_code.contains("depth_prepass_alpha"),
			"Retail issues one depth-writing draw here, not a separate alpha depth prepass.")
		break
	assert_true(found_model_draw)

	_dispatcher.silhouette_anchors = PackedVector3Array([Vector3(0.0, 0.0, -20.0)])
	_dispatcher.render_frame(_camera_xform())
	var near_stats := _dispatcher.get_frame_stats()
	assert_eq(int(near_stats.silhouette_anchors_visible), 0)
	assert_eq(int(near_stats.runtime_silhouette_intents), 0,
		"Anchors shall not enter the silhouette tier before view depth 38.")
	for child in _dispatcher.get_children():
		if child.name.begins_with("FoliageModelDraw"):
			assert_false(child.visible)
			assert_null(child.mesh,
				"Unused draw nodes must release meshes when no longer submitted.")


func test_preview_does_not_manufacture_silhouette_anchors() -> void:
	_foliage_index = 1
	_dispatcher.silhouette_anchors = PackedVector3Array()
	_dispatcher.render_preview(_camera_xform())
	var stats := _dispatcher.get_frame_stats()
	assert_eq(int(stats.silhouette_anchors_input), 0)
	assert_eq(int(stats.runtime_silhouette_intents), 0)


func test_reset_clears_render_batches() -> void:
	_dispatcher.render_preview(_camera_xform())
	_dispatcher.render_preview(_camera_xform())
	assert_gt(_dispatcher.get_total_instances(), 0)
	_dispatcher.reset()
	assert_eq(_dispatcher.get_total_instances(), 0)
	assert_eq(int(_dispatcher.get_frame_stats().terrain_scene_counter), 0)


func test_detail_mesh_cache_reuses_resident_geometry() -> void:
	_dispatcher.render_preview(_camera_xform()) # cache fill after draw
	_dispatcher.render_preview(_camera_xform()) # first resident mesh upload
	var uploaded := _dispatcher.get_frame_stats()
	assert_gt(int(uploaded.detail_mesh_uploads), 0)
	assert_gt(int(uploaded.detail_mesh_hits), 0,
		"The near LOW secondary reuses the mesh uploaded by the preceding HIGH pass.")
	assert_eq(
		int(uploaded.detail_cache_submissions),
		int(uploaded.detail_mesh_uploads) + int(uploaded.detail_mesh_hits),
		"Every first visible submission is accounted as an upload or cache hit."
	)

	_dispatcher.render_preview(_camera_xform())
	var reused := _dispatcher.get_frame_stats()
	assert_eq(int(reused.detail_mesh_uploads), 0,
		"Stable cache revisions must not rebuild ArrayMeshes every render tick.")
	assert_gt(int(reused.detail_mesh_hits), 0)
	assert_eq(int(reused.detail_mesh_hits), int(reused.detail_cache_submissions))


func test_duplicate_silhouette_anchors_reuse_mesh_but_submit_twice() -> void:
	_foliage_index = 1
	var anchor := Vector3(0.0, 0.0, -64.0)
	_dispatcher.silhouette_anchors = PackedVector3Array([anchor, anchor])
	_dispatcher.render_frame(_camera_xform())
	var stats := _dispatcher.get_frame_stats()

	assert_gt(int(stats.model_mesh_uploads), 0)
	assert_gt(int(stats.model_mesh_hits), 0,
		"The second identical anchor must reuse each resident model mesh.")
	assert_eq(int(stats.render_batches), int(stats.model_cache_submissions),
		"Repeated anchors remain distinct retail draw submissions.")
	assert_eq(int(stats.model_cache_submissions), int(stats.model_mesh_uploads) * 2)


func test_terrain_change_invalidates_resident_geometry() -> void:
	var data := NovaTerrainData.new()
	_dispatcher.colormap_source = data
	_dispatcher.render_preview(_camera_xform())
	_dispatcher.render_preview(_camera_xform())
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

	var def := NovaTerrainFoliageDef.new()
	def.graphic = "line_mesh"
	def.match = 1
	_dispatcher.configure_slots([def], [line_mesh], [])
	_dispatcher.render_preview(_camera_xform())
	_dispatcher.render_preview(_camera_xform())
	var stats := _dispatcher.get_frame_stats()
	assert_eq(int(stats.detail_cache_regenerations), 0,
		"Non-triangle public meshes must not enter the triangle expansion path.")
	assert_eq(_dispatcher.get_total_instances(), 0)


func test_slot_diagnostics_explain_every_authored_slot_that_cannot_render() -> void:
	var enabled := NovaTerrainFoliageDef.new()
	enabled.graphic = 'enabled_veg'
	enabled.match = 1
	var missing := NovaTerrainFoliageDef.new()
	missing.graphic = 'missing_veg'
	missing.match = 2
	var invalid := NovaTerrainFoliageDef.new()
	invalid.graphic = 'invalid_veg'
	invalid.match = 3

	var line_mesh := ArrayMesh.new()
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array([Vector3.ZERO, Vector3.ONE])
	line_mesh.add_surface_from_arrays(Mesh.PRIMITIVE_LINES, arrays)

	_dispatcher.configure_slots(
		[enabled, missing, invalid, null],
		[BoxMesh.new(), null, line_mesh, null],
		[null, null, null, null])

	assert_true(_dispatcher.has_method('get_slot_diagnostics'),
		'Production foliage must expose why an authored slot was disabled.')
	if not _dispatcher.has_method('get_slot_diagnostics'):
		return
	var diagnostics: Array = _dispatcher.call('get_slot_diagnostics')
	assert_eq(diagnostics.size(), 4)
	assert_eq(String(diagnostics[0].status), 'enabled')
	assert_eq(String(diagnostics[1].status), 'missing_mesh')
	assert_eq(String(diagnostics[2].status), 'invalid_mesh')
	assert_eq(String(diagnostics[3].status), 'missing_definition')
	assert_eq(String(diagnostics[1].graphic), 'missing_veg')

	var stats := _dispatcher.get_frame_stats()
	assert_eq(int(stats.get('authored_slots', -1)), 3)
	assert_eq(int(stats.get('enabled_slots', -1)), 1)
	assert_eq(int(stats.get('disabled_slots', -1)), 2)
