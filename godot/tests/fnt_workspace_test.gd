extends GutTest

const FntEditorDocument = preload("res://modtools/fonts/fnt_editor_document.gd")
const FntEditorScript = preload("res://modtools/fonts/fnt_editor.gd")
const FontsWorkspaceScript = preload("res://modtools/editor/fonts_workspace.gd")
const FNT_PATH := "res://../fixtures/fnt/Serpen24.fnt"
const TEMP_DIR := "user://test_fnt_workspace"


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


func test_fnt_document_open_dirty_save_round_trip() -> void:
	var doc = autofree(FntEditorDocument.new())
	var err: int = doc.open_fnt(FNT_PATH)
	assert_eq(err, OK, "FNT document should open a bundled Nova font.")
	assert_false(doc.is_dirty, "Opening should leave the document clean.")
	assert_eq(doc.current_path, FNT_PATH, "Opening should set current_path.")
	assert_eq(doc.resource.get_page_count(), 1, "Document should expose the loaded NovaFntResource.")

	doc.resource.set_pixel_alpha(0, 0, 0, 123)
	assert_true(doc.is_dirty, "Mutating the font resource should dirty the document.")

	var save_err: int = doc.save_as(TEMP_DIR)
	assert_eq(save_err, OK, "FNT document should save as raw .fnt.")
	assert_false(doc.is_dirty, "Successful save should mark clean.")
	assert_true(doc.current_path.ends_with(".fnt"), "save_as should choose a .fnt filename.")

	var reloaded = autofree(FntEditorDocument.new())
	assert_eq(reloaded.open_fnt(doc.current_path), OK, "Saved FNT should reopen.")
	assert_eq(reloaded.resource.get_pixel_alpha(0, 0, 0), 123, "Edited alpha should survive save/reload.")


func test_fnt_editor_builds_atlas_authoring_surface_and_edits_alpha() -> void:
	var doc = autofree(FntEditorDocument.new())
	assert_eq(doc.open_fnt(FNT_PATH), OK, "Fixture should open before binding editor.")

	var editor := FntEditorScript.new()
	add_child_autofree(editor)
	editor.set_document(doc)
	await get_tree().process_frame

	assert_not_null(editor.get_node_or_null("%AtlasCanvas"), "Editor should expose an atlas canvas.")
	assert_not_null(editor.get_node_or_null("%GlyphGrid"), "Editor should expose a 224-slot glyph grid.")
	assert_not_null(editor.get_node_or_null("%GlyphInspector"), "Editor should expose selected glyph metadata.")
	assert_not_null(editor.get_node_or_null("%SamplePreview"), "Editor should expose a rendered sample preview.")

	editor.select_char(32)
	var before: int = doc.resource.get_pixel_alpha(0, 1, 1)
	editor.set_active_tool("pencil")
	editor.paint_pixel(0, 1, 1, 222)
	assert_eq(doc.resource.get_pixel_alpha(0, 1, 1), 222, "Pencil tool should edit alpha pixels.")
	assert_true(editor.can_undo(), "Pixel edits should push an undo step.")
	editor.undo()
	assert_eq(doc.resource.get_pixel_alpha(0, 1, 1), before, "Undo should restore the prior alpha value.")
	editor.redo()
	assert_eq(doc.resource.get_pixel_alpha(0, 1, 1), 222, "Redo should reapply the alpha edit.")


func test_fonts_workspace_exposes_document_actions_and_inspector() -> void:
	var workspace = autofree(FontsWorkspaceScript.new())
	assert_eq(workspace.get_workspace_label(), "Fonts", "Fonts workspace should label itself for the rail.")
	assert_eq(workspace.get_open_dialog_filters(), PackedStringArray(["*.fnt,*.FNT ; Nova fonts"]), "Open dialog should target raw Nova .fnt files.")

	assert_eq(workspace.open_file(FNT_PATH), OK, "Workspace should open bundled .fnt files.")
	assert_eq(workspace.get_project_title(), "Serpen24", "Workspace title should show loaded basename.")
	assert_string_contains(workspace.get_status_context(), "224 glyphs", "Status should summarize glyph count.")

	var host := Control.new()
	add_child_autofree(host)
	workspace.build_inspector(host)
	await get_tree().process_frame
	assert_gt(host.get_child_count(), 0, "Fonts workspace should mount an inspector.")
	var label := host.get_child(0).get_node_or_null("Box/FontLabel") as Label
	assert_not_null(label, "Inspector should include a font label.")
	if label != null:
		assert_string_contains(label.text, "Serpen24", "Inspector should show the loaded font name.")
	assert_null(host.get_child(0).get_node_or_null("Box/UsedByStrip"),
		"Without a shell there is no reference index, so no Used-by strip mounts.")


class UsedByShell:
	extends Node

	class IndexStub:
		extends RefCounted
		var queries: Array = []
		func referrers_of(name: String) -> Array:
			queries.append(name)
			return [{"source_path": "nlist.kda", "source_kind": "credits", "site": "entry[0]"}]
		func is_built() -> bool:
			return false

	var index := IndexStub.new()

	func get_reference_index() -> IndexStub:
		return index

	func open_in_workspace(_kind: String, _path: String, _focus: Dictionary = {}) -> Error:
		return OK


func test_inspector_offers_used_by_without_triggering_index_build() -> void:
	var workspace = autofree(FontsWorkspaceScript.new())
	var shell: UsedByShell = add_child_autofree(UsedByShell.new())
	workspace.set_editor_shell(shell)
	assert_eq(workspace.open_file(FNT_PATH), OK)

	var host := Control.new()
	add_child_autofree(host)
	workspace.build_inspector(host)
	await get_tree().process_frame

	var strip = host.get_child(0).get_node_or_null("Box/UsedByStrip")
	assert_not_null(strip, "With a shell the inspector mounts the Used-by strip.")
	if strip == null:
		return
	assert_eq(shell.index.queries, [],
		"Opening a font must never trigger the whole-root reference scan.")
	assert_true(strip.find_button.visible, "The strip offers the explicit Find-uses ask instead.")

	strip.find_button.pressed.emit()
	await get_tree().process_frame
	assert_eq(shell.index.queries, ["Serpen24.fnt", "Serpen24"],
		"The explicit ask queries both spellings fonts are referenced by (menus keep .fnt, credits are bare).")
