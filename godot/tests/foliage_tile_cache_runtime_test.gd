extends GutTest

const DVXI5_TRN := "res://../fixtures/godot/dvxi5/Dvxi5.trn"


func _sample_height(_world_x: float, _world_z: float) -> float:
	return 10.0


func _sample_foliage(_world_x: float, _world_z: float) -> int:
	return 1


func _settle_tile_cache_with_foliage(
		terrain: Terrain, dispatcher: FoliageDispatcher, camera: Camera3D) -> Dictionary:
	var diagnostics: Dictionary = {}
	for _attempt in range(512):
		terrain.render_frame()
		dispatcher.render_frame(camera.global_transform)
		diagnostics = terrain.get_tile_cache_diagnostics()
		if int(diagnostics.get("pending_jobs", -1)) == 0 \
				and int(diagnostics.get("frame_requests", 0)) > 0 \
				and int(diagnostics.get("frame_ready_hits", -1)) \
						== int(diagnostics.get("frame_requests", 0)):
			return diagnostics
		await get_tree().process_frame
	assert_true(false, "the bounded terrain compiler must settle pages before foliage borrows them")
	return diagnostics


func test_runtime_detail_foliage_borrows_terrains_ready_page_binding() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(320, 180)
	add_child_autofree(viewport)

	var data := TerrainData.new()
	data.set_trn_path(ProjectSettings.globalize_path(DVXI5_TRN))
	assert_eq(data.load(), OK, "the Dvxi5 fixture terrain must load")

	var terrain := Terrain.new()
	viewport.add_child(terrain)
	terrain.set_terrain_data(data)
	terrain.build()
	terrain.set_debug_no_frustum(true)

	var camera := Camera3D.new()
	viewport.add_child(camera)
	camera.global_position = Vector3(64.0, 27.0, 64.0)
	camera.make_current()

	var dispatcher := FoliageDispatcher.new()
	viewport.add_child(dispatcher)
	var definition := TerrainFoliageDef.new()
	definition.graphic = "tile_cache_contract"
	definition.match = 1
	dispatcher.configure_slots([definition], [BoxMesh.new()], [])
	dispatcher.height_sampler = Callable(self, "_sample_height")
	dispatcher.foliage_sampler = Callable(self, "_sample_foliage")
	dispatcher.set_terrain(terrain)

	# A cold request queues exact page compilation without composing, rasterizing,
	# or uploading synchronously. Foliage still submits through its analytic
	# fallback until a later terrain frame publishes and selects the ready pages.
	terrain.render_frame()
	var cold := terrain.get_tile_cache_diagnostics()
	assert_gt(int(cold.get("pending_jobs", 0)), 0)
	assert_eq(int(cold.get("frame_ready_hits", -1)), 0)
	assert_eq(int(cold.get("frame_uploads", -1)), 0)
	# The first foliage pass fills retail's detail cache; the second submits its
	# resident geometry without giving terrain a frame-start publication point.
	dispatcher.render_frame(camera.global_transform)
	dispatcher.render_frame(camera.global_transform)
	var pending_fallback_draws := 0
	for child in dispatcher.get_children():
		if not child.name.begins_with("FoliageDetailDraw") or not child.visible:
			continue
		pending_fallback_draws += 1
		var pending_draw := child as MeshInstance3D
		assert_false(bool(pending_draw.get_instance_shader_parameter(
			"u_instance_tile_cache_ready")),
			"pending terrain pages must leave detail foliage on its analytic fallback")
	assert_gt(pending_fallback_draws, 0,
		"the cold frame must exercise visible detail foliage fallback")

	var settled := await _settle_tile_cache_with_foliage(terrain, dispatcher, camera)
	assert_eq(int(settled["pending_jobs"]), 0)
	assert_eq(int(settled["frame_ready_hits"]), int(settled["frame_requests"]))

	var page_array: TextureLayered = terrain.get_tile_cache_texture()
	assert_not_null(page_array)
	var ready_draws := 0
	var fine_ready_draws := 0
	for child in dispatcher.get_children():
		if not child.name.begins_with("FoliageDetailDraw") or not child.visible:
			continue
		var draw := child as MeshInstance3D
		var material := draw.material_override as ShaderMaterial
		assert_not_null(material)
		if material == null:
			continue
		assert_same(material.get_shader_parameter("u_tile_cache"), page_array,
			"Terrain and detail foliage must sample one shared Texture2DArray.")
		assert_true(bool(material.get_shader_parameter("u_has_tile_cache")))
		if not bool(draw.get_instance_shader_parameter("u_instance_tile_cache_ready")):
			continue
		ready_draws += 1
		var layer := int(draw.get_instance_shader_parameter("u_instance_tile_cache_layer"))
		assert_between(layer, 0, page_array.get_layers() - 1)
		var page := draw.get_instance_shader_parameter(
			"u_instance_tile_cache_origin_span") as Vector4
		assert_true(page.w == 64.0 or page.w == 128.0 or page.w == 256.0 or page.w == 512.0)
		if page.w == 64.0:
			fine_ready_draws += 1
		assert_almost_eq(page.z, 1.0 / page.w, 0.000001)
		var shared_point: Vector3 = draw.mesh.get_aabb().get_center()
		assert_between(shared_point.x, page.x, page.x + page.w)
		assert_between(shared_point.z, page.y, page.y + page.w)

	assert_gt(ready_draws, 0,
		"At least one visible detail cell must borrow a terrain-resident page/layer binding.")
	assert_gt(fine_ready_draws, 0,
		"The near-camera warmup must retain a fine page for the cross-frame LOD regression.")

	# On this pinned Dvxi5 view, quality 0.3 keeps the same 25 near detail cells
	# while selecting 128-unit pages over them. A lower 0.15 threshold stops
	# emitting LOD >= 3 terrain nodes and therefore has no valid detail handoff.
	# The prior frame's overlapping 64-unit residents deliberately remain in the
	# 128-slot cache: foliage must borrow this frame's coarser selection, not the
	# finest page retained from an older frame.
	terrain.set_lod_quality(0.3)
	terrain.render_frame()
	dispatcher.render_frame(camera.global_transform)
	for child in dispatcher.get_children():
		if not child.name.begins_with("FoliageDetailDraw") or not child.visible:
			continue
		var transition_draw := child as MeshInstance3D
		if not bool(transition_draw.get_instance_shader_parameter(
				"u_instance_tile_cache_ready")):
			continue
		var transition_page := transition_draw.get_instance_shader_parameter(
			"u_instance_tile_cache_origin_span") as Vector4
		assert_ne(transition_page.w, 64.0,
			"the transition frame must not borrow a stale fine page while coarse work is pending")
	await _settle_tile_cache_with_foliage(terrain, dispatcher, camera)
	assert_gt(int(dispatcher.get_frame_stats().detail_cells), 0,
		"The coarse regression frame must retain a live terrain detail handoff.")
	var current_coarse_draws := 0
	var stale_fine_draws := 0
	for child in dispatcher.get_children():
		if not child.name.begins_with("FoliageDetailDraw") or not child.visible:
			continue
		var draw := child as MeshInstance3D
		if not bool(draw.get_instance_shader_parameter("u_instance_tile_cache_ready")):
			continue
		var page := draw.get_instance_shader_parameter(
			"u_instance_tile_cache_origin_span") as Vector4
		if page.w > 64.0:
			current_coarse_draws += 1
		elif page.w == 64.0:
			stale_fine_draws += 1
	assert_gt(current_coarse_draws, 0,
		"The low-quality frame must expose a current coarser terrain page to foliage.")
	assert_eq(stale_fine_draws, 0,
		"Foliage must not borrow an overlapping fine page retained from the prior frame.")

	# Release native texture owners before RenderingServer teardown; keeping the
	# resource locals alive until process exit makes Godot report false leaks.
	dispatcher.set_terrain(null)
	dispatcher.reset()
	terrain.set_terrain_data(null)
	viewport.free()
	page_array = null
	data = null
