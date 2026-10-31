extends GutTest

var staged_dirs: Array[String] = []


func after_each() -> void:
	for path in staged_dirs:
		TestFs.remove_dir_recursive(path)
	staged_dirs.clear()


# The one-shot channels of the engine mixer (12..25) that play a wave.
func _playing_oneshots(audio: MissionAudio) -> Array[int]:
	var out: Array[int] = []
	for channel in range(12, 26):
		if audio.channel_sounding(channel):
			out.append(channel)
	return out


func test_script_sound_distance_and_target_paths_reach_audio() -> void:
	var root_dir := WorldFixture.stage_minimal_root("wac_sounds", false, {"mnml.wac": ""})
	staged_dirs.append(root_dir)
	var items := FileAccess.get_file_as_string(root_dir.path_join("items.def"))
	WorldFixture.write_file(root_dir.path_join("items.def"), items +
			"\nbegin \"Script Target\"\n id 106088\n type marker\nend\n")
	WorldFixture.stage_sound_bank(root_dir, ["TONE"], "game.lwf", 200)
	var bank := LwfData.new()
	assert_true(bank.load_bytes(FileAccess.get_file_as_bytes(root_dir.path_join("game.lwf"))))
	bank.set_set_field(0, "target_id", 1) # positional fires cull beyond one unit
	bank.set_layer_field(0, 0, "heading", true) # the layer pans from its bearing
	bank.set_member_field(0, 0, 0, "volume", 255)
	bank.set_member_field(0, 0, 0, "clamp_volume", 255)
	assert_eq(bank.save_file(root_dir.path_join("game.lwf")), OK)
	var world := WorldFixture.make_world(self)
	var target_ids: Array[int] = []
	assert_eq(WorldFixture.load_mission(world, root_dir, "mnml.bms",
			func(mission: MissionData) -> void:
				var target := mission.add_entity(
						MissionData.KIND_MARKER, 106088, Vector3(100, 0, 0), Vector3.ZERO)
				assert_not_null(target)
				target_ids.append(target.bms_id)
				assert_true(mission.set_entity_property_int(
						MissionData.KIND_MARKER, target.index, "wp_number", 7))
				WorldFixture.write_file(root_dir.path_join("mnml.wac"),
						("if never then v1=SS_TONE sound(v1,100,128) store(v2) " +
						"sound2tgt(v1,7) store(v3) SS2SSN(v1,%d) store(v4) endif\n")
						% target.bms_id)), OK)
	var sim := world.get_sim()
	var audio := world.get_mission_audio()
	assert_eq(sim.get_mission_variable(1), 1, "the mounted bank resolves the sound handle at boot")
	assert_eq(sim.get_mission_variable(2), 0)
	assert_eq(sim.get_mission_variable(3), 0)
	assert_eq(sim.get_mission_variable(4), 1)
	# The sound-effects option at full: the distance sound's emitter volume is the option word
	# [orig: Sound_PlayTriggerSetScaled @ 0x527b9d].
	var options := AudioOut.option_volumes()
	AudioOut.set_option_volumes(255, int(options[1]), int(options[2]))
	# The process's one mixer starts this play with every channel on the silent buffer.
	world.shutdown_audio_channels()
	world.get_runtime().play()
	world.tick(Vector3.ZERO, Transform3D(), 0.02)
	AudioOut.set_option_volumes(int(options[0]), int(options[1]), int(options[2]))
	var voices := _playing_oneshots(audio)
	var records := audio.recent_fired_soundsets()
	assert_eq(records.size(), 3)
	if records.size() == 3:
		assert_eq(records[0].source_bms_id, target_ids[0], "sound2tgt retains its source for occlusion")
		assert_eq(records[1].source_bms_id, target_ids[0], "SS2SSN retains its source for occlusion")
	assert_eq(voices.size(), 1, "direct sound plays; both positional source commands cull")
	if voices.size() != 1:
		world.unload()
		return
	var first := voices[0]
	assert_eq(audio.channel_volume(first), 63, "explicit half-radius distance produces the native layer gain")
	assert_eq(audio.channel_pan(first), 128, "bearing 128 is the pan byte behind the listener")
	world.unload()
