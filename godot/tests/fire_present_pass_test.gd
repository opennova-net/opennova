extends GutTest

const FirePresentPass := preload("res://engine/world/fire_present_pass.gd")


class SimStub:
	extends RefCounted
	var events: Array = []
	var slot_events: Array = []

	func drain_fire_presentation_events() -> Array:
		var out := events
		events = []
		return out

	func drain_slot_sounds() -> Array:
		var out := slot_events
		slot_events = []
		return out

	func get_tracer_rounds() -> PackedFloat32Array:
		return PackedFloat32Array()


class AudioStub:
	extends RefCounted
	var calls: Array = []
	var slot_calls: Array = []

	func fire_soundset(set_name: String, world_pos: Vector3,
			source_bms_id: int = 0) -> bool:
		calls.append({
			"set": set_name,
			"pos": world_pos,
			"source_bms_id": source_bms_id,
		})
		return true

	func slot_soundset(set_name: String, world_pos: Vector3,
			exclusive_key: String = "") -> bool:
		slot_calls.append({
			"set": set_name,
			"pos": world_pos,
			"key": exclusive_key,
		})
		return true


func _event(pos: Vector3, source_bms_id: int) -> Dictionary:
	return {
		"origin": pos,
		"sound_set": "AI_FIRE",
		"source_bms_id": source_bms_id,
		"is_local_player": false,
	}


func _make_pass(sim: SimStub, audio: AudioStub):
	var presenter = FirePresentPass.new()
	presenter.setup(
		sim,
		null,
		func(): return audio,
		Callable(),
		func(): return Vector3.ZERO,
	)
	return presenter


func test_immediate_fire_keeps_source_identity() -> void:
	var sim := SimStub.new()
	var audio := AudioStub.new()
	var presenter = _make_pass(sim, audio)
	sim.events = [_event(Vector3(10, 0, 0), 77)]

	presenter.present()

	assert_eq(audio.calls.size(), 1)
	if audio.calls.size() == 1:
		assert_eq(int(audio.calls[0]["source_bms_id"]), 77)
	presenter.teardown()


func test_delayed_fire_keeps_source_identity_until_playback() -> void:
	var sim := SimStub.new()
	var audio := AudioStub.new()
	var presenter = _make_pass(sim, audio)
	# At 330 units the witnessed delay is (62 * 330 / 330) >> 2 = 15 ticks.
	sim.events = [_event(Vector3(330, 0, 0), 88)]

	presenter.present(1)
	assert_true(audio.calls.is_empty(), "far fire remains queued after its first tick")
	presenter.present(14)

	assert_eq(audio.calls.size(), 1)
	if audio.calls.size() == 1:
		assert_eq(int(audio.calls[0]["source_bms_id"]), 88)
	presenter.teardown()


func test_slot_sounds_play_immediately_with_exclusive_freefall_key() -> void:
	# Body slot sounds (footsteps/foley/landing/screams) have NO propagation-delay
	# leg — they play the tick they drain [orig: Entity_PlaySound3D_FullVolume
	# @ 0x528e20 direct]. Slots 43/44 carry the per-(handle,slot) exclusive key
	# (the D-SND-10 refire fold); everything else passes an empty key.
	var sim := SimStub.new()
	var audio := AudioStub.new()
	var presenter = _make_pass(sim, audio)
	sim.slot_events = [
		{"set": "FSP_DIRT_L", "pos": Vector3(400, 0, 0), "handle": 3, "slot": 17},
		{"set": "FREEFALL", "pos": Vector3(1, 0, 0), "handle": 3, "slot": 44},
		{"set": "", "pos": Vector3.ZERO, "handle": 3, "slot": 18},
	]

	presenter.present()

	assert_eq(audio.slot_calls.size(), 2, "empty set name is the id-0 no-op")
	if audio.slot_calls.size() == 2:
		assert_eq(String(audio.slot_calls[0]["set"]), "FSP_DIRT_L")
		assert_eq(String(audio.slot_calls[0]["key"]), "")
		assert_eq(String(audio.slot_calls[1]["set"]), "FREEFALL")
		assert_eq(String(audio.slot_calls[1]["key"]), "3:44")
	presenter.teardown()
