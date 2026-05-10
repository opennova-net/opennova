extends Control

signal credits_done

@onready var _player: NovaCreditsPlayer = %CreditsPlayer


func _ready() -> void:
	_player.finished.connect(_on_finished)


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
	credits_done.emit()
