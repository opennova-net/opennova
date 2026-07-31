extends GutTest

# Styles-tab behavior on the merged Menus workspace (formerly the standalone
# Menu Styles workspace): document lifecycle over MnsStyleSheet (lossless
# load/save), the editor's snapshot undo/redo, the grouped variable table,
# the inspector population, cross-jump focus, and the byte-faithfulness
# keystone (open the real shipped menu_style.mns, save it untouched, get
# identical bytes). Plus a check that style_jump from the per-widget inspector
# stays IN-workspace by activating the Styles dock tab and selecting the
# variable.

const MnsEditorDocumentScript = preload("res://modtools/mnu/mns_editor_document.gd")
const MnsEditorScript = preload("res://modtools/mnu/mns_editor.gd")
const MnsInspectorScript = preload("res://modtools/mnu/mns_inspector.gd")
const MnsVariableTableScript = preload("res://modtools/mnu/mns_variable_table.gd")
const MnuEditorWorkspaceScript = preload("res://modtools/mnu/mnu_workspace.gd")

const FIXTURE := "res://../fixtures/mns/test_style.mns"
const REAL_FIXTURE := "res://../fixtures/mns/menu_style.mns"
const MENU_FIXTURE := "res://../fixtures/mnu/jo_main.mnu"
const TEMP_DIR := "user://test_mns_workspace"


func before_each() -> void:
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(TEMP_DIR))


func after_each() -> void:
	_cleanup_dir(TEMP_DIR)


func _cleanup_dir(dir_path: String) -> void:
	var abs := ProjectSettings.globalize_path(dir_path)
	var da := DirAccess.open(abs)
	if da == null:
		return
	da.list_dir_begin()
	var fname := da.get_next()
	while fname != "":
		if not da.current_is_dir():
			da.remove(fname)
		fname = da.get_next()
	da.list_dir_end()
	DirAccess.remove_absolute(abs)


# A workspace with its editor mounted into the tree, in STYLES dock-tab mode so
# save/new/open and undo target the .mns document. Tests can override via
# ws._dock_tab when they need the Properties side.
func _mounted_workspace() -> Array:
	var ws = MnuEditorWorkspaceScript.new()
	ws._dock_tab = MnuEditorWorkspaceScript.DockTab.STYLES
	var mount := Control.new()
	add_child_autofree(mount)
	ws.mount_viewport(mount)
	return [ws, mount]


func _collect_text(node: Node) -> String:
	var out := ""
	if node is Label:
		out += (node as Label).text + "\n"
	elif node is LineEdit:
		out += (node as LineEdit).text + "\n"
	elif node is Button:
		out += (node as Button).text + "\n"
	for child in node.get_children():
		out += _collect_text(child)
	return out


func _find_color_picker(node: Node) -> ColorPickerButton:
	if node is ColorPickerButton:
		return node
	for child in node.get_children():
		var found := _find_color_picker(child)
		if found != null:
			return found
	return null


func test_workspace_id_and_label() -> void:
	var ws = MnuEditorWorkspaceScript.new()
	assert_eq(ws.get_workspace_id(), "mnu", "workspace id")
	assert_eq(ws.get_workspace_label(), "Menus", "workspace label")
	ws.release_viewport()


func test_open_uses_indexed_quick_open_browser() -> void:
	var ws = MnuEditorWorkspaceScript.new()
	assert_true(ws.can_open(), "workspace can open")
	var kinds := ws.get_open_resource_kinds()
	assert_true(kinds.has("menu"), "claims the menu kind for .mnu quick-open")
	assert_true(kinds.has("menu_style"),
		"claims the menu_style kind too — quick-open of a .mns lands here")
	ws.release_viewport()


