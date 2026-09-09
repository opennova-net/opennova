extends GutTest

var staged_dirs: Array[String] = []


func after_each() -> void:
	for path in staged_dirs:
		TestFs.remove_dir_recursive(path)
	staged_dirs.clear()


func _playing_voices(audio: MissionAudio) -> Array[AudioStreamPlayer3D]:
	var voices: Array[AudioStreamPlayer3D] = []
	for child in audio.get_children():
		if child is AudioStreamPlayer3D and child.playing and not child.is_queued_for_deletion():
			voices.append(child)
	return voices


func test_script_sound_distance_and_target_paths_reach_audio_and_retry() -> void:
	var root_dir := WorldFixture.stage_minimal_root("wac_sounds", false, {"mnml.wac": ""})
	staged_dirs.append(root_dir)
	var items := FileAccess.get_file_as_string(root_dir.path_join("items.def"))
	WorldFixture.write_file(root_dir.path_join("items.def"), items +
			"\nbegin \"Script Target\"\n id 106088\n type marker\nend\n")
	WorldFixture.stage_sound_bank(root_dir, ["TONE"], "mnml.lwf", 200)
	var bank := LwfData.new()
	assert_true(bank.load_bytes(FileAccess.get_file_as_bytes(root_dir.path_join("mnml.lwf"))))
	bank.set_set_field(0, "target_id", 1) # positional fires cull beyond one unit
	bank.set_member_field(0, 0, 0, "volume", 255)
	bank.set_member_field(0, 0, 0, "clamp_volume", 255)
	assert_eq(bank.save_file(root_dir.path_join("mnml.lwf")), OK)
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
	world.get_runtime().play()
	world.tick(Vector3.ZERO, Transform3D(), 0.02)
	var voices := _playing_voices(audio)
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
	assert_not_null(first.stream)
	assert_gt((first.stream as AudioStreamWAV).data.size(), 0, "the real WAV reached a physical player")
	assert_almost_eq(first.volume_db, SoundBank.volume_db_from_255(63), 0.001,
			"explicit half-radius distance produces the native layer gain")
	assert_almost_eq(first.position.z, 1.0, 0.0001, "bearing 128 is behind the camera")
	assert_almost_eq(first.position.x, 0.0, 0.0001)
	world.get_runtime().stop()
	assert_false(first.playing, "retry stops the old voice synchronously")
	assert_true(first.is_queued_for_deletion())
	world.get_runtime().play()
	world.tick(Vector3.ZERO, Transform3D(), 0.02)
	voices = _playing_voices(audio)
	assert_eq(voices.size(), 1, "retry replays exactly one initial direct sound")
	if voices.size() == 1:
		assert_ne(voices[0].get_instance_id(), first.get_instance_id())
		assert_almost_eq(voices[0].volume_db, SoundBank.volume_db_from_255(63), 0.001)
	world.unload()
