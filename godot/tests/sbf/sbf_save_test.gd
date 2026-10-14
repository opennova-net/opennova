extends GutTest

# The .sbf save writes the bank's model (formats/sbf: sbf_write_bank), never the bytes
# it was read from: the minted fixture (tests/fixtures/minimal_sbf_gen.cpp, our own
# writer's bytes) comes back byte for byte from its fields, and an added stream leaves
# every other stream's chunks as they were read (no decode and re-encode).

const SRC := "res://../fixtures/sbf/synth_gamemus.sbf"
const DST := "user://test_sbf_save_out.sbf"
const DST_ADDED := "user://test_sbf_save_added.sbf"
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
