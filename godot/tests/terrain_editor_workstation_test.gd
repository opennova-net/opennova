extends GutTest

const TerrainEditorScript = preload("res://modtools/terrain/terrain_editor.gd")
const EditorWorkstationScene = preload("res://modtools/editor/editor_workstation.tscn")
const EditorWorkstationScript = preload("res://modtools/editor/editor_workstation.gd")
const TerrainWorkspaceScript = preload("res://modtools/editor/terrain_workspace.gd")
const TerrainEditorAssetDockScene = preload("res://modtools/terrain/ui/editor_asset_dock.tscn")
const EnvironmentEditorScript = preload("res://modtools/environment/environment_editor.gd")
const EnvironmentInspectorScript = preload("res://modtools/environment/environment_inspector.gd")
const LayoutInspectorScene = preload("res://modtools/terrain/ui/inspectors/layout_inspector.tscn")
const PaintInspectorScene = preload("res://modtools/terrain/ui/inspectors/paint_inspector.tscn")
const ScatterInspectorScene = preload("res://modtools/terrain/ui/inspectors/scatter_inspector.tscn")
const SculptInspectorScene = preload("res://modtools/terrain/ui/inspectors/sculpt_inspector.tscn")
const StampInspectorScene = preload("res://modtools/terrain/ui/inspectors/stamp_inspector.tscn")
const QuadrantBoardScript = preload("res://modtools/terrain/ui/widgets/quadrant_board.gd")


func test_workstation_starts_with_three_domain_workspaces() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())

	var workspace_rail: HBoxContainer = workstation.get_node("%WorkspaceRail")
	assert_eq(workstation.get_active_workspace_id(), EditorWorkstationScript.Workspace.TERRAIN, "Terrain should remain the default workspace.")
	assert_eq(workspace_rail.get_child_count(), 3, "The shell should expose Terrain, Environment, and Mission workspaces.")
	assert_eq((workspace_rail.get_child(0) as Button).text, "Terrain", "Terrain should be the first workspace.")
	assert_eq((workspace_rail.get_child(1) as Button).text, "Environment", "Environment should have a reserved workspace.")
	assert_eq((workspace_rail.get_child(2) as Button).text, "Mission", "Mission should have a reserved workspace.")


func test_mission_placeholder_disables_file_commands() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())
	editor.is_dirty = true

	workstation.set_editor(editor)
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.MISSION)

	assert_eq(workstation.get_node("%ProjectLabel").text, "Mission", "Placeholder workspaces should own the shell title while active.")
	assert_false(workstation.get_node("%AssetDock").visible, "Terrain properties should hide outside the terrain workspace.")
	assert_true((workstation.get_node("%FileMenu") as MenuButton).disabled, "Terrain file commands should be disabled for placeholder domains.")
	assert_true((workstation.get_node("%SaveButton") as Button).disabled, "Save should be disabled until the domain implements persistence.")
	assert_true((workstation.get_node("%ExportButton") as Button).disabled, "Export should be disabled until the domain implements export.")
	assert_true((workstation.get_node("%UndoButton") as Button).disabled, "Undo should route through the active workspace and stay disabled for placeholders.")


func test_environment_workspace_exposes_env_document_controls() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var environment_editor = add_child_autofree(EnvironmentEditorScript.new())
	environment_editor.create_default_environment(false)
	workstation._workspaces[EditorWorkstationScript.Workspace.ENVIRONMENT].set_environment_editor(environment_editor)

	workstation.set_active_workspace(EditorWorkstationScript.Workspace.ENVIRONMENT)
	await get_tree().process_frame

	var file_menu := workstation.get_node("%FileMenu") as MenuButton
	var save_button := workstation.get_node("%SaveButton") as Button
	var export_button := workstation.get_node("%ExportButton") as Button
	var popup := file_menu.get_popup()
	var inspector_host: Control = workstation.get_node("%InspectorHost")
	var inspector := inspector_host.get_child(inspector_host.get_child_count() - 1)
	assert_eq(workstation.get_node("%ProjectLabel").text, "untitled", "Environment should own the shell title when active.")
	assert_false(workstation.get_node("%AssetDock").visible, "Terrain properties should hide outside the environment workspace.")
	assert_false(file_menu.disabled, "Environment should expose file commands.")
	assert_eq(popup.get_item_text(popup.get_item_index(EditorWorkstationScript.FileMenuItem.OPEN)), "Open .env...", "Open should use the active workspace file type.")
	assert_true(save_button.disabled, "Clean new environments should not enable Save until changed.")
	assert_false(export_button.disabled, "Environment export should be available once an EnvFile exists.")
	assert_true(inspector.get_script() == EnvironmentInspectorScript, "Environment should build its inspector instead of a placeholder.")

	environment_editor.env_file.set_env_name("storm_test")
	workstation.sync_from_editor_state()

	assert_eq(workstation.get_node("%ProjectLabel").text, "storm_test*", "Environment edits should dirty the active document title.")
	assert_false(save_button.disabled, "Dirty environments should enable Save.")


