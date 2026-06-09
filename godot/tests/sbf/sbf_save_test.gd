extends GutTest

const SRC := "res://../fixtures/sbf/bhd_menumus.sbf"
const DST := "user://test_sbf_save_out.sbf"


func test_save_passthrough_byte_identical() -> void:
	var bank: NovaSbfBank = load(SRC)
	assert_not_null(bank, "fixture loads")
	if bank == null:
		return
	var err := ResourceSaver.save(bank, DST)
	assert_eq(err, OK, "save returns OK")
	var src_b := FileAccess.get_file_as_bytes(SRC)
	var dst_b := FileAccess.get_file_as_bytes(DST)
	assert_eq(src_b.size(), dst_b.size(), "size matches")
	# Spot-check first 64 bytes (header + first 2 entries).
	var src_hex := src_b.slice(0, 64).hex_encode()
	var dst_hex := dst_b.slice(0, 64).hex_encode()
	assert_eq(src_hex, dst_hex, "first 64 bytes match")
