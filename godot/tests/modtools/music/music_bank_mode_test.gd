extends GutTest

const MusicEditorDocument = preload("res://modtools/music/music_editor_document.gd")
const BankModeScene = preload("res://modtools/music/ui/bank_mode.tscn")

const BANK_FIXTURE := "res://../fixtures/sbf/jo_gamemus.sbf"


func test_bank_mode_populates_table_from_document():
	var doc = MusicEditorDocument.new()
	var err: int = doc.open_bank(BANK_FIXTURE)
	assert_eq(err, OK, "open_bank succeeded")
	var bank_mode: Control = BankModeScene.instantiate()
	add_child_autofree(bank_mode)
	bank_mode.bind_document(doc)
	# bind_document calls _refresh_table once _ready has fired; flush a frame
	# so the Tree is populated before we inspect it.
	await get_tree().process_frame
	var tree: Tree = bank_mode.get_node("VBox/TrackTree")
	var root := tree.get_root()
	assert_not_null(root, "tree root exists")
	var n := 0
	var child := root.get_first_child()
	while child != null:
		n += 1
		child = child.get_next()
	assert_eq(n, 13, "13 tracks listed for jo_gamemus")


func test_bank_mode_no_bank_loaded_shows_open_hint():
	var doc = MusicEditorDocument.new()
	var bank_mode: Control = BankModeScene.instantiate()
	add_child_autofree(bank_mode)
	bank_mode.bind_document(doc)
	await get_tree().process_frame
	var tree: Tree = bank_mode.get_node("VBox/TrackTree")
	var root := tree.get_root()
	assert_not_null(root)
	var hint := root.get_first_child()
	assert_not_null(hint, "no-bank state shows a hint row")
	if hint == null:
		return
	assert_eq(hint.get_text(1), "Open or create a music project to import tracks")
	assert_false(hint.is_selectable(1), "hint row is not a fake track")


func test_no_bank_loaded_disables_add_with_reason():
	var doc = MusicEditorDocument.new()
	var bank_mode: Control = BankModeScene.instantiate()
	add_child_autofree(bank_mode)
	bank_mode.bind_document(doc)
	await get_tree().process_frame
	var add_btn: Button = bank_mode.get_node("%AddButton")
	assert_true(add_btn.disabled, "Add is disabled until a bank is loaded")
	assert_eq(add_btn.tooltip_text, "Open or create a music project first")


func test_no_track_selected_actions_explain_selection_requirement():
	var doc = MusicEditorDocument.new()
	assert_eq(doc.open_bank(BANK_FIXTURE), OK, "open_bank succeeded")
	var bank_mode: Control = BankModeScene.instantiate()
	add_child_autofree(bank_mode)
	bank_mode.bind_document(doc)
	await get_tree().process_frame
	for node_name in ["%ReplaceButton", "%MoveUpButton", "%MoveDownButton", "%RenameButton", "%DeleteButton"]:
		var button: Button = bank_mode.get_node(node_name)
		assert_true(button.disabled, "%s disabled until a row is selected" % node_name)
		assert_eq(button.tooltip_text, "Select a track first")


func test_move_buttons_explain_track_boundaries():
	var doc = MusicEditorDocument.new()
	assert_eq(doc.open_bank(BANK_FIXTURE), OK, "open_bank succeeded")
	var bank_mode: Control = BankModeScene.instantiate()
	add_child_autofree(bank_mode)
	bank_mode.bind_document(doc)
	await get_tree().process_frame
	var tree: Tree = bank_mode.get_node("%TrackTree")
	var first := tree.get_root().get_first_child()
	assert_not_null(first, "first track row present")
	if first == null:
		return
	first.select(0)
	bank_mode._refresh_toolbar_state()
	var up: Button = bank_mode.get_node("%MoveUpButton")
	var down: Button = bank_mode.get_node("%MoveDownButton")
	assert_true(up.disabled, "first track cannot move farther up")
	assert_eq(up.tooltip_text, "Already the first track")
	assert_false(down.disabled, "first track can move down")
	assert_eq(down.tooltip_text, "Move the selected track down.")

	var last := first
	while last.get_next() != null:
		last = last.get_next()
	last.select(0)
	bank_mode._refresh_toolbar_state()
	assert_false(up.disabled, "last track can move up")
	assert_eq(up.tooltip_text, "Move the selected track up.")
	assert_true(down.disabled, "last track cannot move farther down")
	assert_eq(down.tooltip_text, "Already the last track")