func test_switching_workspaces_preserves_terrain_dirty_state() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())
	editor.is_dirty = true

	workstation.set_editor(editor)
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.MISSION)
	workstation.set_active_workspace(EditorWorkstationScript.Workspace.TERRAIN)

	assert_true(editor.is_dirty, "Switching placeholder domains should not reset terrain document state.")
	assert_eq(workstation.get_node("%ProjectLabel").text, "untitled*", "Returning to Terrain should restore the terrain project title and dirty marker.")
	assert_true(workstation.get_node("%AssetDock").visible, "Terrain properties should return when Terrain is active.")


func test_workstation_tracks_mode_from_editor_tool() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())
	editor.current_tool = TerrainEditorScript.Tool.FOLIAGE_PAINT
	editor.brush_radius = 20.0
	editor.brush_strength = 0.8
	editor.brush_hardness = 0.3

	workstation.set_editor(editor)
	workstation.sync_from_editor_state()

	assert_eq(workstation._current_workflow_id, TerrainWorkspaceScript.Workflow.SCATTER, "Workstation should switch to Foliage mode when the editor tool is foliage paint.")

	editor.current_tool = TerrainEditorScript.Tool.TILE_STAMP
	workstation.sync_from_editor_state()

	assert_eq(workstation._current_workflow_id, TerrainWorkspaceScript.Workflow.STAMP, "Workstation should switch to Tile mode when the editor tool becomes tile placement.")


func test_prompt_unsaved_changes_updates_custom_copy() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())

	workstation.prompt_unsaved_changes("quit")

	var prompt_host: Control = workstation.get_node("%PromptHost")
	var prompt_card: PanelContainer = workstation.get_node("%PromptCard")
	var lead: Label = workstation.get_node("%PromptLead")
	var info: Label = workstation.get_node("%PromptInfoLabel")
	var info_panel: PanelContainer = workstation.get_node("%PromptInfoPanel")
	var primary: Button = workstation.get_node("%PromptPrimaryButton")
	var secondary: Button = workstation.get_node("%PromptSecondaryButton")
	var tertiary: Button = workstation.get_node("%PromptTertiaryButton")
	assert_true(prompt_host.visible, "Unsaved prompt should show the custom modal host.")
	assert_true(prompt_card.visible, "Unsaved prompt should show the custom modal card.")
	assert_eq(lead.text, "Save changes?", "Unsaved prompt should keep the lead short.")
	assert_eq(info.text, "", "Unsaved prompt should no longer show a next-step callout.")
	assert_false(info_panel.visible, "Unsaved prompt should hide the info panel when there is no extra copy.")
	assert_eq(primary.text, "Save", "Unsaved prompt should keep save as the primary confirmation button.")
	assert_eq(secondary.text, "Cancel", "Unsaved prompt should let the user stay in the editor explicitly.")
	assert_eq(tertiary.text, "Discard", "Unsaved prompt should keep discard visible as a secondary destructive action.")


