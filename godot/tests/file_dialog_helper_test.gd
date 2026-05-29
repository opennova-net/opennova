extends GutTest

const FileDialogHelperScript = preload("res://modtools/framework/file_dialog_helper.gd")


func test_open_creates_one_native_open_file_dialog_under_host() -> void:
	var host := Control.new()
	add_child_autofree(host)
	var helper = FileDialogHelperScript.new(host)
	helper.open("Pick a file", PackedStringArray(["*.til ; Tiles"]), func(_p): pass)
	var dialog := helper.get_dialog()
	assert_not_null(dialog, "open() should create a FileDialog.")
	assert_eq(dialog.get_parent(), host, "Dialog should be parented to the host.")
	assert_true(dialog is FileDialog, "Helper should build a FileDialog.")
	assert_eq(dialog.access, FileDialog.ACCESS_FILESYSTEM, "Should browse the whole filesystem.")
	assert_eq(dialog.file_mode, FileDialog.FILE_MODE_OPEN_FILE, "Should be an open-file dialog.")
	assert_true(dialog.use_native_dialog, "Should prefer the native dialog.")
	assert_eq(dialog.title, "Pick a file", "Title should be applied.")
	assert_eq(dialog.filters, PackedStringArray(["*.til ; Tiles"]), "Filters should be applied.")
	dialog.hide()


func test_open_reuses_the_same_dialog_instance() -> void:
	var host := Control.new()
	add_child_autofree(host)
	var helper = FileDialogHelperScript.new(host)
	helper.open("First", PackedStringArray(), func(_p): pass)
	var first := helper.get_dialog()
	helper.open("Second", PackedStringArray(), func(_p): pass)
	assert_eq(helper.get_dialog(), first, "open() should reuse the cached dialog, not build a second.")
	assert_eq(host.get_child_count(), 1, "Only one dialog should ever be parented to the host.")
	assert_eq(first.title, "Second", "The reused dialog should pick up the new title.")
	first.hide()


func test_open_rebinds_a_single_one_shot_callback() -> void:
	var host := Control.new()
	add_child_autofree(host)
	var helper = FileDialogHelperScript.new(host)
	var cb1 := func(_p): pass
	var cb2 := func(_p): pass
	helper.open("t", PackedStringArray(), cb1)
	helper.open("t", PackedStringArray(), cb2)
	var conns := helper.get_dialog().file_selected.get_connections()
	assert_eq(conns.size(), 1, "Only the latest callback should remain connected after a rebind.")
	assert_eq(conns[0]["callable"], cb2, "The remaining connection should be the newest callback.")
	helper.get_dialog().hide()


func test_open_applies_current_dir_only_when_provided() -> void:
	var host := Control.new()
	add_child_autofree(host)
	# No dir provided: the dialog keeps FileDialog's default location.
	var default_helper = FileDialogHelperScript.new(host)
	default_helper.open("t", PackedStringArray(), func(_p): pass)
	var default_dir := default_helper.get_dialog().current_dir
	default_helper.get_dialog().hide()
	# Dir provided: the dialog navigates there (FileDialog globalizes the path).
	var target := ProjectSettings.globalize_path("res://tests")
	var helper = FileDialogHelperScript.new(host)
	helper.open("t", PackedStringArray(), func(_p): pass, target)
	var applied := helper.get_dialog().current_dir
	assert_true(applied.ends_with("tests"), "Provided current_dir should navigate into that directory, got %s" % applied)
	assert_ne(applied, default_dir, "Providing current_dir should change the dialog's directory.")
	helper.get_dialog().hide()
