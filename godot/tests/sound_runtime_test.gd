extends GutTest

## Runtime audio tests: SoundBank name indexing/resolution (case-insensitive,
## the grilled name-keyed model) and WavLoader RIFF decode incl. the 8-bit
## unsigned -> signed conversion. Self-contained (no real game data required).



# A REAL ResourceRoot over a per-test temp dir (ADR 0034 typed seam): the
# in-memory wav bytes land as files, and the bank reads them through the same
# native VFS the runtime uses.
var _root_dirs: Array[String] = []


func after_each() -> void:
	for dir in _root_dirs:
		TestFs.remove_dir_recursive(dir)
	_root_dirs.clear()


func _real_root(files: Dictionary) -> ResourceRoot:
	var dir := OS.get_temp_dir().replace("\\", "/") + 			"/opennova_sound_rt_%d_%d" % [Time.get_ticks_usec(), _root_dirs.size()]
	assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	_root_dirs.append(dir)
	for name in files.keys():
		var file := FileAccess.open(dir.path_join(String(name)), FileAccess.WRITE)
		assert_not_null(file)
		if file != null:
			file.store_buffer(files[name])
			file.close()
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(dir), OK)
	return root


func _profile_with_set(set_name: String, wav: String) -> LwfData:
	var d := LwfData.new()
	d.create_empty()
	var si := d.add_set()
	d.set_set_field(si, "name", set_name)
	var li := d.add_layer(si)
	d.set_layer_field(si, li, "internal", true)
	d.set_layer_field(si, li, "external", true)
	var mi := d.add_member(si, li)
	d.set_member_field(si, li, mi, "wav_path", wav)
	return d


func test_menu_audio_routes_its_pooled_players_to_sfx() -> void:
	var samples := PackedByteArray()
	samples.resize(32)
	var root := _real_root({
		"click.wav": _build_wav(samples, 1, 22050, 16),
		"menu.lwf": _profile_with_set("CLICK_SELECT", "click.wav").to_bytes(),
	})
	var audio := MenuAudio.new()
	add_child_autofree(audio)
	audio.set_resource_root(root)

	assert_true(audio.play_widget_sound("CLICK_SELECT", "menu.lwf"),
			"the element's bank plays its trigger on a pooled voice")
	var player := audio.get_node_or_null("_MenuSound0") as AudioStreamPlayer
	assert_not_null(player)
	if player != null:
		assert_eq(player.bus, StringName("SFX"),
				"menu hover/click voices obey the shared sound-FX option")


# A <SOUND> whose bank does not open, or that names none, plays nothing: the
# game's parse leaves the row no bank and its play site consults no other
# (engine/runtime/menu/menu_sound.h carries the witness).
func test_menu_sound_without_its_bank_plays_nothing() -> void:
	var samples := PackedByteArray()
	samples.resize(32)
	var root := _real_root({
		"click.wav": _build_wav(samples, 1, 22050, 16),
		"menu.lwf": _profile_with_set("CLICK_SELECT", "click.wav").to_bytes(),
	})
	var audio := MenuAudio.new()
	add_child_autofree(audio)
	audio.set_resource_root(root)

	assert_false(audio.play_widget_sound("CLICK_SELECT", "missing.lwf"),
			"a bank that does not open plays nothing, menu.lwf in the root or not")
	assert_false(audio.play_widget_sound("CLICK_SELECT", ""),
			"a file-less element plays nothing")
	assert_null(audio.get_node_or_null("_MenuSound0"), "no voice was made")
	assert_true(audio.play_widget_sound("CLICK_SELECT", "menu.lwf"),
			"the bank the element names plays")


func test_bank_indexing_case_insensitive() -> void:
	var bank = SoundBank.create(null)
	bank.add_bank(_profile_with_set("Z00AMB1", "Z00aR100.wav"))
	assert_true(bank.has_set("Z00AMB1"), "exact name resolves")
	assert_true(bank.has_set("z00amb1"), "lookup is case-insensitive")
	assert_false(bank.has_set("NOPE"), "unknown set does not resolve")
	assert_true("z00amb1" in bank.get_set_names())


func test_spawn_unknown_or_unresolvable_returns_null() -> void:
	var bank = SoundBank.create(null)  # no resource root -> no wav decode
	bank.add_bank(_profile_with_set("Z00AMB1", "Z00aR100.wav"))
	var parent := Node3D.new()
	add_child_autofree(parent)
	assert_null(bank.spawn_ambient(parent, Vector3.ZERO, "NOPE", &"Ambient"),
		"unknown set yields no voice")
	assert_null(bank.spawn_ambient(parent, Vector3.ZERO, "Z00AMB1", &"Ambient"),
		"known set with no resolvable .wav yields no voice (graceful)")


