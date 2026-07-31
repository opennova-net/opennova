extends "res://modtools/music/ui/live_mode_section.gd"

# MusicLiveMode's transport + events-log operations (W4-6a): the
# Start/Pause/Resume/Stop handlers, stop_director (the workspace's
# duck-typed capability hook -- the mount keeps the delegate), the
# director event callbacks, the coalescing events log, and the
# state-label / button-state chrome. Verbatim motion from live_mode.gd;
# state stays on the mount, reached through `_lm`.


func _on_start() -> void:
	if _lm._document == null or not _lm._document.bank_loaded() or not _lm._document.script_loaded():
		# Surface the failure on the state label for 2 seconds. push_warning
		# was invisible to the user; this lands in the same spot they're
		# already looking after pressing Start.
		_flash_start_warning("Load a project first")
		return
	if _lm._document.has_method("prepare_script_for_run"):
		var errs: Array = _lm._document.prepare_script_for_run()
		if errs.size() > 0:
			_flash_start_warning("Fix script errors first")
			return
	_lm._director.bank = _lm._document.bank
	_lm._director.load_mus_script(_lm._document.mus_script)
	_lm._refresh_var_labels()
	# Fresh run: clear de-spam memory + section/idle state and reset the meter so
	# the first events log cleanly rather than being suppressed against a prior
	# run's values.
	_lm._last_logged_var.clear()
	_lm._vol_seen = false
	_lm._current_section = &""
	_lm._idle_ticks = 0
	_lm._follow_live = true
	if _lm._volume_meter != null:
		_lm._volume_meter.set_volume(0, 0)
	_lm._director.start()
	_lm._start_warning_until_ms = 0
	_lm._apply_state_label(_lm._director.vm_state())
	_refresh_button_state()
	_lm._refresh_now_playing()
	_lm._refresh_map()


func _on_pause() -> void:
	if _lm._director == null:
		return
	_lm._director.pause()
	_lm._apply_state_label(_lm._director.vm_state())
	_refresh_button_state()
	_log_typed(_lm.EvType.SYSTEM, "paused")


func _on_resume() -> void:
	if _lm._director == null:
		return
	_lm._director.resume()
	_lm._apply_state_label(_lm._director.vm_state())
	_refresh_button_state()
	_log_typed(_lm.EvType.SYSTEM, "resumed")


func _on_stop() -> void:
	if _lm._director == null:
		return
	_lm._director.stop()
	_lm._apply_state_label(_lm.VM_STOPPED)
	_lm._current_section = &""
	_lm._idle_ticks = 0
	_lm._follow_live = true
	_refresh_button_state()
	_lm._refresh_now_playing()
	_lm._refresh_map()
	if _lm._volume_meter != null:
		_lm._volume_meter.set_volume(0, 0)


# Public stop hook used by music_workspace.gd::activate_workflow when leaving
# Live mode so audio doesn't bleed into the next workspace tab. No-op when
# the VM is already stopped.
func stop_director() -> void:
	if _lm._director == null:
		return
	if _lm._director.vm_state() == _lm.VM_STOPPED:
		return
	_lm._director.stop()
	_lm._apply_state_label(_lm.VM_STOPPED)
	_lm._current_section = &""
	_lm._idle_ticks = 0
	_lm._follow_live = true
	_refresh_button_state()
	_lm._refresh_now_playing()
	_lm._refresh_map()
	if _lm._volume_meter != null:
		_lm._volume_meter.set_volume(0, 0)


func _on_sound(idx: int, sound_name: StringName, wait: bool) -> void:
	_log_typed(_lm.EvType.SOUND, "play %d (%s)%s" % [idx, sound_name, " wait" if wait else ""])
	_lm._refresh_now_playing(String(sound_name))


func _on_echo(arg: int) -> void:
	_log_typed(_lm.EvType.ECHO, "debug message %d" % arg)


