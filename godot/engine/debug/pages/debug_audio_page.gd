class_name DebugAudioPage
extends NovaDebugPage
## Audio: the music service's live state (context, section, script VM) and
## the mission-audio surface (markers, banks, channels, dialog) with per-bus
## mute knobs. The mutes poke AudioServer directly — global host state, the
## Sim/Vars precedent for page-local controls — and re-mirror each refresh.

## Bus names per godot/default_bus_layout.tres.
const MUTE_BUSES := ["SFX", "Music", "Ambient", "Voice"]

var _music_label: Label
var _mission_label: Label
var _mute_checks: Dictionary = {}  # bus name -> CheckBox


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


func refresh() -> void:
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
	var bus := AudioServer.get_bus_index(bus_name)
	if bus >= 0:
		AudioServer.set_bus_mute(bus, muted)