func test_spawn_ambient_loops_the_full_decoded_stream() -> void:
	# Regression: AudioStreamWAV.loop_end is an absolute frame index, not
	# "0 = whole stream". With LOOP_FORWARD and loop_end 0, playback wraps at
	# sample 0 forever, so every looping "snd:" ambient marker voice in-game was
	# a constant sample-0 value — silence.
	var samples := PackedByteArray()
	samples.resize(32)  # 16 mono 16-bit frames
	var root := _real_root({"z00ar100.wav": _build_wav(samples, 1, 22050, 16)})
	var bank = SoundBank.create(root)
	bank.add_bank(_profile_with_set("Z00AMB1", "Z00aR100.wav"))
	var parent := Node3D.new()
	add_child_autofree(parent)
	var holder: Node3D = bank.spawn_ambient(parent, Vector3(1.0, 2.0, 3.0), "Z00AMB1", &"Ambient")
	assert_not_null(holder, "resolvable set spawns a voice holder")
	if holder == null:
		return
	var player: AudioStreamPlayer3D = null
	for child in holder.get_children():
		if child is AudioStreamPlayer3D:
			player = child
	assert_not_null(player, "holder carries an AudioStreamPlayer3D voice")
	if player == null:
		return
	assert_eq(player.process_mode, Node.PROCESS_MODE_DISABLED,
		"a dormant ambient candidate does not run internal spatial-audio physics")
	var s: AudioStreamWAV = player.stream
	assert_eq(s.loop_mode, AudioStreamWAV.LOOP_FORWARD, "ambient voice loops")
	assert_eq(s.loop_begin, 0)
	assert_eq(s.loop_end, 16, "loop region spans the full decoded stream")


func test_ambient_description_defers_and_caches_wav_decode() -> void:
	var samples := PackedByteArray()
	samples.resize(32)
	var root := _real_root({"z00ar100.wav": _build_wav(samples, 1, 22050, 16)})
	var bank = SoundBank.create(root)
	bank.add_bank(_profile_with_set("Z00AMB1", "Z00aR100.wav"))

	var descriptors: Array = bank.describe_ambient("Z00AMB1")
	assert_eq(descriptors.size(), 1)
	var first: AudioStreamWAV = bank.resolve_ambient_stream(descriptors[0])
	assert_not_null(first, "the first selected candidate resolves lazily")
	var second: AudioStreamWAV = bank.resolve_ambient_stream(descriptors[0])
	assert_same(first, second,
		"the bank cache serves the same decoded stream instead of a second VFS read")


# --- The witnessed distance-volume curve [orig: SoundBank_CalcDistanceVolPan
# @ 0x75ca20]: vol * (255/256) * (1 - d/r)^2, integer-exact, hard 0 at d >= r,
# ceilinged by clamp_volume. Expectations are hand-computed from the formula.

func test_calc_distance_volume_curve() -> void:
	var S := SoundBank
	# d = 0: inv = 0xFFFF -> ((255*255)>>8) * 0xFFFF^2 >> 32 = 253.
	assert_eq(S.calc_distance_volume(0, 200 << 16, 255, 255), 253, "full volume at the emitter")
	# d = r/2: inv = 0x7FFF -> quadratic quarter -> 63.
	assert_eq(S.calc_distance_volume(100 << 16, 200 << 16, 255, 255), 63, "half distance = quarter volume")
	# At and beyond the radius: hard silent [orig: 0x75ca31].
	assert_eq(S.calc_distance_volume(200 << 16, 200 << 16, 255, 255), 0)
	assert_eq(S.calc_distance_volume(300 << 16, 200 << 16, 255, 255), 0)
	# clamp_volume ceilings the result [orig: 0x75ca65].
	assert_eq(S.calc_distance_volume(0, 200 << 16, 255, 100), 100, "clamp_volume ceiling")
	# Lower member volume scales in before the curve.
	assert_eq(S.calc_distance_volume(0, 200 << 16, 128, 255), 126)
	# A zero radius is silent, not a division.
	assert_eq(S.calc_distance_volume(0, 0, 255, 255), 0)


