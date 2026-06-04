extends GutTest

# Phase 3: MissionEditorWorkspace shell hooks. Covers the capability hooks the
# editor shell reads to wire the Open action, resource browser, title, and
# inspector — without standing up the whole EditorWorkstation. The mission "kind"
# (-> .bms in the resource index) and the *.bms dialog filter are what make the
# Open flow reach a mission at all.

const MissionWorkspace := preload("res://modtools/editor/mission_workspace.gd")


func test_open_resource_kind_is_mission() -> void:
	var ws = MissionWorkspace.new()
	assert_eq(ws.get_open_resource_kind(), "mission")


func test_open_dialog_offers_bms() -> void:
	var ws = MissionWorkspace.new()
	var joined := ""
	for filter in ws.get_open_dialog_filters():
		joined += String(filter)
	assert_string_contains(joined.to_lower(), "bms")


func test_defaults_without_an_editor() -> void:
	var ws = MissionWorkspace.new()
	assert_eq(ws.get_workspace_id(), "mission")
	assert_eq(ws.get_workspace_label(), "Mission")
	assert_eq(ws.get_project_title(), "Mission", "no mission loaded -> plain title")
	assert_false(ws.has_unsaved_changes(), "an unedited workspace reports no unsaved changes")
	assert_false(ws.can_open(), "Open is gated on a bound terrain editor")
	# Visibility (button is created) is decoupled from enablement (can_save*). The shell
	# only builds buttons on workspace switch, so Save / Save As must be visible up front
	# and merely disabled until a mission is loaded / dirtied.
	assert_true(ws.has_save_action(), "Save button is created up front")
	assert_true(ws.has_save_as_action(), "Save As button is created up front")
	assert_false(ws.can_save(), "Save is disabled until a dirty mission with a path")
	assert_false(ws.can_save_as(), "Save As is disabled until a mission is loaded")
	assert_true(ws.shows_camera_status())


func test_build_inspector_mounts_a_panel() -> void:
	var ws = MissionWorkspace.new()
	var host := Control.new()
	add_child_autofree(host)
	ws.build_inspector(host)
	assert_gt(host.get_child_count(), 0, "the mission inspector panel is mounted into the host")


# --- Phase 5: undo / redo hooks -----------------------------------------------

func test_undo_redo_hooks_delegate_to_the_controller() -> void:
	var ws = MissionWorkspace.new()
	assert_false(ws.can_undo(), "no history -> cannot undo")
	assert_false(ws.can_redo(), "no history -> cannot redo")
	# Safe no-ops with nothing loaded (the shell may probe / fire these any time).
	ws.undo()
	ws.redo()

	# can_undo/can_redo read the controller's stacks: seeding them proves delegation
	# without standing up a full mission load.
	ws._controller._undo_stack.append(PackedByteArray([1]))
	assert_true(ws.can_undo(), "can_undo reflects the controller's undo stack")
	ws._controller._redo_stack.append(PackedByteArray([2]))
	assert_true(ws.can_redo(), "can_redo reflects the controller's redo stack")


# --- Right-dock split: the inspector mounts its editor in %AssetDock ----------

func test_uses_asset_dock() -> void:
	var ws = MissionWorkspace.new()
	assert_true(ws.uses_asset_dock(), "the mission workspace opts into the right dock")


func test_asset_dock_before_build_hosts_the_editor() -> void:
	# The shell forwards the dock (set_asset_dock) BEFORE building the inspector on activation; the
	# adapter caches it so the fresh inspector builds its editor straight into the dock.
	var ws = MissionWorkspace.new()
	var dock := PanelContainer.new()
	add_child_autofree(dock)
	var host := Control.new()
	add_child_autofree(host)
	ws.set_asset_dock(dock)
	ws.build_inspector(host)
	assert_not_null(dock.find_child("MissionPosX", true, false), "the entity editor mounts in the dock")
	assert_null(host.find_child("MissionPosX", true, false), "and not in the left inspector host")
	# On switch-away the shell calls set_asset_dock(null) before clearing the dock; the editor must
	# reparent back under the inspector (host) rather than be freed.
	ws.set_asset_dock(null)
	assert_null(dock.find_child("MissionPosX", true, false), "the dock is emptied of the editor")
	assert_not_null(host.find_child("MissionPosX", true, false), "the editor is reparented under the inspector host")
