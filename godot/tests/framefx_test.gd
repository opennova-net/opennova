extends GutTest

const Q3_LUM_3DI := "res://../fixtures/threedi/synth/armory.3di"
const PLACER_ITEMS_DEF := "res://../fixtures/def/items.def"
const PLACER_DEF_ROOT := "res://../fixtures/def"


func _solid_texture(color: Color) -> ImageTexture:
	var image := Image.create(1, 1, false, Image.FORMAT_RGBA8)
	image.set_pixel(0, 0, color)
	return ImageTexture.create_from_image(image)


func _lum_object_data() -> ObjectData:
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(Q3_LUM_3DI)), OK)
	return data


func _hide_non_opaque_lum_surfaces(root: Node) -> void:
	if root is MeshInstance3D:
		var mesh_instance := root as MeshInstance3D
		var material := mesh_instance.get_active_material(0) as ShaderMaterial
		var shader_path := material.shader.resource_path \
				if material != null and material.shader != null else ""
		mesh_instance.visible = "/self_lit/opaque.gdshader" in shader_path
	for child in root.get_children():
		_hide_non_opaque_lum_surfaces(child)


func _q3_lum_view(use_q3: bool) -> Dictionary:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(128, 96)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)

	var environment_resource := Environment.new()
	environment_resource.background_mode = Environment.BG_COLOR
	environment_resource.background_color = Color.BLACK
	environment_resource.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	environment_resource.glow_enabled = false
	var environment := WorldEnvironment.new()
	environment.environment = environment_resource
	viewport.add_child(environment)

	var camera := Camera3D.new()
	camera.current = true
	camera.cull_mask = 215452673
	camera.position = Vector3(0.0, 0.0, 10.0)
	viewport.add_child(camera)
	camera.look_at(Vector3.ZERO, Vector3.UP)

	var model := ObjectModel.new()
	viewport.add_child(model)
	model.set_process(false)
	model.set_object_data(_lum_object_data())
	model.scale = Vector3.ONE * 6.0
	# The armory fixture's authored opaque LUM bulb is centered at Godot-local
	# (-0.3, 2.8, 3.4). Center and enlarge that real strip, then hide the
	# unrelated shell/material sources so its own beauty depth is the reference.
	model.position = Vector3(1.8, -16.8, -20.4)
	_hide_non_opaque_lum_surfaces(model)
	model.advance_runtime_frame(1.0 / 62.0)
	for row in model.get_surface_materials():
		var material := row as ShaderMaterial
		if material != null and material.shader != null \
				and "/self_lit/" in material.shader.resource_path:
			material.set_shader_parameter("u_diffuse", _solid_texture(Color.WHITE))
			material.set_shader_parameter("u_rgb_mod", Vector3.ONE)
			material.set_shader_parameter("u_alpha_mod", 1.0)

	var terminal: Node3D
	if use_q3:
		terminal = FrameFx.new()
	else:
		terminal = DisplayDecode.new()
	viewport.add_child(terminal)
	if terminal is FrameFx:
		(terminal as FrameFx).advance_frame()
	return {"viewport": viewport, "model": model, "terminal": terminal}


func test_explicit_shutdown_detaches_terminal_effect_and_is_idempotent() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(32, 32)
	viewport.own_world_3d = true
	add_child_autofree(viewport)

	var environment := WorldEnvironment.new()
	environment.environment = Environment.new()
	viewport.add_child(environment)

	var camera := Camera3D.new()
	camera.current = true
	camera.cull_mask = 0x12345
	viewport.add_child(camera)

	var renderer := FrameFx.new()
	viewport.add_child(renderer)
	assert_not_null(environment.compositor)
	assert_eq(camera.cull_mask, 215452673)

	renderer.shutdown()
	var report := renderer.get_backend_report()
	assert_true(bool(report.get("shutdown", false)))
	assert_false(bool(report.get("terminal_compositor_installed", true)))
	assert_null(environment.compositor,
			"explicit shutdown restores the inherited compositor")
	assert_eq(camera.cull_mask, 0x12345,
			"explicit shutdown restores the caller's camera mask")

	# The process-exit coordinator and EXIT_TREE fallback can converge here.
	renderer.shutdown()
	renderer.advance_frame()
	assert_null(environment.compositor)
	assert_eq(camera.cull_mask, 0x12345)