func test_emitter_layer_volume_two_radius_model() -> void:
	var S := SoundBank
	# Bare falloff radius [orig: SoundEmitter_UpdateAndMixTop8 @ 0x5286df]:
	# vol_in = (255*255)>>8 = 254 -> d=0 gives 252, half gives 63.
	assert_eq(S.emitter_layer_volume(0, 200, 0, 255, 255, 255), 252)
	assert_eq(S.emitter_layer_volume(100 << 16, 200, 0, 255, 255, 255), 63)
	assert_eq(S.emitter_layer_volume(200 << 16, 200, 0, 255, 255, 255), 0)
	# min_distance rebases the falloff to run min..falloff [orig: @ 0x5286b9]:
	# at d = min the curve is at its peak, half-way through gives the quarter.
	assert_eq(S.emitter_layer_volume(50 << 16, 200, 50, 255, 255, 255), 252)
	assert_eq(S.emitter_layer_volume(125 << 16, 200, 50, 255, 255, 255), 63)
	# Inside min_distance volume RISES as (d/min)^2 — the proximity fade
	# [orig: @ 0x528691]: at half min it is a quarter.
	assert_eq(S.emitter_layer_volume(25 << 16, 200, 50, 255, 255, 255), 63)
	# Fractional distance in the proximity arm: the original subtracts in Q16
	# FIRST then truncates — ((min<<16) - d) >> 16 = floor(min - d), NOT
	# min - floor(d) [orig: @ 0x528691]. d = 25.5, min = 50 -> curve distance
	# 24 (floor(24.5)) -> 68; the un-witnessed form (50 - 25 = 25) gave 63.
	assert_eq(S.emitter_layer_volume(25 * 65536 + 32768, 200, 50, 255, 255, 255), 68)
	# The blend byte (time-of-day crossfade) scales member volume and clamp.
	assert_eq(S.emitter_layer_volume(0, 200, 0, 128, 255, 255), 125)
	# Both radii zero: silent as a looping emitter [orig: @ 0x528704].
	assert_eq(S.emitter_layer_volume(0, 0, 0, 255, 255, 255), 0)


# One layer descriptor (the layer's two radii + member 0's volume/clamp).
func _ambient_layer(falloff: int, min_dist: int, volume: int, clamp_vol: int) -> AmbientLayer:
	var layer := AmbientLayer.new()
	layer.falloff_radius = falloff
	layer.min_distance = min_dist
	layer.volume = volume
	layer.clamp_volume = clamp_vol
	return layer


func test_oneshot_distance_volume_is_not_rebased() -> void:
	var bank = SoundBank.create(null)
	var layer := _ambient_layer(200, 0, 255, 255)
	# One-shots run the plain falloff over 0..r [orig: SoundBank_PlayTriggerEntries
	# @ 0x75cf75]: half distance = quarter volume of the 254-scaled input.
	assert_eq(bank.oneshot_distance_volume(100 << 16, layer), 63)
	assert_eq(bank.oneshot_distance_volume(0, layer), 253)
	assert_eq(bank.oneshot_distance_volume(200 << 16, layer), 0)


func test_oneshot_no_falloff_plays_at_emitter_volume() -> void:
	var bank = SoundBank.create(null)
	# A layer with NO falloff radius plays at the RAW emitter volume — the
	# member volume is not consulted [orig: SoundBank_PlayTriggerEntries
	# @ 0x75cf88 stores emitter_info[2], reimpl emitter = full 255].
	var quiet := _ambient_layer(0, 0, 100, 255)
	assert_eq(bank.oneshot_distance_volume(500 << 16, quiet), 255)
	# A min-only layer computes the proximity stage but the no-falloff branch
	# DISCARDS it with the member volume [orig: @ 0x75cf88] (no JOX layer
	# ships min-only; pinned for the witnessed form).
	var min_only := _ambient_layer(0, 50, 100, 255)
	assert_eq(bank.oneshot_distance_volume(25 << 16, min_only), 255)


func test_zero_range_oneshot_only_fires_at_the_exact_source() -> void:
	var samples := PackedByteArray()
	samples.resize(32)
	var root := _real_root({"tone.wav": _build_wav(samples, 1, 22050, 16)})
	var profile := _profile_with_set("POINT_ONLY", "tone.wav")
	profile.set_set_field(0, "target_id", 0)
	var bank = SoundBank.create(root)
	bank.add_bank(profile)
	var parent := Node3D.new()
	add_child_autofree(parent)

	assert_false(bank.play_oneshot_3d(parent, Vector3(1, 0, 0), "POINT_ONLY", StringName(), Vector3.ZERO),
		"retail's dist <= range gate rejects every nonzero distance when range is zero")
	assert_true(bank.play_oneshot_3d(parent, Vector3.ZERO, "POINT_ONLY", StringName(), Vector3.ZERO),
		"equality passes, so a zero-range set can still fire at its exact source")


