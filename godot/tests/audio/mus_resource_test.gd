extends GutTest

const FIXTURE := "res://../fixtures/mus/synth_gamemus.bin"
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


# The save writes the script from its model (formats/mus: mus_encode_file over the parsed
# programs), never the bytes it was read from: the minted pair (tests/fixtures, our own
# encoder's bytes) comes back byte for byte from its fields.
func test_save_writes_the_script_from_its_model() -> void:
	for fixture in [FIXTURE, "res://../fixtures/mus/synth_menumus.bin"]:
		var ms := MusicScript.new()
		ms.load_from_path(fixture)
		assert_gt(ms.get_script_count(), 0, "%s loads" % fixture)
		var out := "user://test_mus_save_%s" % fixture.get_file()
		assert_eq(ms.save_to_path(out), OK, "%s saves" % fixture)
		assert_eq(FileAccess.get_file_as_bytes(out), FileAccess.get_file_as_bytes(fixture),
				"%s comes back byte for byte from its model" % fixture)
