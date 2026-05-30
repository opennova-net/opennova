extends Control

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")

signal credits_done

@onready var _player: NovaCreditsPlayer = %CreditsPlayer

@export var credits_file: String = "nlist.kda"

var _done := false


func _ready() -> void:
	_player.finished.connect(_on_finished)
	if _player.get_credits_resource() == null:
		_load_credits_from_resource_root()


func _load_credits_from_resource_root() -> void:
	var root := ResourceDirSettings.get_resource_dir()
	if root.is_empty() or credits_file.strip_edges().is_empty():
		return
	var resources := NovaResourceRoot.new()
	if resources.set_root_dir(root) != OK:
		return
	var path := resources.resolve_file(credits_file.strip_edges())
	if path.is_empty():
		return
	var resource := ResourceLoader.load(path, "CbinCreditsResource", ResourceLoader.CACHE_MODE_REPLACE) as CbinCreditsResource
	if resource == null:
		return
	_player.set_credits_resource(resource)
	if _player.get_autoplay():
		_player.play()


func _input(event: InputEvent) -> void:
	if event.is_action_pressed("ui_cancel") or event.is_action_pressed("ui_accept"):
		get_viewport().set_input_as_handled()
		_skip()
	elif event is InputEventMouseButton and event.pressed:
		get_viewport().set_input_as_handled()
		_skip()


func _skip() -> void:
	_player.stop()
	_on_finished()


func _on_finished() -> void:
	if _done:
		return
	_done = true
	credits_done.emit()
