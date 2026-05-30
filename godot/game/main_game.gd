extends Node3D

# Runtime shell: drive a NovaWorld (the shared load-from-resource-dir core that a
# future editor "Play" mode also uses) and own the runtime-only bits — the
# first-launch directory picker and feeding the camera position to the world.
# The engine ships no game data; everything loads from the chosen resource dir.

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")

@onready var _world: NovaWorld = $World
@onready var _camera: Camera3D = $Camera3D

var _picker: FileDialog


func _ready() -> void:
	if _world == null or _camera == null:
		return
	if _world.load_world() != OK:
		_request_resource_dir()


# The resource directory is required (no fallback). Prompt for it, unless headless
# (CI/probes set it explicitly and never block on a dialog).
func _request_resource_dir() -> void:
	if DisplayServer.get_name() == "headless" or _picker != null:
		return
	_picker = FileDialog.new()
	_picker.file_mode = FileDialog.FILE_MODE_OPEN_DIR
	_picker.access = FileDialog.ACCESS_FILESYSTEM
	_picker.use_native_dialog = true
	_picker.title = "Select your OpenNova asset directory (contains %s)" % _world.terrain_file
	_picker.dir_selected.connect(_on_dir_selected)
	_picker.canceled.connect(_on_dir_canceled)
	add_child(_picker)
	_picker.popup_centered_ratio(0.6)


func _on_dir_selected(dir: String) -> void:
	_cleanup_picker()
	if _world.load_world(dir) == OK:
		ResourceDirSettings.set_resource_dir(dir)
	else:
		_request_resource_dir()


func _on_dir_canceled() -> void:
	_cleanup_picker()
	_request_resource_dir()


func _cleanup_picker() -> void:
	if _picker != null:
		_picker.queue_free()
		_picker = null


func _process(_delta: float) -> void:
	_world.tick(_camera.global_position)