func test_document_open_clean_with_status() -> void:
	var pair := _mounted_workspace()
	var ws = pair[0]
	assert_eq(ws.open_file(FIXTURE), OK, "fixture stylesheet opens")
	assert_false(ws.has_unsaved_changes(), "opening leaves the document clean")
	assert_eq(ws.get_project_title(), "test_style", "title is the basename, no dirty star")
	assert_string_contains(ws.get_status_context(), "variable(s)", "status reports the variable count")
	ws.release_viewport()


func test_mount_unmount_does_not_crash() -> void:
	var pair := _mounted_workspace()
	var ws = pair[0]
	var mount: Control = pair[1]
	ws.unmount_viewport(mount)
	ws.mount_viewport(mount)
	assert_eq(ws.open_file(FIXTURE), OK, "open works across a remount")
	ws.release_viewport()
	pass_test("mount/unmount/remount survived")


func test_set_value_dirty_undo_redo() -> void:
	var pair := _mounted_workspace()
	var ws = pair[0]
	assert_eq(ws.open_file(FIXTURE), OK)
	var sheet: MnsStyleSheet = ws._mns_document.resource
	var editor = ws.get_editor_document()
	var original := String(sheet.get_variable("DEF_TEXT_FG"))

	editor.apply_edit({"op": "set_value", "name": "DEF_TEXT_FG", "value": "11223344"})
	assert_true(ws.has_unsaved_changes(), "an edit dirties the document")
	assert_eq(sheet.get_variable("DEF_TEXT_FG"), "11223344", "value applied")
	assert_true(editor.can_undo(), "edit pushed an undo entry")

	editor.undo()
	assert_eq(sheet.get_variable("DEF_TEXT_FG"), original, "undo restores the value")
	assert_true(editor.can_redo(), "undo arms redo")
	editor.redo()
	assert_eq(sheet.get_variable("DEF_TEXT_FG"), "11223344", "redo re-applies")
	ws.release_viewport()


func test_rename_undo_restores_byte_identical_source() -> void:
	var pair := _mounted_workspace()
	var ws = pair[0]
	assert_eq(ws.open_file(FIXTURE), OK)
	var sheet: MnsStyleSheet = ws._mns_document.resource
	var editor = ws.get_editor_document()
	var before := String(sheet.get_source_text())

	editor.apply_edit({"op": "rename", "name": "TRIM_COLOR", "new_name": "EDGE_COLOR"})
	assert_true(sheet.has_variable("EDGE_COLOR"), "rename applied")
	assert_false(sheet.has_variable("TRIM_COLOR"), "old name gone")
	editor.undo()
	assert_eq(String(sheet.get_source_text()), before,
		"undo restores the source text byte-identically (comments, layout, case)")
	ws.release_viewport()


func test_add_remove_variable_round_trip() -> void:
	var pair := _mounted_workspace()
	var ws = pair[0]
	assert_eq(ws.open_file(FIXTURE), OK)
	var sheet: MnsStyleSheet = ws._mns_document.resource
	var editor = ws.get_editor_document()

	editor.apply_edit({"op": "add", "name": "MY_COLOR", "value": "FF102030"})
	assert_eq(sheet.get_variable("MY_COLOR"), "FF102030", "added variable resolves")
	editor.apply_edit({"op": "remove", "name": "MY_COLOR"})
	assert_false(sheet.has_variable("MY_COLOR"), "removed variable is gone")
	editor.undo()
	assert_true(sheet.has_variable("MY_COLOR"), "undo restores the removed variable")
	editor.undo()
	assert_false(sheet.has_variable("MY_COLOR"), "second undo unwinds the add")
	ws.release_viewport()


# The keystone: the real shipped stylesheet saves byte-identical when untouched
# (its 38-line NovaLogic spec header, grouping, tabs, and missing final newline
# all survive the document model).
func test_save_round_trip_byte_equal_untouched() -> void:
	var pair := _mounted_workspace()
	var ws = pair[0]
	assert_eq(ws.open_file(REAL_FIXTURE), OK, "the real menu_style.mns opens")
	assert_eq(ws.save_as(TEMP_DIR), OK, "save as into the temp dir")
	var saved := FileAccess.get_file_as_bytes(TEMP_DIR.path_join("menu_style.mns"))
	var original := FileAccess.get_file_as_bytes(REAL_FIXTURE)
	assert_eq(saved.size(), original.size(), "saved size matches the fixture")
	assert_eq(saved, original, "untouched open + save is byte-identical")
	ws.release_viewport()


