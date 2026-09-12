extends GutTest

# The fire present pass (EntityPresenter's FirePresenter member, ADR 0043 d9)
# on the typed surfaces (ADR 0034): the drained rows are pure data fed through
# the public present_* data legs (the present_snapshot precedent — production
# present_passes() drains the typed Simulation and forwards the same rows),
# and the audio sink is a REAL MissionAudio over a staged root whose mission
# bank authors every set the rows name (ADR 0043 rule 11: no production
# subclass). Its recent-fires ring, mixer channel census, and the spawned
# AudioStreamPlayer3D voices under the container are the read seams.

# Every set the drained rows below name, authored into the staged mission bank
# (one layer each, playing the fixture tone).
const SOUND_SETS: PackedStringArray = [
	"AI_FIRE", "GS_END", "FSP_DIRT_L", "FREEFALL", "V_TRUCK_ILP"]

var _root_dir := ""
var _audio: MissionAudio = null


func after_each() -> void:
	if _audio != null:
		_audio.teardown()
		_audio = null
	if not _root_dir.is_empty():
		TestFs.remove_dir_recursive(_root_dir)
		_root_dir = ""


# A REAL MissionAudio set up over a staged root carrying the mission's co-named
# bank (fire.LWF authors SOUND_SETS), its audio root parented under
# `container`. No listener is stamped: a one-shot fired before the first tick
# skips the set-range cull, the way retail's play-before-first-frame does.
func _staged_audio(container: Node3D) -> MissionAudio:
	_root_dir = OS.get_cache_dir().path_join(
			"opennova_fire_present_pass_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(_root_dir), OK)
	WorldFixture.stage_sound_bank(_root_dir, SOUND_SETS, "fire.LWF")
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(_root_dir), OK, "the staged root mounts")
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	_audio = MissionAudio.create(root, null)
	var stats := _audio.setup(mission, "fire.bms", container)
	assert_eq(int(stats.banks_loaded), 1, "the staged mission bank loads")
	return _audio


# The positional voices MissionAudio spawned under `container`.
func _voices(container: Node) -> Array[AudioStreamPlayer3D]:
	var out: Array[AudioStreamPlayer3D] = []
	for value in container.find_children("*", "AudioStreamPlayer3D", true, false):
		out.append(value as AudioStreamPlayer3D)
	return out


func _event(pos: Vector3, source_bms_id: int, is_local_player: bool = false) -> FirePresentationEvent:
	return FirePresentationEvent.make(pos, source_bms_id, -1, is_local_player)


func _fire_sound(soundset: String, pos: Vector3, source_bms_id: int) -> FireSoundRow:
	return FireSoundRow.make(soundset, pos, source_bms_id)


func _slot_sound(soundset: String, pos: Vector3, handle: int, slot: int) -> SlotSoundRow:
	return SlotSoundRow.make(soundset, pos, handle, slot)


# A REAL EntityPresenter with its fire pass wired over `audio` (the sound
# legs) and, when given, `container` (the tracer geometry's host); no effect
# world, no sim — the data legs are the drive.
func _make_presenter(audio: MissionAudio, container: Node3D = null) -> EntityPresenter:
	var presenter := EntityPresenter.new()
	add_child_autofree(presenter)
	presenter.setup_passes(container, null, null, audio, null, null, null, null)
	return presenter


func test_drained_fire_sounds_play_with_source_identity() -> void:
	# The distance gate and delay countdown live in the sim (world/fire_sound.h,
	# pinned by the fire_sound ctest); the pass plays each drained row verbatim.
	var container := Node3D.new()
	add_child_autofree(container)
	var audio := _staged_audio(container)
	var presenter := _make_presenter(audio)

	presenter.present_fire_sounds([
		_fire_sound("AI_FIRE", Vector3(10, 0, 0), 77),
		_fire_sound("GS_END", Vector3(4, 1, 2), 0),
	])

	var fired := audio.recent_fired_soundsets()
	assert_eq(fired.size(), 2)
	if fired.size() == 2:
		assert_eq(fired[0].set_name, "AI_FIRE")
		assert_eq(fired[0].source_bms_id, 77,
				"the shooter's BMS identity reaches the bank's occlusion leg")
		assert_false(fired[0].slot, "drained fire rows ride the fire_soundset leg")
		assert_true(fired[0].played, "the staged bank resolves the set and spawns its voice")
		assert_eq(fired[1].set_name, "GS_END")
		assert_eq(fired[1].position, Vector3(4, 1, 2))
		assert_eq(fired[1].source_bms_id, 0)
		assert_true(fired[1].played)
	assert_eq(_voices(container).size(), 2, "each drained row spawned one positional voice")
	assert_eq(presenter.get_fire_present_stats().sounds, 2)
	presenter.teardown()


