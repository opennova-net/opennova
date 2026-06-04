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


# --- New + undo / redo hooks --------------------------------------------------

func test_new_action_requires_a_terrain_editor() -> void:
	var ws = MissionWorkspace.new()
	assert_eq(ws.get_new_action_label(), "New Mission")
	assert_false(ws.can_new(), "New is gated on a bound terrain editor")
	# new_current with no terrain editor delegates to the controller, which reports it cannot.
	assert_ne(int(ws.new_current()), OK, "New fails (and is surfaced) without a terrain")


func test_undo_redo_hooks_delegate_to_the_controller() -> void:
	var ws = MissionWorkspace.new()
	assert_false(ws.can_undo(), "no history -> cannot undo")
	assert_false(ws.can_redo(), "no history -> cannot redo")
	# Safe no-ops with nothing loaded (the shell may probe / fire these any time).
	ws.undo()
	ws.redo()

	# The history lives on the document; seed it on a from-scratch mission and hand it to the
	# controller. can_undo/can_redo then reflect it -- proving the hooks delegate through the
	# controller without standing up a full terrain + world load.
	var mission := NovaMissionData.new()
	mission.create_default()
	mission.begin_edit()
	mission.add_entity(NovaMissionData.KIND_ITEM, 101291, Vector3.ZERO, Vector3.ZERO)
	mission.commit_edit()
	ws._controller._mission = mission
	assert_true(ws.can_undo(), "can_undo reflects the document's undo history")
	assert_false(ws.can_redo(), "nothing to redo yet")
	mission.undo()
	assert_false(ws.can_undo(), "the step was consumed")
	assert_true(ws.can_redo(), "and is now redoable")


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