func test_mission_retry_keeps_the_sequential_member_cursor() -> void:
	# Stop/Start stops the bank's voices but never reloads the bank: retail's
	# round restart re-enters Game_StartMission, whose bank loop returns early
	# on a loaded slot [orig: SoundBank_OpenFile @ 0x75caa5], so a sequential
	# (0x10) layer's cursor (playlist word +2) carries on where it left off.
	var samples := PackedByteArray()
	samples.resize(32)
	var root := _real_root({
		"a.wav": _build_wav(samples, 1, 22050, 16),
		"b.wav": _build_wav(samples, 1, 22050, 16),
		"c.wav": _build_wav(samples, 1, 22050, 16),
	})
	var profile := _profile_with_set("SEQ", "a.wav")
	profile.set_layer_field(0, 0, "selection_mode", LwfData.SELECTION_SEQUENTIAL)
	for wav in ["b.wav", "c.wav"]:
		var mi := profile.add_member(0, 0)
		profile.set_member_field(0, 0, mi, "wav_path", wav)
	var bank = SoundBank.create(root)
	bank.add_bank(profile)
	var parent := Node3D.new()
	add_child_autofree(parent)
	var streams: Array = []
	for _fire in range(2):
		assert_true(bank.play_oneshot_3d(parent, Vector3.ZERO, "SEQ", StringName(), Vector3.ZERO))
		var voice := parent.get_child(parent.get_child_count() - 1) as AudioStreamPlayer3D
		streams.append(voice.stream)
	assert_ne(streams[0], streams[1], "a sequential layer walks its members in order")
	var first_voices := parent.get_children()
	bank.reset_oneshots(parent)
	for voice in first_voices:
		assert_true(voice.is_queued_for_deletion(), "the retry stops the bank's live voices")
	assert_true(bank.play_oneshot_3d(parent, Vector3.ZERO, "SEQ", StringName(), Vector3.ZERO))
	var third := (parent.get_child(parent.get_child_count() - 1) as AudioStreamPlayer3D).stream
	assert_ne(third, streams[0], "the cursor survives the retry: the third member, not the first")
	assert_ne(third, streams[1])


func test_oneshot_occlusion_distance_drives_fire_volume() -> void:
	# Raw distance is 50u, but the witnessed two-ray result inflates it to 100u.
	# With a 200u falloff, that is the pinned half-range volume byte 63.
	var samples := PackedByteArray()
	samples.resize(32)
	var root := _real_root({"tone.wav": _build_wav(samples, 1, 22050, 16)})
	var profile := _profile_with_set("OCCLUDED", "tone.wav")
	profile.set_set_field(0, "target_id", 200)
	profile.set_layer_field(0, 0, "falloff_radius", 200)
	var provider := OcclusionRecorder.new(100 << 16)
	var bank = SoundBank.create(root)
	bank.set_occlusion_override(provider.occlude)
	bank.add_bank(profile)
	var parent := Node3D.new()
	add_child_autofree(parent)

	assert_true(bank.play_oneshot_3d(
		parent, Vector3(50, 0, 0), "OCCLUDED", StringName(), Vector3.ZERO, 321))
	assert_eq(provider.calls, 1, "one fire asks for one occluded distance")
	assert_eq(provider.source_bms_ids, [321], "one-shot occlusion keeps source identity")
	var voice := parent.get_child(0) as AudioStreamPlayer3D
	assert_not_null(voice)
	if voice != null:
		assert_almost_eq(voice.volume_db, -12.14399, 0.001,
			"the inflated distance, not raw 50u, feeds fire-time volume")


func test_oneshot_occlusion_distance_rechecks_set_cull_range() -> void:
	# Raw 100u passes the set's 120u cull. Occlusion inflates it to 130u,
	# so the witnessed post-LOS range recheck rejects the voice.
	var samples := PackedByteArray()
	samples.resize(32)
	var root := _real_root({"tone.wav": _build_wav(samples, 1, 22050, 16)})
	var profile := _profile_with_set("OCCLUDED_CULL", "tone.wav")
	profile.set_set_field(0, "target_id", 120)
	profile.set_layer_field(0, 0, "falloff_radius", 200)
	var provider := OcclusionRecorder.new(130 << 16)
	var bank = SoundBank.create(root)
	bank.set_occlusion_override(provider.occlude)
	bank.add_bank(profile)
	var parent := Node3D.new()
	add_child_autofree(parent)

	assert_false(bank.play_oneshot_3d(
		parent, Vector3(100, 0, 0), "OCCLUDED_CULL", StringName(), Vector3.ZERO))
	assert_eq(provider.calls, 1, "raw-in-range fire reaches the occlusion query")
	assert_eq(parent.get_child_count(), 0, "inflation beyond set range spawns no voice")
