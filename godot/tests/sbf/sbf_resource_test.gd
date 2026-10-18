extends GutTest

# SbfBank over the synthetic bank fixtures/sbf/synth_gamemus.sbf (13 entries
# minted by tests/fixtures/minimal_sbf_gen.cpp; SILENCE first, TONE01..12).
const FIXTURE := "res://../fixtures/sbf/synth_gamemus.sbf"
func test_entry_count_matches() -> void:
	var bank := SbfBank.new()
	bank.load_from_path(FIXTURE)
	assert_not_null(bank, "fixture loads as SbfBank")
	if bank == null:
		return
	assert_eq(bank.get_entry_count(), 13, "the synthetic bank has 13 entries")
	assert_eq(bank.get_entry_name(0), "SILENCE")
	assert_eq(bank.get_entry_name(1), "TONE01")


func test_has_entry_case_insensitive() -> void:
	var bank := SbfBank.new()
	bank.load_from_path(FIXTURE)
	assert_not_null(bank, "fixture loads as SbfBank")
	if bank == null:
		return
	assert_true(bank.has_entry(&"TONE01"), "TONE01 (uppercase) is present")
	assert_true(bank.has_entry(&"tone01"), "tone01 (lowercase) is present (case-insensitive)")
	assert_false(bank.has_entry(&"NOPE"), "NOPE absent from bank")


# The stream hands the mixer a built-in playback (C-164): Godot's AudioStreamWAV
# over the entry's PCM as the engine decodes it (sbf_decode_entry, held sample for
# sample to the deleted streaming playback's PCM by the sbf_decode ctest), 16-bit
# stereo at 22050 Hz. TONE01 is 4096 + 4096 + 1000 samples: 4596 frames.
func test_stream_plays_a_builtin_wav_of_the_entry_pcm() -> void:
	var bank := SbfBank.new()
	bank.load_from_path(FIXTURE)
	var stream: SbfAudioStream = bank.get_stream_at(1)
	assert_not_null(stream, "TONE01's stream")
	if stream == null:
		return
	var wav: AudioStreamWAV = stream.get_wav()
	assert_not_null(wav, "the entry's PCM as a WAV")
	if wav == null:
		return
	assert_eq(wav.format, AudioStreamWAV.FORMAT_16_BITS)
	assert_true(wav.stereo, "byte-paired stereo")
	assert_eq(wav.mix_rate, 22050)
	assert_eq(wav.data.size(), (4096 + 4096 + 1000) * 2, "every sample of the three chunks, 16-bit")
	var playback := stream.instantiate_playback()
	assert_not_null(playback, "a playback")
	if playback != null:
		assert_eq(playback.get_class(), "AudioStreamPlaybackWAV",
				"the AudioServer holds a built-in playback, never an extension one")