func test_export_flavor_dialog_updates_format_copy() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())

	workstation._show_export_flavor_dialog("C:/Exports/TestTerrain")

	var prompt_host: Control = workstation.get_node("%PromptHost")
	var prompt_card: PanelContainer = workstation.get_node("%PromptCard")
	var lead: Label = workstation.get_node("%PromptLead")
	var body: Label = workstation.get_node("%PromptBody")
	var info: Label = workstation.get_node("%PromptInfoLabel")
	var format_bhd: Button = workstation.get_node("%PromptFormatBHD")
	var format_cdep: Button = workstation.get_node("%PromptFormatCDEP")
	var primary: Button = workstation.get_node("%PromptPrimaryButton")
	var secondary: Button = workstation.get_node("%PromptSecondaryButton")
	assert_true(prompt_host.visible, "Export prompt should show the custom modal host.")
	assert_true(prompt_card.visible, "Export prompt should show the custom modal card.")
	assert_eq(lead.text, "Export", "Export prompt should keep the lead minimal.")
	assert_eq(body.text, "", "Export prompt should not show extra body copy.")
	assert_false(body.visible, "Export prompt should hide the body when there is no extra copy.")
	assert_eq(info.text, "", "Export prompt should not show the folder path.")
	assert_false(workstation.get_node("%PromptInfoPanel").visible, "Export prompt should not show a folder summary.")
	assert_eq(format_bhd.text, "BHD", "Export choices should keep BHD simple.")
	assert_eq(format_cdep.text, "JO/DFX", "Export choices should keep JO/DFX simple.")
	assert_eq(primary.text, "Export", "Export prompt should keep Export as the primary action.")
	assert_eq(secondary.text, "Cancel", "Export prompt should keep Cancel as the secondary action.")
	assert_null(workstation.get_node_or_null("%PromptFormatLabel"), "Export prompt should no longer show a separate format label.")


func test_asset_dock_builds_preview_cards_for_shared_maps() -> void:
	var dock = add_child_autofree(TerrainEditorAssetDockScene.instantiate())

	assert_true(dock._slot_previews.has("charmap"), "Asset dock should build a preview card for the surface-type map.")
	assert_true(dock._slot_previews.has("foliagemap"), "Asset dock should build a preview card for the foliage map.")
	assert_true(dock._slot_previews.has("tilestrip"), "Asset dock should build a preview card for the tile atlas.")


func test_asset_dock_syncs_slot_filename_labels() -> void:
	var dock = add_child_autofree(TerrainEditorAssetDockScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())
	editor.texture_files = {
		"tilestrip": "atlas_01.pcx",
	}

	dock.set_editor(editor)
	dock.sync_from_editor_state()

	var filename_label: Label = dock._slot_filename_labels["tilestrip"]
	assert_eq(filename_label.text, "atlas_01.pcx", "Asset dock should show the current texture filename for shared asset slots.")


func test_asset_dock_uses_properties_tab_and_removes_old_toggles() -> void:
	var dock = add_child_autofree(TerrainEditorAssetDockScene.instantiate())
	var tabs: TabContainer = dock.get_node("%Tabs")

	assert_eq(tabs.get_tab_count(), 2, "Asset dock should only expose Properties and Camera tabs.")
	assert_eq(tabs.get_tab_title(0), "Properties", "The first dock tab should be renamed to Properties.")
	assert_null(dock.get_node_or_null("Tabs/Document"), "The old Document tab should be removed.")
	assert_null(dock.get_node_or_null("%ViewTab"), "The old View tab should be removed.")
	assert_null(dock.get_node_or_null("%WaterVisibleToggle"), "The water-plane toggle should move out of the dock.")
	assert_null(dock.get_node_or_null("%HorizonSpin"), "Horizon controls should be removed from the dock.")
	assert_null(dock.get_node_or_null("%ColormapToggle"), "Colormap visibility toggle should be removed from the dock.")
	assert_null(dock.get_node_or_null("%WireframeToggle"), "Wireframe toggle should be removed from the dock.")


func test_asset_dock_and_inspectors_do_not_poll_when_idle() -> void:
	var dock = add_child_autofree(TerrainEditorAssetDockScene.instantiate())
	var sculpt = add_child_autofree(SculptInspectorScene.instantiate())
	var paint = add_child_autofree(PaintInspectorScene.instantiate())
	var scatter = add_child_autofree(ScatterInspectorScene.instantiate())
	var stamp = add_child_autofree(StampInspectorScene.instantiate())
	var layout = add_child_autofree(LayoutInspectorScene.instantiate())

	assert_false(dock.is_processing(), "Asset dock should sync from editor state changes instead of idle polling.")
	assert_false(sculpt.is_processing(), "Sculpt inspector should sync from editor state changes instead of idle polling.")
	assert_false(paint.is_processing(), "Paint inspector should sync from editor state changes instead of idle polling.")
	assert_false(scatter.is_processing(), "Foliage inspector should sync from editor state changes instead of idle polling.")
	assert_false(stamp.is_processing(), "Tile inspector should sync from editor state changes instead of idle polling.")
	assert_false(layout.is_processing(), "Layout inspector should sync from editor state changes instead of idle polling.")