func test_display_decode_reentry_recreates_released_terminal_effect() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(32, 32)
	viewport.own_world_3d = true
	add_child_autofree(viewport)

	var environment := WorldEnvironment.new()
	environment.environment = Environment.new()
	viewport.add_child(environment)

	var decoder := DisplayDecode.new()
	viewport.add_child(decoder)
	var first_compositor := environment.compositor
	assert_not_null(first_compositor)
	var first_effect := first_compositor.compositor_effects[0] \
			as FrameFxCompositorEffect
	assert_not_null(first_effect)
	assert_true(first_effect.enabled)

	viewport.remove_child(decoder)
	assert_null(environment.compositor)
	assert_false(first_effect.enabled,
			"leaving the tree disables the detached render callback")

	decoder.request_ready()
	viewport.add_child(decoder)
	var second_compositor := environment.compositor
	assert_not_null(second_compositor)
	var second_effect := second_compositor.compositor_effects[0] \
			as FrameFxCompositorEffect
	assert_not_null(second_effect)
	assert_true(second_effect.enabled)
	assert_ne(second_effect.get_instance_id(), first_effect.get_instance_id(),
			"re-entry uses a fresh effect after the prior device owner shut down")

	viewport.remove_child(decoder)
	assert_null(environment.compositor)
	assert_false(second_effect.enabled)
	decoder.free()


func test_framefx_reentry_recreates_released_terminal_effect() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(32, 32)
	viewport.own_world_3d = true
	add_child_autofree(viewport)

	var environment := WorldEnvironment.new()
	environment.environment = Environment.new()
	viewport.add_child(environment)

	var camera := Camera3D.new()
	camera.current = true
	camera.cull_mask = 0x12345
	viewport.add_child(camera)

	var renderer := FrameFx.new()
	viewport.add_child(renderer)
	var first_compositor := environment.compositor
	assert_not_null(first_compositor)
	var first_effect := first_compositor.compositor_effects[0] \
			as FrameFxCompositorEffect
	assert_not_null(first_effect)
	assert_true(first_effect.enabled)
	assert_eq(camera.cull_mask, 215452673)

	viewport.remove_child(renderer)
	assert_null(environment.compositor,
			"leaving the tree restores the inherited compositor")
	assert_false(first_effect.enabled,
			"leaving the tree disables the detached render callback")
	assert_eq(camera.cull_mask, 0x12345)
	assert_true(bool(renderer.get_backend_report().get("shutdown", false)))

	# A plain re-add (no request_ready from the caller) must revive the node.
	viewport.add_child(renderer)
	var second_compositor := environment.compositor
	assert_not_null(second_compositor,
			"re-entry reinstalls the terminal compositor")
	var second_effect := second_compositor.compositor_effects[0] \
			as FrameFxCompositorEffect
	assert_not_null(second_effect)
	assert_true(second_effect.enabled)
	assert_ne(second_effect.get_instance_id(), first_effect.get_instance_id(),
			"re-entry uses a fresh effect after the prior device owner shut down")
	assert_false(bool(renderer.get_backend_report().get("shutdown", true)),
			"re-entry clears the shutdown latch")
	assert_eq(camera.cull_mask, 215452673,
			"re-entry re-applies the beauty camera signature")

	viewport.remove_child(renderer)
	assert_null(environment.compositor)
	assert_false(second_effect.enabled)
	assert_eq(camera.cull_mask, 0x12345)
	renderer.free()


