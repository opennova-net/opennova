extends GutTest

const FntEditorDocument = preload("res://modtools/fonts/fnt_editor_document.gd")
const FntEditorScript = preload("res://modtools/fonts/fnt_editor.gd")
const FontsWorkspaceScript = preload("res://modtools/fonts/fonts_workspace.gd")
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


func test_engine_preview_panel_renders_the_edited_font_through_the_game_path() -> void:
	# FNT-1 gate (docs/oned/workspace-maturity-program.md): the type-a-line sample
	# is the F2 EngineTextPreview — the game's own draw path (NovaFntResource ->
	# FontFile + HudText.draw_text) — not a Godot Label truth-claim. The glyph
	# canvas stays beside it as the paint surface.
	var doc = autofree(FntEditorDocument.new())
	assert_eq(doc.open_fnt(FNT_PATH), OK, "Fixture should open before binding the editor.")

	var editor := FntEditorScript.new()
	add_child_autofree(editor)
	editor.set_document(doc)
	await get_tree().process_frame

	var preview: Control = editor.get_node_or_null("%SamplePreview")
	assert_not_null(preview, "The editor mounts the engine text sample panel.")
	if preview == null:
		return
	assert_false(preview is Label, "The sample panel is the engine draw, not a Label approximation.")
	assert_true(preview.has_font(), "The edited document's font adopts into the panel on load.")
	assert_gt(preview.get_font_file().get_fixed_size(), 0,
		"The adopted FontFile carries the .fnt's own fixed pixel size (the engine draw resolution).")

	var sample_edit: LineEdit = editor.get_node_or_null("%SampleEdit")
	assert_not_null(sample_edit, "The panel is type-a-line: a sample edit feeds it.")
	if sample_edit != null:
		sample_edit.text = "AMMO 30 / 90"
		sample_edit.text_changed.emit(sample_edit.text)
		assert_eq(preview.get_sample_text(), "AMMO 30 / 90", "Typing a line updates the engine-drawn sample.")

	assert_not_null(editor.get_node_or_null("%AtlasCanvas"),
		"The glyph canvas stays beside the sample — it is a paint surface, not a preview claim.")


func test_engine_preview_panel_is_empty_until_a_font_loads() -> void:
	var editor := FntEditorScript.new()
	add_child_autofree(editor)
	await get_tree().process_frame
	var preview: Control = editor.get_node_or_null("%SamplePreview")
	assert_not_null(preview, "The panel exists before any document binds.")
	if preview == null:
		return
	assert_false(preview.has_font(), "Without a document there is no font claim to render.")

	var doc = autofree(FntEditorDocument.new())
	assert_eq(doc.open_fnt(FNT_PATH), OK)
	editor.set_document(doc)
	assert_true(preview.has_font(), "Binding a loaded document adopts its font into the panel.")


func test_fonts_workspace_exposes_document_actions_and_inspector() -> void:
	var workspace = autofree(FontsWorkspaceScript.new())
	assert_eq(workspace.get_workspace_label(), "Fonts", "Fonts workspace should label itself for the rail.")
	assert_eq(workspace.get_open_dialog_filters(), PackedStringArray(["*.fnt,*.FNT ; Nova fonts"]), "Open dialog should target raw Nova .fnt files.")

	assert_eq(workspace.open_file(FNT_PATH), OK, "Workspace should open bundled .fnt files.")
	assert_eq(workspace.get_project_title(), "Serpen24", "Workspace title should show loaded basename.")
	assert_string_contains(workspace.get_status_context(), "224 glyphs", "Status should summarize glyph count.")

	var mount := Control.new()
	add_child_autofree(mount)
	workspace.build_inspector(mount)
	await get_tree().process_frame
	assert_gt(mount.get_child_count(), 0, "Fonts workspace should mount an inspector.")
	var label := mount.get_child(0).get_node_or_null("Box/FontLabel") as Label
	assert_not_null(label, "Inspector should include a font label.")
	if label != null:
		assert_string_contains(label.text, "Serpen24", "Inspector should show the loaded font name.")
	assert_null(mount.get_child(0).get_node_or_null("Box/UsedByStrip"),
		"Without a shell there is no reference index, so no Used-by strip mounts.")


class UsedByShell:
	extends Node

	class IndexStub:
		extends RefCounted
		var queries: Array = []
		func referrers_of(name: String) -> Array:
			queries.append(name)
			return [{"source_path": "nlist.kda", "source_kind": "credits",
				"site": "entry[0]", "target_kind": "font"}]
		func is_built() -> bool:
			return false

	var index := IndexStub.new()
	# A real root (the workspace base's _resource_root() is typed to it) over a
	# cache-dir fixture holding the referrer file the index stub reports.
	var root: NovaResourceRoot
	var opened: Array = []

	func get_reference_index() -> IndexStub:
		return index

	func get_resource_root() -> NovaResourceRoot:
		return root

	func open_in_workspace(kind: String, path: String, _focus: FocusPayload = null) -> Error:
		opened.append([kind, path])
		return OK


func test_inspector_offers_used_by_without_triggering_index_build() -> void:
	var root_dir := OS.get_cache_dir().path_join("opennova_test_fnt_used_by")
	DirAccess.make_dir_recursive_absolute(root_dir)
	var ref_file := FileAccess.open(root_dir.path_join("nlist.kda"), FileAccess.WRITE)
	ref_file.store_string("existence is what resolve_file probes")
	ref_file.close()

	var workspace = autofree(FontsWorkspaceScript.new())
	var shell: UsedByShell = add_child_autofree(UsedByShell.new())
	shell.root = NovaResourceRoot.new()
	assert_eq(shell.root.set_root_dir(root_dir), OK)
	workspace.set_editor_shell(shell)
	assert_eq(workspace.open_file(FNT_PATH), OK)

	var mount := Control.new()
	add_child_autofree(mount)
	workspace.build_inspector(mount)
	await get_tree().process_frame

	var strip = mount.get_child(0).get_node_or_null("Box/UsedByStrip")
	assert_not_null(strip, "With a shell the inspector mounts the Used-by strip.")
	if strip == null:
		return
	assert_eq(shell.index.queries, [],
		"Opening a font must never trigger the whole-root reference scan.")
	assert_true(strip.find_button.visible, "The strip offers the explicit Find-uses ask instead.")

	strip.find_button.pressed.emit()
	await get_tree().process_frame
	await get_tree().process_frame
	assert_eq(shell.index.queries, ["Serpen24.fnt", "Serpen24"],
		"The explicit ask queries both spellings fonts are referenced by (menus keep .fnt, credits are bare).")

	# A referrer row click resolves the VFS-logical source name to a real path
	# before jumping - the credits/menu workspaces open from disk only.
	var rows = strip.rows.get_children().filter(func(c): return not c.is_queued_for_deletion())
	assert_eq(rows.size(), 1, "the stub's one referrer renders one row")
	if rows.size() == 1:
		(rows[0] as Button).pressed.emit()
		assert_eq(shell.opened.size(), 1, "the row click jumps once")
		assert_eq(String(shell.opened[0][0]), "credits")
		var opened_path := String(shell.opened[0][1])
		assert_ne(opened_path, "nlist.kda",
			"the jump carries the resolved disk path, not the bare logical name")
		assert_true(FileAccess.file_exists(opened_path), "...and that path opens from disk")

	DirAccess.remove_absolute(root_dir.path_join("nlist.kda"))
	DirAccess.remove_absolute(root_dir)
