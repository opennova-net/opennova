extends GutTest

# Exercises the read-only ONED HUD preview workspace end-to-end (open -> preview +
# inspector) without a shell. Uses the repo hudpos.def fixture on disk.

const HUDPOS_PATH := "res://../fixtures/def/hudpos.def"

var _ws: HudPreviewWorkspace


func before_each() -> void:
	_ws = HudPreviewWorkspace.new()


func test_identity() -> void:
	assert_eq(_ws.get_workspace_id(), "hud")
	assert_eq(_ws.get_workspace_label(), "HUD")
	assert_eq(_ws.get_open_resource_kind(), "hudpos")


func test_read_only_actions() -> void:
	# Preview-only: open is offered, edit/save/export are not.
	assert_true(_ws.can_open(), "Open is available.")
	assert_false(_ws.can_new(), "No New action (read-only).")
	assert_false(_ws.can_save(), "No Save action (read-only).")
	assert_false(_ws.can_export(), "No Export action (read-only).")
	assert_false(_ws.has_unsaved_changes(), "Preview is never dirty.")


func test_open_populates() -> void:
	var abs := ProjectSettings.globalize_path(HUDPOS_PATH)
	var err := _ws.open_file(abs)
	assert_eq(err, OK, "open_file should parse the fixture.")
	assert_false(_ws.get_current_resource_path().is_empty(), "Source path recorded.")
	assert_string_contains(_ws.get_status_context(), "stance", "Status reports stance count.")
	assert_eq(_ws.get_project_title(), "hudpos", "Title from the fixture basename.")


func test_viewport_and_inspector() -> void:
	var host := Control.new()
	add_child_autofree(host)

	_ws.mount_viewport(host)
	var preview := host.get_child(0) if host.get_child_count() > 0 else null
	assert_not_null(preview, "mount_viewport adds the preview control.")
	assert_true(preview is HudLayoutPreview, "Preview is a HudLayoutPreview.")

	var inspector_host := Control.new()
	add_child_autofree(inspector_host)
	_ws.build_inspector(inspector_host)
	assert_gt(inspector_host.get_child_count(), 0, "build_inspector populates the host.")

	# Opening after mount refreshes the preview without error.
	var abs := ProjectSettings.globalize_path(HUDPOS_PATH)
	assert_eq(_ws.open_file(abs), OK)

	_ws.release_viewport()
	assert_eq(host.get_child_count(), 0, "release_viewport detaches the preview.")