func test_wav_loader_decodes_pcm8_unsigned() -> void:
	# Minimal 8-bit unsigned mono 22050 Hz PCM WAV with 4 samples.
	var samples := PackedByteArray([0x80, 0x00, 0xFF, 0x80])  # center, min, max, center
	var wav := _build_wav(samples, 1, 22050, 8)
	var stream := WavLoader.from_bytes(wav)
	assert_not_null(stream, "PCM8 WAV decodes")
	# 8-bit input is upconverted to signed 16-bit (matches Godot's own .wav importer).
	assert_eq(stream.format, AudioStreamWAV.FORMAT_16_BITS)
	assert_eq(stream.mix_rate, 22050)
	assert_false(stream.stereo)
	var data := stream.data
	assert_eq(data.size(), 8, "4 mono samples x 2 bytes each (16-bit)")
	# sample16 = (u - 128) << 8: 0x80 -> 0 (silence), 0x00 -> -32768, 0xFF -> +32512.
	assert_eq(data.decode_s16(0), 0, "0x80 unsigned (silence) -> 0")
	assert_eq(data.decode_s16(2), -32768, "0x00 unsigned -> minimum")
	assert_eq(data.decode_s16(4), 32512, "0xFF unsigned -> 0x7F00")
	assert_eq(data.decode_s16(6), 0, "0x80 unsigned (silence) -> 0")


func test_wav_loader_rejects_non_riff() -> void:
	assert_null(WavLoader.from_bytes(PackedByteArray([1, 2, 3, 4])), "garbage is rejected")


func test_wav_loader_decodes_ima_adpcm() -> void:
	# One mono IMA-ADPCM block: predictor=1000, step index 0, then a 4-byte word of
	# zero-nibbles. At step index 0 a zero nibble adds 0, so every sample stays 1000.
	var wav := _build_ima_wav(1000, 0, PackedByteArray([0, 0, 0, 0]), 11025, 8)
	var stream := WavLoader.from_bytes(wav)
	assert_not_null(stream, "IMA-ADPCM WAV decodes")
	assert_eq(stream.format, AudioStreamWAV.FORMAT_16_BITS, "ADPCM is decoded to 16-bit PCM")
	assert_eq(stream.mix_rate, 11025)
	assert_false(stream.stereo)
	var d := stream.data
	# 1 header predictor sample + 8 nibble samples = 9 samples x 2 bytes.
	assert_eq(d.size(), 18, "9 mono 16-bit samples")
	assert_eq(d.decode_s16(0), 1000, "first sample is the block predictor")
	assert_eq(d.decode_s16(2), 1000, "zero nibble at step 0 keeps the predictor flat")
	assert_eq(d.decode_s16(16), 1000)


# Build a minimal one-block mono IMA-ADPCM WAV (audioFormat 0x11) with the
# `fact` chunk the game's wave loader requires of 4-bit samples (D-SND-43):
# its count is the block's frames, the predictor and two per nibble byte.
func _build_ima_wav(predictor: int, step_index: int, nibble_bytes: PackedByteArray, rate: int, block_align: int) -> PackedByteArray:
	var blk := StreamPeerBuffer.new()
	blk.big_endian = false
	blk.put_16(predictor)      # int16 LE predictor
	blk.put_u8(step_index)
	blk.put_u8(0)              # reserved
	blk.put_data(nibble_bytes)
	var data := blk.data_array
	var buf := StreamPeerBuffer.new()
	buf.big_endian = false
	buf.put_data("RIFF".to_ascii_buffer())
	buf.put_u32(48 + data.size())
	buf.put_data("WAVE".to_ascii_buffer())
	buf.put_data("fmt ".to_ascii_buffer())
	buf.put_u32(16)
	buf.put_u16(0x11)          # IMA ADPCM
	buf.put_u16(1)             # mono
	buf.put_u32(rate)
	buf.put_u32(rate)          # byteRate (loader ignores)
	buf.put_u16(block_align)
	buf.put_u16(4)             # bits per sample
	buf.put_data("fact".to_ascii_buffer())
	buf.put_u32(4)
	buf.put_u32(1 + 2 * nibble_bytes.size())  # sample count
	buf.put_data("data".to_ascii_buffer())
	buf.put_u32(data.size())
	buf.put_data(data)
	return buf.data_array


