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


func _write_temp_avatars(name: String, text: String) -> String:
	var path := ProjectSettings.globalize_path("user://%s" % name)
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file, "temp avatar file opens for write")
	file.store_string(text)
	file.close()
	return path


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


func test_status_context_reports_parser_diagnostics() -> void:
	var path := _write_temp_avatars("avatars_workspace_diagnostics.def",
		"define body BODY_A\n{\n\tgraphic body.3di\n}\n"
		+ "nationality N00 AV_NAT\n{\n\tdivision D00 AV_DIV\n\t{\n"
		+ "\t\tcombo 001 MISSING_HEAD BODY_A\n\t}\n}\n")
	assert_eq(_ws.open_file(path), OK)
	assert_string_contains(_ws.get_status_context(), "1 issue", "status includes diagnostics")
	DirAccess.remove_absolute(path)


func test_mount_viewport_adds_child_and_release_tears_down() -> void:
	var mount := Control.new()
	add_child_autofree(mount)
	_ws.mount_viewport(mount)
	assert_eq(mount.get_child_count(), 1, "mount adds the preview")
	var first_preview = mount.get_child(0)

	# A remount reuses the single preview (no second instance).
	_ws.unmount_viewport(mount)
	assert_eq(mount.get_child_count(), 0, "unmount detaches the preview")
	_ws.mount_viewport(mount)
	assert_eq(mount.get_child_count(), 1, "remount re-attaches")
	assert_eq(mount.get_child(0), first_preview, "remount reuses the same preview")

	assert_not_null(_ws.get_viewport_camera(), "preview exposes its camera")

	_ws.release_viewport()
	assert_eq(mount.get_child_count(), 0, "release tears the preview down")


func test_each_workflow_inspector_builds() -> void:
	var path := _fixture_path()
	if FileAccess.file_exists(path):
		_ws.open_file(path)
	for workflow_id in [_ws.Workflow.TREE, _ws.Workflow.PARTS, _ws.Workflow.COMBOS]:
		var mount := Control.new()
		add_child_autofree(mount)
		_ws.build_workflow_inspector(workflow_id, mount)
		assert_gt(mount.get_child_count(), 0, "workflow %d builds inspector content" % workflow_id)


func test_parts_inspector_uses_reference_widgets() -> void:
	var path := _fixture_path()
	if not FileAccess.file_exists(path):
		pending("Avatars.def fixture missing")
		return
	assert_eq(_ws.open_file(path), OK)
	var mount := Control.new()
	add_child_autofree(mount)
	_ws.build_workflow_inspector(_ws.Workflow.PARTS, mount)
	var graphic := mount.find_child("PartGraphicRef", true, false) as ResourceRefWidget
	assert_not_null(graphic, "graphic field uses ResourceRefWidget")
	assert_true(graphic.get_value().to_lower().ends_with(".3di"), "graphic ref keeps .3di filename")
	var display := mount.find_child("PartDisplayRef", true, false) as StringRefWidget
	assert_not_null(display, "display key uses StringRefWidget")


func test_workflow_defs_present() -> void:
	var defs: Array = _ws.get_workflows()
	assert_eq(defs.size(), 3, "three workflow inspectors")
	# TREE is the first (default) workflow.
	assert_eq(int((defs[0] as InspectorDef).id), _ws.Workflow.TREE, "TREE first")


func test_focus_reference_selects_avatar_entities() -> void:
	var path := _fixture_path()
	if not FileAccess.file_exists(path):
		pending("Avatars.def fixture missing")
		return
	assert_eq(_ws.open_file(path), OK)
	assert_eq(_ws.focus_reference(FocusPayload.for_part(NovaAvatarDatabase.PART_BODY, "JO_BODY_SEAL_1")), OK)
	assert_eq(_ws.get_active_workflow_id(), _ws.Workflow.PARTS)
	assert_eq(_ws.focus_reference(FocusPayload.for_combo(0, 0, 0)), OK)
	assert_eq(_ws.get_active_workflow_id(), _ws.Workflow.TREE)
	assert_eq(_ws.focus_reference(FocusPayload.for_part(NovaAvatarDatabase.PART_HEAD, "NO_SUCH_PART")), ERR_DOES_NOT_EXIST)


func test_selecting_combo_composes_preview() -> void:
	var path := _fixture_path()
	if not FileAccess.file_exists(path):
		pending("Avatars.def fixture missing")
		return
	assert_eq(_ws.open_file(path), OK)
	var mount := Control.new()
	add_child_autofree(mount)
	_ws.mount_viewport(mount)
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
	assert_eq(mount.get_child_count(), 1, "preview still mounted after show_combo")


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
