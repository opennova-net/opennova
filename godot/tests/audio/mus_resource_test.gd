extends GutTest

const FIXTURE := "res://../fixtures/mus/jo_gamemus.bin"
func test_script_count_at_least_one() -> void:
	var ms := MusicScript.new()
	ms.load_from_path(FIXTURE)
	assert_not_null(ms, "fixture loads as MusicScript")
	if ms == null:
		return
	assert_gt(ms.get_script_count(), 0, "at least one MU01 chunk parsed")


func test_decompile_yields_text() -> void:
	var ms := MusicScript.new()
	ms.load_from_path(FIXTURE)
	assert_not_null(ms, "fixture loads as MusicScript")
	if ms == null:
		return
	var text := ms.get_decompiled_text(StringName(ms.get_default_script_name()))
	assert_gt(text.length(), 100, "decompiled text non-trivial")