func test_stable_q3_scene_packs_once_and_never_reads_the_server_back() -> void:
	# The view compiles twice: the READY compile read each LUM surface once
	# into the retained cache, the second compile of the unchanged scene must
	# have been served entirely from it.
	var view := _q3_lum_view(true)
	var renderer := view.terminal as FrameFx
	var first := renderer.get_backend_report()
	assert_gt(int(first.get("q3_submitted_commands", 0)), 0)
	assert_gt(int(first.get("q3_cached_entries", 0)), 0,
			"first sight packed each surface into the cache")
	assert_gt(int(first.get("q3_cached_vertex_bytes", 0)), 0)
	assert_eq(int(first.get("q3_readbacks_this_frame", -1)), 0,
			"a stable frame reads nothing back through the server")
	assert_eq(int(first.get("q3_packed_vertices", -1)), 0,
			"a stable frame re-packs nothing")
	assert_eq(int(first.get("q3_repacked_entries", -1)), 0)
	assert_eq(String(first.get("q3_geometry_submission", "")),
			"cached_per_source_surface_streams")

	renderer.advance_frame()
	var second := renderer.get_backend_report()
	assert_eq(int(second.get("q3_submitted_commands", -1)),
			int(first.get("q3_submitted_commands", 0)))
	assert_eq(int(second.get("q3_readbacks_this_frame", -1)), 0)
	assert_eq(int(second.get("q3_packed_vertices", -1)), 0)
	assert_eq(int(second.get("q3_repacked_entries", -1)), 0)
	assert_eq(int(second.get("q3_cached_entries", -1)),
			int(first.get("q3_cached_entries", 0)))

	# An invalidated source (a rebuilt mesh, carved MultiMesh rows) re-packs
	# its surfaces once at its next sight, from the mesh's retained arrays
	# (RetainedArrayMesh: no server readback), then settles again.
	var bulb := _first_visible_mesh(view.model)
	assert_not_null(bulb)
	if bulb == null:
		return
	FrameFx.invalidate_q3_source(bulb)
	renderer.advance_frame()
	var third := renderer.get_backend_report()
	assert_eq(int(third.get("q3_readbacks_this_frame", -1)), 0,
			"invalidation re-reads the retained arrays, never the server")
	assert_gt(int(third.get("q3_packed_vertices", 0)), 0,
			"invalidation re-packs the source once")
	renderer.advance_frame()
	var fourth := renderer.get_backend_report()
	assert_eq(int(fourth.get("q3_readbacks_this_frame", -1)), 0)
	assert_eq(int(fourth.get("q3_packed_vertices", -1)), 0)


