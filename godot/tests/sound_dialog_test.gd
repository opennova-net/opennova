extends GutTest

## End-to-end mission DIALOG audio (the .dbf path) over minted data: the
## synthetic dialog bank fixtures/dbf/synth_bank.dbf (minimal_dbf_gen: eleven
## groups dlg001..dlg011, lines SynR100..SynR115) as a mission's co-named
## .DBF, its dialog bank's sounds (the co-named .LWF, written through LwfData)
## naming a wave per line, and the shared IMA-ADPCM wave as dlg001's line.
## Replaces the retail 00TRg witness (its .DBF/.LWF and z00gr100.wav, an
## IMA-ADPCM dialog wave): every dialog id resolves to a wave of the dialog
## bank, and the ADPCM wave decodes to 16-bit PCM without an audio device.

const MISSION := "synthdlg.bms"
const DIALOG_BANK := "res://../fixtures/dbf/synth_bank.dbf"
# The IMA-ADPCM (WAVE format 0x11) wave lane L2 mints for wav_source.
const ADPCM_WAVE := "res://../fixtures/lwf/ima_adpcm.wav"
const ADPCM_NAME := "ima_adpcm.wav"
# synth_bank.dbf's groups and the line names each carries (minimal_dbf_gen.cpp).
const GROUP_COUNT := 11
const LINE_NAMES := [
	"SynR100", "SynR101", "SynR102", "SynR103", "SynR104", "SynR105", "SynR106",
	"SynR107", "SynR108", "SynR109", "SynR110", "SynR111", "SynR112", "SynR113",
	"SynR114", "SynR115",
]

var _dir := ""


func after_each() -> void:
	if not _dir.is_empty():
		TestFs.remove_dir_recursive(_dir)
	_dir = ""


func test_minted_mission_dialog_resolves() -> void:
	_dir = TestFs.cache_dir(self, "sound_dialog")
	TestFs.copy(self, ProjectSettings.globalize_path(DIALOG_BANK), _dir.path_join("synthdlg.DBF"))
	TestFs.copy(self, ProjectSettings.globalize_path("res://../fixtures/lwf/tone.wav"),
			_dir.path_join("tone.wav"))
	TestFs.copy(self, ProjectSettings.globalize_path(ADPCM_WAVE), _dir.path_join(ADPCM_NAME))
	# The dialog bank's sounds: a wave named as each line names it (a dialog line
	# plays the bank's wave of its name, never a set: Dialog_LoadAudioClip ->
	# SoundBank_FindEntryByName); dlg001's one line is the ADPCM wave.
	var lwf := LwfData.new()
	lwf.create_empty()
	for i in LINE_NAMES.size():
		var set_i := lwf.add_set()
		lwf.set_set_field(set_i, "name", "DLG_%d" % i)
		var layer_i := lwf.add_layer(set_i)
		lwf.set_layer_field(set_i, layer_i, "internal", true)
		lwf.set_layer_field(set_i, layer_i, "external", true)
		lwf.set_layer_field(set_i, layer_i, "falloff_radius", 200)
		var member_i := lwf.add_member(set_i, layer_i)
		lwf.set_member_field(set_i, layer_i, member_i, "wav_path",
				ADPCM_NAME if i == 0 else "tone.wav")
		lwf.set_member_field(set_i, layer_i, member_i, "name", LINE_NAMES[i])
	assert_eq(lwf.save_file(_dir.path_join("synthdlg.LWF")), OK)

	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(_dir), OK)
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)

	var container := Node3D.new()
	add_child_autofree(container)
	var audio = MissionAudio.create(root, null)
	var stats := audio.setup(mission, MISSION, container)
	gut.p("minted dialog audio stats: %s" % str(stats.to_json_value()))

	var dialog_count := int(stats.dialogs)
	assert_gt(dialog_count, 0, "the mission .DBF loaded with dialog groups")
	assert_eq(dialog_count, GROUP_COUNT, "every group of the bank loads")

	# Every dialog id should resolve to a wave the dialog bank's sounds (the
	# mission's .LWF beside its .DBF) hold -- proving the .dbf -> dialog bank
	# pipeline end-to-end without an audio device.
	var resolved := 0
	for i in range(1, dialog_count + 1):
		var wave := audio.resolve_dialog_wave(i)
		if not wave.is_empty():
			resolved += 1
	gut.p("resolved %d / %d dialog ids to the dialog bank's waves" % [resolved, dialog_count])
	assert_eq(resolved, dialog_count, "every dialog id resolves to a wave of the dialog bank")
	assert_eq(audio.resolve_dialog_wave(1), ADPCM_NAME, "dlg001's line plays the ADPCM wave")

	# Dialog/zone waves are IMA-ADPCM (audioFormat 0x11): they decode to a
	# non-empty 16-bit stream (WavLoader once returned null -> silence).
	var wbytes := root.read_file(ADPCM_NAME)
	assert_false(wbytes.is_empty(), "the ADPCM wave is served from the root")
	assert_eq(_wave_format(wbytes), 0x11, "the staged wave is IMA-ADPCM")
	var stream := WavLoader.from_bytes(wbytes)
	assert_not_null(stream, "IMA-ADPCM dialog wav decodes")
	if stream != null:
		assert_eq(stream.format, AudioStreamWAV.FORMAT_16_BITS)
		assert_gt(stream.data.size(), 0, "decoded PCM is non-empty")
	audio.teardown()


# The `fmt ` chunk's audio format tag of a RIFF WAVE, -1 when it has none.
func _wave_format(bytes: PackedByteArray) -> int:
	var pos := 12
	while pos + 8 <= bytes.size():
		var size := bytes.decode_u32(pos + 4)
		if bytes.slice(pos, pos + 4).get_string_from_ascii() == "fmt ":
			return bytes.decode_u16(pos + 8)
		pos += 8 + size + (size & 1)
	return -1
