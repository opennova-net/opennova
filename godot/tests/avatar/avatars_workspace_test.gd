extends GutTest

# AvatarsEditorWorkspace surface: identity/filters, open from the globalized
# fixture path, the ViewportMount lifecycle, the three workflow inspectors, and
# combo selection driving the preview.
const AvatarsWorkspaceScript = preload("res://modtools/avatar/avatars_workspace.gd")
const AVATARS_FIXTURE := "res://../fixtures/avatars/Avatars.def"

var _ws


func before_each() -> void:
	_ws = AvatarsWorkspaceScript.new()


func _fixture_path() -> String:
	return ProjectSettings.globalize_path(AVATARS_FIXTURE)


func test_identity_and_filters() -> void:
	assert_eq(_ws.get_workspace_label(), "Avatars", "label")
	assert_eq(_ws.get_workspace_id(), "avatar", "id")
	assert_eq(_ws.get_open_resource_kind(), "avatar", "open kind")
	var filters: PackedStringArray = _ws.get_open_dialog_filters()
	assert_eq(filters.size(), 1, "one open filter")
	assert_true(String(filters[0]).to_lower().contains("def"), "filter targets .def")


func test_open_file_sets_project_title() -> void:
	var path := _fixture_path()
	if not FileAccess.file_exists(path):
		pending("Avatars.def fixture missing")
		return
	assert_eq(_ws.open_file(path), OK, "open_file from disk")
	assert_eq(_ws.get_project_title(), "Avatars", "project title from the opened file")
	assert_true(_ws.db().is_loaded(), "database loaded after open")
	# Save (vs Save As) gates on unsaved changes, so a clean open is not "savable"
	# yet; Save As is always available once a resource exists.
	assert_false(_ws.can_save(), "a clean opened document has nothing to Save")
	assert_true(_ws.can_save_as(), "an opened document can Save As")


func test_mount_viewport_adds_child_and_release_tears_down() -> void:
	var host := Control.new()
	add_child_autofree(host)
	_ws.mount_viewport(host)
	assert_eq(host.get_child_count(), 1, "mount adds the preview")
	var first_preview = host.get_child(0)

	# A remount reuses the single preview (no second instance).
	_ws.unmount_viewport(host)
	assert_eq(host.get_child_count(), 0, "unmount detaches the preview")
	_ws.mount_viewport(host)
	assert_eq(host.get_child_count(), 1, "remount re-attaches")
	assert_eq(host.get_child(0), first_preview, "remount reuses the same preview")

	assert_not_null(_ws.get_viewport_camera(), "preview exposes its camera")

	_ws.release_viewport()
	assert_eq(host.get_child_count(), 0, "release tears the preview down")


func test_each_workflow_inspector_builds() -> void:
	var path := _fixture_path()
	if FileAccess.file_exists(path):
		_ws.open_file(path)
	for workflow_id in [_ws.Workflow.TREE, _ws.Workflow.PARTS, _ws.Workflow.COMBOS]:
		var host := Control.new()
		add_child_autofree(host)
		_ws.build_workflow_inspector(workflow_id, host)
		assert_gt(host.get_child_count(), 0, "workflow %d builds inspector content" % workflow_id)


func test_workflow_defs_present() -> void:
	var defs: Array = _ws.get_workflows()
	assert_eq(defs.size(), 3, "three workflow inspectors")
	# TREE is the first (default) workflow.
	assert_eq(int((defs[0] as InspectorDef).id), _ws.Workflow.TREE, "TREE first")


func test_selecting_combo_composes_preview() -> void:
	var path := _fixture_path()
	if not FileAccess.file_exists(path):
		pending("Avatars.def fixture missing")
		return
	assert_eq(_ws.open_file(path), OK)
	var host := Control.new()
	add_child_autofree(host)
	_ws.mount_viewport(host)
	await get_tree().process_frame

	# Find a (nat, div, combo) and drive the preview. Without a mounted resource
	# root the parts will not resolve (compose 0 models) but show_combo must run
	# cleanly and clear/compose without erroring.
	var database = _ws.db()
	var shown := false
	for n in range(database.get_nationality_count()):
		for d in range(database.get_division_count(n)):
			if database.get_combo_count(n, d) > 0:
				_ws.show_combo(n, d, 0)
				shown = true
				break
		if shown:
			break
	assert_true(shown, "found a combo to show")
	# The preview node exists and survived the show_combo call.
	assert_eq(host.get_child_count(), 1, "preview still mounted after show_combo")


func test_apply_model_marks_dirty() -> void:
	var path := _fixture_path()
	if not FileAccess.file_exists(path):
		pending("Avatars.def fixture missing")
		return
	assert_eq(_ws.open_file(path), OK)
	assert_false(_ws.has_unsaved_changes(), "clean after open")
	var model: Dictionary = _ws.db().get_model()
	var parts: Array = model.get("parts", [])
	if parts.size() > 0:
		(parts[0] as Dictionary)["display_name"] = "EDITED_KEY"
	_ws.apply_model(model)
	assert_true(_ws.has_unsaved_changes(), "apply_model marks dirty through the document")
