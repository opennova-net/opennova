extends GutTest

## Runtime audio tests: NovaSoundBank name indexing/resolution (case-insensitive,
## the grilled name-keyed model) and NovaWavLoader RIFF decode incl. the 8-bit
## unsigned -> signed conversion. Self-contained (no real game data required).

const NovaSoundBankScript = preload("res://engine/world/nova_sound_bank.gd")


# Duck-typed NovaResourceRoot stand-in serving in-memory wav bytes.
class ResourceRootStub:
	extends RefCounted
	var files := {}  # filename(lower) -> PackedByteArray
	var read_calls := 0

	func has_file(name: String) -> bool:
		return files.has(name.to_lower())

	func read_file(name: String) -> PackedByteArray:
		read_calls += 1
		return files.get(name.to_lower(), PackedByteArray())


# Public NovaSoundBank dependency seam: the live NovaSimulation implements this
# method; the value-only stand-in lets the test pin what the bank does with the
# returned retail occlusion distance without fabricating collision internals.
class OcclusionProviderStub:
	extends RefCounted
	var distance_q16 := -1
	var calls := 0
	var source_bms_ids: Array[int] = []

	func _init(p_distance_q16: int = -1) -> void:
		distance_q16 = p_distance_q16

	func sound_occlusion_distance_q16(
			_listener_pos: Vector3, _source_pos: Vector3, raw_distance_q16: int,
			source_bms_id: int = 0) -> int:
		calls += 1
		source_bms_ids.append(source_bms_id)
		return raw_distance_q16 if distance_q16 < 0 else distance_q16


func _profile_with_set(set_name: String, wav: String) -> NovaLwfData:
	var d := NovaLwfData.new()
	d.create_empty()
	var si := d.add_set()
	d.set_set_field(si, "name", set_name)
	var li := d.add_layer(si)
	var mi := d.add_member(si, li)
	d.set_member_field(si, li, mi, "wav_path", wav)
	return d


func test_bank_indexing_case_insensitive() -> void:
	var bank = NovaSoundBankScript.new(null)
	bank.add_bank(_profile_with_set("Z00AMB1", "Z00aR100.wav"))
	assert_true(bank.has_set("Z00AMB1"), "exact name resolves")
	assert_true(bank.has_set("z00amb1"), "lookup is case-insensitive")
	assert_false(bank.has_set("NOPE"), "unknown set does not resolve")
	assert_true("z00amb1" in bank.get_set_names())


func test_spawn_unknown_or_unresolvable_returns_null() -> void:
	var bank = NovaSoundBankScript.new(null)  # no resource root -> no wav decode
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
	var root := ResourceRootStub.new()
	var samples := PackedByteArray()
	samples.resize(32)  # 16 mono 16-bit frames
	root.files["z00ar100.wav"] = _build_wav(samples, 1, 22050, 16)
	var bank = NovaSoundBankScript.new(root)
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
	var root := ResourceRootStub.new()
	var samples := PackedByteArray()
	samples.resize(32)
	root.files["z00ar100.wav"] = _build_wav(samples, 1, 22050, 16)
	var bank = NovaSoundBankScript.new(root)
	bank.add_bank(_profile_with_set("Z00AMB1", "Z00aR100.wav"))

	var descriptors: Array = bank.describe_ambient("Z00AMB1")
	assert_eq(descriptors.size(), 1)
	assert_eq(root.read_calls, 0,
		"describing a virtual ambient layer does not read or decode its WAV")
	assert_not_null(bank.resolve_ambient_stream(descriptors[0]))
	assert_eq(root.read_calls, 1, "the first selected candidate resolves lazily")
	assert_not_null(bank.resolve_ambient_stream(descriptors[0]))
	assert_eq(root.read_calls, 1, "the bank cache prevents a second VFS read")


# --- The witnessed distance-volume curve [orig: SoundBank_CalcDistanceVolPan
# @ 0x75ca20]: vol * (255/256) * (1 - d/r)^2, integer-exact, hard 0 at d >= r,
# ceilinged by clamp_volume. Expectations are hand-computed from the formula.

func test_calc_distance_volume_curve() -> void:
	var S := NovaSoundBankScript
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
	var S := NovaSoundBankScript
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


