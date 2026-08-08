class_name DebugSimPage
extends NovaDebugPage
## Sim transport (play / pause / step / stop) + the tick/entity/event/WAC
## status lines. Every mutation goes through the shared debug session, so F3
## and runtime automation use the same unlock and multiplayer-authority policy.

var _play_button: Button
var _pause_button: Button
var _step_button: Button
var _stop_button: Button
var _tick_label: Label
var _entities_label: Label
var _events_label: Label
var _wac_label: Label
var _wac_pause_check: CheckBox


func page_id() -> StringName:
	return &"Sim"


func page_category() -> StringName:
	return CATEGORY_SIM


func _build() -> void:
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
	_debug_controls[&"runtime_wac_paused"] = _wac_pause_check
	_refresh_transport_policy()


func refresh() -> void:
	_refresh_transport_policy()
	var runtime := _ctx.runtime()
	var sim := _ctx.sim()
	if runtime == null or sim == null:
		_clear_live()
		return
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


func _clear_live() -> void:
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


func _request_refresh() -> void:
	if _ctx.request_refresh.is_valid():
		_ctx.request_refresh.call()


func _on_play_pressed() -> void:
	_use_transport("resume")


func _on_pause_pressed() -> void:
	_use_transport("pause")


func _on_step_pressed() -> void:
	_use_transport("step")


func _on_stop_pressed() -> void:
	if _ctx.session == null:
		return
	var result: Dictionary = _ctx.session.invoke_control(
			&"runtime_return_to_menu")
	if int(result.get("error", ERR_UNAVAILABLE)) == OK:
		_request_refresh()


func _on_wac_pause_toggled(pressed: bool) -> void:
	if _ctx.session != null \
			and _ctx.session.set_control_value(
					&"runtime_wac_paused", pressed) == OK:
		_request_refresh()


func _use_transport(action: String) -> void:
	if _ctx.session == null:
		return
	var result: Dictionary = _ctx.session.invoke_control(
			&"runtime_transport", action)
	if int(result.get("error", ERR_UNAVAILABLE)) != OK:
		return
	_request_refresh()


func _refresh_transport_policy() -> void:
	var transport_buttons: Array[Button] = [
		_play_button, _pause_button, _step_button]
	if _ctx.session == null:
		for button in transport_buttons:
			button.disabled = true
		_stop_button.disabled = true
		return
	var state := _ctx.session.get_control_state(&"runtime_transport")
	var disabled := not state.available or not state.writable
	var reason := state.reason
	_play_button.disabled = disabled
	_play_button.tooltip_text = reason
	var world := _ctx.world()
	var is_network_session := world != null \
			and world.has_method("is_net_session") \
			and bool(world.call("is_net_session"))
	var pause_disabled := disabled or is_network_session
	var pause_reason := reason
	if not disabled and is_network_session:
		pause_reason = (
				"Pause and step are unavailable during multiplayer because "
				+ "the network pump must keep running.")
	for button in [_pause_button, _step_button]:
		button.disabled = pause_disabled
		button.tooltip_text = pause_reason
	var stop_state := _ctx.session.get_control_state(&"runtime_return_to_menu")
	_stop_button.disabled = _ctx.sim() == null \
			or not stop_state.available or not stop_state.writable
	_stop_button.tooltip_text = stop_state.reason
