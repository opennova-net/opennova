extends GutTest

# The Environment inspector's A7 surface: sky-map texture rows with previews
# and the sky models as object_model link widgets, all committing one undo
# step per edit through the real environment editor document.

const EnvironmentEditorScript = preload("res://modtools/environment/environment_editor.gd")
const EnvironmentInspectorScript = preload("res://modtools/environment/environment_inspector.gd")


func _make() -> Dictionary:
	var editor = add_child_autofree(EnvironmentEditorScript.new())
	editor.create_default_environment(false)
	var inspector = add_child_autofree(EnvironmentInspectorScript.new())
	inspector.set_environment_editor(editor)
	return {"editor": editor, "inspector": inspector}


func test_sky_map_widget_reads_env_value_and_preview() -> void:
	var ctx := _make()
	var widget := ctx.inspector.find_child("EnvSkyMapCloudmap1", true, false) as TextureRefWidget
	assert_not_null(widget, "the cloud map 1 row is built")
	assert_eq(widget.get_value(), ctx.editor.env_file.get_sky_map1(), "the row reads the env value")
	assert_eq(widget.preview.get_texture(), ctx.editor.env_file.get_sky_map1_tex(),
		"the preview shows exactly the texture the sky renders (push feed)")


func test_editing_sky_map_commits_one_undo_step() -> void:
	var ctx := _make()
	var widget := ctx.inspector.find_child("EnvSkyMapCloudmap1", true, false) as TextureRefWidget
	var original: String = ctx.editor.env_file.get_sky_map1()
	widget.ref_row.name_edit.text = "cloud02.pcx"
	widget.ref_row.name_edit.text_submitted.emit("cloud02.pcx")
	assert_eq(ctx.editor.env_file.get_sky_map1(), "cloud02.pcx", "the edit reaches the env file")
	assert_true(ctx.editor.can_undo(), "the edit is one undo step")
	ctx.editor.undo()
	assert_eq(ctx.editor.env_file.get_sky_map1(), original, "undo restores the prior map")


func test_sun_model_commit_routes_to_set_sun_3di() -> void:
	var ctx := _make()
	var widget := ctx.inspector.find_child("EnvModelSun", true, false) as ResourceRefWidget
	assert_not_null(widget, "the sun model row is built")
	assert_eq(widget.get_value(), ctx.editor.env_file.get_sun_3di(), "the row reads the env value")
	widget.name_edit.text = "othersun.3di"
	widget.name_edit.text_submitted.emit("othersun.3di")
	assert_eq(ctx.editor.env_file.get_sun_3di(), "othersun.3di", "the commit routes to set_sun_3di")
	assert_true(ctx.editor.can_undo(), "the edit is one undo step")


func test_sync_does_not_echo_commits() -> void:
	var ctx := _make()
	var widget := ctx.inspector.find_child("EnvModelSun", true, false) as ResourceRefWidget
	var history_before: bool = ctx.editor.can_undo()
	ctx.inspector.sync_from_editor()
	assert_eq(ctx.editor.can_undo(), history_before, "a programmatic sync must not push undo steps")
