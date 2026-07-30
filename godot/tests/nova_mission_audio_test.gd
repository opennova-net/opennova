extends GutTest

## The ambient emitter mix [orig: SoundEmitter_UpdateAndMixTop8 @ 0x5284a0]:
## loudest-8 channel budget, time-of-day slot activation, and idempotent
## writes. Asserts on volume_db — the headless dummy audio driver ignores
## stream_paused (always reads back false), so volume is the observable.

const NovaMissionAudioScript = preload("res://engine/world/nova_mission_audio.gd")

const SILENT_DB := -80.0


# Public NovaMissionAudio dependency seam implemented by NovaSimulation.
class OcclusionProviderStub:
	extends RefCounted
	var calls := 0
	var source_bms_ids: Array[int] = []

	func sound_occlusion_distance_q16(
			_listener_pos: Vector3, _source_pos: Vector3, raw_distance_q16: int,
			source_bms_id: int = 0) -> int:
		calls += 1
		source_bms_ids.append(source_bms_id)
		return raw_distance_q16


func _marker(pos: Vector3, slot_sets: PackedStringArray,
		layers_by_set: Dictionary, stagger := 0.0, source_bms_id := 0) -> Dictionary:
	return {
		"pos": pos,
		"source_bms_id": source_bms_id,
		"slot_sets": slot_sets,
		"stagger_h": stagger,
		"layers_by_set": layers_by_set,
	}


func _layer(falloff: int, min_dist := 0, volume := 255, clamp_vol := 255) -> Dictionary:
	var stream := AudioStreamWAV.new()
	stream.format = AudioStreamWAV.FORMAT_16_BITS
	stream.mix_rate = 22050
	var samples := PackedByteArray()
	samples.resize(32)
	stream.data = samples
	return {
		"stream": stream,
		"falloff_radius": falloff,
		"min_distance": min_dist,
		"volume": volume,
		"clamp_volume": clamp_vol,
		"base_pitch": 1.0,
	}


func _players(container: Node) -> Array[AudioStreamPlayer3D]:
	var out: Array[AudioStreamPlayer3D] = []
	for value in container.find_children("*", "AudioStreamPlayer3D", true, false):
		out.append(value as AudioStreamPlayer3D)
	return out


func _active_ids(container: Node) -> Array[int]:
	var out: Array[int] = []
	for player in _players(container):
		if player.has_meta("ambient_candidate_id"):
			out.append(int(player.get_meta("ambient_candidate_id")))
	out.sort()
	return out


func _active_player(container: Node, candidate_id: int) -> AudioStreamPlayer3D:
	for player in _players(container):
		if int(player.get_meta("ambient_candidate_id", -1)) == candidate_id:
			return player
	return null


func _player_at_position(container: Node, pos: Vector3) -> AudioStreamPlayer3D:
	for player in _players(container):
		if player.position.is_equal_approx(pos):
			return player
	return null


func test_native_mixer_keeps_legacy_rows_and_versions_pitch_rows() -> void:
	var mixer := NovaAmbientMixer.new()
	var layer := PackedInt32Array([7, 2000, 0, 255, 255])
	mixer.add_marker(Vector3(10, 2, 3), 0, 0, 30,
		PackedInt32Array([0, 0, 0, 0]), [layer])
	mixer.advance_to_tick(0)
	var legacy: PackedFloat32Array = mixer.mix(Vector3.ZERO)
	var pitched: PackedFloat32Array = mixer.mix_v2(Vector3.ZERO)
	assert_eq(legacy.size(), 5,
		"mix() retains its public stride-5 ABI for older scripts")
	assert_eq(pitched.size(), 6,
		"mix_v2() carries pitch without silently reframing legacy rows")
	if legacy.size() == 5 and pitched.size() == 6:
		assert_eq(int(legacy[0]), 7)
		assert_eq(int(pitched[0]), 7)
		assert_eq(int(pitched[2]), 0x10000)
		assert_eq(
			Vector3(legacy[2], legacy[3], legacy[4]),
			Vector3(pitched[3], pitched[4], pitched[5]))


