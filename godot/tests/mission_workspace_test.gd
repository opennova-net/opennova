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
	assert_false(ws.has_unsaved_changes(), "read-only workspace never reports unsaved changes")
	assert_false(ws.can_open(), "Open is gated on a bound terrain editor")
	assert_false(ws.can_save(), "no Save action this phase")
	assert_false(ws.can_save_as(), "no Save As action this phase")
	assert_true(ws.shows_camera_status())


func test_build_inspector_mounts_a_panel() -> void:
	var ws = MissionWorkspace.new()
	var host := Control.new()
	add_child_autofree(host)
	ws.build_inspector(host)
	assert_gt(host.get_child_count(), 0, "the mission inspector panel is mounted into the host")
