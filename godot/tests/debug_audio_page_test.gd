extends GutTest

# DebugAudioPage: mission-audio counters from a duck-typed stub, the per-bus
# mute knobs against the REAL AudioServer layout (restored after each test),
# and the empty states.

const PageScript := preload("res://engine/debug/pages/debug_audio_page.gd")

var _saved_mutes: Dictionary = {}


func before_each() -> void:
	_saved_mutes.clear()
	for bus_name in DebugAudioPage.MUTE_BUSES:
		var bus := AudioServer.get_bus_index(String(bus_name))
		if bus >= 0:
			_saved_mutes[bus_name] = AudioServer.is_bus_mute(bus)


func after_each() -> void:
	for bus_name in _saved_mutes:
		var bus := AudioServer.get_bus_index(String(bus_name))
		if bus >= 0:
			AudioServer.set_bus_mute(bus, bool(_saved_mutes[bus_name]))


class StubMissionAudio:
	extends Node

	func get_stats() -> Dictionary:
		return {"markers_total": 6, "markers_resolved": 5, "banks_loaded": 2,
				"ambient_candidates": 9, "channel_budget": 8}

	func get_perf_counters() -> Dictionary:
		return {"tick_us": 210, "active_channels": 3}


class StubWorld:
	extends Node
	var audio := StubMissionAudio.new()

	func _init() -> void:
		add_child(audio)

	func get_mission_audio() -> StubMissionAudio:
		return audio


func _make_page(world: Node = null) -> DebugAudioPage:
	var ctx := NovaDebugContext.new()
	ctx.options = NovaDebugOptionState.new()
	if world != null:
		ctx.world_source = func(): return world
	var page: DebugAudioPage = PageScript.new()
	page.setup(ctx)
	add_child_autofree(page)
	return page


func test_renders_empty_states_without_sources() -> void:
	var page := _make_page()
	page.refresh()
	assert_string_contains((page.find_child("MissionAudioState", true, false) as Label).text,
			"No mission audio")


func test_formats_the_mission_audio_counters() -> void:
	var world := StubWorld.new()
	add_child_autofree(world)
	var page := _make_page(world)
	page.refresh()
	var text := (page.find_child("MissionAudioState", true, false) as Label).text
	assert_string_contains(text, "Markers 5/6")
	assert_string_contains(text, "banks 2")
	assert_string_contains(text, "Channels 3 live / 8 budget")
	assert_string_contains(text, "0.21 ms")


func test_mute_knobs_drive_and_mirror_the_real_buses() -> void:
	var page := _make_page()
	page.refresh()
	var sfx_bus := AudioServer.get_bus_index("SFX")
	if sfx_bus < 0:
		pass_test("no SFX bus in this layout; the knob disables itself")
		return
	var check := page.find_child("MuteSFX", true, false) as CheckBox
	assert_false(check.disabled, "a real bus arms its knob")

	check.toggled.emit(true)
	assert_true(AudioServer.is_bus_mute(sfx_bus), "the knob pokes AudioServer directly")
	AudioServer.set_bus_mute(sfx_bus, false)
	page.refresh()
	assert_false(check.button_pressed, "refresh mirrors an externally-changed mute")
