extends GutTest

const FIXTURE := "res://../fixtures/sbf/bhd_menumus.sbf"


func test_load_returns_bank() -> void:
	var res := load(FIXTURE)
	assert_not_null(res, "loader returns non-null for " + FIXTURE)
	assert_true(res is NovaSbfBank, "loader returns NovaSbfBank")


func test_entry_count_matches() -> void:
	var bank := load(FIXTURE) as NovaSbfBank
	assert_not_null(bank, "fixture loads as NovaSbfBank")
	if bank == null:
		return
	assert_eq(bank.get_entry_count(), 155, "BHD menumus has 155 entries")


func test_has_entry_case_insensitive() -> void:
	var bank := load(FIXTURE) as NovaSbfBank
	assert_not_null(bank, "fixture loads as NovaSbfBank")
	if bank == null:
		return
	assert_true(bank.has_entry(&"MENU101"), "MENU101 (uppercase) is present")
	assert_true(bank.has_entry(&"menu101"), "menu101 (lowercase) is present (case-insensitive)")
	assert_false(bank.has_entry(&"NOPE"), "NOPE absent from bank")