func test_only_the_loudest_eight_candidates_mix() -> void:
	var audio = NovaMissionAudioScript.new(null, null)
	var holder := Node3D.new()
	add_child_autofree(holder)
	var markers: Array = []
	# 12 markers on a line, nearer = louder under the witnessed (1 - d/r)^2 curve.
	for i in range(12):
		markers.append(_marker(
			Vector3(float(10 + i * 50), 0, 0),
			["amb", "amb", "amb", "amb"], {"amb": [_layer(2000)]}))
	audio.set_markers(markers, holder)
	assert_eq(_players(holder).size(), 0,
		"virtual ambient candidates do not create SceneTree audio nodes")
	audio.tick(Vector3.ZERO, 0.2)
	var players := _players(holder)
	assert_eq(players.size(), 8, "the physical ambient pool never exceeds its eight channels")
	players.sort_custom(func(a, b): return a.position.x < b.position.x)
	for i in players.size():
		assert_eq(players[i].position.x, float(10 + i * 50),
			"only the nearest eight virtual candidates occupy channels")
		assert_gt(players[i].volume_db, SILENT_DB)
		assert_eq(players[i].process_mode, Node.PROCESS_MODE_INHERIT)
	# Nearer candidates are louder (quadratic falloff ordering).
	assert_gt(players[0].volume_db, players[7].volume_db, "closest voice is loudest")


func test_beyond_falloff_radius_is_hard_silent() -> void:
	var audio = NovaMissionAudioScript.new(null, null)
	var holder := Node3D.new()
	add_child_autofree(holder)
	audio.set_markers([_marker(
		Vector3(150, 0, 0), ["amb", "amb", "amb", "amb"],
		{"amb": [_layer(100)]})], holder)
	audio.tick(Vector3.ZERO, 0.2)
	assert_eq(_players(holder).size(), 0,
		"a candidate at d >= falloff_radius consumes no physical channel [orig: 0x75ca31]")


func test_ambient_queries_occlusion_once_per_raw_audible_marker() -> void:
	# Two active layers on one audible marker share one two-ray result. A second
	# active marker is already silent by raw falloff and must not spend a query.
	var audio = NovaMissionAudioScript.new(null, null)
	var provider := OcclusionProviderStub.new()
	audio.set_simulation(provider)
	var holder := Node3D.new()
	add_child_autofree(holder)
	audio.set_markers([
		_marker(Vector3(10, 0, 0), ["amb", "amb", "amb", "amb"],
			{"amb": [_layer(100), _layer(100)]}, 0.0, 123),
		_marker(Vector3(150, 0, 0), ["amb", "amb", "amb", "amb"],
			{"amb": [_layer(100)]}, 0.0, 456),
	], holder)

	audio.tick(Vector3.ZERO, 0.2)

	assert_eq(provider.calls, 1,
		"only the raw-audible marker queries once despite carrying two layers")
	assert_eq(provider.source_bms_ids, [123], "the audible marker keeps its source identity")
	assert_eq(_players(holder).size(), 2)
	for player in _players(holder):
		assert_gt(player.volume_db, SILENT_DB)


func test_time_of_day_slot_selects_the_active_set() -> void:
	var audio = NovaMissionAudioScript.new(null, null)
	var holder := Node3D.new()
	add_child_autofree(holder)
	# Night-only marker (a flourescent light): soundloop_4 filled, 1..3 empty.
	audio.set_markers([_marker(
		Vector3(10, 0, 0), ["", "", "", "night_hum"],
		{"night_hum": [_layer(500)]})], holder)

	audio.set_time_of_day_hhmm(1200.0)  # noon -> region 1 (day) -> empty slot
	audio.tick(Vector3.ZERO, 0.2)
	assert_eq(_players(holder).size(), 0, "day region with an empty slot plays nothing")

	audio.set_time_of_day_hhmm(2300.0)  # 23:00 -> region 3 (night)
	audio.tick(Vector3.ZERO, 0.2)
	assert_eq(_players(holder).size(), 1)
	assert_gt(_players(holder)[0].volume_db, SILENT_DB, "night region plays the night slot")


