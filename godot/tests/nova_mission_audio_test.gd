extends GutTest

## The ambient emitter mix [orig: SoundEmitter_UpdateAndMixTop8 @ 0x5284a0]:
## loudest-8 channel budget, time-of-day slot activation, and idempotent
## writes. Asserts on volume_db — the headless dummy audio driver ignores
## stream_paused (always reads back false), so volume is the observable.

const NovaMissionAudioScript = preload("res://engine/world/nova_mission_audio.gd")

const SILENT_DB := -80.0


func _marker(holder: Node3D, pos: Vector3, slot_sets: PackedStringArray, players_by_set: Dictionary, stagger := 0.0) -> Dictionary:
	return {
		"node": holder,
		"pos": pos,
		"slot_sets": slot_sets,
		"stagger_h": stagger,
		"voices": players_by_set,
	}


func _voice(holder: Node3D, falloff: int, min_dist := 0, volume := 255, clamp_vol := 255) -> AudioStreamPlayer3D:
	var player := AudioStreamPlayer3D.new()
	player.volume_db = SILENT_DB
	player.set_meta("layer_params", {
		"falloff_radius": falloff,
		"min_distance": min_dist,
		"volume": volume,
		"clamp_volume": clamp_vol,
	})
	holder.add_child(player)
	return player


func test_only_the_loudest_eight_voices_mix() -> void:
	var audio = NovaMissionAudioScript.new(null, null)
	var holder := Node3D.new()
	add_child_autofree(holder)
	var players: Array[AudioStreamPlayer3D] = []
	var markers: Array = []
	# 12 markers on a line, nearer = louder under the witnessed (1 - d/r)^2 curve.
	for i in range(12):
		var p := _voice(holder, 2000)
		players.append(p)
		markers.append(_marker(holder, Vector3(float(10 + i * 50), 0, 0), ["amb", "amb", "amb", "amb"], {"amb": [p]}))
	audio._markers = markers
	audio.tick(Vector3.ZERO)
	for i in range(12):
		if i < 8:
			assert_gt(players[i].volume_db, SILENT_DB, "voice %d (near) is in the 8-channel mix" % i)
		else:
			assert_eq(players[i].volume_db, SILENT_DB, "voice %d (far) is silenced by the budget" % i)
	# Nearer voices are louder (quadratic falloff ordering).
	assert_gt(players[0].volume_db, players[7].volume_db, "closest voice is loudest")


func test_beyond_falloff_radius_is_hard_silent() -> void:
	var audio = NovaMissionAudioScript.new(null, null)
	var holder := Node3D.new()
	add_child_autofree(holder)
	var p := _voice(holder, 100)
	audio._markers = [_marker(holder, Vector3(150, 0, 0), ["amb", "amb", "amb", "amb"], {"amb": [p]})]
	audio.tick(Vector3.ZERO)
	assert_eq(p.volume_db, SILENT_DB, "a voice at d >= falloff_radius is silent [orig: 0x75ca31]")


func test_time_of_day_slot_selects_the_active_set() -> void:
	var audio = NovaMissionAudioScript.new(null, null)
	var holder := Node3D.new()
	add_child_autofree(holder)
	var night := _voice(holder, 500)
	# Night-only marker (a flourescent light): soundloop_4 filled, 1..3 empty.
	audio._markers = [_marker(holder, Vector3(10, 0, 0), ["", "", "", "night_hum"], {"night_hum": [night]})]

	audio.set_time_of_day_hhmm(1200.0)  # noon -> region 1 (day) -> empty slot
	audio.tick(Vector3.ZERO)
	assert_eq(night.volume_db, SILENT_DB, "day region with an empty slot plays nothing")

	audio.set_time_of_day_hhmm(2300.0)  # 23:00 -> region 3 (night)
	audio.tick(Vector3.ZERO)
	assert_gt(night.volume_db, SILENT_DB, "night region plays the night slot")


func test_region_crossfade_scales_volume() -> void:
	var audio = NovaMissionAudioScript.new(null, null)
	var holder := Node3D.new()
	add_child_autofree(holder)
	var day := _voice(holder, 500)
	var markers := [_marker(holder, Vector3(10, 0, 0), ["", "day_amb", "", ""], {"day_amb": [day]})]
	audio._markers = markers

	audio.set_time_of_day_hhmm(1200.0)  # mid-day: full blend
	audio.tick(Vector3.ZERO)
	var full_db := day.volume_db
	assert_gt(full_db, SILENT_DB)

	# 10:01 is inside the ~5-minute fade-in after the 10h cut [orig: @ 0x408203].
	audio.set_time_of_day_hhmm(1001.0)
	audio.tick(Vector3.ZERO)
	assert_gt(day.volume_db, SILENT_DB, "fading-in slot is audible")
	assert_lt(day.volume_db, full_db, "crossfade blend attenuates the entering region")


func test_same_set_neighbours_suppress_the_crossfade_dip() -> void:
	var audio = NovaMissionAudioScript.new(null, null)
	var holder := Node3D.new()
	add_child_autofree(holder)
	var allday := _voice(holder, 500)
	# The same set in every slot (the sound_profile fallback shape).
	audio._markers = [_marker(holder, Vector3(10, 0, 0), ["amb", "amb", "amb", "amb"], {"amb": [allday]})]

	audio.set_time_of_day_hhmm(1200.0)
	audio.tick(Vector3.ZERO)
	var full_db := allday.volume_db

	audio.set_time_of_day_hhmm(1001.0)  # in the 10h blend window
	audio.tick(Vector3.ZERO)
	assert_eq(allday.volume_db, full_db,
		"identical adjacent slot keeps full volume through the boundary [orig: 0x4a819d]")


func test_tick_writes_only_on_change() -> void:
	var audio = NovaMissionAudioScript.new(null, null)
	var holder := Node3D.new()
	add_child_autofree(holder)
	var p := _voice(holder, 500)
	audio._markers = [_marker(holder, Vector3(10, 0, 0), ["amb", "amb", "amb", "amb"], {"amb": [p]})]

	audio.tick(Vector3.ZERO)
	assert_eq(int(audio.get_perf_counters().get("voice_writes", -1)), 1, "first tick writes the voice on")

	audio.tick(Vector3.ZERO)
	assert_eq(int(audio.get_perf_counters().get("voice_writes", -1)), 0, "unchanged mix writes nothing")

	audio.tick(Vector3(2000, 0, 0))  # walk out of range
	assert_eq(int(audio.get_perf_counters().get("voice_writes", -1)), 1, "leaving range writes the silence once")
	assert_eq(p.volume_db, SILENT_DB)

	audio.tick(Vector3(2000, 0, 0))
	assert_eq(int(audio.get_perf_counters().get("voice_writes", -1)), 0, "steady silence writes nothing")
