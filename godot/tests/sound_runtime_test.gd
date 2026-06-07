extends GutTest

## Runtime audio tests: NovaSoundBank name indexing/resolution (case-insensitive,
## the grilled name-keyed model) and NovaWavLoader RIFF decode incl. the 8-bit
## unsigned -> signed conversion. Self-contained (no real game data required).

const NovaSoundBankScript = preload("res://engine/world/nova_sound_bank.gd")


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