func test_region_crossfade_scales_volume() -> void:
	var audio = NovaMissionAudioScript.new(null, null)
	var holder := Node3D.new()
	add_child_autofree(holder)
	var markers := [_marker(
		Vector3(10, 0, 0), ["", "day_amb", "", ""],
		{"day_amb": [_layer(500)]})]
	audio.set_markers(markers, holder)

	audio.set_time_of_day_hhmm(1200.0)  # mid-day: full blend
	audio.tick(Vector3.ZERO, 0.2)
	var day := _players(holder)[0]
	var full_db := day.volume_db
	assert_gt(full_db, SILENT_DB)

	# 10:01 is inside the ~5-minute fade-in after the 10h cut [orig: @ 0x408203].
	audio.set_time_of_day_hhmm(1001.0)
	audio.tick(Vector3.ZERO, 0.2)
	assert_gt(day.volume_db, SILENT_DB, "fading-in slot is audible")
	assert_lt(day.volume_db, full_db, "crossfade blend attenuates the entering region")


func test_same_set_neighbours_suppress_the_crossfade_dip() -> void:
	var audio = NovaMissionAudioScript.new(null, null)
	var holder := Node3D.new()
	add_child_autofree(holder)
	# The same set in every slot (a marker whose soundloop_1..4 all name one set).
	audio.set_markers([_marker(
		Vector3(10, 0, 0), ["amb", "amb", "amb", "amb"],
		{"amb": [_layer(500)]})], holder)

	audio.set_time_of_day_hhmm(1200.0)
	audio.tick(Vector3.ZERO, 0.2)
	var allday := _players(holder)[0]
	var full_db := allday.volume_db

	audio.set_time_of_day_hhmm(1001.0)  # in the 10h blend window
	audio.tick(Vector3.ZERO, 0.2)
	assert_eq(allday.volume_db, full_db,
		"identical adjacent slot keeps full volume through the boundary [orig: 0x4a819d]")


func test_tick_writes_only_on_change() -> void:
	var audio = NovaMissionAudioScript.new(null, null)
	var holder := Node3D.new()
	add_child_autofree(holder)
	audio.set_markers([_marker(
		Vector3(10, 0, 0), ["amb", "amb", "amb", "amb"],
		{"amb": [_layer(500)]})], holder)

	audio.tick(Vector3.ZERO, 0.2)
	assert_eq(int(audio.get_perf_counters().get("voice_writes", -1)), 1, "first tick writes the voice on")
	var p := _players(holder)[0]
	var first_stream := p.stream

	audio.tick(Vector3.ZERO, 0.2)
	assert_eq(int(audio.get_perf_counters().get("voice_writes", -1)), 0, "unchanged mix writes nothing")
	assert_eq(_players(holder)[0], p, "the incumbent keeps its physical channel")
	assert_eq(_players(holder)[0].stream, first_stream, "the incumbent playback is not restarted")

	audio.tick(Vector3(2000, 0, 0), 0.2)  # walk out of range
	assert_eq(int(audio.get_perf_counters().get("voice_writes", -1)), 1, "leaving range writes the silence once")
	assert_eq(p.volume_db, SILENT_DB)
	assert_null(p.stream, "a dropout releases its bound stream")
	assert_eq(p.process_mode, Node.PROCESS_MODE_DISABLED,
		"an unused physical channel leaves SceneTree processing")

	audio.tick(Vector3(2000, 0, 0), 0.2)
	assert_eq(int(audio.get_perf_counters().get("voice_writes", -1)), 0, "steady silence writes nothing")

	audio.tick(Vector3.ZERO, 0.2)
	assert_eq(int(audio.get_perf_counters().get("voice_writes", -1)), 1,
		"re-entering the mix binds and restarts the voice once")
	assert_eq(_players(holder)[0], p, "the bounded pool reuses its free channel")
	assert_ne(_players(holder)[0].stream, first_stream,
		"a candidate that dropped out restarts with a fresh looping stream binding")
	assert_eq(p.process_mode, Node.PROCESS_MODE_INHERIT,
		"an audible entrant returns the physical channel to processing")

	audio.tick(Vector3.ZERO, 0.2)
	assert_eq(int(audio.get_perf_counters().get("voice_writes", -1)), 0,
		"the resumed steady mix stays write-free")


