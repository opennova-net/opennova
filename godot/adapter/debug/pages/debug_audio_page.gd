class_name DebugAudioPage
extends NovaDebugPage
## Audio: the music service's live state (context, section, script VM) and
## the mission-audio surface (markers, banks, channels, dialog) with per-bus
## mixer knobs. Reads mirror AudioServer each refresh; mutations route through
## the shared debug session and MainGame's public process-local audio surface,
## exactly like runtime MCP.

## Bus names per godot/default_bus_layout.tres.
const MUTE_BUSES := ["SFX", "Music", "Ambient", "Voice"]

var _music_label: Label
var _mission_label: Label
var _driver_label: Label
var _bus_rows: VBoxContainer
var _bus_names: PackedStringArray
var _mute_checks: Dictionary = {}  # bus name -> CheckBox
var _bus_controls: Dictionary = {}  # bus name -> control record


func page_id() -> StringName:
	return &"Audio"


func page_category() -> StringName:
	return CATEGORY_WORLD


func _build() -> void:
	add_theme_constant_override("separation", 6)

	_music_label = Label.new()
	_music_label.name = "MusicState"
	_music_label.text = "No music service."
	_music_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_music_label)

	_mission_label = Label.new()
	_mission_label.name = "MissionAudioState"
	_mission_label.text = "No mission audio."
	_mission_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_mission_label)

	var mute_header := Label.new()
	mute_header.name = "MuteHeader"
	mute_header.text = "Mute"
	add_child(mute_header)
	for bus_name in MUTE_BUSES:
		var check := CheckBox.new()
		check.name = "Mute%s" % bus_name
		check.text = bus_name
		check.tooltip_text = "Silence the %s bus — flip it to isolate what you are hearing." % bus_name
		check.toggled.connect(_on_mute_toggled.bind(String(bus_name)))
		add_child(check)
		_mute_checks[bus_name] = check
		# Retain these stable nodes for compatibility probes; the dynamic bus
		# rows below are the visible controls and cover every configured bus.
		check.visible = false

	_driver_label = Label.new()
	_driver_label.name = "AudioDriver"
	_driver_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_driver_label)

	var bus_scroll := ScrollContainer.new()
	bus_scroll.name = "AudioBusScroll"
	bus_scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	bus_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	add_child(bus_scroll)
	_bus_rows = VBoxContainer.new()
	_bus_rows.name = "AudioBusRows"
	_bus_rows.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	bus_scroll.add_child(_bus_rows)
	_rebuild_bus_rows()


func _rebuild_bus_rows() -> void:
	for child in _bus_rows.get_children():
		_bus_rows.remove_child(child)
		child.queue_free()
	_bus_controls.clear()
	_bus_names = PackedStringArray()
	for bus in range(AudioServer.bus_count):
		var bus_name := AudioServer.get_bus_name(bus)
		_bus_names.append(bus_name)
		var safe_name := bus_name.validate_node_name()
		var bus_box := VBoxContainer.new()
		bus_box.name = "Bus%s" % safe_name
		_bus_rows.add_child(bus_box)

		var main_row := HBoxContainer.new()
		main_row.name = "BusMain"
		bus_box.add_child(main_row)
		var name_label := Label.new()
		name_label.text = bus_name
		name_label.custom_minimum_size = Vector2(72, 0)
		main_row.add_child(name_label)
		var volume := HSlider.new()
		volume.name = "Volume%s" % safe_name
		volume.min_value = -60.0
		volume.max_value = 6.0
		volume.step = 0.5
		volume.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		volume.tooltip_text = "Adjust the %s bus volume in decibels." % bus_name
		volume.value_changed.connect(_on_volume_changed.bind(bus_name))
		main_row.add_child(volume)
		var db_label := Label.new()
		db_label.name = "VolumeValue"
		db_label.custom_minimum_size = Vector2(54, 0)
		main_row.add_child(db_label)

		var switches := HBoxContainer.new()
		switches.name = "BusSwitches"
		bus_box.add_child(switches)
		var mute := CheckBox.new()
		mute.name = "BusMute%s" % safe_name
		mute.text = "Mute"
		mute.toggled.connect(_on_mute_toggled.bind(bus_name))
		switches.add_child(mute)
		var solo := CheckBox.new()
		solo.name = "Solo%s" % safe_name
		solo.text = "Solo"
		solo.toggled.connect(_on_solo_toggled.bind(bus_name))
		switches.add_child(solo)
		var bypass := CheckBox.new()
		bypass.name = "Bypass%s" % safe_name
		bypass.text = "Bypass FX"
		bypass.toggled.connect(_on_bypass_toggled.bind(bus_name))
		switches.add_child(bypass)
		var peak := Label.new()
		peak.name = "Peak"
		peak.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		peak.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
		switches.add_child(peak)
		_bus_controls[bus_name] = {
			"volume": volume,
			"db": db_label,
			"mute": mute,
			"solo": solo,
			"bypass": bypass,
			"peak": peak,
		}