# Polish A1: play column has a visible glyph rather than a 1x1 transparent
# placeholder. We can't query the per-row button texture from outside Tree,
# but the bank_mode owns the texture in _play_button_icon; assert it is a
# Texture2D with non-zero size.
func test_bank_mode_play_icon_is_visible_texture():
	var bank_mode: Control = BankModeScene.instantiate()
	add_child_autofree(bank_mode)
	await get_tree().process_frame
	assert_not_null(bank_mode._play_button_icon, "play icon allocated")
	var tex: Texture2D = bank_mode._play_button_icon
	if tex != null:
		var size := tex.get_size()
		assert_gt(size.x, 1.0, "play icon width > 1px (not transparent placeholder)")
		assert_gt(size.y, 1.0, "play icon height > 1px (not transparent placeholder)")


# Polish A3: global Stop button is present on the toolbar and wired up.
func test_bank_mode_stop_button_exists():
	var bank_mode: Control = BankModeScene.instantiate()
	add_child_autofree(bank_mode)
	await get_tree().process_frame
	var stop_btn: Button = bank_mode.get_node_or_null("VBox/Toolbar/StopButton")
	assert_not_null(stop_btn, "StopButton present in toolbar")
	if stop_btn != null:
		# Pressing it should not raise even when no preview is in flight; the
		# preview pool's stop_all() short-circuits gracefully.
		stop_btn.pressed.emit()
		pass_test("stop press did not crash")


func test_load_wav_as_float32_decodes_16bit_pcm():
	# Build a tiny 16-bit PCM WAV (4 stereo samples = 8 int16) in memory
	# and write it to user:// so the parser can open it like any artist
	# WAV. Verifies the parser walks past extra chunks as well.
	var path := "user://test_wav_parser.wav"
	var w := FileAccess.open(path, FileAccess.WRITE)
	# RIFF header: tag, total_size (filled below), "WAVE"
	w.store_buffer(PackedByteArray([0x52, 0x49, 0x46, 0x46]))  # "RIFF"
	w.store_32(0)  # placeholder; we won't validate it
	w.store_buffer(PackedByteArray([0x57, 0x41, 0x56, 0x45]))  # "WAVE"
	# fmt chunk
	w.store_buffer(PackedByteArray([0x66, 0x6d, 0x74, 0x20]))  # "fmt "
	w.store_32(16)
	w.store_16(1)       # PCM
	w.store_16(2)       # 2 channels
	w.store_32(22050)   # sample rate
	w.store_32(22050 * 2 * 2)  # byte rate
	w.store_16(4)       # block align
	w.store_16(16)      # bits per sample
	# JUNK chunk (validates skip)
	w.store_buffer(PackedByteArray([0x4a, 0x55, 0x4e, 0x4b]))  # "JUNK"
	w.store_32(2)
	w.store_buffer(PackedByteArray([0xaa, 0xbb]))
	# data chunk: 8 int16 samples (4 stereo frames)
	w.store_buffer(PackedByteArray([0x64, 0x61, 0x74, 0x61]))  # "data"
	w.store_32(16)
	w.store_16(0)
	w.store_16(0x4000)  # ~ +0.5
	w.store_16(0xc000)  # ~ -0.5 (signed 16-bit two's complement)
	w.store_16(0x7fff)  # ~ +1.0
	w.store_16(0x8001)  # near -1.0
	w.store_16(0)
	w.store_16(0)
	w.store_16(0)
	w.close()

	var bank_mode: Control = BankModeScene.instantiate()
	add_child_autofree(bank_mode)
	var samples: PackedFloat32Array = bank_mode._load_wav_as_float32(path)
	assert_eq(samples.size(), 8, "8 int16 samples parsed")
	assert_almost_eq(samples[0], 0.0, 0.001)
	assert_almost_eq(samples[1], 0.5, 0.01)
	assert_almost_eq(samples[2], -0.5, 0.01)
	assert_almost_eq(samples[3], 1.0, 0.01)