func test_top_eight_membership_reuses_pool_and_restarts_only_entrants() -> void:
	var audio = NovaMissionAudioScript.new(null, null)
	var holder := Node3D.new()
	add_child_autofree(holder)
	var markers: Array = []
	for i in range(12):
		markers.append(_marker(
			Vector3(float(i * 100), 0, 0), ["amb", "amb", "amb", "amb"],
			{"amb": [_layer(2000)]}))
	audio.set_markers(markers, holder)

	audio.tick(Vector3.ZERO, 0.2)
	assert_eq(_active_ids(holder), [1, 2, 3, 4, 5, 6, 7, 8])
	var candidate_one_stream := _active_player(holder, 1).stream
	var pool_ids: Array[int] = []
	for player in _players(holder):
		pool_ids.append(player.get_instance_id())
	pool_ids.sort()

	audio.tick(Vector3(1100, 0, 0), 0.2)
	assert_eq(_active_ids(holder), [5, 6, 7, 8, 9, 10, 11, 12],
		"the closest eight virtual candidates replace the four dropouts")
	var moved_pool_ids: Array[int] = []
	for player in _players(holder):
		moved_pool_ids.append(player.get_instance_id())
	moved_pool_ids.sort()
	assert_eq(moved_pool_ids, pool_ids, "entrant replacement allocates no ninth channel")

	audio.tick(Vector3.ZERO, 0.2)
	assert_eq(_active_ids(holder), [1, 2, 3, 4, 5, 6, 7, 8])
	assert_ne(_active_player(holder, 1).stream, candidate_one_stream,
		"a dropped candidate restarts when it becomes an entrant again")
	assert_eq(_players(holder).size(), 8)


