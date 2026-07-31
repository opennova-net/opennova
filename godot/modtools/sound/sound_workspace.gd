class_name SoundEditorWorkspace
extends EditorWorkspace

## ONED workspace for authoring NovaLogic .lwf sound profiles.
##
## A non-3D data editor (no viewport camera) mirroring the Strings workspace:
## the center viewport mount carries a self-contained SoundEditorView (a Tree of
## Sound Sets -> Layers -> Members with an add/remove/reorder toolbar and per-row
## Play buttons); the left inspector carries the per-selection property form. This
## adapter owns the SoundController document and fans its change channels out:
## `structure_changed` -> Tree rebuild, `selection_changed` -> inspector repopulate,
## `edited` -> title refresh.

const SoundControllerScript = preload("res://modtools/sound/sound_controller.gd")
const SoundEditorViewScript = preload("res://modtools/sound/ui/sound_editor_view.gd")
const SoundInspectorScript = preload("res://modtools/sound/sound_inspector.gd")
const SoundPreviewPlayerScript = preload("res://modtools/sound/sound_preview_player.gd")

var controller: SoundController

var _mount: ViewportMount
var _view: Control
var _inspector: Control
var _preview: SoundPreviewPlayer


func set_editor_shell(value: Node) -> void:
	super.set_editor_shell(value)
	_ensure_editor()


# --- Identity ---

func get_workspace_id() -> String:
	return "sound"


func get_workspace_label() -> String:
	return "Sound"


func get_workspace_tooltip() -> String:
	return "Author .lwf sound profiles (sound sets, layers, members)."


func get_project_title() -> String:
	return controller.get_project_title() if controller else "Sound"


func get_status_tool() -> String:
	return "Sound"


func get_status_context() -> String:
	return controller.get_status_context() if controller else "No sound profile open."


# --- Lifecycle ---

func activate() -> void:
	_ensure_editor()


func deactivate() -> void:
	# Stop any audition so a looping preview does not bleed across workspaces.
	if _preview != null:
		_preview.stop_preview()


func _ensure_editor() -> void:
	if controller != null:
		return
	controller = SoundControllerScript.new()
	controller.name = "SoundController"
	_mount_under_shell(controller)
	controller.new_profile(false)
	controller.structure_changed.connect(_on_structure_changed)
	controller.selection_changed.connect(_on_selection_changed)
	controller.edited.connect(_on_edited)


func _ensure_preview() -> SoundPreviewPlayer:
	if _preview == null:
		_preview = SoundPreviewPlayerScript.new()
		_preview.name = "SoundPreviewPlayer"
		_mount_under_shell(_preview)
	return _preview


# --- Center: self-contained Tree view ---

func _ensure_mount() -> ViewportMount:
	if _mount == null:
		_mount = ViewportMount.new(&"SoundEditorView", _create_view)
	return _mount


func _create_view() -> Control:
	_view = SoundEditorViewScript.new()
	return _view


func mount_viewport(mount: Control) -> void:
	if mount == null:
		return
	_ensure_editor()
	_ensure_mount().mount(mount)
	if _view != null:
		_view.set_workspace(self)


func unmount_viewport(_released: Control) -> void:
	if _mount != null:
		_mount.unmount()


func release_viewport() -> void:
	if _mount != null:
		_mount.release()
	_view = null
	if _preview != null:
		_preview.stop_preview()


# --- Left: inspector ---

func build_inspector(mount: Control) -> void:
	_ensure_editor()
	_inspector = SoundInspectorScript.new()
	mount.add_child(_inspector)
	_inspector.setup(self)


# --- Coordinator API shared with the views ---

func get_document() -> SoundController:
	_ensure_editor()
	return controller


func preview_member(si: int, li: int, mi: int) -> void:
	_ensure_preview().preview_member(controller.data, _resource_root(), si, li, mi)


func preview_set(si: int) -> void:
	_ensure_preview().preview_set(controller.data, _resource_root(), si)


func stop_preview() -> void:
	if _preview != null:
		_preview.stop_preview()


func _on_structure_changed() -> void:
	if _view != null:
		_view.rebuild()
	if _inspector != null:
		_inspector.refresh()
	_sync_shell()


func _on_selection_changed() -> void:
	if _view != null:
		_view.sync_selection()
	if _inspector != null:
		_inspector.refresh()


func _on_edited() -> void:
	_sync_shell()


# --- Document actions ---

# The domain document the EditorWorkspace base derives undo/redo + dirty from.
func get_editor_document() -> Object:
	return controller


func can_new() -> bool:
	return true


func get_new_action_label() -> String:
	return "New Sound Profile"


func new_current() -> Error:
	_ensure_editor()
	controller.new_profile(true)
	return OK


func can_open() -> bool:
	return true


func get_open_action_label() -> String:
	return "Open Sound Profile..."


func get_open_dialog_title() -> String:
	return "Open .lwf sound profile"


func get_open_dialog_filters() -> PackedStringArray:
	return PackedStringArray(["*.lwf,*.LWF ; Sound profiles"])


func get_open_dialog_dir() -> String:
	return controller.get_last_open_dir() if controller else ""


func get_open_resource_kind() -> String:
	return "sound"


func get_current_resource_path() -> String:
	return controller.current_path if controller else ""


func open_file(path: String) -> Error:
	_ensure_editor()
	var vfs := _vfs_root_for_open(path)
	if vfs != null:
		return controller.open_lwf_bytes(vfs.read_file(path), _vfs_display_path(vfs, path))
	return controller.open_lwf(path)


func get_resource_root() -> NovaResourceRoot:
	return _resource_root()


func can_save() -> bool:
	return controller != null and controller.is_dirty and not controller.current_path.is_empty()


func get_save_action_label() -> String:
	return "Save Sound Profile"


func can_save_as() -> bool:
	return controller != null


func get_save_as_action_label() -> String:
	return "Save Sound Profile As..."


func save_current() -> Error:
	return controller.save_current() if controller else ERR_UNAVAILABLE


func save_as(dir_path: String) -> Error:
	return controller.save_as(dir_path) if controller else ERR_UNAVAILABLE


func get_save_dialog_title() -> String:
	return "Choose where to save the .lwf sound profile"


func get_save_dialog_dir() -> String:
	return controller.get_last_save_dir() if controller else ""

