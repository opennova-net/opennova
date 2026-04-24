class_name EditorDocument
extends Node

# Shared document state for editor-side authoring controllers. No direct IDA
# equivalent; this is editor-only glue around ported data/model classes.

signal state_changed

var current_path: String = ""
var is_dirty: bool = false

var _last_open_dir: String = ""
var _last_save_dir: String = ""
var _last_export_dir: String = ""


func set_current_path(path: String) -> void:
	current_path = path


func mark_dirty() -> void:
	is_dirty = true


func mark_clean() -> void:
	is_dirty = false


func remember_open_path(path: String) -> void:
	_last_open_dir = path.get_base_dir()


func remember_save_dir(dir_path: String) -> void:
	_last_save_dir = dir_path


func remember_export_dir(dir_path: String) -> void:
	_last_export_dir = dir_path


func get_last_open_dir() -> String:
	return _last_open_dir


func get_last_save_dir() -> String:
	return _last_save_dir


func get_last_export_dir() -> String:
	return _last_export_dir if not _last_export_dir.is_empty() else _last_save_dir