func test_dynamic_vehicle_emitter_joins_pool_refreshes_and_clears_by_key() -> void:
	var fixture_dir := OS.get_cache_dir().path_join(
		"mission_audio_vehicle_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(fixture_dir), OK)
	_write_bytes(fixture_dir.path_join("tone.wav"),
		FileAccess.get_file_as_bytes(
			ProjectSettings.globalize_path(
				"res://../fixtures/menu_sound/selecta1.wav")))
	var lwf := NovaLwfData.new()
	lwf.create_empty()
	_add_lwf_set(lwf, "V_TRUCK_ILP", "tone.wav", 2000)
	assert_eq(lwf.save_file(fixture_dir.path_join("game.LWF")), OK)

	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(fixture_dir), OK)
	var mission := NovaMissionData.new()
	assert_eq(mission.create_default(), OK)
	var container := Node3D.new()
	add_child_autofree(container)
	var audio = NovaMissionAudioScript.new(root, null)
	audio.setup(mission, "vehicle_probe.bms", container)

	# Dynamic engine voices share retail's loudest-eight emitter budget with
	# placed ambience [orig: SoundEmitter_UpdateAndMixTop8 @ 0x5284a0].
	# Eight quieter placed voices plus this source must still own only eight
	# physical players, with the truck displacing the weakest marker.
	var vehicle_pos := Vector3(900, 4, -300)
	var markers: Array = []
	for i in range(8):
		var marker_offset := 1500.0 if i == 7 else float(20 + i * 20)
		markers.append(_marker(
			vehicle_pos + Vector3(marker_offset, 0, 0),
			["amb", "amb", "amb", "amb"],
			{"amb": [_layer(2000)]}))
	audio.set_markers(markers)
	var idle := {
		"source_spawn_id": 77,
		"handle": 0x10001,
		"source_bms_id": 42,
		"lane": 0,
		"lifetime": 30,
		"pitch_q16": 0x10000,
		"volume_q8_8": 0xFFFF,
		"slot": 0,
		"set": "V_TRUCK_ILP",
		"pos": vehicle_pos,
	}
	audio.apply_sound_emitters([idle])
	audio.tick(vehicle_pos, 0.2)

	assert_eq(_players(container).size(), NovaMissionAudioScript.MIX_CHANNELS,
		"the vehicle voice competes inside the same bounded emitter pool")
	var voice := _player_at_position(container, vehicle_pos)
	assert_not_null(voice, "the full-gain idle voice displaces a quieter ambient marker")
	if voice == null:
		audio.teardown()
		_remove_dir_recursive(fixture_dir)
		return
	var first_stream := voice.stream
	assert_true(voice.playing)
	assert_eq((voice.stream as AudioStreamWAV).loop_mode,
		AudioStreamWAV.LOOP_FORWARD, "the engine emitter is a persistent loop")
	assert_almost_eq(voice.pitch_scale, 1.0, 0.0001)
	assert_almost_eq(voice.volume_db, linear_to_db(252.0 / 255.0), 0.001,
		"Q8.8 full gain enters the witnessed emitter volume curve")

	# A per-tick refresh of the same (source lifetime, lane) updates its live
	# controls and pose without rebinding/restarting its stream.
	var refreshed := idle.duplicate()
	var refreshed_pos := vehicle_pos + Vector3(1, 0, 0)
	refreshed["pos"] = refreshed_pos
	refreshed["pitch_q16"] = 0xC000
	refreshed["volume_q8_8"] = 0x8000
	audio.apply_sound_emitters([refreshed])
	audio.tick(refreshed_pos, 0.2)
	assert_eq(_player_at_position(container, refreshed_pos), voice)
	assert_eq(voice.stream, first_stream,
		"a keyed refresh preserves the incumbent playback")
	assert_almost_eq(voice.pitch_scale, 0.75, 0.0001)
	assert_almost_eq(voice.volume_db, linear_to_db(125.0 / 255.0), 0.001)

	var clear := refreshed.duplicate()
	clear["pitch_q16"] = 0
	clear["volume_q8_8"] = 0
	audio.apply_sound_emitters([clear])
	audio.tick(refreshed_pos, 0.2)
	assert_null(_player_at_position(container, refreshed_pos),
		"the zeroed source/lane update removes the vehicle emitter immediately")
	assert_eq(_players(container).size(), NovaMissionAudioScript.MIX_CHANNELS,
		"the released channel is reused by the displaced ambient marker")

	audio.teardown()
	_remove_dir_recursive(fixture_dir)


func test_dynamic_emitter_catchup_uses_producer_tick_and_recycles_identity() -> void:
	var fixture_dir := OS.get_cache_dir().path_join(
		"mission_audio_vehicle_catchup_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(fixture_dir), OK)
	_write_bytes(fixture_dir.path_join("tone.wav"),
		FileAccess.get_file_as_bytes(
			ProjectSettings.globalize_path(
				"res://../fixtures/menu_sound/selecta1.wav")))
	var lwf := NovaLwfData.new()
	lwf.create_empty()
	_add_lwf_set(lwf, "V_TRUCK_ILP", "tone.wav", 2000)
	assert_eq(lwf.save_file(fixture_dir.path_join("game.LWF")), OK)

	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(fixture_dir), OK)
	var mission := NovaMissionData.new()
	assert_eq(mission.create_default(), OK)
	var container := Node3D.new()
	add_child_autofree(container)
	var audio = NovaMissionAudioScript.new(root, null)
	audio.setup(mission, "vehicle_catchup.bms", container)

	var idle := {
		"source_spawn_id": 77,
		"source_bms_id": 42,
		"emitted_tick": 1,
		"lane": 0,
		"lifetime": 30,
		"pitch_q16": 0x10000,
		"volume_q8_8": 0xFFFF,
		"set": "V_TRUCK_ILP",
		"pos": Vector3(40, 0, 0),
	}
	audio.apply_sound_emitters([idle])
	# One render frame catches up 32 world ticks. The registration must retain
	# tick 1 as its refresh time rather than being reborn at the final tick.
	audio.advance_ticks(32)
	audio.tick(Vector3.ZERO)
	var expiring_ids := _active_ids(container)
	assert_eq(expiring_ids.size(), 1,
		"the slot is serviced once on the tick its lifetime reaches zero")
	var first_id := int(expiring_ids[0]) if expiring_ids.size() == 1 else -1

	# The next same-clock mix releases the zero-lifetime slot and its physical
	# channel. Only then can a later lane reuse the float-packed identity.
	audio.tick(Vector3.ZERO)
	assert_null(_active_player(container, first_id))

	var replacement := idle.duplicate()
	replacement["source_spawn_id"] = 88
	replacement["emitted_tick"] = 33
	replacement["pos"] = Vector3(80, 0, 0)
	audio.apply_sound_emitters([replacement])
	audio.advance_ticks(33)
	audio.tick(Vector3.ZERO)
	assert_not_null(_active_player(container, first_id),
		"retired dynamic IDs stay float-exact by recycling after channel release")

	audio.teardown()
	_remove_dir_recursive(fixture_dir)


