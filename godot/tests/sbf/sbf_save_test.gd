extends GutTest

# The .sbf save writes the bank's model (formats/sbf: sbf_write_bank), never the bytes
# it was read from: the minted fixture (tests/fixtures/minimal_sbf_gen.cpp, our own
# writer's bytes) comes back byte for byte from its fields, and an added stream leaves
# every other stream's chunks as they were read (no decode and re-encode).

const SRC := "res://../fixtures/sbf/synth_gamemus.sbf"
const DST := "user://test_sbf_save_out.sbf"
const DST_ADDED := "user://test_sbf_save_added.sbf"
const DST_ADPCM := "user://test_sbf_save_adpcm.sbf"
const HEADER := 24
const ENTRY := 32


func test_save_writes_the_bank_from_its_model() -> void:
	var bank := SbfBank.new()
	bank.load_from_path(SRC)
	assert_gt(bank.get_entry_count(), 0, "fixture loads")
	var err := bank.save_to_path(DST)
	assert_eq(err, OK, "save returns OK")
	var src_b := FileAccess.get_file_as_bytes(SRC)
	var dst_b := FileAccess.get_file_as_bytes(DST)
	assert_eq(dst_b, src_b, "the minted bank comes back byte for byte from its fields")


# A bank whose flags word is not 1 streams IMA ADPCM, and the PCM edits encode only the
# byte-paired form: both refuse such a bank before they change it (the fixture's bytes with the
# header's flags word at +8 cleared).
func test_the_pcm_edits_refuse_an_adpcm_bank() -> void:
	var bytes := FileAccess.get_file_as_bytes(SRC)
	bytes.encode_u32(8, 0)
	var file := FileAccess.open(DST_ADPCM, FileAccess.WRITE)
	file.store_buffer(bytes)
	file.close()
	var bank := SbfBank.new()
	bank.load_from_path(DST_ADPCM)
	var count := bank.get_entry_count()
	assert_gt(count, 0, "the flagged copy loads")
	var samples := PackedFloat32Array()
	samples.resize(100)
	assert_eq(bank.set_entry_pcm(0, samples), ERR_UNAVAILABLE, "set_entry_pcm refuses the ADPCM bank")
	assert_eq(bank.add_entry("ADDED", samples), ERR_UNAVAILABLE, "add_entry refuses the ADPCM bank")
	assert_eq(bank.get_entry_count(), count, "nothing was added to the index")
	assert_false(bank.is_dirty(), "the refused edits leave the bank as it was read")
	# The bank has a model (the refusal is its flags word, not a bank it could not read).
	assert_eq(bank.save_to_path(DST_ADPCM + ".out"), OK, "the ADPCM bank still writes from its model")


# An ADPCM stream's length is the frames its wave decodes (up to 4068 a block against the entry's
# samples left), not the byte-paired form's 2048 a chunk.
func test_an_adpcm_stream_reports_its_decoded_length() -> void:
	var bytes := FileAccess.get_file_as_bytes(SRC)
	bytes.encode_u32(8, 0)
	var file := FileAccess.open(DST_ADPCM, FileAccess.WRITE)
	file.store_buffer(bytes)
	file.close()
	var bank := SbfBank.new()
	bank.load_from_path(DST_ADPCM)
	assert_gt(bank.get_entry_count(), 0, "the flagged copy loads")
	for index in range(bank.get_entry_count()):
		var stream: SbfAudioStream = bank.get_stream_at(index)
		var wav: AudioStreamWAV = stream.get_wav()
		var frames := wav.data.size() / 4
		assert_almost_eq(stream.get_length() * 22050.0, float(frames), 0.01,
				"stream %d's length is its decoded frames" % index)


func test_add_entry_keeps_every_other_stream_as_read() -> void:
	var bank := SbfBank.new()
	bank.load_from_path(SRC)
	var count := bank.get_entry_count()
	assert_gt(count, 0, "fixture loads")
	var samples := PackedFloat32Array()
	samples.resize(100)
	assert_eq(bank.add_entry("ADDED", samples), OK, "add_entry returns OK")
	assert_eq(bank.save_to_path(DST_ADDED), OK, "save returns OK")
	var src_b := FileAccess.get_file_as_bytes(SRC)
	var dst_b := FileAccess.get_file_as_bytes(DST_ADDED)
	assert_eq(dst_b.decode_u32(20), count + 1, "the index lists the added stream")
	# The streams follow the index, one after another: the bank's own read after its
	# shorter index, the same bytes after the longer one, the added stream after them.
	var streams := src_b.slice(HEADER + ENTRY * count)
	var start := HEADER + ENTRY * (count + 1)
	assert_eq(dst_b.slice(start, start + streams.size()), streams,
			"every other stream's chunks are written as they were read")
	var reloaded := SbfBank.new()
	reloaded.load_from_path(DST_ADDED)
	assert_eq(reloaded.get_entry_count(), count + 1, "the saved bank loads with the added stream")
	assert_eq(reloaded.get_entry_name(count), "ADDED", "the added stream's name")
