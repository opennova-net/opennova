extends GutTest

var staged_dirs: Array[String] = []


func after_each() -> void:
	for path in staged_dirs:
		TestFs.remove_dir_recursive(path)
	staged_dirs.clear()


func _boot_voice_world(aoa: bool = false) -> GameWorld:
	var script := (
		"if never() then wave(\"tone.wav\") endif\n" +
		"if eq(v1,1) then wave(\"tone.wav\") set(v1,0) endif\n" +
		"if waveready() then set(v3,1) else set(v3,0) endif\n")
	var root_dir := WorldFixture.stage_minimal_root("wac_voice", false,
		{"mnml.wac": script})
	staged_dirs.append(root_dir)
	WorldFixture.stage_sound_bank(root_dir, PackedStringArray())
	if aoa:
		var bytes := "AOA1".to_ascii_buffer()
		bytes.resize(16 + 22050)
		bytes.encode_u32(4, 22050)
		bytes.encode_u32(8, 32768) # half the retail 44100 Hz device rate
		bytes[12] = 1 # signed PCM8; zero-filled payload is silence
		var stream := FileAccess.open(root_dir.path_join("tone.wav"), FileAccess.WRITE)
		stream.store_buffer(bytes)
		stream.close()
	# The minimal pack intentionally omits the listen-player template 105305.
	# Voice validation requires its real ItemDef, independently of its model.
	var items := FileAccess.get_file_as_string(root_dir.path_join("items.def"))
	WorldFixture.write_file(root_dir.path_join("items.def"), items +
		"\nbegin \"WAC voice player\"\n id 105305\n type person\n hp 150\n" +
		" ai_function plyr\n move_function org2\nend\n")
	var world := WorldFixture.make_world(self)
	world.set_playable(true)
	assert_eq(WorldFixture.load_mission(world, root_dir), OK)
	world.get_mission_audio().sync_script_voice()
	return world


func _playing_voice(audio: MissionAudio) -> AudioStreamPlayer:
	for child in audio.find_children("*", "AudioStreamPlayer", true, false):
		var player := child as AudioStreamPlayer
		if player.playing:
			return player
	return null


func _script_pass(sim: Simulation) -> void:
	for _tick in range(62):
		sim.step()


func test_wac_voice_completion_interrupt_and_retry_use_the_real_channel() -> void:
	var world := _boot_voice_world()
	var sim := world.get_sim()
	var audio := world.get_mission_audio()
	var first := _playing_voice(audio)
	assert_not_null(first, "initial WAC wave reaches a physical audio player")
	assert_eq(sim.get_mission_variable(3), 0, "waveready is false during playback")
	if first == null:
		world.unload()
		return
	assert_almost_eq(first.volume_db, linear_to_db(207.0 / 255.0), 0.001)

	# The actual VM replaces the line on its next scheduled pass.
	sim.set_mission_variable(1, 1)
	_script_pass(sim)
	audio.sync_script_voice()
	var second := _playing_voice(audio)
	assert_not_null(second)
	assert_ne(second, first)
	assert_false(first.playing)
	first.finished.emit() # delayed callback from the interrupted physical channel
	_script_pass(sim)
	assert_eq(sim.get_mission_variable(3), 0, "old completion cannot release the new line")

	# The live line's own completion releases waveready. Its `finished` is
	# emitted here as the interrupted channel's is above: the player emits it
	# on its next process frame after the mixer drops `playing`, so a
	# real-time poll of `playing` could run the pass first and read v3 as 0.
	if second != null:
		# The clip plays once, so the driver's `finished` does come (WavLoader).
		assert_eq((second.stream as AudioStreamWAV).loop_mode, AudioStreamWAV.LOOP_DISABLED)
		second.finished.emit()
	_script_pass(sim)
	assert_eq(sim.get_mission_variable(3), 1, "finished signal releases waveready")
	assert_null(_playing_voice(audio))

	sim.reset_session()
	audio.sync_script_voice()
	var restarted := _playing_voice(audio)
	assert_not_null(restarted, "retry replays the startup baseline's voice")
	_script_pass(sim)
	assert_eq(sim.get_mission_variable(3), 0)
	world.unload()


func test_aoa1_script_voice_reaches_the_audio_player() -> void:
	var world := _boot_voice_world(true)
	var voice := _playing_voice(world.get_mission_audio())
	assert_not_null(voice)
	if voice != null:
		var clip := voice.stream as AudioStreamWAV
		assert_not_null(clip)
		assert_eq(clip.mix_rate, 22050)
		assert_eq(clip.format, AudioStreamWAV.FORMAT_16_BITS)
		assert_eq(clip.data.size(), 44100)
		assert_false(clip.stereo)
	world.unload()


func test_missing_replacement_stops_the_previous_physical_voice() -> void:
	var world := _boot_voice_world()
	var audio := world.get_mission_audio()
	var first := _playing_voice(audio)
	assert_not_null(first)
	assert_false(audio.play_wac_wave("missing.wav"))
	if first != null:
		assert_false(first.playing, "reset occurs before the failed file lookup")
	assert_null(_playing_voice(audio))
	_script_pass(world.get_sim())
	assert_eq(world.get_sim().get_mission_variable(3), 1)
	world.unload()


func test_ssnwave_uses_a_following_spatial_player_and_engine_distance_gain() -> void:
	var root_dir := WorldFixture.stage_minimal_root("wac_spatial_voice", false,
		{"mnml.wac": ""})
	staged_dirs.append(root_dir)
	WorldFixture.stage_sound_bank(root_dir, PackedStringArray())
	var speaker_id := [0]
	var world := WorldFixture.make_world(self)
	world.set_playable(false)
	assert_eq(WorldFixture.load_mission(world, root_dir, "mnml.bms",
		func(mission: MissionData) -> void:
			var speaker := mission.add_entity(MissionData.KIND_ITEM, 106005,
				Vector3(5, 0, 0), Vector3.ZERO)
			speaker_id[0] = speaker.bms_id
			WorldFixture.write_file(root_dir.path_join("mnml.wac"),
				"if never() then SSNwave(%d,\"tone.wav\",20) endif\n" % speaker.bms_id)
	), OK)
	var audio := world.get_mission_audio()
	var sim := world.get_sim()
	sim.debug_set_world_entity_position(speaker_id[0], Vector3(5, 0, 0))
	audio.tick(Vector3.ZERO, 0.0)
	var voice: AudioStreamPlayer3D = null
	for child in audio.find_children("*", "AudioStreamPlayer3D", true, false):
		if child.playing:
			voice = child
			break
	assert_not_null(voice, "SSNwave creates the dedicated spatial channel")
	if voice == null:
		world.unload()
		return
	assert_eq(voice.attenuation_model, AudioStreamPlayer3D.ATTENUATION_DISABLED)
	assert_almost_eq(voice.global_position.x, 5.0, 0.001)
	var near_volume := voice.volume_db
	sim.debug_set_world_entity_position(speaker_id[0], Vector3(10, 0, 0))
	audio.tick(Vector3.ZERO, 0.0)
	assert_almost_eq(voice.global_position.x, 10.0, 0.001)
	assert_lt(voice.volume_db, near_volume)
	sim.debug_set_world_entity_position(speaker_id[0], Vector3(20, 0, 0))
	audio.tick(Vector3.ZERO, 0.0)
	assert_eq(voice.volume_db, -80.0, "the authored range is a hard silence boundary")
	assert_true(voice.playing, "out-of-range playback continues through its natural duration")
	world.unload()
