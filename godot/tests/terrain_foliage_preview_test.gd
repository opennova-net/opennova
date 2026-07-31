extends GutTest

const TerrainFoliagePreviewScript := preload("res://modtools/terrain/terrain_foliage_preview.gd")
const EditorMainScene := preload("res://modtools/editor/editor_main.tscn")
const EditorWorkstationScript := preload("res://modtools/editor/editor_workstation.gd")


func test_preview_accepts_and_clears_injected_tile_context_without_anchors() -> void:
	var preview: TerrainFoliagePreview = add_child_autofree(TerrainFoliagePreviewScript.new())
	await get_tree().process_frame

	var diagnostics: TerrainFoliagePreview.SurfaceInputDiagnostics = \
		preview.get_surface_input_diagnostics()
	assert_false(diagnostics.overrides_active,
		"A standalone preview without editor surface inputs has no active overrides.")

	assert_true(preview.has_method("get_tile_info"),
		"The preview needs a public blocker diagnostic at its owner seam.")
	if not preview.has_method("get_tile_info"):
		return

	var tile_info := NovaTerrainTileInfo.new()
	preview.call("set_preview_state", null, null, null, [], null, null, tile_info)
	assert_same(preview.call("get_tile_info"), tile_info)

	preview.call("set_preview_state", null, null, null, [], null, null, null)
	assert_null(preview.call("get_tile_info"), "Clearing context removes stale mission blockers.")
	# The preview never manufactures silhouette anchors: retail's MODEL tier
	# generates only around crouched/prone infantry, which a preview lacks
	# [orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7dc2/0x5c7ded].
	assert_false(preview.has_method("get_silhouette_anchors"),
		"The retired placed-object anchor seam must not return.")


