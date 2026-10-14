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
# programs), never the bytes it was read from: each program as it was read, its editor debug
# section with it (the source path, the sections', globals', parameters' and functions' names,
# the line table), which MDEdit writes into every script from its source. The game reads none
# of that section (the loader only relocates it; its one reader, a source viewer, has no
# caller). The minted gamescript, the encoder's own output, saves as its own bytes, and the
# save reads back as the same programs.
func test_save_writes_the_script_from_its_model() -> void:
	var ms := MusicScript.new()
	ms.load_from_path(FIXTURE)
	assert_gt(ms.get_script_count(), 0, "the minted gamescript loads")
	var out := "user://test_mus_save_gamemus.bin"
	assert_eq(ms.save_to_path(out), OK, "the script saves")
	var saved := FileAccess.get_file_as_bytes(out)
	assert_eq(saved, FileAccess.get_file_as_bytes(FIXTURE), "the save is the minted script's own bytes")
	for debug in ["synth_gamemus.mus", "Multiplayerstart", "MessageHandler::msgtype", "Testmission"]:
		assert_true(_holds(saved, debug), "the save holds %s" % debug)
	var again := MusicScript.new()
	again.load_from_decrypted_bytes(saved, out)
	assert_eq(again.get_script_count(), ms.get_script_count(), "the saved script holds the same programs")
	assert_eq(again.get_default_script_name(), ms.get_default_script_name(), "by name")
	assert_eq(again.get_section_names(StringName(again.get_default_script_name())),
			ms.get_section_names(StringName(ms.get_default_script_name())), "and their sections, by name")


# Whether `bytes` holds `text`'s ASCII bytes anywhere (a match on a whole byte).
func _holds(bytes: PackedByteArray, text: String) -> bool:
	var hex := bytes.hex_encode()
	var want := text.to_ascii_buffer().hex_encode()
	var at := hex.find(want)
	while at >= 0:
		if at % 2 == 0:
			return true
		at = hex.find(want, at + 1)
	return false
