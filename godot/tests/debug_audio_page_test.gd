extends GutTest

# DebugAudioPage: mission-audio counters from a duck-typed stub, the per-bus
# knobs routed through the shared debug catalog into a public shell surface,
# and the empty states.

const PageScript := preload("res://engine/debug/pages/debug_audio_page.gd")

var _saved_buses: Dictionary = {}


func before_each() -> void:
	_saved_buses.clear()
	for bus in range(AudioServer.bus_count):
		var bus_name := String(AudioServer.get_bus_name(bus))
		_saved_buses[bus_name] = {
			"volume": AudioServer.get_bus_volume_db(bus),
			"mute": AudioServer.is_bus_mute(bus),
			"solo": AudioServer.is_bus_solo(bus),
			"bypass": AudioServer.is_bus_bypassing_effects(bus),
		}


func after_each() -> void:
	for bus_name in _saved_buses:
		var bus := AudioServer.get_bus_index(String(bus_name))
		if bus >= 0:
			var state: Dictionary = _saved_buses[bus_name]
			AudioServer.set_bus_volume_db(bus, float(state["volume"]))
			AudioServer.set_bus_mute(bus, bool(state["mute"]))
			AudioServer.set_bus_solo(bus, bool(state["solo"]))
			AudioServer.set_bus_bypass_effects(bus, bool(state["bypass"]))


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


class AudioShellStub:
	extends RefCounted

	var calls: Array = []

	func debug_set_audio_bus_volume(bus_name: String, volume_db: float) -> Error:
		calls.append([&"volume", bus_name, volume_db])
		var bus := AudioServer.get_bus_index(bus_name)
		if bus < 0:
			return ERR_DOES_NOT_EXIST
		AudioServer.set_bus_volume_db(bus, volume_db)
		return OK

	func debug_set_audio_bus_mute(bus_name: String, muted: bool) -> Error:
		calls.append([&"mute", bus_name, muted])
		var bus := AudioServer.get_bus_index(bus_name)
		if bus < 0:
			return ERR_DOES_NOT_EXIST
		AudioServer.set_bus_mute(bus, muted)
		return OK

	func debug_set_audio_bus_solo(bus_name: String, soloed: bool) -> Error:
		calls.append([&"solo", bus_name, soloed])
		var bus := AudioServer.get_bus_index(bus_name)
		if bus < 0:
			return ERR_DOES_NOT_EXIST
		AudioServer.set_bus_solo(bus, soloed)
		return OK

	func debug_set_audio_bus_bypass(bus_name: String, bypassed: bool) -> Error:
		calls.append([&"bypass", bus_name, bypassed])
		var bus := AudioServer.get_bus_index(bus_name)
		if bus < 0:
			return ERR_DOES_NOT_EXIST
		AudioServer.set_bus_bypass_effects(bus, bypassed)
		return OK


func _make_page(
		world: Node = null,
		audio_shell: AudioShellStub = null) -> DebugAudioPage:
	var ctx := NovaDebugContext.new()
	ctx.options = NovaDebugOptionState.new()
	ctx.session = NovaDebugSession.new()
	NovaDebugCatalog.install(ctx.session)
	var shell := audio_shell if audio_shell != null else AudioShellStub.new()
	ctx.session.set_target_source(
			NovaDebugCatalog.TARGET_GAME_SHELL, func(): return shell)
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
	var shell := AudioShellStub.new()
	var page := _make_page(null, shell)
	page.refresh()
	var sfx_bus := AudioServer.get_bus_index("SFX")
	if sfx_bus < 0:
		pass_test("no SFX bus in this layout; the knob disables itself")
		return
	var check := page.find_child("MuteSFX", true, false) as CheckBox
	assert_false(check.disabled, "a real bus arms its knob")

	check.toggled.emit(true)
	assert_eq(shell.calls, [[&"mute", "SFX", true]],
			"the knob routes through the catalog into the public shell surface")
	assert_true(AudioServer.is_bus_mute(sfx_bus))
	AudioServer.set_bus_mute(sfx_bus, false)
	page.refresh()
	assert_false(check.button_pressed, "refresh mirrors an externally-changed mute")


func test_dynamic_bus_knobs_share_the_catalog_actions() -> void:
	var shell := AudioShellStub.new()
	var page := _make_page(null, shell)
	page.refresh()
	var sfx_bus := AudioServer.get_bus_index("SFX")
	if sfx_bus < 0:
		pass_test("no SFX bus in this layout")
		return

	(page.find_child("VolumeSFX", true, false) as HSlider).value_changed.emit(-12.5)
	(page.find_child("SoloSFX", true, false) as CheckBox).toggled.emit(true)
	(page.find_child("BypassSFX", true, false) as CheckBox).toggled.emit(true)

	assert_eq(shell.calls, [
		[&"volume", "SFX", -12.5],
		[&"solo", "SFX", true],
		[&"bypass", "SFX", true],
	])
	assert_almost_eq(AudioServer.get_bus_volume_db(sfx_bus), -12.5, 0.001)
	assert_true(AudioServer.is_bus_solo(sfx_bus))
	assert_true(AudioServer.is_bus_bypassing_effects(sfx_bus))