func test_terrain_editor_uses_mission_tile_override_without_leaking_authoring_overlays() -> void:
	var app: EditorApp = add_child_autofree(EditorMainScene.instantiate())
	await get_tree().process_frame
	var editor: TerrainEditor = app.get_terrain_editor()
	var workstation: EditorWorkstation = app.workstation
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.TERRAIN)
	await get_tree().process_frame
	var surface_inputs: NovaTerrainSurfaceInputs = editor.terrain_mesh.get_surface_inputs()
	assert_same(surface_inputs.get_terrain_data(), editor.get_data(),
		"Default terrain mounting must fully rebuild the shared surface inputs.")
	assert_true(surface_inputs.has_normalized_blend())
	assert_true(surface_inputs.has_heightfield_normal())
	assert_false(surface_inputs.has_detail_coefficient(),
		"Default terrain has no authored detail coefficient source.")
	assert_true(editor.has_method("set_mission_preview_context"))
	assert_true(editor.has_method("clear_mission_preview_context"))
	assert_true(editor.has_method("get_effective_tile_info"))
	assert_true(editor.has_method("is_mission_preview_context_active"))
	if not editor.has_method("set_mission_preview_context") \
			or not editor.has_method("clear_mission_preview_context") \
			or not editor.has_method("get_effective_tile_info") \
			or not editor.has_method("is_mission_preview_context_active"):
		return

	var tilestrip_image := Image.create(64, 64, false, Image.FORMAT_RGBA8)
	tilestrip_image.fill(Color(0.2, 0.45, 0.15, 1.0))
	editor.get_data().set_tilestrip_tex(ImageTexture.create_from_image(tilestrip_image))
	editor.new_tileinfo()
	var authored_tile_info = editor.call("get_effective_tile_info")
	assert_not_null(authored_tile_info)
	editor.set_tile_stamp_tile_index(0)
	editor.set_tile_stamp_flags(NovaTerrainTileInfo.FLAG_OUTLINE)
	assert_true(editor.stamp_tileinfo_cell(0, 0))
	editor.select_tileinfo_entry(0)
	await get_tree().process_frame
	var layers: TerrainEditor.TileOverlayPreviewDiagnostics = editor.get_tile_overlay_preview_diagnostics()
	assert_true(editor.terrain_mesh.has_tile_overlay_texture(),
		"Authored tile edits must rebuild the terrain material composite.")
	assert_false(layers.base_overlay_visible,
		"The separate base mesh must be hidden when the material composite exists.")
	assert_true(layers.outline_visible,
		"Terrain hides only duplicate base albedo; authored FLAG_OUTLINE feedback stays visible.")
	assert_true(layers.selection_visible,
		"Terrain mode retains its authoring selection layer.")
	editor.set_sector_overlay_visible(true)
	editor.set_grid_guide_visible(true)
	editor.set_tool(TerrainEditor.Tool.SURFACE_PAINT)
	var material: ShaderMaterial = editor.terrain_mesh.get_material()
	assert_true(bool(material.get_shader_parameter("u_show_sector_overlay")))
	assert_true(bool(material.get_shader_parameter("u_show_grid")))
	assert_true(bool(material.get_shader_parameter("u_show_surface_overlay")))

	# A mission with no co-named .til follows GameWorld's terrain-authored fallback.
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.MISSION)
	await get_tree().process_frame
	assert_same(editor.call("get_effective_tile_info"), authored_tile_info)
	layers = editor.get_tile_overlay_preview_diagnostics()
	assert_false(layers.selection_visible,
		"Mission fallback suppresses Terrain selection authoring geometry.")
	assert_false(layers.ghost_visible,
		"Mission fallback suppresses Terrain tile-stamp ghosts.")
	assert_false(layers.base_overlay_visible)
	assert_false(layers.outline_visible,
		"Mission hides Terrain-only FLAG_OUTLINE authoring feedback.")

	var mission_tile_info := NovaTerrainTileInfo.new()
	var mission_entry := NovaTerrainTileEntry.new()
	mission_entry.set_cell(1, 1)
	mission_entry.set_tile_index(0)
	mission_entry.set_flags(NovaTerrainTileInfo.FLAG_OUTLINE)
	mission_tile_info.add_entry(mission_entry)
	editor.call("set_mission_preview_context", mission_tile_info)
	await get_tree().process_frame
	assert_true(bool(editor.call("is_mission_preview_context_active")))
	assert_same(editor.call("get_effective_tile_info"), mission_tile_info)
	layers = editor.get_tile_overlay_preview_diagnostics()
	assert_true(editor.terrain_mesh.has_tile_overlay_texture())
	assert_false(layers.base_overlay_visible,
		"Mission composite also replaces separate base geometry.")
	assert_false(layers.selection_visible)
	assert_false(layers.ghost_visible,
		"Mission never renders Terrain tile-stamp authoring geometry.")
	assert_false(layers.outline_visible,
		"Mission tile overrides never expose FLAG_OUTLINE authoring feedback.")
	assert_false(bool(material.get_shader_parameter("u_show_sector_overlay")),
		"Terrain sector diagnostics must not leak into the Mission view.")
	assert_false(bool(material.get_shader_parameter("u_show_grid")),
		"Terrain grid guides must not leak into the Mission view.")
	assert_false(bool(material.get_shader_parameter("u_show_surface_overlay")),
		"Terrain surface-paint diagnostics must not leak into the Mission view.")

	workstation.set_active_workspace(EditorWorkstationScript.Workspace.TERRAIN)
	await get_tree().process_frame
	assert_false(bool(editor.call("is_mission_preview_context_active")))
	assert_same(editor.call("get_effective_tile_info"), authored_tile_info)
	layers = editor.get_tile_overlay_preview_diagnostics()
	assert_false(layers.base_overlay_visible)
	assert_true(layers.selection_visible,
		"Clearing Mission restores Terrain authoring layers.")
	assert_true(layers.outline_visible,
		"Returning to Terrain restores FLAG_OUTLINE authoring feedback.")
	assert_true(bool(material.get_shader_parameter("u_show_sector_overlay")),
		"Clearing Mission context restores the authored Terrain workspace state.")
	assert_true(bool(material.get_shader_parameter("u_show_grid")))
	assert_true(bool(material.get_shader_parameter("u_show_surface_overlay")))