func test_static_row_rewrite_rereads_instance_rows_without_a_readback() -> void:
	# A static RLOD switch or destruction carve rewrites MultiMesh rows only;
	# the population's mesh never changes. The placer therefore invalidates
	# the population's INSTANCES: the cache re-reads its rows once and keeps
	# the packed surfaces, so no server readback and no re-pack follow.
	var viewport := SubViewport.new()
	viewport.size = Vector2i(96, 64)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.current = true
	viewport.add_child(camera)

	# The production LUM material and mesh: the armory fixture's opaque bulb,
	# harvested from a template ObjectModel the way the placer harvests.
	var template := ObjectModel.new()
	viewport.add_child(template)
	template.set_process(false)
	template.set_object_data(_lum_object_data())
	_hide_non_opaque_lum_surfaces(template)
	var bulb := _first_visible_mesh(template)
	assert_not_null(bulb, "the armory fixture carries an opaque LUM bulb")
	if bulb == null:
		return
	var bulb_mesh: Mesh = bulb.mesh
	var bulb_material: Material = bulb.get_active_material(0)
	viewport.remove_child(template)
	template.free()

	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var record := mission.add_entity(
			MissionData.KIND_BUILDING, 105004, Vector3.ZERO, Vector3.ZERO)
	assert_not_null(record)
	# A second bulb in the same 512-unit bin keeps the population live after
	# the carve below: the dense populations hide an emptied level, and a
	# hidden population is (rightly) never compiled or re-read.
	assert_not_null(mission.add_entity(
			MissionData.KIND_BUILDING, 105004, Vector3(2.0, 0.0, 0.0),
			Vector3.ZERO))
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(PLACER_ITEMS_DEF)), OK)
	var root := ResourceRoot.new()
	root.set_root_dir(ProjectSettings.globalize_path(PLACER_DEF_ROOT))
	var placer := MissionObjectPlacer.create(root, item_db)
	assert_true(placer.register_resolved_static_graphic(
			"StaticCrate1", ObjectData.new(), [{
				"mesh": bulb_mesh, "material": bulb_material,
				"offset": Transform3D.IDENTITY, "submesh": 0,
			}]))
	var parent := Node3D.new()
	viewport.add_child(parent)
	var stats := placer.place(mission, parent)
	assert_eq(stats.batched, 2,
			"the bulb population is one static batch over two rows: %s" % stats.to_json_value())
	var population := parent.get_node_or_null(
			"MissionObjects/StaticPopulations/Batch_StaticCrate1_0") as MultiMeshInstance3D
	assert_not_null(population, "the population is emitted")
	if population == null:
		return
	# Look at the bulb from a few units away so both the population's source
	# bounds and its one instance pass the frustum test under every renderer
	# (headless Godot keeps no MultiMesh row data and reads rows as identity).
	var center: Vector3 = population.global_transform * bulb_mesh.get_aabb().get_center()
	camera.position = center + Vector3(0.0, 0.0, 8.0)
	camera.look_at(center, Vector3.UP)

	# The READY compile is the population's first sight.
	var renderer := FrameFx.new()
	viewport.add_child(renderer)
	var first := renderer.get_backend_report()
	assert_gt(int(first.get("q3_submitted_commands", 0)), 0,
			"the static population is a typed Q3 source: %s" % first)
	assert_eq(int(first.get("q3_instance_row_reads_this_frame", -1)), 1,
			"first sight reads the population's rows once")
	assert_eq(int(first.get("q3_readbacks_this_frame", -1)), 0,
			"first sight packs the model's retained arrays, no server readback")
	renderer.advance_frame()
	var stable := renderer.get_backend_report()
	assert_eq(int(stable.get("q3_readbacks_this_frame", -1)), 0)
	assert_eq(int(stable.get("q3_packed_vertices", -1)), 0)
	assert_eq(int(stable.get("q3_instance_row_reads_this_frame", -1)), 0,
			"a stable frame re-reads no rows")
	var cached_entries := int(stable.get("q3_cached_entries", 0))
	assert_gt(cached_entries, 0)

	# The destruction carve rewrites the population's rows through the
	# production path (the RLOD switch shares _write_static_instance_slots).
	var bms_id := record.bms_id
	assert_true(placer.hide_static_instance(bms_id) is Transform3D)
	renderer.advance_frame()
	var carved := renderer.get_backend_report()
	assert_eq(int(carved.get("q3_instance_row_reads_this_frame", -1)), 1,
			"the rewritten rows are re-read once")
	assert_eq(int(carved.get("q3_readbacks_this_frame", -1)), 0,
			"a row rewrite never reads the population's mesh back: %s" % carved)
	assert_eq(int(carved.get("q3_packed_vertices", -1)), 0,
			"a row rewrite never re-packs the population's surfaces")
	assert_eq(int(carved.get("q3_repacked_entries", -1)), 0)
	assert_eq(int(carved.get("q3_cached_entries", -1)), cached_entries,
			"the packed surfaces stay cached across the rewrite")
	renderer.advance_frame()
	var settled := renderer.get_backend_report()
	assert_eq(int(settled.get("q3_instance_row_reads_this_frame", -1)), 0)
	assert_eq(int(settled.get("q3_readbacks_this_frame", -1)), 0)
	assert_eq(int(settled.get("q3_packed_vertices", -1)), 0)
	renderer.shutdown()


func test_offscreen_production_q3_source_is_culled_before_vertex_packing() -> void:
	var view := _q3_lum_view(true)
	(view.model as ObjectModel).position = Vector3(100000.0, 0.0, 0.0)
	var renderer := view.terminal as FrameFx
	renderer.advance_frame()
	for _frame in 2:
		await get_tree().process_frame
	var report := renderer.get_backend_report()
	assert_gt(int(report.get("q3_registered_sources", 0)), 0)
	assert_gt(int(report.get("q3_frustum_culled_sources", 0)), 0,
			"the offscreen real ObjectModel is rejected at its source AABB")
	assert_eq(int(report.get("q3_packed_vertices", -1)), 0,
			"source rejection precedes surface array packing")
	assert_eq(int(report.get("q3_submitted_commands", -1)), 0)