func refresh() -> void:
	if _bus_names.size() != AudioServer.bus_count:
		_rebuild_bus_rows()
	else:
		for bus in range(AudioServer.bus_count):
			if _bus_names[bus] != AudioServer.get_bus_name(bus):
				_rebuild_bus_rows()
				break
	var driver := String(AudioServer.get_driver_name()) \
			if AudioServer.has_method("get_driver_name") else "unknown"
	var latency_ms := float(AudioServer.get_output_latency()) * 1000.0 \
			if AudioServer.has_method("get_output_latency") else 0.0
	_driver_label.text = "Driver: %s | output latency %.1f ms" % [driver, latency_ms]

	var music := get_node_or_null("/root/NovaMusicService")
	if music == null or not music.has_method("current_context"):
		_music_label.text = "No music service."
	else:
		var line := "Music context: %s" % [
			String(music.current_context()) if not String(music.current_context()).is_empty()
			else "-"]
		if music.has_method("director"):
			var director: Variant = music.director()
			if director != null and is_instance_valid(director) \
					and (director as Object).has_method("current_section"):
				line += "\nSection %s   vm %s   pc %s   players %d" % [
					String(director.current_section()), str(director.vm_state()),
					str(director.current_pc()), int(director.get_player_pool_size())]
				var last_error := String(director.last_error())
				if not last_error.is_empty():
					line += "\nScript error: %s" % last_error
		_music_label.text = line

	var mission_audio := _mission_audio()
	if mission_audio == null:
		_mission_label.text = "No mission audio."
	else:
		var stats: Dictionary = mission_audio.get_stats()
		var perf: Dictionary = mission_audio.get_perf_counters()
		_mission_label.text = (
				"Markers %d/%d resolved   banks %d   ambient %d\n"
				+ "Channels %d live / %d budget   eval %.2f ms") % [
			int(stats.get("markers_resolved", 0)), int(stats.get("markers_total", 0)),
			int(stats.get("banks_loaded", 0)), int(stats.get("ambient_candidates", 0)),
			int(perf.get("active_channels", 0)), int(stats.get("channel_budget", 0)),
			float(perf.get("tick_us", 0)) / 1000.0]

	for bus_name in MUTE_BUSES:
		var bus := AudioServer.get_bus_index(String(bus_name))
		var check := _mute_checks[bus_name] as CheckBox
		check.disabled = bus < 0
		if bus >= 0:
			check.set_pressed_no_signal(AudioServer.is_bus_mute(bus))

	for bus_name in _bus_names:
		var bus := AudioServer.get_bus_index(String(bus_name))
		var controls: Dictionary = _bus_controls.get(bus_name, {})
		if bus < 0 or controls.is_empty():
			continue
		var volume_db := AudioServer.get_bus_volume_db(bus)
		(controls["volume"] as HSlider).set_value_no_signal(volume_db)
		(controls["db"] as Label).text = "%.1f dB" % volume_db
		(controls["mute"] as CheckBox).set_pressed_no_signal(
				AudioServer.is_bus_mute(bus))
		(controls["solo"] as CheckBox).set_pressed_no_signal(
				AudioServer.is_bus_solo(bus))
		(controls["bypass"] as CheckBox).set_pressed_no_signal(
				AudioServer.is_bus_bypassing_effects(bus))
		var left := AudioServer.get_bus_peak_volume_left_db(bus, 0)
		var right := AudioServer.get_bus_peak_volume_right_db(bus, 0)
		(controls["peak"] as Label).text = "L %.0f  R %.0f dB" % [left, right]


func _mission_audio() -> Object:
	var world := _ctx.world()
	if world == null or not world.has_method("get_mission_audio"):
		return null
	var audio: Variant = world.get_mission_audio()
	if audio is Object and is_instance_valid(audio) \
			and (audio as Object).has_method("get_stats") \
			and (audio as Object).has_method("get_perf_counters"):
		return audio
	return null


func _on_mute_toggled(muted: bool, bus_name: String) -> void:
	_invoke_audio_control(&"set_audio_bus_mute", [bus_name, muted])


func _on_volume_changed(volume_db: float, bus_name: String) -> void:
	_invoke_audio_control(&"set_audio_bus_volume", [bus_name, volume_db])


func _on_solo_toggled(soloed: bool, bus_name: String) -> void:
	_invoke_audio_control(&"set_audio_bus_solo", [bus_name, soloed])


func _on_bypass_toggled(bypassed: bool, bus_name: String) -> void:
	_invoke_audio_control(&"set_audio_bus_bypass", [bus_name, bypassed])


func _invoke_audio_control(id: StringName, args: Array) -> void:
	if _ctx == null or _ctx.session == null:
		return
	_ctx.session.invoke_control(id, args)
