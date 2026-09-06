@tool
extends EditorPlugin
## Native .3di models in the Godot editor (ADR 0046): a model opens as an
## editable scene under the authoring tree with its typed manifest beside it,
## and an authoring scene exports back through the engine's parity writer.
## The scene is the source; the .3di and its textures are built artifacts.

const AUTHORING_DIR := ModelExport.AUTHORING_DIR
const MENU_OPEN := "OpenNova: Open model..."
const MENU_EXPORT := "OpenNova: Export model to .3di"
const MENU_EXPORT_ALL := "OpenNova: Export all authored models"

var _open_dialog: FileDialog
var _confirm: ConfirmationDialog
var _report: AcceptDialog
var _toolbar: HBoxContainer
var _status: Label
var _pending_model := ""


func _get_plugin_name() -> String:
	return "OpenNova Model"


func _enter_tree() -> void:
	_toolbar = HBoxContainer.new()
	_toolbar.add_theme_constant_override("separation", 4)
	var export_button := Button.new()
	export_button.text = "Export .3di"
	export_button.tooltip_text = "Export this authoring scene through its manifest into the game data."
	export_button.pressed.connect(_export_current)
	_toolbar.add_child(export_button)
	var verify_button := Button.new()
	verify_button.text = "Verify"
	verify_button.tooltip_text = "Re-export in memory and compare with the artifacts on disk."
	verify_button.pressed.connect(_verify_current)
	_toolbar.add_child(verify_button)
	_status = Label.new()
	_status.custom_minimum_size = Vector2(150, 0)
	_status.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_status.clip_text = true
	_toolbar.add_child(_status)
	add_control_to_container(CONTAINER_SPATIAL_EDITOR_MENU, _toolbar)
	_open_dialog = FileDialog.new()
	_open_dialog.title = "Open native model"
	_open_dialog.file_mode = FileDialog.FILE_MODE_OPEN_FILE
	_open_dialog.access = FileDialog.ACCESS_FILESYSTEM
	_open_dialog.filters = PackedStringArray(["*.3di ; NovaLogic models"])
	_open_dialog.file_selected.connect(_model_selected)
	_toolbar.add_child(_open_dialog)
	_confirm = ConfirmationDialog.new()
	_confirm.title = "Create an authoring scene"
	_confirm.ok_button_text = "Create"
	_confirm.confirmed.connect(_import_pending)
	_toolbar.add_child(_confirm)
	_report = AcceptDialog.new()
	_report.title = "OpenNova model"
	_toolbar.add_child(_report)
	add_tool_menu_item(MENU_OPEN, _open_model)
	add_tool_menu_item(MENU_EXPORT, _export_current)
	add_tool_menu_item(MENU_EXPORT_ALL, _export_all)
	scene_changed.connect(_scene_changed)
	_scene_changed(EditorInterface.get_edited_scene_root())


func _exit_tree() -> void:
	remove_tool_menu_item(MENU_OPEN)
	remove_tool_menu_item(MENU_EXPORT)
	remove_tool_menu_item(MENU_EXPORT_ALL)
	remove_control_from_container(CONTAINER_SPATIAL_EDITOR_MENU, _toolbar)
	_toolbar.queue_free()


## The manifest beside the edited scene (`<stem>.tres`), or "".
func _current_manifest() -> String:
	var root := EditorInterface.get_edited_scene_root()
	if root == null or root.scene_file_path.is_empty():
		return ""
	var manifest := root.scene_file_path.get_basename() + ".tres"
	return manifest if ResourceLoader.exists(manifest, "ModelAuthoringManifest") else ""


func _scene_changed(_root: Node) -> void:
	var manifest := _current_manifest()
	_toolbar.visible = not manifest.is_empty()
	_status.text = manifest.get_file() if not manifest.is_empty() else ""
	_status.tooltip_text = manifest


func _open_model() -> void:
	_open_dialog.popup_centered_ratio(0.65)


func _model_selected(path: String) -> void:
	_pending_model = path
	var name := path.get_file().get_basename().to_lower()
	_confirm.dialog_text = ("Create %s/%s/ with the projected scene, its manifest and PNG copies of the " +
			"textures, then open it.\n\nOnly our own models become sources: a projection of retail bytes " +
			"may be inspected here but never saved into the authoring tree or exported (assets/README.md).") % [
			AUTHORING_DIR, name]
	_confirm.popup_centered(Vector2i(640, 200))