# Build a minimal RIFF/WAVE PCM container around `samples`.
func _build_wav(samples: PackedByteArray, channels: int, rate: int, bits: int) -> PackedByteArray:
	var data_size := samples.size()
	var byte_rate := rate * channels * (bits / 8)
	var block_align := channels * (bits / 8)
	var buf := StreamPeerBuffer.new()
	buf.big_endian = false
	buf.put_data("RIFF".to_ascii_buffer())
	buf.put_u32(36 + data_size)
	buf.put_data("WAVE".to_ascii_buffer())
	buf.put_data("fmt ".to_ascii_buffer())
	buf.put_u32(16)
	buf.put_u16(1)            # PCM
	buf.put_u16(channels)
	buf.put_u32(rate)
	buf.put_u32(byte_rate)
	buf.put_u16(block_align)
	buf.put_u16(bits)
	buf.put_data("data".to_ascii_buffer())
	buf.put_u32(data_size)
	buf.put_data(samples)
	return buf.data_array


func test_oneshots_share_fourteen_unreserved_channels_and_apply_pitch() -> void:
	var samples := PackedByteArray()
	samples.resize(22050 * 2 * 5)
	var root := _real_root({"pool.wav": _build_wav(samples, 1, 22050, 16)})
	var profile := _profile_with_set("POOL", "pool.wav")
	profile.set_set_field(0, "pitch_base", 98304)
	profile.set_set_field(0, "pitch_random_range", 32768)
	profile.set_member_field(0, 0, 0, "base_pitch", 1.0)
	profile.set_member_field(0, 0, 0, "rand_pitch", 0.125)
	var bank = SoundBank.create(root)
	bank.add_bank(profile)
	var parent := Node3D.new()
	add_child_autofree(parent)
	for _i in range(14):
		assert_true(bank.play_oneshot_3d(parent, Vector3.ZERO, "POOL", StringName(), Vector3.ZERO))
	var first := parent.get_child(0) as AudioStreamPlayer3D
	assert_almost_eq(first.pitch_scale, 110686.0 / 65536.0, 0.00001,
			"set and member pitch jitter reach the physical voice")
	assert_true(bank.play_oneshot_3d(parent, Vector3.ZERO, "POOL", StringName(), Vector3.ZERO))
	assert_true(first.is_queued_for_deletion(), "the fifteenth voice steals the first tied slot")
	var live := 0
	for child in parent.get_children():
		if child is AudioStreamPlayer3D and not child.is_queued_for_deletion():
			live += 1
	assert_eq(live, 14, "channels 0 through 11 remain reserved in the 26-channel device")


# The game's own AUD1 buffer: the sample count, the Q16 pitch ratio, the width
# byte (2 = 16-bit) and its signed samples (engine/formats/lwf/wav_pcm.cpp).
func _build_aud1(count: int, pitch_q16: int) -> PackedByteArray:
	var buf := StreamPeerBuffer.new()
	buf.big_endian = false
	buf.put_data("AUD1".to_ascii_buffer())
	buf.put_u32(count)
	buf.put_u32(pitch_q16)
	buf.put_u32(2)
	for _i in range(count):
		buf.put_16(0)
	return buf.data_array


# A wave of pitch 0 plays at the mixer's least step whatever the voice pitch:
# the step composes the play factor with the wave's pitch, so a pitch of 0 is
# forced to the least step (44100 / 512, the stream's 86 Hz) and the voice's
# pitch never scales it, where a wave of its own rate takes the voice pitch
# (D-SND-49; opennova::lwf::wave_pitch_scale carries the witness).
func test_zero_pitch_wave_ignores_the_voice_pitch() -> void:
	var root := _real_root({
		"zero.wav": _build_aud1(64, 0),
		"rate.wav": _build_wav(PackedByteArray([0, 0, 0, 0]), 1, 22050, 16),
	})
	var bank = SoundBank.create(root)
	for pair in [["ZERO", "zero.wav"], ["RATE", "rate.wav"]]:
		var profile := _profile_with_set(pair[0], pair[1])
		profile.set_set_field(0, "pitch_base", 131072)  # the voice pitch 2.0
		profile.set_member_field(0, 0, 0, "base_pitch", 2.0)
		bank.add_bank(profile)
	var parent := Node3D.new()
	add_child_autofree(parent)
	assert_true(bank.play_oneshot_3d(parent, Vector3.ZERO, "ZERO", StringName(), Vector3.ZERO))
	assert_true(bank.play_oneshot_3d(parent, Vector3.ZERO, "RATE", StringName(), Vector3.ZERO))
	var zero := parent.get_child(0) as AudioStreamPlayer3D
	var rate := parent.get_child(1) as AudioStreamPlayer3D
	assert_eq((zero.stream as AudioStreamWAV).mix_rate, 86, "a pitch-0 wave's stream runs at the least step")
	assert_almost_eq(zero.pitch_scale, 1.0, 0.00001, "the one-shot's voice pitch never scales it")
	assert_almost_eq(rate.pitch_scale, 4.0, 0.00001, "a wave of its own rate takes the voice pitch")
	var holder: Node3D = bank.spawn_ambient(parent, Vector3.ZERO, "ZERO", &"Ambient")
	assert_not_null(holder)
	if holder != null:
		var ambient := holder.get_child(0) as AudioStreamPlayer3D
		assert_almost_eq(ambient.pitch_scale, 1.0, 0.00001, "nor does an ambient layer's member pitch")
		var looped := ambient.stream as WaveStream
		assert_not_null(looped, "the ambient loop copy stays a decoded wave's stream")
		if looped != null:
			assert_eq(looped.loader_pitch_q16, 0, "the copy keeps the loader pitch word the channel updates read")
			assert_eq(WavLoader.pitch_scale_for(looped, 3.0), 1.0, "so a channel update keeps the least step")
	var voice: AudioStreamPlayer = bank.spawn_oneshot_2d(parent, "ZERO", StringName())
	assert_not_null(voice)
	if voice != null:
		assert_almost_eq(voice.pitch_scale, 1.0, 0.00001, "nor a 2D voice's member pitch")