func test_oneshot_distance_volume_is_not_rebased() -> void:
	var bank = NovaSoundBankScript.new(null)
	var layer := {"falloff_radius": 200, "min_distance": 0}
	var member := {"volume": 255, "clamp_volume": 255}
	# One-shots run the plain falloff over 0..r [orig: SoundBank_PlayTriggerEntries
	# @ 0x75cf75]: half distance = quarter volume of the 254-scaled input.
	assert_eq(bank.oneshot_distance_volume(100 << 16, layer, member), 63)
	assert_eq(bank.oneshot_distance_volume(0, layer, member), 253)
	assert_eq(bank.oneshot_distance_volume(200 << 16, layer, member), 0)


func test_oneshot_no_falloff_plays_at_emitter_volume() -> void:
	var bank = NovaSoundBankScript.new(null)
	# A layer with NO falloff radius plays at the RAW emitter volume — the
	# member volume is not consulted [orig: SoundBank_PlayTriggerEntries
	# @ 0x75cf88 stores emitter_info[2], reimpl emitter = full 255].
	var quiet := {"volume": 100, "clamp_volume": 255}
	assert_eq(bank.oneshot_distance_volume(500 << 16, {}, quiet), 255)
	# A min-only layer computes the proximity stage but the no-falloff branch
	# DISCARDS it with the member volume [orig: @ 0x75cf88] (no JOX layer
	# ships min-only; pinned for the witnessed form).
	var min_only := {"falloff_radius": 0, "min_distance": 50}
	assert_eq(bank.oneshot_distance_volume(25 << 16, min_only, quiet), 255)


func test_zero_range_oneshot_only_fires_at_the_exact_source() -> void:
	var root := ResourceRootStub.new()
	var samples := PackedByteArray()
	samples.resize(32)
	root.files["tone.wav"] = _build_wav(samples, 1, 22050, 16)
	var profile := _profile_with_set("POINT_ONLY", "tone.wav")
	profile.set_set_field(0, "target_id", 0)
	var bank = NovaSoundBankScript.new(root)
	bank.add_bank(profile)
	var parent := Node3D.new()
	add_child_autofree(parent)

	assert_false(bank.play_oneshot_3d(parent, Vector3(1, 0, 0), "POINT_ONLY", StringName(), Vector3.ZERO),
		"retail's dist <= range gate rejects every nonzero distance when range is zero")
	assert_true(bank.play_oneshot_3d(parent, Vector3.ZERO, "POINT_ONLY", StringName(), Vector3.ZERO),
		"equality passes, so a zero-range set can still fire at its exact source")


func test_oneshot_occlusion_distance_drives_fire_volume() -> void:
	# Raw distance is 50u, but the witnessed two-ray result inflates it to 100u.
	# With a 200u falloff, that is the pinned half-range volume byte 63.
	var root := ResourceRootStub.new()
	var samples := PackedByteArray()
	samples.resize(32)
	root.files["tone.wav"] = _build_wav(samples, 1, 22050, 16)
	var profile := _profile_with_set("OCCLUDED", "tone.wav")
	profile.set_set_field(0, "target_id", 200)
	profile.set_layer_field(0, 0, "falloff_radius", 200)
	var provider := OcclusionProviderStub.new(100 << 16)
	var bank = NovaSoundBankScript.new(root)
	bank.occlusion_provider = provider
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
	var root := ResourceRootStub.new()
	var samples := PackedByteArray()
	samples.resize(32)
	root.files["tone.wav"] = _build_wav(samples, 1, 22050, 16)
	var profile := _profile_with_set("OCCLUDED_CULL", "tone.wav")
	profile.set_set_field(0, "target_id", 120)
	profile.set_layer_field(0, 0, "falloff_radius", 200)
	var provider := OcclusionProviderStub.new(130 << 16)
	var bank = NovaSoundBankScript.new(root)
	bank.occlusion_provider = provider
	bank.add_bank(profile)
	var parent := Node3D.new()
	add_child_autofree(parent)

	assert_false(bank.play_oneshot_3d(
		parent, Vector3(100, 0, 0), "OCCLUDED_CULL", StringName(), Vector3.ZERO))
	assert_eq(provider.calls, 1, "raw-in-range fire reaches the occlusion query")
	assert_eq(parent.get_child_count(), 0, "inflation beyond set range spawns no voice")


