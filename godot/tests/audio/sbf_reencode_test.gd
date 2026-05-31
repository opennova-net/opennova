extends GutTest

# Round-trip the re-encode saver path: load a real SBF, override one entry's
# audio with silence via set_entry_pcm, save, and reload. Asserts the dirty
# flag flips on edit + clears on save and that the saved file parses back as
# a valid bank with the same entry count.

const SRC := "res://../fixtures/sbf/jo_gamemus.sbf"
const DST := "user://test_sbf_reencode.sbf"


func test_dirty_bank_reencodes_on_save() -> void:
	var bank: NovaSbfBank = load(SRC)
	assert_not_null(bank, "fixture loads")
	if bank == null:
		return
	var original_count := bank.get_entry_count()
	assert_gt(original_count, 0, "fixture has at least one entry")

	# 1024 samples of silence (zeros); encoder will produce a single padded
	# chunk for entry 0 and reuse decoded originals for every other entry.
	var silence := PackedFloat32Array()
	silence.resize(1024)
	for i in range(1024):
		silence[i] = 0.0

	var set_err := bank.set_entry_pcm(0, silence)
	assert_eq(set_err, OK, "set_entry_pcm returns OK")
	assert_true(bank.is_dirty(), "bank is dirty after set_entry_pcm")

	var err := ResourceSaver.save(bank, DST)
	assert_eq(err, OK, "ResourceSaver.save returns OK")
	assert_false(bank.is_dirty(), "save clears dirty flag")

	var reloaded: NovaSbfBank = load(DST)
	assert_not_null(reloaded, "saved file loads back as NovaSbfBank")
	if reloaded == null:
		return
	assert_eq(reloaded.get_entry_count(), original_count,
			"re-encoded entry count matches original")