# A rate past INT32_MAX: the player's whole mix rate holds INT32_MAX, so the
# stream boxes its rate there and keeps its own, which every player scales the
# box back up to through WavLoader.pitch_scale_for (D-SND-52;
# opennova::lwf::wave_pitch_scale). An AUD1 pitch of 0xFFFFFFFF plays at
# (pitch * 44100 + 0x8000) >> 16 = 2890137599 Hz, a RIFF rate of 0x80000000 at
# its own; a RIFF rate from 0xAC440000 faults the game's loader, which ours
# refuses (D-SND-54).
func test_rate_past_int32_max_is_boxed() -> void:
	var aud := WavLoader.from_bytes(_build_aud1(4, 0xFFFFFFFF)) as WaveStream
	assert_not_null(aud)
	if aud != null:
		assert_eq(aud.mix_rate, 2147483647, "the stream's mix rate is boxed at INT32_MAX")
		assert_eq(aud.wave_rate, 2890137599, "the stream keeps its own rate")
		assert_almost_eq(aud.mix_rate * WavLoader.pitch_scale_for(aud, 1.0), 2890137599.0, 1.0,
				"its player plays at its own rate")
		assert_almost_eq(aud.mix_rate * WavLoader.pitch_scale_for(aud, 0.5), 1445068799.5, 1.0,
				"and takes the voice pitch on it")
		var looped := aud.duplicate() as WaveStream
		assert_eq(looped.wave_rate, 2890137599, "a loop copy keeps the rate its channel updates read")
	var samples := PackedByteArray([0, 0, 0, 0])
	var riff := WavLoader.from_bytes(_build_wav(samples, 1, 0x80000000, 16))
	assert_not_null(riff)
	if riff != null:
		assert_eq(riff.mix_rate, 2147483647)
		assert_almost_eq(riff.mix_rate * WavLoader.pitch_scale_for(riff, 1.0), 2147483648.0, 1.0)
	assert_null(WavLoader.from_bytes(_build_wav(samples, 1, 0xAC440000, 16)),
			"a RIFF rate from 0xAC440000 is refused where the game's loader faults")


func test_entity_refire_retakes_its_own_channel() -> void:
	# Retail keys the open call on the source entity: a channel already playing
	# the same wave for the same entity scores zero and is retaken, so a body
	# re-firing a wave restarts its own voice instead of stealing the quietest
	# other channel (docs/audio/lwf-dbf-sound-re.md D-SND-10).
	var samples := PackedByteArray()
	samples.resize(22050 * 2 * 5)
	var root := _real_root({
		"quiet.wav": _build_wav(samples, 1, 22050, 16),
		"loud.wav": _build_wav(samples, 1, 22050, 16),
	})
	var quiet := _profile_with_set("QUIET", "quiet.wav")
	quiet.set_layer_field(0, 0, "falloff_radius", 200)
	quiet.set_member_field(0, 0, 0, "volume", 10)
	var loud := _profile_with_set("LOUD", "loud.wav")
	loud.set_layer_field(0, 0, "falloff_radius", 200)
	loud.set_member_field(0, 0, 0, "volume", 100)
	var bank = SoundBank.create(root)
	bank.add_bank(quiet)
	bank.add_bank(loud)
	var parent := Node3D.new()
	add_child_autofree(parent)
	# Thirteen id-less quiet voices and entity 5's loud one fill the fourteen
	# general channels.
	for _i in range(13):
		assert_true(bank.play_oneshot_3d(parent, Vector3.ZERO, "QUIET", StringName(), Vector3.ZERO))
	assert_true(bank.play_oneshot_3d(parent, Vector3.ZERO, "LOUD", StringName(), Vector3.ZERO, 0, 5))
	var first_quiet := parent.get_child(0) as AudioStreamPlayer3D
	var first_loud := parent.get_child(13) as AudioStreamPlayer3D
	assert_true(bank.play_oneshot_3d(parent, Vector3.ZERO, "LOUD", StringName(), Vector3.ZERO, 0, 5))
	assert_true(first_loud.is_queued_for_deletion(), "entity 5's refire retakes its own channel")
	assert_false(first_quiet.is_queued_for_deletion(), "the quietest other voice survives the refire")
	assert_true(bank.play_oneshot_3d(parent, Vector3.ZERO, "LOUD", StringName(), Vector3.ZERO))
	assert_true(first_quiet.is_queued_for_deletion(), "an id-less fire steals the quietest channel instead")