func test_stable_q3_frame_touches_no_records_beyond_the_frustum_test() -> void:
	# Every registered source is a persistent record: a stable frame walks
	# the live records, probes their transforms and refreshes nothing; a moved
	# or hidden source is picked up through its record alone, never a walk
	# that re-reads every source.
	var view := _q3_lum_view(true)
	var renderer := view.terminal as FrameFx
	renderer.advance_frame()
	var stable := renderer.get_backend_report()
	assert_gt(int(stable.get("q3_records", 0)), 0,
			"the LUM model's sources are live records: %s" % stable)
	assert_eq(int(stable.get("q3_records_touched_this_frame", -1)), 0,
			"a stable frame refreshes no record: %s" % stable)
	assert_eq(int(stable.get("q3_material_reads_this_frame", -1)), 0,
			"a stable frame reads no material block")
	var stable_commands := int(stable.get("q3_submitted_commands", 0))
	assert_gt(stable_commands, 0)

	var bulb := _first_visible_mesh(view.model)
	assert_not_null(bulb)
	if bulb == null:
		return
	# A move refreshes the bounds of the moved model's visible sources only;
	# its hidden LOD/material sources and every other record stay untouched.
	(view.model as ObjectModel).position += Vector3(0.5, 0.0, 0.0)
	renderer.advance_frame()
	var moved := renderer.get_backend_report()
	var moved_touched := int(moved.get("q3_records_touched_this_frame", 0))
	assert_gt(moved_touched, 0, "the move is picked up: %s" % moved)
	assert_lte(moved_touched, int(moved.get("q3_submitted_commands", 0)) +
			int(moved.get("q3_frustum_culled_sources", 0)),
			"only the visible sources refresh: %s" % moved)
	assert_eq(int(moved.get("q3_submitted_commands", -1)), stable_commands)
	renderer.advance_frame()
	var settled := renderer.get_backend_report()
	assert_eq(int(settled.get("q3_records_touched_this_frame", -1)), 0,
			"the moved model settles: %s" % settled)

	# A visibility change reaches its record through the node's signal.
	bulb.visible = false
	renderer.advance_frame()
	var hidden := renderer.get_backend_report()
	assert_eq(int(hidden.get("q3_records_touched_this_frame", -1)), 1,
			"the hidden source alone is refreshed: %s" % hidden)
	assert_lt(int(hidden.get("q3_submitted_commands", 0)), stable_commands,
			"and its copy leaves the draw list")
	bulb.visible = true
	renderer.advance_frame()
	var shown := renderer.get_backend_report()
	assert_eq(int(shown.get("q3_records_touched_this_frame", -1)), 1,
			"the shown source alone is refreshed: %s" % shown)
	assert_eq(int(shown.get("q3_submitted_commands", -1)), stable_commands,
			"and its copy is back")
	assert_eq(int(shown.get("q3_readbacks_this_frame", -1)), 0,
			"visibility never re-reads geometry")


