extends GutTest

const TerrainEditorScript = preload("res://modtools/terrain/terrain_editor.gd")


func test_noop_brush_and_visual_setters_do_not_emit_ui_state() -> void:
	var editor = autofree(TerrainEditorScript.new())
	var initial_version: int = editor.get_ui_state_version()

	editor.set_tool(editor.current_tool)
	editor.set_brush_radius_value(editor.brush_radius)
	editor.set_brush_strength_value(editor.brush_strength)
	editor.set_brush_hardness_value(editor.brush_hardness)
	editor.set_selected_surface_index(editor.get_selected_surface_index())
	editor.set_water_visible(editor.is_water_visible())
	editor.set_sector_overlay_visible(editor.is_sector_overlay_visible())

	assert_eq(editor.get_ui_state_version(), initial_version, "No-op UI setters should not emit terrain editor UI state changes.")
	assert_false(editor.is_dirty, "No-op UI setters should not dirty the terrain document.")


func test_noop_metadata_setters_do_not_dirty_document() -> void:
	var editor = autofree(TerrainEditorScript.new())
	editor._document.data = NovaTerrainData.new()
	editor._document.data.set_terrain_name("untitled")
	editor._document.data.set_sector_count(8)
	editor._document.data.set_sector_rows(8)
	editor.is_dirty = false
	var initial_version: int = editor.get_ui_state_version()

	editor.set_terrain_name_value("untitled")
	editor.set_detail_density_value(editor.get_detail_density())
	editor.set_detail_density2_value(editor.get_detail_density2())
	editor.set_wrap_x_enabled(editor.get_wrap_x_enabled())
	editor.set_wrap_y_enabled(editor.get_wrap_y_enabled())
	editor.set_sector_size(editor.get_sector_size())
	editor.set_origin_x_value(editor.get_origin_x())
	editor.set_origin_y_value(editor.get_origin_y())
	editor.set_water_height_value(editor.get_water_height())

	assert_false(editor.is_dirty, "No-op metadata setters should not dirty the terrain document.")
	assert_eq(editor.get_ui_state_version(), initial_version, "No-op metadata setters should not emit UI state changes.")


func test_real_brush_setter_still_emits_ui_state() -> void:
	var editor = autofree(TerrainEditorScript.new())
	var initial_version: int = editor.get_ui_state_version()

	editor.set_brush_radius_value(editor.brush_radius + 1.0)

	assert_eq(editor.get_ui_state_version(), initial_version + 1, "Real brush changes should still emit a UI state change.")