# Emitted by the VM when a section runs to its terminal `done`. Treat as a
# clean end-of-track and surface it visually so the user sees Stop wasn't
# required.
func _on_halted() -> void:
	_lm._apply_state_label(_lm.VM_HALTED)
	_lm._refresh_now_playing()
	_log_typed(_lm.EvType.SYSTEM, "halted")


# vm_error means mus_vm_load_script or runtime hit a hard fault. The director
# already flipped its internal state to ERROR; we mirror it.
func _on_vm_error(message: String) -> void:
	_lm._apply_state_label(_lm.VM_ERROR)
	if message.is_empty():
		_log_typed(_lm.EvType.SYSTEM, "error")
	else:
		_log_typed(_lm.EvType.SYSTEM, "error: %s" % message)


func _on_variable_changed(var_index: int, value: int) -> void:
	# The Variables tab mirrors every write live; only log a line when the
	# value actually changes (and the var filter is on), so per-frame writes
	# don't drown the log.
	if _lm._last_logged_var.get(var_index, null) == value:
		return
	_lm._last_logged_var[var_index] = value
	# Friendly per-script name when known ("MissionActive (Var01)"), else raw
	# "Var01". Same mus_var_names map the Variables tab uses, so the log and the
	# inspector agree instead of the log showing opaque indices.
	var label := "Var%02d" % var_index
	if _lm._document != null and _lm._document.script_loaded():
		label = _lm.MusVarNames.label_for(String(_lm._document.mus_script.get_default_script_name()), var_index)
	_log_typed(_lm.EvType.VAR, "%s = %d" % [label, value])


func _on_volume_changed(left: int, right: int) -> void:
	# GSV/GSDV emit 16.16 fixed-point ints (witnessed: Jointops.exe!Intrinsic_GSV @
	# 0x6720E0). The meter always shows the latest value; the log line only
	# fires on an actual change (and when the volume filter is on) so a
	# per-tick volume loop doesn't flood the list.
	if _lm._volume_meter != null:
		_lm._volume_meter.set_volume(left, right)
	if _lm._vol_seen and left == _lm._last_vol_l and right == _lm._last_vol_r:
		return
	_lm._vol_seen = true
	_lm._last_vol_l = left
	_lm._last_vol_r = right
	_log_typed(_lm.EvType.VOLUME, "volume L=%.2f R=%.2f" % [left / 65536.0, right / 65536.0])


func _on_clear() -> void:
	if _lm._events == null:
		return
	_lm._events.clear()
	_lm._last_log_type = -1
	_lm._last_log_text = ""
	_lm._last_log_count = 0


# --- Events log --------------------------------------------------------


# Append a typed, colour-coded line, drop the oldest entry past the 50-row cap,
# and scroll the tail into view. ensure_current_is_visible only nudges the
# scroll when an item is current, so we set the new tail current first.
func _log_typed(type: int, text: String) -> void:
	if _lm._events == null:
		return
	if not _lm._is_type_shown(type):
		return
	# Coalesce a run of identical (type, text) events into the existing tail row
	# as "<text>  xN" rather than appending duplicates. A distinct line breaks
	# the run (so the cap test's distinct "flood N" lines never fold).
	if _lm._events.item_count > 0 and type == _lm._last_log_type and text == _lm._last_log_text:
		_lm._last_log_count += 1
		var tail: int = _lm._events.item_count - 1
		_lm._events.set_item_text(tail, "[%s] %s  x%d" % [_timestamp(), text, _lm._last_log_count])
		if _lm._EV_COLOR.has(type):
			_lm._events.set_item_custom_fg_color(tail, _lm._EV_COLOR[type])
		_lm._events.select(tail)
		_lm._events.ensure_current_is_visible()
		return
	_lm._events.add_item("[%s] %s" % [_timestamp(), text])
	if _lm._EV_COLOR.has(type):
		_lm._events.set_item_custom_fg_color(_lm._events.item_count - 1, _lm._EV_COLOR[type])
	if _lm._events.item_count > 50:
		_lm._events.remove_item(0)
	_lm._last_log_type = type
	_lm._last_log_text = text
	_lm._last_log_count = 1
	if _lm._events.item_count > 0:
		_lm._events.select(_lm._events.item_count - 1)
		_lm._events.ensure_current_is_visible()


