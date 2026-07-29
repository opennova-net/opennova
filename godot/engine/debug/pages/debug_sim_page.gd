class_name DebugSimPage
extends VBoxContainer
## Sim transport (play / pause / step / stop) + the tick/entity/event/WAC
## status lines. The transport drives the runtime directly (the overlay is
## host-neutral); hosts whose own UI mirrors transport state listen to the
## re-emitted `transport_used` and re-read.

## Fired after a transport press (play/pause/step/stop) or the script pause
## toggle acted on the runtime.
signal transport_used(action: String)

var _resolve_runtime := Callable()
var _resolve_sim := Callable()
var _request_refresh := Callable()

var _play_button: Button
var _pause_button: Button
var _step_button: Button
var _stop_button: Button
var _tick_label: Label
var _entities_label: Label
var _events_label: Label
var _wac_label: Label
var _wac_pause_check: CheckBox


func _init() -> void:
	name = "Sim"
	add_theme_constant_override("separation", 6)

	var transport := HBoxContainer.new()
	transport.name = "SimTransport"
	transport.add_theme_constant_override("separation", 4)
	add_child(transport)
	_play_button = _transport_button(transport, "SimPlay", "Play", _on_play_pressed)
	_pause_button = _transport_button(transport, "SimPause", "Pause", _on_pause_pressed)
	_step_button = _transport_button(transport, "SimStep", "Step", _on_step_pressed)
	_stop_button = _transport_button(transport, "SimStop", "Stop", _on_stop_pressed)

	_tick_label = _info_label("SimTick")
	_entities_label = _info_label("SimEntities")
	_events_label = _info_label("SimEvents")
	_wac_label = _info_label("SimWac")

	_wac_pause_check = CheckBox.new()
	_wac_pause_check.name = "SimWacPause"
	_wac_pause_check.text = "Pause scripts"
	_wac_pause_check.tooltip_text = "Stops the mission's scripts while the world keeps running."
	_wac_pause_check.toggled.connect(_on_wac_pause_toggled)
	add_child(_wac_pause_check)


## The transport re-resolves the runtime/sim on every press (reloads recreate
## them); `request_refresh` is the overlay-wide refresh so a press repaints
## every pane, not just this one.
func setup(resolve_runtime: Callable, resolve_sim: Callable, request_refresh: Callable) -> void:
	_resolve_runtime = resolve_runtime
	_resolve_sim = resolve_sim
	_request_refresh = request_refresh


func refresh(runtime: Object, sim: Object) -> void:
	_tick_label.text = "tick %d%s" % [int(sim.get_logic_tick()),
			"" if bool(runtime.is_playing()) else "  (paused)"]
	_entities_label.text = "%d units" % int(sim.get_entity_count())
	var fired: PackedByteArray = sim.get_fired_events_snapshot()
	var fired_count := 0
	for flag in fired:
		if flag != 0:
			fired_count += 1
	_events_label.text = "events fired: %d / %d" % [fired_count, fired.size()]
	var wac: Dictionary = sim.get_wac_state()
	if bool(wac.get("loaded", false)):
		_wac_label.text = "scripts: loaded, %d run(s)%s" % [int(wac.get("runs", 0)),
				"  (paused)" if bool(wac.get("paused", false)) else ""]
	else:
		_wac_label.text = "scripts: none"
	_wac_pause_check.set_pressed_no_signal(bool(wac.get("paused", false)))


func clear_live() -> void:
	_tick_label.text = ""
	_entities_label.text = ""
	_events_label.text = ""
	_wac_label.text = ""


func _transport_button(parent: Control, node_name: String, text: String, handler: Callable) -> Button:
	var button := Button.new()
	button.name = node_name
	button.text = text
	button.focus_mode = Control.FOCUS_NONE
	button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	button.pressed.connect(handler)
	parent.add_child(button)
	return button


func _info_label(node_name: String) -> Label:
	var label := Label.new()
	label.name = node_name
	add_child(label)
	return label


func _runtime() -> Object:
	return _resolve_runtime.call() if _resolve_runtime.is_valid() else null


func _on_play_pressed() -> void:
	var runtime := _runtime()
	if runtime != null:
		runtime.play()
		_request_refresh.call()
		transport_used.emit("play")


func _on_pause_pressed() -> void:
	var runtime := _runtime()
	if runtime != null:
		runtime.pause()
		_request_refresh.call()
		transport_used.emit("pause")


func _on_step_pressed() -> void:
	var runtime := _runtime()
	if runtime != null:
		runtime.step_once()
		_request_refresh.call()
		transport_used.emit("step")


func _on_stop_pressed() -> void:
	var runtime := _runtime()
	if runtime != null:
		runtime.stop()
		_request_refresh.call()
		transport_used.emit("stop")


func _on_wac_pause_toggled(pressed: bool) -> void:
	var sim: Object = _resolve_sim.call() if _resolve_sim.is_valid() else null
	if sim != null:
		sim.set_wac_paused(pressed)
		transport_used.emit("wac_pause")
