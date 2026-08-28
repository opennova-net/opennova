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
