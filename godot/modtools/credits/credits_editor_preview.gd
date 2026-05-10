class_name CreditsEditorPreview
extends Control

@onready var _toolbar: HBoxContainer = $Toolbar
@onready var _play_button: Button = $Toolbar/Play
@onready var _pause_button: Button = $Toolbar/Pause
@onready var _stop_button: Button = $Toolbar/Stop
@onready var _speed_spin: SpinBox = $Toolbar/Speed
@onready var _player: NovaCreditsPlayer = $Player

var _resource: CbinCreditsResource
var _paused := false

func set_resource(value: CbinCreditsResource) -> void:
	_resource = value
	_player.credits_resource = value
	_paused = false
	call_deferred("_refresh_toolbar_state")

func _ready() -> void:
	_player.font_base_path = "res://assets/fonts/"
	_player.texture_base_path = "res://assets/textures/"
	_play_button.pressed.connect(_on_play_pressed)
	_pause_button.pressed.connect(_on_pause_pressed)
	_stop_button.pressed.connect(_on_stop_pressed)
	_speed_spin.value_changed.connect(func(v): _player.speed_scale = v)
	_player.started.connect(_refresh_toolbar_state)
	_player.finished.connect(_on_player_finished)
	_play_button.tooltip_text = "Play credits preview"
	_pause_button.tooltip_text = "Pause credits preview"
	_stop_button.tooltip_text = "Stop and show the first entries"
	_speed_spin.tooltip_text = "Preview playback speed"
	_refresh_toolbar_state()

func _on_player_finished() -> void:
	_on_stop_pressed()

func _on_play_pressed() -> void:
	if _resource == null:
		return
	if _paused:
		_player.resume()
		_paused = false
	else:
		_player.play()
	_refresh_toolbar_state()

func _on_pause_pressed() -> void:
	if not _player.is_playing():
		return
	_player.pause()
	_paused = true
	_refresh_toolbar_state()

func _on_stop_pressed() -> void:
	_player.stop()
	_paused = false
	_player.set_scroll_offset(_player.get_size().y)
	_refresh_toolbar_state()

func _refresh_toolbar_state() -> void:
	var has_resource := _resource != null
	_play_button.disabled = not has_resource
	_pause_button.disabled = not _player.is_playing()
	_stop_button.disabled = not has_resource
	_play_button.text = "Resume" if _paused else "Play"
