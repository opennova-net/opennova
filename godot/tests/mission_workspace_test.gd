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
