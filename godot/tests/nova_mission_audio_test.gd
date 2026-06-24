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

	# Assert on the marker's cull state, not player.stream_paused: the headless
	# dummy audio driver ignores AudioStreamPlayer3D.stream_paused (it always reads
	# back false), so the marker flag is the observable proxy. voice_writes proves
	# the stream_paused assignment actually executed on the cull transition.
	audio.tick(Vector3.ZERO)
	assert_false(bool(audio._markers[0]["paused"]), "inside the cull radius remains unpaused")
	assert_eq(int(audio.get_perf_counters().get("voice_writes", -1)), 0,
		"unchanged cull state writes nothing")

	audio.tick(Vector3(1000.0, 0.0, 0.0))
	assert_true(bool(audio._markers[0]["paused"]), "outside the cull radius pauses the voice")
	assert_eq(int(audio.get_perf_counters().get("voice_writes", -1)), 1,
		"state transition writes once")

	audio.tick(Vector3(1000.0, 0.0, 0.0))
	assert_true(bool(audio._markers[0]["paused"]), "remaining outside stays paused")
	assert_eq(int(audio.get_perf_counters().get("voice_writes", -1)), 0,
		"repeated outside tick does not rewrite stream_paused")