func test_sculpt_inspector_syncs_from_editor_ui_state_signal() -> void:
	var inspector = add_child_autofree(SculptInspectorScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())

	inspector.set_editor(editor)
	editor.set_brush_radius_value(37.0)

	var radius_spin: SpinBox = inspector._brush.get_radius_spin()
	assert_eq(radius_spin.value, 37.0, "Editor UI state changes should update subscribed inspectors without per-frame polling.")


func test_workstation_uses_clip_text_for_long_labels() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())

	var project_label: Label = workstation.get_node("%ProjectLabel")
	var status_context_label: Label = workstation.get_node("%StatusContextLabel")
	var status_camera_label: Label = workstation.get_node("%StatusCameraLabel")

	assert_true(project_label.clip_text, "Project label should clip rather than forcing the top bar wider.")
	assert_true(status_context_label.clip_text, "Status context should clip instead of forcing horizontal overflow.")
	assert_true(status_camera_label.clip_text, "Status camera text should clip instead of forcing horizontal overflow.")


func test_pressing_layout_mode_restores_edit_sectors_tool() -> void:
	var workstation = add_child_autofree(EditorWorkstationScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())
	editor.current_tool = TerrainEditorScript.Tool.PAINT_DETAIL

	workstation.set_editor(editor)
	workstation._on_workflow_pressed(TerrainWorkspaceScript.Workflow.LAYOUT)

	assert_eq(editor.current_tool, TerrainEditorScript.Tool.EDIT_SECTORS, "Selecting Layout should restore the sector editing tool.")


func test_layout_inspector_is_trimmed_to_board_and_legend() -> void:
	var inspector = add_child_autofree(LayoutInspectorScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())
	editor.current_tool = TerrainEditorScript.Tool.PAINT_DETAIL

	inspector.set_editor(editor)

	assert_eq(editor.current_tool, TerrainEditorScript.Tool.EDIT_SECTORS, "Layout inspector should force sector editing when it becomes active.")
	assert_null(inspector.get_node_or_null("Scroll/Box/Intro"), "Layout inspector should remove the extra intro copy.")
	assert_null(inspector.get_node_or_null("%BrushPickerRow"), "Layout inspector should remove the old brush picker.")
	assert_null(inspector.get_node_or_null("Scroll/Box/BoardSection/SizeRow/SizeSuffix"), "Layout inspector should remove the square suffix.")
	assert_null(inspector.get_node_or_null("Scroll/Box/BoardSection/SizeHint"), "Layout inspector should remove the non-square helper copy.")
	assert_true(inspector.get_node_or_null("%LegendGrid") != null, "Layout inspector should keep a passive legend.")
	assert_true(inspector.get_node_or_null("%SectorOverlayToggle") != null, "Layout inspector should expose a sector overlay toggle under the map layout board.")


func test_layout_inspector_sector_overlay_toggle_syncs_with_editor() -> void:
	var inspector = add_child_autofree(LayoutInspectorScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())
	editor.set_sector_overlay_visible(true)

	inspector.set_editor(editor)

	var toggle: CheckBox = inspector.get_node("%SectorOverlayToggle")
	assert_true(toggle.button_pressed, "Layout inspector should reflect the editor's current sector overlay visibility.")

	inspector._on_sector_overlay_toggled(false)
	assert_false(editor.is_sector_overlay_visible(), "Toggling sector overlay off in Layout should update editor state.")


func test_foliage_inspector_selection_updates_editor_selection() -> void:
	var inspector = add_child_autofree(ScatterInspectorScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())
	editor.add_foliage_def()
	editor.add_foliage_def()
	editor.current_tool = TerrainEditorScript.Tool.FOLIAGE_PAINT

	inspector.set_editor(editor)
	inspector._on_list_selected(1)

	assert_eq(editor.get_selected_foliage_def_index(), 1, "Selecting a foliage list item should update the editor selection used by paint.")
	assert_true(editor.get_selected_foliage_def() != null, "The selected foliage def should be available after selecting it in the inspector.")


