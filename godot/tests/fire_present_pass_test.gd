extends GutTest

# FirePresentPass on the typed surfaces (ADR 0034): the drained rows are pure
# data fed through the public present_* data legs (the present_snapshot
# precedent — production present() drains the typed Simulation and forwards the
# same rows), and the audio sink is a real MissionAudio subclass capturing the
# typed calls.

const FirePresentPass := preload("res://game/world/fire_present_pass.gd")


class CaptureAudio:
	extends MissionAudio
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


func _event(pos: Vector3, source_bms_id: int) -> FirePresentationEvent:
	var event := FirePresentationEvent.new()
	event.origin = pos
	event.source_bms_id = source_bms_id
	return event


func _fire_sound(soundset: String, pos: Vector3, source_bms_id: int) -> FireSoundRow:
	var row := FireSoundRow.new()
	row.soundset = soundset
	row.pos = pos
	row.source_bms_id = source_bms_id
	return row


func _slot_sound(soundset: String, pos: Vector3, handle: int, slot: int) -> SlotSoundRow:
	var row := SlotSoundRow.new()
	row.soundset = soundset
	row.pos = pos
	row.handle = handle
	row.slot = slot
	return row


func _make_pass(audio: CaptureAudio) -> FirePresentPass:
	var presenter := FirePresentPass.new()
	presenter.setup(
		null,
		null,
		func(): return audio,
		Callable(),
		func(): return Vector3.ZERO,
	)
	return presenter


func test_drained_fire_sounds_play_with_source_identity() -> void:
	# The distance gate and delay countdown live in the sim (world/fire_sound.h,
	# pinned by the fire_sound ctest); the pass plays each drained row verbatim.
	var audio := CaptureAudio.new(null, null)
	var presenter := _make_pass(audio)

	presenter.present_fire_sounds([
		_fire_sound("AI_FIRE", Vector3(10, 0, 0), 77),
		_fire_sound("GS_END", Vector3(4, 1, 2), 0),
	])

	assert_eq(audio.calls.size(), 2)
	if audio.calls.size() == 2:
		assert_eq(String(audio.calls[0]["set"]), "AI_FIRE")
		assert_eq(int(audio.calls[0]["source_bms_id"]), 77)
		assert_eq(audio.calls[1]["pos"], Vector3(4, 1, 2))
	assert_eq(presenter.get_stats().sounds, 2)
	presenter.teardown()


func test_joiner_style_drain_presents_remote_and_discards_local_prediction() -> void:
	# A joiner feeds both its local predicted round and decoded remote tag-2
	# rounds into one visual RoundSim queue. The pass must present only the
	# remote record; the local action-slot leg already presented the prediction.
	# (The sim's own seed applies the same local filter to the sound queue —
	# the fire_sound ctest pins that half; the drain-consumption itself is the
	# native queue's contract, exercised by present() over the typed sim.)
	var audio := CaptureAudio.new(null, null)
	var presenter := _make_pass(audio)
	var local := _event(Vector3(1, 0, 0), 11)
	local.is_local_player = true
	var remote := _event(Vector3(2, 0, 0), 22)

	for _frame in range(128):
		presenter.present_fires([local, remote])

	assert_eq(presenter.get_stats().fires, 128,
			"only the decoded remote shot reaches the presentation legs")
	presenter.teardown()


func test_tracer_trails_build_ribbon_strip() -> void:
	# A live stdred channel (style 1, 4 points along +X) must produce ONE additive
	# triangle-strip surface: pairs at points 0..count-2 (the newest point steers
	# direction only), 2 verts per pair [orig: CEffectChannel_RenderRibbon @ 0x5DB8A0].
	var audio := CaptureAudio.new(null, null)
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := FirePresentPass.new()
	presenter.setup(null, container, func(): return audio, Callable(),
			func(): return Vector3(0, 5, 10))

	presenter.draw_tracer_rows(PackedFloat32Array([
		1.0, 1.0, 4.0,  # style stdred, age 1, count 4
		0.0, 1.0, 0.0, 1.0,
		2.0, 1.0, 0.0, 1.0,
		4.0, 1.0, 0.0, 1.0,
		6.0, 1.0, 0.0, 1.0,
	]))

	var mesh: ImmediateMesh = presenter.ribbon_mesh()
	assert_eq(mesh.get_surface_count(), 1, "one additive strip surface, no smoke surface")
	if mesh.get_surface_count() == 1:
		var arrays := mesh.surface_get_arrays(0)
		var verts: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		assert_eq(verts.size(), 6, "3 drawn pairs (points 0..2), 2 verts each")
		var cols: PackedColorArray = arrays[Mesh.ARRAY_COLOR]
		assert_almost_eq(cols[0].r, 0.0, 0.01, "oldest pair rides the base color (black)")
		assert_gt(cols[4].r, 0.5, "newer pairs ride the red ramp")
	assert_eq(presenter.get_stats().tracer_peak, 1)
	presenter.teardown()


func test_tracer_smoke_style_lands_on_the_alpha_surface() -> void:
	# A rocket channel (style 3) draws on the smoke surface: alpha blend + scene fog
	# [orig: style +0 additive flag 0 -> SetFogAndBlendMode mode 0].
	var audio := CaptureAudio.new(null, null)
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := FirePresentPass.new()
	presenter.setup(null, container, func(): return audio, Callable(),
			func(): return Vector3(0, 5, 10))

	presenter.draw_tracer_rows(PackedFloat32Array([
		3.0, 1.0, 3.0,
		0.0, 1.0, 0.0, 1.0,
		2.0, 1.0, 0.0, 1.02,
		4.0, 1.0, 0.0, 0.98,
	]))

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
	var audio := CaptureAudio.new(null, null)
	var presenter := _make_pass(audio)

	presenter.present_slot_sounds([
		_slot_sound("FSP_DIRT_L", Vector3(400, 0, 0), 3, 17),
		_slot_sound("FREEFALL", Vector3(1, 0, 0), 3, 44),
		_slot_sound("", Vector3.ZERO, 3, 18),
	])

	assert_eq(audio.slot_calls.size(), 2, "empty set name is the id-0 no-op")
	if audio.slot_calls.size() == 2:
		assert_eq(String(audio.slot_calls[0]["set"]), "FSP_DIRT_L")
		assert_eq(String(audio.slot_calls[0]["key"]), "")
		assert_eq(String(audio.slot_calls[1]["set"]), "FREEFALL")
		assert_eq(String(audio.slot_calls[1]["key"]), "3:44")
	presenter.teardown()


func test_persistent_sound_emitters_drain_into_the_shared_audio_layer() -> void:
	var audio := CaptureAudio.new(null, null)
	var presenter := _make_pass(audio)
	var idle := SoundEmitterRow.new()
	idle.source_spawn_id = 77
	idle.handle = 0x10001
	idle.source_bms_id = 42
	idle.emitted_tick = 12
	idle.lane = 0
	idle.lifetime = 30
	idle.pitch_q16 = 0x10000
	idle.volume_q8_8 = 0xFFFF
	idle.soundset = "V_TRUCK_ILP"
	idle.pos = Vector3(10, 0, 0)

	presenter.present_sound_emitters([idle])

	assert_eq(audio.sound_emitter_calls.size(), 1)
	if audio.sound_emitter_calls.size() == 1:
		var batch: Array = audio.sound_emitter_calls[0]
		assert_eq(batch.size(), 1)
		assert_eq(batch[0], idle)
	presenter.teardown()
