extends GutTest

const CreditsWorkspaceScript = preload("res://modtools/editor/credits_workspace.gd")
const KDA_SOURCE_PATH := "res://assets/credits/nlist.kda"
const TEMP_DIR := "user://test_credits_ws"

var _kda_path: String = ""


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