func test_setup_dispatches_envs_items_across_entity_kinds() -> void:
	var fixture_dir := OS.get_cache_dir().path_join("mission_audio_envs_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(fixture_dir)
	var items := """begin "Env building"
  id 100001
  type decoration
  move_function envs
  soundloop_1 BUILD_AMB
  soundloop_2 BUILD_AMB
  soundloop_3 BUILD_AMB
  soundloop_4 BUILD_AMB
end

begin "Non-env marker"
  id 100002
  type marker
  soundloop_1 MISSING_AMB
  soundloop_2 MISSING_AMB
  soundloop_3 MISSING_AMB
  soundloop_4 MISSING_AMB
end
"""
	_write_text(fixture_dir.path_join("items.def"), items)
	_write_bytes(fixture_dir.path_join("tone.wav"),
		FileAccess.get_file_as_bytes(ProjectSettings.globalize_path("res://../fixtures/menu_sound/selecta1.wav")))
	var lwf := NovaLwfData.new()
	lwf.create_empty()
	var si := lwf.add_set()
	lwf.set_set_field(si, "name", "BUILD_AMB")
	var li := lwf.add_layer(si)
	lwf.set_layer_field(si, li, "falloff_radius", 200)
	var mi := lwf.add_member(si, li)
	lwf.set_member_field(si, li, mi, "wav_path", "tone.wav")
	assert_eq(lwf.save_file(fixture_dir.path_join("probe.LWF")), OK)

	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(fixture_dir), OK)
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load_from_resource_root(root, "items.def"), OK)
	var mission := NovaMissionData.new()
	mission.create_default()
	var env_building := mission.add_entity(
		NovaMissionData.KIND_BUILDING, 100001, Vector3(10, 0, 0), Vector3.ZERO)
	mission.add_entity(NovaMissionData.KIND_MARKER, 100002, Vector3(20, 0, 0), Vector3.ZERO)
	var container := Node3D.new()
	add_child_autofree(container)
	var audio = NovaMissionAudioScript.new(root, item_db)
	var provider := OcclusionProviderStub.new()
	audio.set_simulation(provider)
	var stats: Dictionary = audio.setup(mission, "probe.bms", container)

	assert_eq(int(stats.get("markers_total", 0)), 1,
		"only the items.def envs class participates, regardless of BMS entity kind")
	assert_eq(int(stats.get("markers_resolved", 0)), 1,
		"an envs decoration/building resolves its ambient soundloop")
	assert_gt(int(stats.get("ambient_candidates", 0)), 0)
	assert_eq(int(stats.get("physical_channels", -1)), 0)
	assert_eq(_players(container).size(), 0,
		"mission setup stores marker/layer data without creating candidate nodes")
	audio.tick(Vector3.ZERO, 0.2)
	assert_lte(_players(container).size(), NovaMissionAudioScript.MIX_CHANNELS)
	assert_eq(int(audio.get_stats().get("physical_channels", -1)), _players(container).size())
	assert_eq(provider.source_bms_ids, [int(env_building.get("bms_id", 0))],
		"setup retains the authored emitter identity through the ambient LOS call")
	audio.teardown()
	_remove_dir_recursive(fixture_dir)


