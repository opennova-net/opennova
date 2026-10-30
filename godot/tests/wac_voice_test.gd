extends GutTest

# The scripted voice rides the engine mixer's channel zero.
const VOICE_CHANNEL := 0
# A second of the mixer's 44100 Hz device frames: the fixture's clip plays out well inside it.
const PLAY_OUT_FRAMES := 44100


var staged_dirs: Array[String] = []
var _options := PackedInt32Array()


# The dialogue option at full: a WAC start's gain is (210 * 255 + 128) >> 8, 209, through the
# distance curve clamped at 255 (engine world::ScriptVoiceChannel).
func before_each() -> void:
	_options = AudioOut.option_volumes()
	AudioOut.set_option_volumes(_options[0], 255, _options[2])


func after_each() -> void:
	AudioOut.set_option_volumes(_options[0], _options[1], _options[2])
	for path in staged_dirs:
		TestFs.remove_dir_recursive(path)
	staged_dirs.clear()


func _boot_voice_world(aud: bool = false) -> GameWorld:
	var script := (
		"if never() then wave(\"tone.wav\") endif\n" +
		"if eq(v1,1) then wave(\"tone.wav\") set(v1,0) endif\n" +
		"if waveready() then set(v3,1) else set(v3,0) endif\n")
	var root_dir := WorldFixture.stage_minimal_root("wac_voice", false,
		{"mnml.wac": script})
	staged_dirs.append(root_dir)
	WorldFixture.stage_sound_bank(root_dir, PackedStringArray())
	if aud:
		# The game's own buffer, AUD1, which its wave loader takes unchecked.
		var bytes := "AUD1".to_ascii_buffer()
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
	world.get_mission_audio().update_script_voice()
	return world


func _script_pass(sim: Simulation) -> void:
	for _tick in range(62):
		sim.step()


func test_wac_voice_completion_and_interrupt_use_the_real_channel() -> void:
	var world := _boot_voice_world()
	var sim := world.get_sim()
	var audio := world.get_mission_audio()
	var first := audio.script_voice_handle()
	assert_ne(first, 0, "initial WAC wave opens the voice channel")
	assert_true(audio.channel_sounding(VOICE_CHANNEL))
	assert_eq(sim.get_mission_variable(3), 0, "waveready is false during playback")
	if first == 0:
		world.unload()
		return
	assert_eq(audio.channel_volume(VOICE_CHANNEL), 207)

	# The actual VM replaces the line on its next scheduled pass.
	sim.set_mission_variable(1, 1)
	_script_pass(sim)
	audio.update_script_voice()
	var second := audio.script_voice_handle()
	assert_ne(second, 0)
	assert_ne(second, first, "the new line opens the channel anew")
	audio.update_script_voice()
	_script_pass(sim)
	assert_eq(sim.get_mission_variable(3), 0, "the old line's handle cannot release the new line")

	# The live line's own completion releases waveready: the mixer renders the clip out (it
	# plays once), and the scene frame's validation reads the channel no longer holding it
	# (retail's Audio_UpdateAmbientStream runs from the scene frame alone): a frame the status
	# page stands in for leaves waveready false.
	AudioOut.mix_frames(PLAY_OUT_FRAMES)
	assert_false(audio.channel_sounding(VOICE_CHANNEL), "the clip played out")
	_script_pass(sim)
	assert_eq(sim.get_mission_variable(3), 0, "the release waits for the scene frame")
	audio.update_script_voice()
	_script_pass(sim)
	assert_eq(sim.get_mission_variable(3), 1, "the scene frame's validation releases waveready")
	assert_eq(audio.script_voice_handle(), 0)
	world.unload()


# The game's AUD1 buffer reaches the channel at its header's play word: 32768, half the 44100 Hz
# device rate.
func test_aud1_script_voice_reaches_the_channel() -> void:
	var world := _boot_voice_world(true)
	var audio := world.get_mission_audio()
	assert_true(audio.channel_sounding(VOICE_CHANNEL))
	assert_eq(audio.channel_play(VOICE_CHANNEL), 32768)
	world.unload()


func test_missing_replacement_stops_the_previous_voice() -> void:
	var world := _boot_voice_world()
	var audio := world.get_mission_audio()
	assert_ne(audio.script_voice_handle(), 0)
	assert_false(audio.play_wac_wave("missing.wav"))
	assert_false(audio.channel_sounding(VOICE_CHANNEL), "reset occurs before the failed file lookup")
	# The reset handle keeps the busy word until the scene frame's validation.
	_script_pass(world.get_sim())
	assert_eq(world.get_sim().get_mission_variable(3), 0)
	audio.update_script_voice()
	_script_pass(world.get_sim())
	assert_eq(world.get_sim().get_mission_variable(3), 1)
	world.unload()


# SSNwave's voice follows its speaker: the channel's volume byte is the engine's distance gain,
# zero at the authored range while the clip plays on to its natural end [orig:
# Audio_UpdateAmbientStream @ 0x4edb3e -> AudioChannel_SetAndPlay].
func test_ssnwave_follows_its_speaker_at_the_engine_distance_gain() -> void:
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
	audio.update_script_voice()
	assert_ne(audio.script_voice_handle(), 0, "SSNwave opens the voice channel")
	if audio.script_voice_handle() == 0:
		world.unload()
		return
	var near_volume := audio.channel_volume(VOICE_CHANNEL)
	assert_gt(near_volume, 0)
	sim.debug_set_world_entity_position(speaker_id[0], Vector3(10, 0, 0))
	audio.tick(Vector3.ZERO, 0.0)
	audio.update_script_voice()
	assert_lt(audio.channel_volume(VOICE_CHANNEL), near_volume)
	sim.debug_set_world_entity_position(speaker_id[0], Vector3(20, 0, 0))
	audio.tick(Vector3.ZERO, 0.0)
	audio.update_script_voice()
	assert_eq(audio.channel_volume(VOICE_CHANNEL), 0, "the authored range is a hard silence boundary")
	assert_true(audio.channel_sounding(VOICE_CHANNEL), "out-of-range playback continues through its natural duration")
	world.unload()