func _import_pending() -> void:
	var path := _pending_model
	_pending_model = ""
	if path.is_empty():
		return
	var name := path.get_file().get_basename().to_lower()
	var folder := AUTHORING_DIR.path_join(name)
	if DirAccess.dir_exists_absolute(ProjectSettings.globalize_path(folder)):
		_show("%s already exists; remove it or pick another model." % folder)
		return
	var document := ModelDocument.new()
	if document.load_from_path(path) != OK:
		_show(document.get_last_error())
		return
	var manifest := ModelAuthoringManifest.new()
	var projector := ModelSceneProjector.new()
	var root := projector.project(document, manifest, path.get_base_dir())
	if root == null:
		_show("This model cannot become a scene yet:\n- " + "\n- ".join(projector.get_refusals()))
		return
	var error := _write_authoring_tree(folder, name, root, manifest, path.get_base_dir())
	root.free()
	if not error.is_empty():
		_show(error)
		return
	EditorInterface.get_resource_filesystem().scan()
	await get_tree().process_frame
	EditorInterface.open_scene_from_path(folder.path_join(name + ".tscn"))
	await get_tree().process_frame
	EditorInterface.set_main_screen_editor("3D")


## Writes the scene, the PNG texture copies, and the manifest that maps them
## to the artifacts the model names.
func _write_authoring_tree(folder: String, name: String, root: Node3D, manifest: ModelAuthoringManifest,
		model_dir: String) -> String:
	var absolute := ProjectSettings.globalize_path(folder)
	if DirAccess.make_dir_recursive_absolute(absolute) != OK:
		return "Cannot create %s." % folder
	var scene_path := folder.path_join(name + ".tscn")
	var packed := PackedScene.new()
	if packed.pack(root) != OK:
		return "Cannot pack the projected scene."
	var err := ResourceSaver.save(packed, scene_path)
	if err != OK:
		return "Cannot save %s (%s)." % [scene_path, error_string(err)]
	var sources: Array[ModelTextureSource] = []
	var seen := PackedStringArray()
	for material in manifest.materials:
		for row in (material as ModelMaterialSpec).textures:
			var texture_name: String = (row as ModelTextureRow).texture_name
			if texture_name.is_empty() or seen.has(texture_name.to_lower()):
				continue
			seen.append(texture_name.to_lower())
			var source := _copy_texture_source(model_dir, texture_name, folder)
			if source != null:
				sources.append(source)
	manifest.texture_sources = sources
	manifest.output_directory = "res://../assets"
	manifest.scene = load(scene_path)
	err = ResourceSaver.save(manifest, folder.path_join(name + ".tres"))
	if err != OK:
		return "Cannot save the manifest (%s)." % error_string(err)
	return ""


## Decodes the texture the model names (beside the .3di) and saves it as a
## PNG source; the row remembers whether the original carried alpha.
func _copy_texture_source(model_dir: String, texture_name: String, folder: String) -> ModelTextureSource:
	var found := Paths.resolve_file(model_dir, texture_name)
	if found.is_empty():
		found = Paths.resolve_file(model_dir, texture_name.get_basename() + ".dds")
	if found.is_empty():
		return null
	var bytes := FileAccess.get_file_as_bytes(found)
	var image := Image.new()
	var extension := found.get_extension().to_lower()
	var err := ERR_FILE_UNRECOGNIZED
	if extension == "tga":
		err = image.load_tga_from_buffer(bytes)
	elif extension == "png":
		err = image.load_png_from_buffer(bytes)
	if err != OK or image.is_empty():
		return null
	var stem := texture_name.get_basename()
	var png_path := folder.path_join(stem + ".png")
	if image.save_png(ProjectSettings.globalize_path(png_path)) != OK:
		return null
	var source := ModelTextureSource.new()
	source.source_path = png_path
	source.output_name = stem + ".tga"
	source.with_alpha = extension == "tga" and bytes.size() > 16 and bytes[16] == 32
	return source


func _export_current() -> void:
	var manifest := _current_manifest()
	if manifest.is_empty():
		_show("The edited scene has no manifest beside it (<scene stem>.tres under %s)." % AUTHORING_DIR)
		return
	EditorInterface.save_scene()
	var result := ModelExport.export_manifest(manifest)
	_status.text = ("Exported " + ", ".join(result.artifacts)) if result.ok else result.error
	_status.tooltip_text = _status.text
	if not result.ok:
		_show(result.error)


func _verify_current() -> void:
	var manifest := _current_manifest()
	if manifest.is_empty():
		return
	var result := ModelExport.verify_manifest(manifest)
	_status.text = "Artifacts match" if result.ok else result.error
	_status.tooltip_text = _status.text


func _export_all() -> void:
	var lines := PackedStringArray()
	for manifest in ModelExport.list_manifests():
		var result := ModelExport.export_manifest(manifest)
		lines.append("%s: %s" % [manifest.get_file(), ("exported " + ", ".join(result.artifacts)) if result.ok else result.error])
	if lines.is_empty():
		lines.append("No manifests under %s." % AUTHORING_DIR)
	_show("\n".join(lines))


func _show(text: String) -> void:
	_report.dialog_text = text
	_report.popup_centered(Vector2i(680, 240))