func test_foliage_inspector_reflects_editor_selection_and_add_remove() -> void:
	var inspector = add_child_autofree(ScatterInspectorScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())
	editor.add_foliage_def()
	editor.add_foliage_def()
	editor.current_tool = TerrainEditorScript.Tool.FOLIAGE_PAINT
	editor.set_selected_foliage_def_index(1)

	inspector.set_editor(editor)

	var list: ItemList = inspector.get_node("%FoliageList")
	assert_true(list.is_selected(1), "The foliage inspector should highlight the editor's selected foliage def.")

	inspector._on_add_pressed()
	assert_eq(editor.get_selected_foliage_def_index(), 2, "Adding a foliage def should leave the new foliage type selected in editor state.")

	inspector._on_remove_pressed()
	assert_eq(editor.get_selected_foliage_def_index(), 1, "Removing the selected foliage def should clamp selection to the remaining valid index.")
	assert_true(list.is_selected(1), "The foliage inspector should stay aligned with the clamped editor selection after removal.")


func test_stamp_inspector_removes_entry_list_and_apply_workflow() -> void:
	var inspector = add_child_autofree(StampInspectorScene.instantiate())

	assert_null(inspector.get_node_or_null("%EntriesList"), "Tile inspector should remove the placed-overlays list.")
	assert_null(inspector.get_node_or_null("%ApplyButton"), "Tile inspector should remove the explicit Apply flow.")
	assert_null(inspector.get_node_or_null("%FocusButton"), "Tile inspector should remove list-based camera focusing.")
	assert_null(inspector.get_node_or_null("%DeleteButton"), "Tile inspector should remove list-based delete controls.")
	assert_true(inspector.get_node_or_null("%SelectionDoneButton") != null, "Tile inspector should expose a direct exit action for selection mode.")
	assert_true(inspector.get_node_or_null("%SelectionDeleteButton") != null, "Tile inspector should expose a direct delete action for the selected tile.")


func test_stamp_inspector_atlas_click_replaces_selected_tile_immediately() -> void:
	var inspector = add_child_autofree(StampInspectorScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())
	editor._document.new_tileinfo()
	editor._document.set_tile_stamp_tile_index(2)
	editor._document.stamp_tileinfo_cell(3, 4)
	editor.select_tileinfo_entry(0)

	inspector.set_editor(editor)
	inspector._on_atlas_selected(5)

	var entry: NovaTerrainTileEntry = editor.get_selected_tileinfo_entry()
	assert_not_null(entry, "Tile selection should remain valid after replacing from the atlas.")
	assert_eq(entry.get_tile_index(), 5, "Atlas clicks should replace the selected tile immediately.")


func test_stamp_inspector_atlas_focus_follows_selected_tile() -> void:
	var inspector = add_child_autofree(StampInspectorScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())
	editor._document.data = NovaTerrainData.new()
	editor._document.new_tileinfo()
	# Seed a dummy tilestrip so the atlas has enough tiles for index 2 to be selectable.
	var strip_image := Image.create(256, 64, false, Image.FORMAT_RGBA8)
	strip_image.fill(Color(0.5, 0.5, 0.5, 1.0))
	editor._document.data.set_tilestrip_tex(ImageTexture.create_from_image(strip_image))
	editor._document.set_tile_stamp_tile_index(2)
	editor._document.stamp_tileinfo_cell(3, 4)
	editor.select_tileinfo_entry(0)

	inspector.set_editor(editor)
	inspector.sync_from_editor()

	var atlas_status: Label = inspector.get_node("%AtlasStatus")
	var atlas_list: ItemList = inspector.get_node("%AtlasList")
	assert_string_contains(atlas_status.text, "editing 002", "Atlas status should reflect the selected tile when replace-on-click is active.")
	assert_true(atlas_list.is_selected(2), "Atlas selection should follow the selected tile while a placed tile is active.")


