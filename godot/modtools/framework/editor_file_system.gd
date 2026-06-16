class_name EditorFileSystem
extends Node

## Handles Virtual File System (VFS) operations for the Editor, decoupled from the UI.

static var _instance: EditorFileSystem

static func get_instance() -> EditorFileSystem:
	if _instance == null:
		_instance = EditorFileSystem.new()
	return _instance

var editor_shell: Node

func set_editor_shell(shell: Node) -> void:
	editor_shell = shell

func get_resource_root() -> NovaResourceRoot:
	if editor_shell != null and editor_shell.has_method("get_resource_root"):
		return editor_shell.get_resource_root()
	return null

func get_resource_root_or_settings() -> NovaResourceRoot:
	var root := get_resource_root()
	if root != null:
		return root
	var dir := NovaResourceDirSettings.get_resource_dir()
	if dir.is_empty():
		return null
	var resources := NovaResourceRoot.new()
	return resources if resources.set_root_dir(dir) == OK else null

func get_vfs_root_for_open(path: String) -> NovaResourceRoot:
	var resources := get_resource_root()
	if not FileAccess.file_exists(path) and resources != null and resources.has_file(path):
		return resources
	return null

func get_vfs_display_path(root: NovaResourceRoot, path: String) -> String:
	return root.get_root_dir().path_join(path.get_file())