func test_cleared_water_strip_leaves_the_q3_draw_list() -> void:
	# The water strip is a per-frame producer: its record follows every
	# publication, and a frame whose march yields no strip clears the surface
	# AND names the source, so the last published strip is never drawn again.
	var viewport := SubViewport.new()
	viewport.size = Vector2i(256, 144)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.position = Vector3(100.3, 27.0, -33.7)
	camera.current = true
	viewport.add_child(camera)
	var water := Water.new()
	water.water_height = 7.0
	viewport.add_child(water)
	water.advance_frame(1.0 / 62.0)
	assert_gt((water.get_mesh_instance().mesh as ArrayMesh).get_surface_count(), 0)
	var renderer := FrameFx.new()
	viewport.add_child(renderer)
	renderer.advance_frame()
	var drawn := renderer.get_backend_report()
	assert_gt(int(drawn.get("q3_submitted_commands", 0)), 0,
			"the strip compiles into Q3: %s" % drawn)

	# The g_WaterActive gate (every tracked visible terrain sector above the
	# water height) clears the beauty strip, but the bloom pass's nightvision
	# redraw is not gated on it (retail FrameFX_RenderGlowSource @ 0x582a59..
	# 0x582a5d calls Render_WaterSurface(0, 1) unconditionally): its strip
	# stays in the Q3 draw list.
	water.set_visible_terrain_bounds(true, 100.0, 200.0)
	water.advance_frame(1.0 / 62.0)
	assert_eq((water.get_mesh_instance().mesh as ArrayMesh).get_surface_count(), 0,
			"the inactive water pass clears the beauty strip")
	renderer.advance_frame()
	var ungated := renderer.get_backend_report()
	assert_gt(int(ungated.get("q3_submitted_commands", 0)), 0,
			"the nightvision redraw ignores g_WaterActive: %s" % ungated)

	# Below the plane the redraw has no side: the view-0 call requires the
	# camera strictly above the water (retail Render_WaterSurface @ 0x5c3304).
	# Its strip clears and names the source, so the last publication is never
	# drawn again.
	camera.position = Vector3(100.3, 5.0, -33.7)
	water.advance_frame(1.0 / 62.0)
	renderer.advance_frame()
	var cleared := renderer.get_backend_report()
	assert_eq(int(cleared.get("q3_submitted_commands", -1)), 0,
			"the cleared strip is not drawn from its last publication: %s" % cleared)
	assert_eq(int(cleared.get("q3_readbacks_this_frame", -1)), 0)

	# The camera comes back above: the rebuilt strip is published and drawn
	# again, from memory.
	camera.position = Vector3(100.3, 27.0, -33.7)
	water.set_visible_terrain_bounds(false, 0.0, 0.0)
	water.advance_frame(1.0 / 62.0)
	renderer.advance_frame()
	var restored := renderer.get_backend_report()
	assert_gt(int(restored.get("q3_submitted_commands", 0)), 0,
			"the strip is back: %s" % restored)
	assert_eq(int(restored.get("q3_readbacks_this_frame", -1)), 0,
			"the published strip is never read back through the server")
	renderer.shutdown()
	water.release_runtime_renderer_resources()


class ExitTreeWaterReleaser extends Node3D:
	# The shell's exit-tree path: GameWorld releases the water's renderer
	# resources (the strip MeshInstance3D is freed) while the shell subtree is
	# leaving the tree. A node freed there never emits tree_exited.
	var water: Water = null

	func _exit_tree() -> void:
		if water != null:
			water.release_runtime_renderer_resources()


func test_q3_source_freed_inside_an_ancestor_exit_tree_leaves_the_records() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(256, 144)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.position = Vector3(100.3, 27.0, -33.7)
	camera.current = true
	viewport.add_child(camera)
	var holder := ExitTreeWaterReleaser.new()
	viewport.add_child(holder)
	var water := Water.new()
	water.water_height = 7.0
	holder.add_child(water)
	holder.water = water
	water.advance_frame(1.0 / 62.0)
	var renderer := FrameFx.new()
	viewport.add_child(renderer)
	renderer.advance_frame()
	var drawn := renderer.get_backend_report()
	var live_records := int(drawn.get("q3_records", 0))
	assert_gt(int(drawn.get("q3_submitted_commands", 0)), 0,
			"the strip compiles into Q3: %s" % drawn)
	assert_gt(live_records, 0)

	# The holder leaves the tree; its _exit_tree frees the registered strip
	# node after the strip's own exit-tree notification and before any
	# tree_exited signal could reach the registry.
	viewport.remove_child(holder)
	assert_null(water.get_mesh_instance(),
			"the strip node is freed inside the ancestor's exit-tree handler")
	renderer.advance_frame()
	var after := renderer.get_backend_report()
	assert_eq(int(after.get("q3_records", -1)), live_records - 1,
			"the freed strip's record is reaped, never walked: %s" % after)
	assert_eq(int(after.get("q3_submitted_commands", -1)), 0,
			"nothing draws from the freed source")
	holder.free()
	renderer.shutdown()


func _first_visible_mesh(root: Node) -> MeshInstance3D:
	if root is MeshInstance3D and (root as MeshInstance3D).visible:
		return root as MeshInstance3D
	for child in root.get_children():
		var found := _first_visible_mesh(child)
		if found != null:
			return found
	return null
