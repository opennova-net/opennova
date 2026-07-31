extends GutTest

const CreditsWorkspaceScript = preload("res://modtools/credits/credits_workspace.gd")
const ResourceDirSettings = preload("res://engine/resource_index/resource_dir_settings.gd")
const KDA_SOURCE_PATH := "res://../fixtures/cbin/nlist.reference.kda"
const TEMP_DIR := "user://test_credits_ws"
const MINIMAL_SOURCE := "[ENV]\nscroll_rate=1.50\nvertical_space=21\ncenter_x=420\n\n[TEXT]\nSaved source edit\n"

var _kda_path: String = ""
var _saved_resource_dir := ""


func before_all() -> void:
	_saved_resource_dir = ResourceDirSettings.get_resource_dir()
	ResourceDirSettings.set_resource_dir("")


func after_all() -> void:
	ResourceDirSettings.set_resource_dir(_saved_resource_dir)


func before_each() -> void:
	# Copy the fixture to a unique user:// path so each test gets its own
	# CACHE_MODE_REPLACE slot and we don't hit the "another resource is loaded
	# at path" error when multiple tests load the same res:// path.
	var abs := ProjectSettings.globalize_path(TEMP_DIR)
	DirAccess.make_dir_recursive_absolute(abs)
	_kda_path = TEMP_DIR.path_join("nlist_%d.kda" % Time.get_ticks_usec())
	var src := FileAccess.open(
		ProjectSettings.globalize_path(KDA_SOURCE_PATH), FileAccess.READ)
	if src != null:
		var dst := FileAccess.open(
			ProjectSettings.globalize_path(_kda_path), FileAccess.WRITE)
		if dst != null:
			dst.store_buffer(src.get_buffer(src.get_length()))
			dst.close()
		src.close()


func after_each() -> void:
	_cleanup_dir(TEMP_DIR)
	_kda_path = ""


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


func test_workspace_identity() -> void:
	var ws = autofree(CreditsWorkspaceScript.new())

	assert_eq(ws.get_workspace_id(), "credits",
		"get_workspace_id should return 'credits'.")
	assert_eq(ws.get_workspace_label(), "Credits",
		"get_workspace_label should return 'Credits'.")
	assert_eq(ws.get_status_tool(), "Credits",
		"get_status_tool should return 'Credits'.")


func test_fresh_workspace_shows_untitled() -> void:
	var ws = autofree(CreditsWorkspaceScript.new())

	assert_eq(ws.get_project_title(), "untitled",
		"Fresh workspace title should be 'untitled' with no dirty marker.")


func test_dirty_marks_title_with_star() -> void:
	var ws = autofree(CreditsWorkspaceScript.new())

	var err = ws.open_file(_kda_path)
	assert_eq(err, OK, "Opening fixture should succeed.")
	if err != OK:
		return

	assert_false(ws.get_project_title().ends_with("*"),
		"Title should not end with '*' when document is clean after open.")

	# Access the document's resource directly (GDScript does not enforce privacy).
	var entry := CbinTextEntry.new()
	entry.set_text("dirty marker")
	ws._document.resource.add_entry(entry)

	assert_true(ws.has_unsaved_changes(),
		"has_unsaved_changes should be true after mutating the resource.")
	assert_true(ws.get_project_title().ends_with("*"),
		"get_project_title should end with '*' when document is dirty.")


func test_status_context_shows_entry_count() -> void:
	var ws = autofree(CreditsWorkspaceScript.new())

	var err = ws.open_file(_kda_path)
	assert_eq(err, OK, "Opening fixture should succeed.")
	if err != OK:
		return

	var ctx: String = ws.get_status_context()
	assert_false(ctx.is_empty(), "Status context should not be empty after loading a file.")
	assert_true(ctx.ends_with("entries"),
		"Status context should end with 'entries', got: '%s'" % ctx)

	var parts := ctx.split(" ")
	assert_eq(parts.size(), 2, "Status context should have format '<N> entries'.")
	if parts.size() >= 1:
		assert_true(parts[0].is_valid_int(),
			"First token of status context should be an integer.")
		assert_true(parts[0].to_int() > 0,
			"Entry count in status context should be > 0 for nlist.kda.")


func test_dialog_filter_contains_kda() -> void:
	var ws = autofree(CreditsWorkspaceScript.new())

	var filters: PackedStringArray = ws.get_open_dialog_filters()
	assert_true(filters.size() > 0, "get_open_dialog_filters should return at least one entry.")
	var found := false
	for f in filters:
		if (f as String).contains("*.kda"):
			found = true
			break
	assert_true(found, "Open dialog filters should contain '*.kda'.")


func test_can_new_returns_true() -> void:
	var ws = autofree(CreditsWorkspaceScript.new())
	assert_true(ws.can_new(), "can_new() should always return true for the credits workspace.")