func test_crossfade_volume_byte_rounding() -> void:
	var A := preload("res://engine/world/nova_mission_audio.gd")
	# The register volume word is (0xFFFF * blend + 0x8000) >> 16, ROUNDED, and
	# the mixer reads its high byte [orig: Entity_UpdateEnvSoundEmitter
	# @ 0x4a81c6]. Full blend (the 0xFFFF sentinel) -> 255; half -> 128 (the
	# +0x8000 round add: a floor form gives 127); zero -> 0.
	assert_eq(A.crossfade_volume_byte(1.0), 255)
	assert_eq(A.crossfade_volume_byte(0.5), 128)
	assert_eq(A.crossfade_volume_byte(0.0), 0)


func test_time_of_day_regions_and_blend() -> void:
	var A := preload("res://engine/world/nova_mission_audio.gd")
	# Region cuts [orig: Entity_CalcTimeOfDayRegion @ 0x408110]:
	# [4,10) morning, [10,17) day, [17,21) evening, else night.
	assert_eq(int(A.time_of_day_region(6.0).region), 0)
	assert_eq(int(A.time_of_day_region(12.0).region), 1)
	assert_eq(int(A.time_of_day_region(18.0).region), 2)
	assert_eq(int(A.time_of_day_region(23.0).region), 3)
	assert_eq(int(A.time_of_day_region(0.5).region), 3, "night wraps past midnight")
	assert_eq(int(A.time_of_day_region(3.99).region), 3)
	# The regions are OPEN at the low cut: the exact cut instant classifies as
	# night at full blend (the original's unsigned range-check idiom starts
	# each interval one tick past the cut) [orig: @ 0x408175].
	assert_eq(int(A.time_of_day_region(4.0).region), 3, "exact cut instant falls through to night")
	assert_eq(float(A.time_of_day_region(4.0).blend), 1.0)
	# Mid-region: full blend.
	assert_eq(float(A.time_of_day_region(12.0).blend), 1.0)
	# Just after a cut: fading in, adjacent = the previous region.
	var fade_in = A.time_of_day_region(10.02)
	assert_eq(int(fade_in.region), 1)
	assert_eq(int(fade_in.adjacent), 0)
	assert_between(float(fade_in.blend), 0.1, 0.5)
	# Just before a cut: fading out, adjacent = the next region.
	var fade_out = A.time_of_day_region(9.98)
	assert_eq(int(fade_out.region), 0)
	assert_eq(int(fade_out.adjacent), 1)
	assert_between(float(fade_out.blend), 0.1, 0.5)
	# Night holds full volume up to the 4h cut (the wrapped region's far edge
	# never blends [orig: @ 0x40820f]).
	assert_eq(float(A.time_of_day_region(3.98).blend), 1.0)
	# The exact cut instant reads full volume — the original's zero-blend-distance
	# guard [orig: @ 0x408251].
	assert_eq(float(A.time_of_day_region(10.0).blend), 1.0)


func test_wav_loader_decodes_pcm8_unsigned() -> void:
	# Minimal 8-bit unsigned mono 22050 Hz PCM WAV with 4 samples.
	var samples := PackedByteArray([0x80, 0x00, 0xFF, 0x80])  # center, min, max, center
	var wav := _build_wav(samples, 1, 22050, 8)
	var stream := NovaWavLoader.from_bytes(wav)
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
	assert_null(NovaWavLoader.from_bytes(PackedByteArray([1, 2, 3, 4])), "garbage is rejected")


func test_wav_loader_decodes_ima_adpcm() -> void:
	# One mono IMA-ADPCM block: predictor=1000, step index 0, then a 4-byte word of
	# zero-nibbles. At step index 0 a zero nibble adds 0, so every sample stays 1000.
	var wav := _build_ima_wav(1000, 0, PackedByteArray([0, 0, 0, 0]), 11025, 8)
	var stream := NovaWavLoader.from_bytes(wav)
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


# Build a minimal one-block mono IMA-ADPCM WAV (audioFormat 0x11).
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
	buf.put_u32(36 + data.size())
	buf.put_data("WAVE".to_ascii_buffer())
	buf.put_data("fmt ".to_ascii_buffer())
	buf.put_u32(16)
	buf.put_u16(0x11)          # IMA ADPCM
	buf.put_u16(1)             # mono
	buf.put_u32(rate)
	buf.put_u32(rate)          # byteRate (loader ignores)
	buf.put_u16(block_align)
	buf.put_u16(4)             # bits per sample
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
