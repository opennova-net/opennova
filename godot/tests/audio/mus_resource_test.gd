extends GutTest

const FIXTURE := "res://../fixtures/mus/jo_gamemus.bin"


func test_load_returns_music_script() -> void:
	var res := load(FIXTURE)
	assert_not_null(res, "loader returns non-null for " + FIXTURE)
	assert_true(res is NovaMusicScript, "loader returns NovaMusicScript")


func test_script_count_at_least_one() -> void:
	var ms := load(FIXTURE) as NovaMusicScript
	assert_not_null(ms, "fixture loads as NovaMusicScript")
	if ms == null:
		return
	assert_gt(ms.get_script_count(), 0, "at least one MU01 chunk parsed")


func test_decompile_yields_text() -> void:
	var ms := load(FIXTURE) as NovaMusicScript
	assert_not_null(ms, "fixture loads as NovaMusicScript")
	if ms == null:
		return
	var text := ms.get_decompiled_text(StringName(ms.get_default_script_name()))
	assert_gt(text.length(), 100, "decompiled text non-trivial")