func test_value_edit_preserves_comments_and_layout() -> void:
	var pair := _mounted_workspace()
	var ws = pair[0]
	assert_eq(ws.open_file(REAL_FIXTURE), OK)
	var editor = ws.get_editor_document()
	editor.apply_edit({"op": "set_value", "name": "DEF_TEXT_FG", "value": "11223344"})
	assert_eq(ws.save_as(TEMP_DIR), OK)

	var saved := FileAccess.get_file_as_bytes(TEMP_DIR.path_join("menu_style.mns")) \
		.get_string_from_utf8().split("\n")
	var original := FileAccess.get_file_as_bytes(REAL_FIXTURE).get_string_from_utf8().split("\n")
	assert_eq(saved.size(), original.size(), "line count unchanged")
	var diffs := 0
	for i in original.size():
		if saved[i] != original[i]:
			diffs += 1
			assert_eq(saved[i], original[i].replace("FFFFFFFF", "11223344"),
				"only the value text changed; name and alignment tabs survive")
	assert_eq(diffs, 1, "exactly one line differs")
	ws.release_viewport()


func test_focus_reference_selects_variable() -> void:
	var pair := _mounted_workspace()
	var ws = pair[0]
	assert_eq(ws.open_file(FIXTURE), OK)
	assert_eq(ws.focus_reference(FocusPayload.for_variable("TRIM_COLOR")), OK, "known variable focuses")
	assert_eq(ws._mns_editor.get_selected_variable(), "TRIM_COLOR", "selection landed")
	assert_eq(ws.focus_reference(FocusPayload.for_variable("NOPE")), ERR_DOES_NOT_EXIST, "unknown variable reports")
	assert_eq(ws.focus_reference(FocusPayload.new()), OK, "empty focus is a no-op")
	ws.release_viewport()


# The %VAR% "Edit style" jump stays in-workspace: it activates the Styles dock
# tab and selects the variable, never crossing a shell boundary.
func test_style_jump_activates_styles_tab_locally() -> void:
	var pair := _mounted_workspace()
	var ws = pair[0]
	assert_eq(ws.open_file(FIXTURE), OK)
	# Start the workspace on Properties; the jump should flip it to Styles.
	ws._dock_tab = MnuEditorWorkspaceScript.DockTab.PROPERTIES
	ws._on_style_jump("TRIM_COLOR")
	assert_eq(ws._dock_tab, MnuEditorWorkspaceScript.DockTab.STYLES,
		"a style jump activates the Styles dock tab")
	assert_eq(ws._mns_editor.get_selected_variable(), "TRIM_COLOR",
		"the named variable is selected")
	ws.release_viewport()


func test_source_apply_routes_through_undo() -> void:
	var pair := _mounted_workspace()
	var ws = pair[0]
	assert_eq(ws.open_file(FIXTURE), OK)
	var sheet: MnsStyleSheet = ws._mns_document.resource
	var editor = ws.get_editor_document()
	var before := String(sheet.get_source_text())

	editor.apply_edit({"op": "source", "text": "// rewritten\nONLY_VAR FF0000FF\n"})
	assert_eq(sheet.get_variable_count(), 1, "source apply replaced the document")
	assert_true(ws.has_unsaved_changes(), "source apply dirties")
	editor.undo()
	assert_eq(String(sheet.get_source_text()), before, "one undo step restores the prior text")
	ws.release_viewport()