func test_no_audio_skips_the_sound_legs_but_not_the_effect_legs() -> void:
	# The dedicated-host tri-state: a presenter with no MissionAudio plays no
	# fire or slot sound (nothing to play them on) while the fire drain's
	# effect legs still run and count.
	var presenter := _make_presenter(null)

	presenter.present_fire_sounds([_fire_sound("AI_FIRE", Vector3(10, 0, 0), 77)])
	presenter.present_slot_sounds([_slot_sound("FSP_DIRT_L", Vector3(1, 0, 0), 3, 17)])
	presenter.present_fires([_event(Vector3(2, 0, 0), 22)])

	var stats := presenter.get_fire_present_stats()
	assert_eq(stats.sounds, 0, "no audio: the sound legs are skipped")
	assert_eq(stats.fires, 1, "no audio: the effect legs still present the fire")
	presenter.teardown()


func test_joiner_style_drain_presents_remote_and_discards_local_prediction() -> void:
	# A joiner feeds both its local predicted round and decoded remote tag-2
	# rounds into one visual RoundSim queue. The pass must present only the
	# remote record; the local action-slot leg already presented the prediction.
	# (The sim's own seed applies the same local filter to the sound queue —
	# the fire_sound ctest pins that half; the drain-consumption itself is the
	# native queue's contract, exercised by present() over the typed sim.)
	var audio := MissionAudio.create(null, null)
	autofree(audio)
	var presenter := _make_presenter(audio)
	var local := _event(Vector3(1, 0, 0), 11, true)
	var remote := _event(Vector3(2, 0, 0), 22)

	for _frame in range(128):
		presenter.present_fires([local, remote])

	assert_eq(presenter.get_fire_present_stats().fires, 128,
			"only the decoded remote shot reaches the presentation legs")
	assert_true(audio.recent_fired_soundsets().is_empty(),
			"the effect drain plays no sound of its own")
	presenter.teardown()


func test_tracer_trails_build_ribbon_strip() -> void:
	# A live stdred channel (style 1, 4 points along +X) must produce ONE additive
	# triangle-strip surface: pairs at points 0..count-2 (the newest point steers
	# direction only), 2 verts per pair [orig: CEffectChannel_RenderRibbon @ 0x5DB8A0].
	var audio := MissionAudio.create(null, null)
	autofree(audio)
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := _make_presenter(audio, container)
	presenter.set_listener_position(Vector3(0, 5, 10))

	presenter.draw_tracer_rows(PackedFloat32Array([
		1.0, 1.0, 4.0,  # style stdred, age 1, count 4
		0.0, 1.0, 0.0, 1.0,
		2.0, 1.0, 0.0, 1.0,
		4.0, 1.0, 0.0, 1.0,
		6.0, 1.0, 0.0, 1.0,
	]))

	var mesh: ImmediateMesh = presenter.fire_ribbon_mesh()
	assert_not_null(mesh, "a container hosts the ribbon geometry")
	if mesh == null:
		return
	assert_eq(mesh.get_surface_count(), 1, "one additive strip surface, no smoke surface")
	if mesh.get_surface_count() == 1:
		var arrays := mesh.surface_get_arrays(0)
		var verts: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		assert_eq(verts.size(), 6, "3 drawn pairs (points 0..2), 2 verts each")
		var cols: PackedColorArray = arrays[Mesh.ARRAY_COLOR]
		assert_almost_eq(cols[0].r, 0.0, 0.01, "oldest pair rides the base color (black)")
		assert_gt(cols[4].r, 0.5, "newer pairs ride the red ramp")
	assert_eq(presenter.get_fire_present_stats().tracer_peak, 1)
	presenter.teardown()