# Back-compat shim: untyped log lines are system events. Kept so callers/tests
# that use _log() keep working.
func _log(text: String) -> void:
	_log_typed(_lm.EvType.SYSTEM, text)


func _timestamp() -> String:
	var t := Time.get_time_dict_from_system()
	return "%02d:%02d:%02d" % [t["hour"], t["minute"], t["second"]]


# Show a 2-second red message owning the state label, e.g. "Load a project
# first" after Start was pressed without a project. _process clears it once
# the deadline passes and snaps the label back to vm_state().
func _flash_start_warning(message: String) -> void:
	if _lm._state_label == null:
		return
	_lm._state_label.text = message
	_lm._state_label.add_theme_color_override("font_color", _lm.COLOR_ERROR)
	_lm._start_warning_until_ms = Time.get_ticks_msec() + 2000


# Toolbar button enable rules:
# - Start: enabled iff a bank+script are both loaded AND VM is not running/paused
# - Pause: enabled iff RUNNING
# - Resume: enabled iff PAUSED
# - Stop: enabled iff RUNNING or PAUSED
# Also keeps the jump dropdown's RUNNING-only rule in sync.
func _refresh_button_state() -> void:
	var has_project: bool = _lm._document != null and _lm._document.bank_loaded() and _lm._document.script_loaded()
	var state: int = _lm._last_state
	if _lm._start_btn != null:
		var start_enabled: bool = has_project and state != _lm.VM_RUNNING and state != _lm.VM_PAUSED
		var start_tip: String = _lm.TRANSPORT_START_TOOLTIP
		if not has_project:
			start_tip = _lm.TRANSPORT_OPEN_PROJECT_TOOLTIP
		elif state == _lm.VM_RUNNING:
			start_tip = _lm.TRANSPORT_ALREADY_RUNNING_TOOLTIP
		elif state == _lm.VM_PAUSED:
			start_tip = _lm.TRANSPORT_RESUME_OR_STOP_TOOLTIP
		_set_button_state(_lm._start_btn, start_enabled, start_tip)
	if _lm._pause_btn != null:
		_set_button_state(_lm._pause_btn, state == _lm.VM_RUNNING,
			_lm.TRANSPORT_PAUSE_TOOLTIP if state == _lm.VM_RUNNING else _lm.TRANSPORT_ALREADY_PAUSED_TOOLTIP if state == _lm.VM_PAUSED else _lm.TRANSPORT_START_FIRST_TOOLTIP)
	if _lm._resume_btn != null:
		_set_button_state(_lm._resume_btn, state == _lm.VM_PAUSED,
			_lm.TRANSPORT_RESUME_TOOLTIP if state == _lm.VM_PAUSED else _lm.TRANSPORT_PAUSE_FIRST_TOOLTIP)
	if _lm._stop_btn != null:
		var can_stop: bool = state == _lm.VM_RUNNING or state == _lm.VM_PAUSED
		_set_button_state(_lm._stop_btn, can_stop,
			_lm.TRANSPORT_STOP_TOOLTIP if can_stop else _lm.TRANSPORT_START_FIRST_TOOLTIP)
	_refresh_add_state_button_state()
	_lm._update_jump_enabled()


func _set_button_state(button: Button, enabled: bool, tooltip: String) -> void:
	button.disabled = not enabled
	button.tooltip_text = tooltip


func _refresh_add_state_button_state() -> void:
	var reason: String = _lm._authoring_blocked_reason()
	var blocked := reason != ""
	if _lm._add_state_btn != null:
		_lm._add_state_btn.disabled = blocked
		_lm._add_state_btn.tooltip_text = reason if blocked else _lm.ADD_STATE_TOOLTIP
	if _lm._sidebar_add_state_btn != null:
		_lm._sidebar_add_state_btn.disabled = blocked
		_lm._sidebar_add_state_btn.tooltip_text = reason if blocked else _lm.ADD_STATE_TOOLTIP
