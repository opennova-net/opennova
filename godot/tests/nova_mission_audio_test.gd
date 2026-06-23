extends GutTest

const NovaMissionAudioScript = preload("res://engine/world/nova_mission_audio.gd")


func test_tick_only_writes_when_cull_state_changes() -> void:
	var audio = NovaMissionAudioScript.new(null, null)
	var holder := Node3D.new()
	add_child_autofree(holder)
	var player := AudioStreamPlayer3D.new()
	holder.add_child(player)
	audio._markers = [{
		"node": holder,
		"pos": Vector3.ZERO,
		"players": [player],
		"paused": false,
	}]

	audio.tick(Vector3.ZERO)
	assert_false(player.stream_paused, "inside the cull radius remains unpaused")
	assert_eq(int(audio.get_perf_counters().get("voice_writes", -1)), 0,
		"unchanged cull state writes nothing")

	audio.tick(Vector3(1000.0, 0.0, 0.0))
	assert_true(player.stream_paused, "outside the cull radius pauses the voice")
	assert_eq(int(audio.get_perf_counters().get("voice_writes", -1)), 1,
		"state transition writes once")

	audio.tick(Vector3(1000.0, 0.0, 0.0))
	assert_true(player.stream_paused, "remaining outside stays paused")
	assert_eq(int(audio.get_perf_counters().get("voice_writes", -1)), 0,
		"repeated outside tick does not rewrite stream_paused")