func test_tracer_smoke_style_lands_on_the_alpha_surface() -> void:
	# A rocket channel (style 3) draws on the smoke surface: alpha blend + scene fog
	# [orig: style +0 additive flag 0 -> SetFogAndBlendMode mode 0].
	var audio := MissionAudio.create(null, null)
	autofree(audio)
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := _make_presenter(audio, container)
	presenter.set_listener_position(Vector3(0, 5, 10))

	presenter.draw_tracer_rows(PackedFloat32Array([
		3.0, 1.0, 3.0,
		0.0, 1.0, 0.0, 1.0,
		2.0, 1.0, 0.0, 1.02,
		4.0, 1.0, 0.0, 0.98,
	]))

	var mesh: ImmediateMesh = presenter.fire_ribbon_mesh()
	assert_not_null(mesh)
	if mesh == null:
		return
	assert_eq(mesh.get_surface_count(), 1, "one smoke strip surface")
	if mesh.get_surface_count() == 1:
		var cols: PackedColorArray = mesh.surface_get_arrays(0)[Mesh.ARRAY_COLOR]
		# Smoke keeps its authored alpha fade (base gray pair alpha 1.0 is the
		# +0x14 base color; ramp entries carry the quadratic fade).
		assert_almost_eq(cols[2].r, 0.75, 0.01, "0xC0 gray ramp")
	presenter.teardown()


func test_slot_sounds_play_immediately() -> void:
	# Body slot sounds (footsteps/foley/landing/screams) have NO propagation-delay
	# leg — they play the tick they drain [orig: Entity_PlaySound3D_FullVolume
	# @ 0x528e20 direct]. Every fire goes through the finite channel pool.
	var container := Node3D.new()
	add_child_autofree(container)
	var audio := _staged_audio(container)
	var presenter := _make_presenter(audio)

	presenter.present_slot_sounds([
		_slot_sound("FSP_DIRT_L", Vector3(400, 0, 0), 3, 17),
		_slot_sound("FREEFALL", Vector3(1, 0, 0), 3, 44),
		_slot_sound("", Vector3.ZERO, 3, 18),
	])

	var fired := audio.recent_fired_soundsets()
	assert_eq(fired.size(), 2, "empty set name is the id-0 no-op")
	if fired.size() == 2:
		assert_eq(fired[0].set_name, "FSP_DIRT_L")
		assert_true(fired[0].slot, "body slot rows ride the slot_soundset leg")
		assert_true(fired[0].played, "the footstep plays the tick it drains")
		assert_eq(fired[0].position, Vector3(400, 0, 0))
		assert_eq(fired[1].set_name, "FREEFALL")
		assert_true(fired[1].played)
	assert_eq(_voices(container).size(), 2, "each named slot row spawned one positional voice")
	assert_eq(presenter.get_fire_present_stats().sounds, 2,
			"the pass counts every slot row the bank played")
	presenter.teardown()


func test_persistent_sound_emitters_drain_into_the_shared_audio_layer() -> void:
	var container := Node3D.new()
	add_child_autofree(container)
	var audio := _staged_audio(container)
	var presenter := _make_presenter(audio)
	var idle := SoundEmitterRow.make(77, 0x10001, 42, Vector3(10, 0, 0), 0, 0, 30, 12,
			0x10000, 0xFFFF, false, "V_TRUCK_ILP")

	presenter.present_sound_emitters([idle])

	# The registration is a keep-alive intent for the shared loudest-eight
	# emitter table, not a one-shot: it lands at the next audio pass and
	# occupies one physical channel at the row's emitter position.
	assert_true(audio.recent_fired_soundsets().is_empty(),
			"emitter registrations never enter the one-shot ring")
	assert_true(audio.active_ambient_candidate_ids().is_empty(),
			"the intent queues until the audio pass advances the mixer")
	audio.tick(idle.pos, 0.2)
	var ids := audio.active_ambient_candidate_ids()
	assert_eq(ids.size(), 1, "the idle lane registers one voice in the shared emitter mix")
	assert_eq(int(audio.get_perf_counters().active_channels), 1)
	if ids.size() == 1:
		var voice := audio.ambient_player_for_candidate(ids[0])
		assert_not_null(voice, "the registered lane holds a physical channel")
		if voice != null:
			assert_true(voice.position.is_equal_approx(idle.pos),
					"the voice rides the row's emitter position")
			assert_true(voice.playing)
			assert_eq((voice.stream as AudioStreamWAV).loop_mode,
					AudioStreamWAV.LOOP_FORWARD, "the engine emitter is a persistent loop")
	presenter.teardown()