func test_lazy_decode_failure_is_counted_and_falls_through_to_playable_candidate() -> void:
	var fixture_dir := OS.get_cache_dir().path_join(
		"mission_audio_bad_wav_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(fixture_dir)
	var items := """begin "Bad ambient"
  id 100001
  type decoration
  move_function envs
  soundloop_1 BAD_AMB
  soundloop_2 BAD_AMB
  soundloop_3 BAD_AMB
  soundloop_4 BAD_AMB
end

begin "Good ambient"
  id 100002
  type decoration
  move_function envs
  soundloop_1 GOOD_AMB
  soundloop_2 GOOD_AMB
  soundloop_3 GOOD_AMB
  soundloop_4 GOOD_AMB
end
"""
	_write_text(fixture_dir.path_join("items.def"), items)
	_write_bytes(fixture_dir.path_join("bad.wav"), PackedByteArray([1, 2, 3, 4]))
	_write_bytes(fixture_dir.path_join("good.wav"),
		FileAccess.get_file_as_bytes(
			ProjectSettings.globalize_path("res://../fixtures/menu_sound/selecta1.wav")))
	var lwf := NovaLwfData.new()
	lwf.create_empty()
	_add_lwf_set(lwf, "BAD_AMB", "bad.wav", 2000)
	_add_lwf_set(lwf, "GOOD_AMB", "good.wav", 2000)
	assert_eq(lwf.save_file(fixture_dir.path_join("probe.LWF")), OK)

	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(fixture_dir), OK)
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load_from_resource_root(root, "items.def"), OK)
	var mission := NovaMissionData.new()
	mission.create_default()
	mission.add_entity(
		NovaMissionData.KIND_BUILDING, 100001, Vector3(1, 0, 0), Vector3.ZERO)
	mission.add_entity(
		NovaMissionData.KIND_BUILDING, 100002, Vector3(10, 0, 0), Vector3.ZERO)
	var container := Node3D.new()
	add_child_autofree(container)
	var audio = NovaMissionAudioScript.new(root, item_db)
	var stats: Dictionary = audio.setup(mission, "probe.bms", container)

	assert_eq(int(stats.get("ambient_candidates", 0)), 2)
	assert_eq(int(stats.get("ambient_candidates_validated", -1)), 0,
		"setup remains descriptor-only and has not decoded either WAV")
	assert_eq(int(stats.get("ambient_decode_failures", -1)), 0)
	audio.tick(Vector3.ZERO, 0.2)
	assert_eq(int(stats.get("ambient_candidates_validated", -1)), 2)
	assert_eq(int(stats.get("ambient_decode_failures", -1)), 1,
		"the corrupt virtual candidate is visible in runtime stats")
	assert_eq(_players(container).size(), 1,
		"the next-ranked playable candidate receives the physical channel")
	audio.teardown()
	_remove_dir_recursive(fixture_dir)