func test_can_save_false_on_fresh_true_after_dirty_with_path() -> void:
	var ws = autofree(CreditsWorkspaceScript.new())

	# Fresh: not dirty, no path -> can_save == false.
	assert_false(ws.can_save(),
		"can_save should be false on a fresh workspace (not dirty, no path).")

	# After open: has path but not dirty -> can_save == false.
	var err = ws.open_file(_kda_path)
	assert_eq(err, OK, "open_file should succeed.")
	if err != OK:
		return
	assert_false(ws.can_save(),
		"can_save should be false immediately after a clean open (not dirty).")

	# Mutate to make it dirty: has path AND is dirty -> can_save == true.
	var entry := CbinTextEntry.new()
	entry.set_text("dirty for can_save")
	ws._document.resource.add_entry(entry)

	assert_true(ws.can_save(),
		"can_save should be true when document is dirty and current_path is set.")


func test_save_current_flushes_pending_source_edits() -> void:
	var ws = autofree(CreditsWorkspaceScript.new())
	var mount := Control.new()
	add_child_autofree(mount)
	ws.mount_viewport(mount)
	await get_tree().process_frame

	var err = ws.open_file(_kda_path)
	assert_eq(err, OK, "open_file should succeed.")
	if err != OK:
		return
	await get_tree().process_frame

	var editor: Control = ws._editor
	var source_button: Button = editor.get_node("%SourceButton")
	source_button.button_pressed = true
	await get_tree().process_frame

	var code_edit: CodeEdit = editor.get_node("HSplit/LeftPane/ContentStack/SourceViewMount/CodeEdit")
	code_edit.text = MINIMAL_SOURCE

	err = ws.save_current()
	assert_eq(err, OK, "save_current should apply pending source text before saving.")
	if err != OK:
		return

	var reloaded := ResourceLoader.load(_kda_path, "CbinCreditsResource",
		ResourceLoader.CACHE_MODE_REPLACE) as CbinCreditsResource
	assert_not_null(reloaded, "saved KDA should reload.")
	if reloaded == null:
		return
	assert_eq(reloaded.get_entry_count(), 1, "pending source text should be saved.")
	assert_eq((reloaded.get_entry(0) as CbinTextEntry).get_text(), "Saved source edit",
		"saved file should contain pending source text.")
	assert_eq(reloaded.get_vertical_space(), 21, "saved file should contain pending ENV values.")


func test_visual_save_ignores_hidden_stale_source_text_and_preserves_entries() -> void:
	var ws = autofree(CreditsWorkspaceScript.new())
	var mount := Control.new()
	add_child_autofree(mount)
	ws.mount_viewport(mount)
	await get_tree().process_frame

	var err = ws.open_file(_kda_path)
	assert_eq(err, OK, "open_file should succeed.")
	if err != OK:
		return
	await get_tree().process_frame

	var original_count: int = ws._document.resource.get_entry_count()
	assert_gt(original_count, 10, "fixture should have enough entries to prove save preservation.")
	var deep_index := -1
	var deep_entry: CbinTextEntry = null
	for i in range(1, original_count):
		var candidate := ws._document.resource.get_entry(i) as CbinTextEntry
		if candidate != null:
			deep_index = i
			deep_entry = candidate
			break
	assert_not_null(deep_entry, "fixture should contain an unedited text entry for preservation check.")
	if deep_entry == null:
		return
	var deep_text := deep_entry.get_text()

	var editor: Control = ws._editor
	var visual_button: Button = editor.get_node("%VisualButton")
	visual_button.button_pressed = true
	await get_tree().process_frame

	var code_edit: CodeEdit = editor.get_node("HSplit/LeftPane/ContentStack/SourceViewMount/CodeEdit")
	code_edit.text = MINIMAL_SOURCE

	var first_entry := ws._document.resource.get_entry(0) as CbinTextEntry
	assert_not_null(first_entry, "fixture first entry should be text for visual edit.")
	if first_entry == null:
		return
	first_entry.set_text("Visual save sentinel")

	err = ws.save_current()
	assert_eq(err, OK, "visual-mode save should succeed without applying hidden source text.")
	if err != OK:
		return

	var reloaded := ResourceLoader.load(_kda_path, "CbinCreditsResource",
		ResourceLoader.CACHE_MODE_REPLACE) as CbinCreditsResource
	assert_not_null(reloaded, "saved KDA should reload.")
	if reloaded == null:
		return
	assert_eq(reloaded.get_entry_count(), original_count,
		"visual-mode save should preserve unedited entries from the original KDA.")
	if reloaded.get_entry_count() != original_count:
		return
	assert_eq((reloaded.get_entry(0) as CbinTextEntry).get_text(), "Visual save sentinel",
		"visual edit should be saved.")
	assert_eq((reloaded.get_entry(deep_index) as CbinTextEntry).get_text(), deep_text,
		"unedited deep entry should survive visual save.")