func test_dialog_line_plays_at_member_pitch_without_draws() -> void:
	# The dialog module resolves its line to one wave entry and plays it at the
	# fixed dialog frequency; it never enters the trigger-set player, so a set's
	# authored pitch jitter neither shifts the line nor consumes the shared ROL3
	# stream (docs/audio/lwf-dbf-sound-re.md, Dialog_LoadAudioClip).
	var samples := PackedByteArray()
	samples.resize(22050 * 2 * 5)
	var root := _real_root({
		"line.wav": _build_wav(samples, 1, 22050, 16),
		"pool.wav": _build_wav(samples, 1, 22050, 16),
	})
	var line := _profile_with_set("LINE", "line.wav")
	line.set_set_field(0, "pitch_base", 98304)
	line.set_set_field(0, "pitch_random_range", 32768)
	line.set_layer_field(0, 0, "selection_mode", LwfData.SELECTION_SEQUENTIAL)
	line.set_member_field(0, 0, 0, "rand_pitch", 0.125)
	var pool := _profile_with_set("POOL", "pool.wav")
	pool.set_set_field(0, "pitch_base", 98304)
	pool.set_set_field(0, "pitch_random_range", 32768)
	pool.set_member_field(0, 0, 0, "rand_pitch", 0.125)
	var bank = SoundBank.create(root)
	bank.add_bank(line)
	bank.add_bank(pool)
	var parent := Node3D.new()
	add_child_autofree(parent)
	var voice: AudioStreamPlayer = bank.spawn_oneshot_2d(parent, "LINE", StringName())
	assert_not_null(voice)
	if voice == null:
		return
	assert_almost_eq(voice.pitch_scale, 1.0, 0.00001,
			"the line plays at the member's base pitch, unjittered")
	# The next trigger-set fire sees the untouched stream: the pinned fresh-bank
	# pitch of the fourteen-channel test above, not the value two draws later.
	assert_true(bank.play_oneshot_3d(parent, Vector3.ZERO, "POOL", StringName(), Vector3.ZERO))
	var fire := parent.get_child(parent.get_child_count() - 1) as AudioStreamPlayer3D
	assert_almost_eq(fire.pitch_scale, 110686.0 / 65536.0, 0.00001,
			"the dialog spawn consumed no ROL3 draws")


func test_oneshot_layers_follow_the_live_simulation_view() -> void:
	var samples := PackedByteArray()
	samples.resize(32)
	var root := _real_root({"view.wav": _build_wav(samples, 1, 22050, 16)})
	var profile := _profile_with_set("INTERNAL_ONLY", "view.wav")
	profile.set_layer_field(0, 0, "external", false)
	var bank := SoundBank.create(root)
	bank.add_bank(profile)
	var parent := Node3D.new()
	add_child_autofree(parent)
	var sim := Simulation.new()
	sim.build_demo_mission()
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	bank.set_occlusion_provider(sim)
	sim.set_local_player_debug_third_person(false)
	assert_true(bank.play_oneshot_3d(parent, Vector3.ZERO, "INTERNAL_ONLY", &"SFX"),
			"internal layers are admitted in first person, including distance-flat fires")
	sim.set_local_player_debug_third_person(true)
	assert_false(bank.play_oneshot_3d(parent, Vector3.ZERO, "INTERNAL_ONLY", &"SFX"),
			"the next fire reads the live external view and creates no voice")
	sim.set_local_player_debug_third_person(false)
	assert_true(bank.play_oneshot_3d(parent, Vector3.ZERO, "INTERNAL_ONLY", &"SFX"))
	bank.set_occlusion_provider(null)
	assert_true(bank.play_oneshot_3d(parent, Vector3.ZERO, "INTERNAL_ONLY", &"SFX"),
			"an isolated menu bank retains the neutral both-views listener")