func test_repeated_setup_clears_dialog_dbf_queue_and_wac_voice() -> void:
	var fixture_dir := OS.get_cache_dir().path_join(
		"mission_audio_reuse_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(fixture_dir)
	_write_bytes(fixture_dir.path_join("first.DBF"),
		FileAccess.get_file_as_bytes(
			ProjectSettings.globalize_path("res://../fixtures/dbf/00TRg.DBF")))
	_write_bytes(fixture_dir.path_join("tone.wav"),
		FileAccess.get_file_as_bytes(
			ProjectSettings.globalize_path("res://../fixtures/menu_sound/selecta1.wav")))
	var lwf := NovaLwfData.new()
	lwf.create_empty()
	_add_lwf_set(lwf, "Z00gR100", "tone.wav", 200)
	assert_eq(lwf.save_file(fixture_dir.path_join("game.LWF")), OK)

	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(fixture_dir), OK)
	var mission := NovaMissionData.new()
	mission.create_default()
	var container := Node3D.new()
	add_child_autofree(container)
	var audio = NovaMissionAudioScript.new(root, null)
	audio.setup(mission, "first.bms", container)
	assert_eq(audio.resolve_dialog_set(1), "Z00gR100")
	assert_true(audio.play_dialog(1))
	var old_dialog: AudioStreamPlayer = audio.dialog_voice()
	assert_not_null(old_dialog)
	assert_true(audio.play_dialog(1), "a second line is queued behind the active voice")
	assert_true(audio.play_wac_wave("tone.wav"))
	var old_wac: AudioStreamPlayer = null
	for value in container.find_children("*", "AudioStreamPlayer", true, false):
		var player := value as AudioStreamPlayer
		if player != old_dialog:
			old_wac = player
			break
	assert_not_null(old_wac)

	audio.setup(mission, "second.bms", container)
	assert_null(audio.dialog_voice(), "the old mission's active dialog is released")
	assert_eq(audio.resolve_dialog_set(1), "",
		"a mission without a DBF cannot retain the previous mission's dialog mapping")
	if old_dialog != null:
		assert_false(old_dialog.playing)
		old_dialog.finished.emit()
	assert_null(audio.dialog_voice(),
		"a late finished signal cannot pump the previous mission's queued dialog")
	if old_wac != null:
		assert_false(old_wac.playing, "the previous mission's WAC channel is stopped")
	audio.teardown()
	_remove_dir_recursive(fixture_dir)


func test_teardown_removes_the_mission_reverb_from_the_ambient_bus() -> void:
	var fixture_dir := OS.get_cache_dir().path_join("mission_audio_reverb_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(fixture_dir)
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(fixture_dir), OK)
	var mission := NovaMissionData.new()
	mission.create_default()
	mission.set_header_int("reverb", 1)
	var container := Node3D.new()
	add_child_autofree(container)
	var audio = NovaMissionAudioScript.new(root, null)
	var ambient_bus := AudioServer.get_bus_index(&"Ambient")
	assert_gte(ambient_bus, 0)
	audio.setup(mission, "reverb_probe.bms", container)
	assert_eq(_reverb_count(ambient_bus), 1, "mission setup installs its Ambient reverb")

	audio.teardown()
	assert_eq(_reverb_count(ambient_bus), 0,
		"unloading the mission cannot leave its global bus effect in the menu/next world")
	_remove_dir_recursive(fixture_dir)


func _write_text(path: String, value: String) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file)
	if file != null:
		file.store_string(value)


func _write_bytes(path: String, value: PackedByteArray) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file)
	if file != null:
		file.store_buffer(value)


func _add_lwf_set(
		lwf: NovaLwfData, set_name: String, wav_path: String,
		falloff_radius: int) -> void:
	var set_i := lwf.add_set()
	lwf.set_set_field(set_i, "name", set_name)
	var layer_i := lwf.add_layer(set_i)
	lwf.set_layer_field(set_i, layer_i, "falloff_radius", falloff_radius)
	var member_i := lwf.add_member(set_i, layer_i)
	lwf.set_member_field(set_i, layer_i, member_i, "wav_path", wav_path)


func _reverb_count(bus_idx: int) -> int:
	var count := 0
	for i in AudioServer.get_bus_effect_count(bus_idx):
		if AudioServer.get_bus_effect(bus_idx, i) is AudioEffectReverb:
			count += 1
	return count


func _remove_dir_recursive(path: String) -> void:
	var dir := DirAccess.open(path)
	if dir == null:
		return
	dir.list_dir_begin()
	var entry := dir.get_next()
	while entry != "":
		var child := path.path_join(entry)
		if dir.current_is_dir():
			_remove_dir_recursive(child)
		else:
			DirAccess.remove_absolute(child)
		entry = dir.get_next()
	dir.list_dir_end()
	DirAccess.remove_absolute(path)
