extends GutTest

const FIXTURE := "res://../fixtures/sbf/bhd_menumus.sbf"
func test_entry_count_matches() -> void:
	var bank := SbfBank.new()
	bank.load_from_path(FIXTURE)
	assert_not_null(bank, "fixture loads as SbfBank")
	if bank == null:
		return
	assert_eq(bank.get_entry_count(), 155, "BHD menumus has 155 entries")


func test_has_entry_case_insensitive() -> void:
	var bank := SbfBank.new()
	bank.load_from_path(FIXTURE)
	assert_not_null(bank, "fixture loads as SbfBank")
	if bank == null:
		return
	assert_true(bank.has_entry(&"MENU101"), "MENU101 (uppercase) is present")
	assert_true(bank.has_entry(&"menu101"), "menu101 (lowercase) is present (case-insensitive)")
	assert_false(bank.has_entry(&"NOPE"), "NOPE absent from bank")
