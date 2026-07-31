extends GutTest

const FirePresentPass := preload("res://engine/world/fire_present_pass.gd")


class SimStub:
	extends RefCounted
	var events: Array = []
	var slot_events: Array = []
	var sound_emitter_events: Array = []

	func drain_fire_presentation_events() -> Array:
		var out := events
		events = []
		return out

	func drain_slot_sounds() -> Array:
		var out := slot_events
		slot_events = []
		return out

	func drain_sound_emitters() -> Array:
		var out := sound_emitter_events
		sound_emitter_events = []
		return out

	# Trail channels framed [style_id, age, count, count x (x, y, z, w)] — the
	# tracer_trails.h witness map.
	var trails := PackedFloat32Array()

	func get_tracer_trails() -> PackedFloat32Array:
		return trails


class AudioStub:
	extends RefCounted
	var calls: Array = []
	var slot_calls: Array = []
	var sound_emitter_calls: Array = []

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

	func apply_sound_emitters(events: Array) -> void:
		sound_emitter_calls.append(events.duplicate(true))


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


func test_joiner_style_drain_presents_remote_and_discards_local_prediction() -> void:
	# A joiner feeds both its local predicted round and decoded remote tag-2
	# rounds into one visual RoundSim queue. The pass must consume every record
	# (so the queue cannot grow frame-over-frame) while only presenting the
	# remote record; the local action-slot leg already presented the prediction.
	var sim := SimStub.new()
	var audio := AudioStub.new()
	var presenter = _make_pass(sim, audio)
	var local := _event(Vector3(1, 0, 0), 11)
	local["is_local_player"] = true
	var remote := _event(Vector3(2, 0, 0), 22)

	for _frame in range(128):
		sim.events = [local.duplicate(), remote.duplicate()]
		presenter.present()
		assert_true(sim.events.is_empty(),
				"the complete mixed queue is drained each presentation")

	assert_eq(audio.calls.size(), 128,
			"only one decoded remote shot is presented per frame")
	assert_eq(int(presenter.get_stats()["fires"]), 128)
	for call_v in audio.calls:
		assert_eq(int((call_v as Dictionary)["source_bms_id"]), 22,
				"the local predicted record never reaches the remote-fire leg")
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


func test_tracer_trails_build_ribbon_strip() -> void:
	# A live stdred channel (style 1, 4 points along +X) must produce ONE additive
	# triangle-strip surface: pairs at points 0..count-2 (the newest point steers
	# direction only), 2 verts per pair [orig: CEffectChannel_RenderRibbon @ 0x5DB8A0].
	var sim := SimStub.new()
	var audio := AudioStub.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter = FirePresentPass.new()
	presenter.setup(sim, container, func(): return audio, Callable(),
			func(): return Vector3(0, 5, 10))
	sim.trails = PackedFloat32Array([
		1.0, 1.0, 4.0,  # style stdred, age 1, count 4
		0.0, 1.0, 0.0, 1.0,
		2.0, 1.0, 0.0, 1.0,
		4.0, 1.0, 0.0, 1.0,
		6.0, 1.0, 0.0, 1.0,
	])

	presenter.present()

	var mesh: ImmediateMesh = presenter.ribbon_mesh()
	assert_eq(mesh.get_surface_count(), 1, "one additive strip surface, no smoke surface")
	if mesh.get_surface_count() == 1:
		var arrays := mesh.surface_get_arrays(0)
		var verts: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		assert_eq(verts.size(), 6, "3 drawn pairs (points 0..2), 2 verts each")
		var cols: PackedColorArray = arrays[Mesh.ARRAY_COLOR]
		assert_almost_eq(cols[0].r, 0.0, 0.01, "oldest pair rides the base color (black)")
		assert_gt(cols[4].r, 0.5, "newer pairs ride the red ramp")
	assert_eq(int(presenter.get_stats()["tracer_peak"]), 1)
	presenter.teardown()


func test_tracer_smoke_style_lands_on_the_alpha_surface() -> void:
	# A rocket channel (style 3) draws on the smoke surface: alpha blend + scene fog
	# [orig: style +0 additive flag 0 -> SetFogAndBlendMode mode 0].
	var sim := SimStub.new()
	var audio := AudioStub.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter = FirePresentPass.new()
	presenter.setup(sim, container, func(): return audio, Callable(),
			func(): return Vector3(0, 5, 10))
	sim.trails = PackedFloat32Array([
		3.0, 1.0, 3.0,
		0.0, 1.0, 0.0, 1.0,
		2.0, 1.0, 0.0, 1.02,
		4.0, 1.0, 0.0, 0.98,
	])

	presenter.present()

	var mesh: ImmediateMesh = presenter.ribbon_mesh()
	assert_eq(mesh.get_surface_count(), 1, "one smoke strip surface")
	if mesh.get_surface_count() == 1:
		var cols: PackedColorArray = mesh.surface_get_arrays(0)[Mesh.ARRAY_COLOR]
		# Smoke keeps its authored alpha fade (base gray pair alpha 1.0 is the
		# +0x14 base color; ramp entries carry the quadratic fade).
		assert_almost_eq(cols[2].r, 0.75, 0.01, "0xC0 gray ramp")
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


func test_persistent_sound_emitters_drain_into_the_shared_audio_layer() -> void:
	var sim := SimStub.new()
	var audio := AudioStub.new()
	var presenter = _make_pass(sim, audio)
	var idle := {
		"source_spawn_id": 77,
		"handle": 0x10001,
		"source_bms_id": 42,
		"emitted_tick": 12,
		"lane": 0,
		"lifetime": 30,
		"pitch_q16": 0x10000,
		"volume_q8_8": 0xFFFF,
		"source_only": false,
		"slot": 0,
		"set": "V_TRUCK_ILP",
		"pos": Vector3(10, 0, 0),
	}
	sim.sound_emitter_events = [idle]

	presenter.present()

	assert_true(sim.sound_emitter_events.is_empty(),
			"the simulation queue is consumed once per present")
	assert_eq(audio.sound_emitter_calls.size(), 1)
	if audio.sound_emitter_calls.size() == 1:
		var batch: Array = audio.sound_emitter_calls[0]
		assert_eq(batch.size(), 1)
		assert_eq(batch[0], idle)
	presenter.teardown()