func test_new_seeds_commented_template() -> void:
	var pair := _mounted_workspace()
	var ws = pair[0]
	assert_eq(ws.new_current(), OK, "New seeds a fresh stylesheet")
	assert_false(ws.has_unsaved_changes(), "fresh document is clean")
	assert_eq(ws.get_current_resource_path(), "", "fresh document is pathless")
	var source := String(ws._mns_document.resource.get_source_text())
	assert_true(source.begins_with("//"), "template opens with a comment header")
	assert_eq(ws.save_current(), ERR_INVALID_PARAMETER,
		"pathless save routes the shell to Save As")
	ws.release_viewport()


func test_inspector_populates_for_selection() -> void:
	var pair := _mounted_workspace()
	var ws = pair[0]
	var dock := Control.new()
	add_child_autofree(dock)
	assert_eq(ws.open_file(FIXTURE), OK)
	ws.build_inspector(dock)
	# The Styles tab page mounts the per-variable inspector below the editor.
	var styles_page: Control = ws._styles_page
	assert_not_null(styles_page, "Styles tab page exists")
	var text := _collect_text(styles_page)
	assert_string_contains(text, "DEF_FONTNAME", "inspector shows the selected variable")
	assert_string_contains(text, "Delete variable", "inspector offers delete")
	ws.release_viewport()


func test_inspector_color_picker_preserves_raw_value_until_changed() -> void:
	var sheet := MnsStyleSheet.new()
	assert_eq(sheet.load_from_bytes("LOWER aabbcc\n".to_utf8_buffer()), OK)
	var inspector = MnsInspectorScript.new()
	add_child_autofree(inspector)
	inspector.show_variable(sheet, "LOWER")
	var edits: Array = []
	inspector.edit_requested.connect(func(edit: Dictionary) -> void:
		edits.append(edit))
	var picker := _find_color_picker(inspector)
	assert_not_null(picker, "color variables have a real picker")
	if picker == null:
		return
	picker.popup_closed.emit()
	assert_eq(edits.size(), 0,
		"opening and closing leaves lowercase/raw authoring untouched")
	picker.color = Color(1.0, 0.0, 0.0)
	picker.color_changed.emit(picker.color)
	picker.popup_closed.emit()
	assert_eq(edits.size(), 1, "an actual pick commits exactly once")
	if edits.size() == 1:
		assert_eq(String(edits[0].get("value", "")), "FF0000",
			"the chosen literal retains the source's six-digit shape")


func test_inspector_image_uses_resource_picker_and_commits_once() -> void:
	var sheet := MnsStyleSheet.new()
	assert_eq(sheet.load_from_bytes("PICTURE art.tga\n".to_utf8_buffer()), OK)
	var inspector = MnsInspectorScript.new()
	add_child_autofree(inspector)
	inspector.show_variable(sheet, "PICTURE")
	var edits: Array = []
	inspector.edit_requested.connect(func(edit: Dictionary) -> void:
		edits.append(edit))
	var ref = inspector.find_child("MnsImageRef", true, false)
	assert_not_null(ref, "image variables use the shared texture resource picker")
	if ref == null:
		return
	ref.value_changed.emit("replacement.tga")
	assert_eq(edits.size(), 1, "one resource pick emits one edit")
	assert_eq(String(edits[0].get("value", "")), "replacement.tga")


func test_variable_table_groups_and_header() -> void:
	var sheet := MnsStyleSheet.new()
	sheet.load_from_bytes(FileAccess.get_file_as_bytes(REAL_FIXTURE))
	var scroll := ScrollContainer.new()
	add_child_autofree(scroll)
	var table = MnsVariableTableScript.new()
	scroll.add_child(table)
	table.set_stylesheet(sheet)
	var text := _collect_text(scroll)
	assert_string_contains(text, "File header", "the spec header collapses into a disclosure")
	assert_string_contains(text, "DEF_FONTNAME", "rows render the variables")
	table.select_name("TRIM_COLOR")
	assert_eq(table.get_selected_name(), "TRIM_COLOR", "selection by name")