func test_stamp_inspector_reuses_tile_preview_icons_until_atlas_changes() -> void:
	var inspector = add_child_autofree(StampInspectorScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())
	editor._document.data = NovaTerrainData.new()
	var strip_image := Image.create(256, 64, false, Image.FORMAT_RGBA8)
	strip_image.fill(Color(0.5, 0.5, 0.5, 1.0))
	var tilestrip := ImageTexture.create_from_image(strip_image)
	editor._document.data.set_tilestrip_tex(tilestrip)

	inspector.set_editor(editor)
	var cache_size: int = inspector._tile_icon_cache.size()
	var first: Texture2D = inspector._build_icon(tilestrip, 2, 4)
	var second: Texture2D = inspector._build_icon(tilestrip, 2, 4)

	assert_true(first == second, "Tile preview icons should be cached while the tilestrip is unchanged.")
	assert_eq(inspector._tile_icon_cache.size(), cache_size, "Repeated tile icon requests should not allocate duplicate AtlasTextures.")


func test_stamp_inspector_flag_toggle_updates_selected_tile_immediately() -> void:
	var inspector = add_child_autofree(StampInspectorScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())
	editor._document.new_tileinfo()
	editor._document.set_tile_stamp_tile_index(2)
	editor._document.stamp_tileinfo_cell(3, 4)
	editor.select_tileinfo_entry(0)

	inspector.set_editor(editor)
	inspector._on_flip_x(true)

	var entry: NovaTerrainTileEntry = editor.get_selected_tileinfo_entry()
	assert_not_null(entry, "Tile selection should remain valid after toggling a transform flag.")
	assert_true((entry.get_flags() & NovaTerrainTileInfo.FLAG_FLIP_X) != 0, "Tile transform toggles should update the selected tile immediately.")


func test_stamp_inspector_shows_quiet_empty_selection_state() -> void:
	var inspector = add_child_autofree(StampInspectorScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())
	editor._document.new_tileinfo()

	inspector.set_editor(editor)

	var done_button: Button = inspector.get_node("%SelectionDoneButton")
	var summary: Label = inspector.get_node("%SelectionSummary")
	var delete_button: Button = inspector.get_node("%SelectionDeleteButton")
	assert_eq(summary.text, "No tile selected.", "Tile inspector should use a quiet empty-state until the user selects a placed tile.")
	assert_true(done_button.disabled, "Done should stay disabled until a tile is selected.")
	assert_true(delete_button.disabled, "Delete should stay disabled until a tile is selected.")


func test_stamp_inspector_done_clears_selection() -> void:
	var inspector = add_child_autofree(StampInspectorScene.instantiate())
	var editor = autofree(TerrainEditorScript.new())
	editor._document.new_tileinfo()
	editor._document.set_tile_stamp_tile_index(2)
	editor._document.stamp_tileinfo_cell(3, 4)
	editor.select_tileinfo_entry(0)

	inspector.set_editor(editor)
	inspector._on_selection_done_pressed()

	assert_false(editor.has_selected_tileinfo_entry(), "Done should leave Tile mode in placement state with no selected tile.")


func test_quadrant_board_cycles_left_click_and_clears_right_click() -> void:
	var board = add_child_autofree(QuadrantBoardScript.new())
	var values := PackedInt32Array()
	values.resize(256)
	board.set_grid_state(2, 2, values)

	board._stamp_at(Vector2i(0, 0), MOUSE_BUTTON_LEFT)
	assert_eq(board.grid[0], 1, "First left-click should cycle Empty to NW.")

	board._stamp_at(Vector2i(0, 0), MOUSE_BUTTON_LEFT)
	assert_eq(board.grid[0], 3, "Second left-click should cycle NW to NE.")

	board._stamp_at(Vector2i(0, 0), MOUSE_BUTTON_LEFT)
	assert_eq(board.grid[0], 2, "Third left-click should cycle NE to SW.")

	board._stamp_at(Vector2i(0, 0), MOUSE_BUTTON_LEFT)
	assert_eq(board.grid[0], 4, "Fourth left-click should cycle SW to SE.")

	board._stamp_at(Vector2i(0, 0), MOUSE_BUTTON_LEFT)
	assert_eq(board.grid[0], 1, "Fifth left-click should wrap SE back to NW.")

	board._stamp_at(Vector2i(0, 0), MOUSE_BUTTON_RIGHT)
	assert_eq(board.grid[0], 0, "Right-click should clear the cell to Empty.")
