class_name MusicBankMode
extends Control

# View of the current document's NovaSbfBank. The track Tree lists every
# entry (#, name, preview button column); the toolbar above carries the
# reorder / rename / replace / add / delete affordances. The Replace WAV
# and Add Track flows arrive in Phase E3.

## Import/replace failures surface twice on purpose: the console line stays at
## the source (push_error) and this signal carries the same message upward for
## the shell toast (root relays it to the workspace; standalone owners just log).
signal error_reported(message: String)

const MusicAudioPreviewClass = preload("res://modtools/music/music_audio_preview.gd")
const ADD_TRACK_TOOLTIP := "Import a 16-bit WAV into the loaded sound bank."
const NO_BANK_TOOLTIP := "Open or create a music project first"
const SELECT_TRACK_TOOLTIP := "Select a track first"
const FIRST_TRACK_TOOLTIP := "Already the first track"
const LAST_TRACK_TOOLTIP := "Already the last track"
const NO_BANK_ROW_TEXT := "Open or create a music project to import tracks"
const EMPTY_BANK_ROW_TEXT := "No tracks yet — ＋ Add to import a 16-bit WAV"

var _document: RefCounted   # MusicEditorDocument
var _preview: Node           # MusicAudioPreview
var _play_button_icon: Texture2D

@onready var _track_tree: Tree = %TrackTree
@onready var _toolbar: HFlowContainer = %Toolbar
@onready var _add_button: Button = %AddButton
@onready var _replace_button: Button = %ReplaceButton
@onready var _move_up_button: Button = %MoveUpButton
@onready var _move_down_button: Button = %MoveDownButton
@onready var _rename_button: Button = %RenameButton
@onready var _delete_button: Button = %DeleteButton
@onready var _stop_button: Button = %StopButton


func _ready() -> void:
	_track_tree.columns = 3
	_track_tree.set_column_title(0, "#")
	_track_tree.set_column_title(1, "name")
	_track_tree.set_column_title(2, "play")
	_track_tree.column_titles_visible = true
	# The bank is a flat list (create_item(root) children, no nesting), so hide
	# the root row and the per-item fold-arrow gutter. The gutter is column 0's
	# (the "#" column's) dead space; reclaiming it lets two/three-digit indices
	# show in full instead of clipping "10/11/12" down to "1".
	_track_tree.hide_root = true
	_track_tree.hide_folding = true
	# Column widths: # column fixed narrow, name expands, play column fixed
	# narrow on the right. Without expand_ratio(2, 0), Godot would let the
	# play column eat ~1/3 of the table because all columns default to
	# expand_ratio == 1.
	_track_tree.set_column_expand(0, false)
	_track_tree.set_column_expand_ratio(0, 0)
	_track_tree.set_column_custom_minimum_width(0, 48)
	_track_tree.set_column_expand(1, true)
	_track_tree.set_column_expand_ratio(1, 1)
	_track_tree.set_column_expand(2, false)
	_track_tree.set_column_expand_ratio(2, 0)
	_track_tree.set_column_custom_minimum_width(2, 32)
	_play_button_icon = _build_play_icon()
	_preview = MusicAudioPreviewClass.new()
	_preview.name = "AudioPreview"
	add_child(_preview)
	_track_tree.button_clicked.connect(_on_row_button)
	_track_tree.cell_selected.connect(_on_cell_selected)
	_track_tree.item_edited.connect(_on_track_edited)
	_add_button.pressed.connect(_on_add_pressed)
	_replace_button.pressed.connect(_on_replace_wav_pressed)
	_move_up_button.pressed.connect(_on_move_up_pressed)
	_move_down_button.pressed.connect(_on_move_down_pressed)
	_rename_button.pressed.connect(_on_rename_pressed)
	_delete_button.pressed.connect(_on_delete_pressed)
	_stop_button.pressed.connect(_on_stop_pressed)
	# Tracks are a drag source: drop one on a state in the section map to add a
	# `play` (the drop side is gated behind the structured-edit parity check).
	_track_tree.set_drag_forwarding(_tree_get_drag_data, Callable(), Callable())
	_refresh_table()
	_refresh_toolbar_state()


func bind_document(document: RefCounted) -> void:
	if _document == document:
		return
	if _document != null and _document.has_signal("changed"):
		if _document.changed.is_connected(_refresh_table):
			_document.changed.disconnect(_refresh_table)
	_document = document
	if _document != null and _document.has_signal("changed"):
		_document.changed.connect(_refresh_table)
	if is_node_ready():
		_refresh_table()
		_refresh_toolbar_state()


func _refresh_table() -> void:
	if _track_tree == null:
		return
	_track_tree.clear()
	var root := _track_tree.create_item()
	if _document == null or not _document.bank_loaded():
		_add_hint_row(root, NO_BANK_ROW_TEXT)
		_refresh_toolbar_state()
		return
	var bank: NovaSbfBank = _document.bank
	var entries: Array = bank.get_entries()
	if entries.is_empty():
		# A freshly-created (or emptied) bank: point at the import affordance so
		# the dock doesn't read as a dead blank list.
		_add_hint_row(root, EMPTY_BANK_ROW_TEXT)
		_refresh_toolbar_state()
		return
	for i in range(entries.size()):
		var d: Dictionary = entries[i]
		var item := _track_tree.create_item(root)
		item.set_text(0, str(i))
		item.set_text(1, str(d.get("name", "")))
		# Play button in column 2; ID 0 distinguishes it for _on_row_button.
		# Tree.add_button rejects a null Texture, so we always feed the
		# editor-icon-or-fallback texture built in _ready.
		if _play_button_icon != null:
			item.add_button(2, _play_button_icon, 0, false, "Preview")
		item.set_metadata(0, i)
	_refresh_toolbar_state()


func _add_hint_row(root: TreeItem, text: String) -> void:
	var hint := _track_tree.create_item(root)
	hint.set_text(1, text)
	hint.set_custom_color(1, Color(0.6, 0.6, 0.6))
	hint.set_selectable(0, false)
	hint.set_selectable(1, false)
	hint.set_selectable(2, false)


# Real play glyph for the per-row preview button. In editor context the
# theme exposes a Play icon directly; in runtime context (no editor theme)
# we fall back to a procedurally drawn white triangle so the column always
# renders something visible. The previous 1x1 transparent placeholder
# masqueraded as the affordance and was effectively invisible.
func _build_play_icon() -> Texture2D:
	var theme_icon: Texture2D = null
	if has_theme_icon(&"Play", &"EditorIcons"):
		theme_icon = get_theme_icon(&"Play", &"EditorIcons")
	if theme_icon != null:
		return theme_icon
	# Fallback: a 16x16 image with a right-pointing white triangle.
	var size := 16
	var img := Image.create(size, size, false, Image.FORMAT_RGBA8)
	img.fill(Color(0, 0, 0, 0))
	for y in range(size):
		# Triangle vertices at (3,3), (3,size-3), (size-3, size/2). Width
		# tapers linearly from y == 3 (width = size-6) to y == size-3.
		var dy := absi(y - size / 2)
		if dy >= (size / 2) - 3:
			continue
		var width := (size - 6) - dy * 2
		for x in range(width):
			img.set_pixel(3 + x, y, Color(1, 1, 1, 1))
	return ImageTexture.create_from_image(img)


func _on_row_button(item: TreeItem, _column: int, _id: int, _mouse_button_index: int) -> void:
	if item == null:
		return
	var index: int = int(item.get_metadata(0))
	_preview_index(index)


func _on_cell_selected() -> void:
	# Selection-driven preview lands with the waveform meter in a later phase;
	# for now the button alone triggers playback so quick scanning still works.
	# We do hook this to refresh the selection-dependent toolbar buttons.
	_refresh_toolbar_state()


# Stop button handler: halts every in-pool preview AudioStreamPlayer. Always
# enabled (no selection required) so the user can shut up the audio without
# first hunting for a row.
func _on_stop_pressed() -> void:
	if _preview != null:
		_preview.stop_all()


# Selection-dependent toolbar buttons gray out when no row is selected; move
# buttons also gray out at the first/last row where pressing them would do
# nothing. Called on _ready, document binding, table refresh, and selection.
func _refresh_toolbar_state() -> void:
	var idx: int = _selected_index() if _track_tree != null else -1
	var has_selection: bool = idx >= 0
	var has_bank: bool = _document != null and _document.bank_loaded()
	var row_count := _row_count()
	if _add_button != null:
		_add_button.disabled = not has_bank
		_add_button.tooltip_text = ADD_TRACK_TOOLTIP if has_bank else NO_BANK_TOOLTIP
	_set_toolbar_button(_move_up_button, has_selection and idx > 0,
		"Move the selected track up.", SELECT_TRACK_TOOLTIP if not has_selection else FIRST_TRACK_TOOLTIP)
	_set_toolbar_button(_move_down_button, has_selection and idx < row_count - 1,
		"Move the selected track down.", SELECT_TRACK_TOOLTIP if not has_selection else LAST_TRACK_TOOLTIP)
	_set_selection_button(_rename_button, has_selection, "Rename the selected track.")
	_set_selection_button(_delete_button, has_selection, "Delete the selected track.")
	_set_selection_button(_replace_button, has_selection, "Replace the selected track audio with a WAV.")


func _set_selection_button(button: Button, has_selection: bool, enabled_tip: String) -> void:
	_set_toolbar_button(button, has_selection, enabled_tip, SELECT_TRACK_TOOLTIP)


func _set_toolbar_button(button: Button, enabled: bool, enabled_tip: String, disabled_tip: String) -> void:
	if button == null:
		return
	button.disabled = not enabled
	button.tooltip_text = enabled_tip if enabled else disabled_tip


func _preview_index(index: int) -> void:
	if _preview == null:
		return
	if _document == null or not _document.bank_loaded():
		return
	var stream: NovaSbfAudioStream = _document.bank.get_stream_at(index)
	if stream != null:
		_preview.play_stream(stream)


# --- Drag source: a track row can be dragged onto a state in the section map
# to add a `play` for it. The payload is {kind, index, name}; the map's drop
# side stays disabled until the structured-edit parity gate is green.

func _track_drag_payload(index: int) -> Dictionary:
	if _document == null or not _document.bank_loaded():
		return {}
	var entries: Array = _document.bank.get_entries()
	if index < 0 or index >= entries.size():
		return {}
	return {"kind": "mus_track", "index": index, "name": String(entries[index].get("name", ""))}


func _tree_get_drag_data(at_position: Vector2) -> Variant:
	var item := _track_tree.get_item_at_position(at_position)
	if item == null:
		return null
	var payload := _track_drag_payload(int(item.get_metadata(0)))
	if payload.is_empty():
		return null
	var preview := Label.new()
	preview.text = "♪ %s" % payload["name"]
	set_drag_preview(preview)
	return payload


# --- Phase E2: toolbar handlers ------------------------------------------

func _selected_index() -> int:
	var item := _track_tree.get_selected()
	if item == null:
		return -1
	return int(item.get_metadata(0))


func _row_count() -> int:
	if _document == null or not _document.bank_loaded():
		return 0
	return _document.bank.get_entry_count()


func _select_index(index: int) -> void:
	# Re-resolve TreeItem after the table rebuilds; keep the same row in
	# focus so subsequent reorder clicks chain naturally.
	var root := _track_tree.get_root()
	if root == null:
		return
	var item := root.get_first_child()
	while item != null:
		if int(item.get_metadata(0)) == index:
			item.select(0)
			_track_tree.scroll_to_item(item)
			return
		item = item.get_next()


func _on_move_up_pressed() -> void:
	if _document == null:
		return
	var idx := _selected_index()
	if idx > 0:
		_document.reorder_track(idx, idx - 1)
		_select_index(idx - 1)


func _on_move_down_pressed() -> void:
	if _document == null:
		return
	var idx := _selected_index()
	if idx < 0:
		return
	if idx < _row_count() - 1:
		_document.reorder_track(idx, idx + 1)
		_select_index(idx + 1)


func _on_rename_pressed() -> void:
	if _document == null:
		return
	var idx := _selected_index()
	if idx < 0:
		return
	var item := _track_tree.get_selected()
	if item == null:
		return
	# Inline edit on the name column. Tree.edit_selected drives the in-place
	# editor; the item_edited signal lands in _on_track_edited below.
	item.set_editable(1, true)
	_track_tree.edit_selected()


func _on_track_edited() -> void:
	if _document == null:
		return
	var item := _track_tree.get_edited()
	if item == null:
		return
	var col := _track_tree.get_edited_column()
	if col != 1:
		return
	var idx: int = int(item.get_metadata(0))
	var new_name: String = item.get_text(1)
	if new_name.length() > 15:
		new_name = new_name.substr(0, 15)
	_document.rename_track(idx, StringName(new_name))
	item.set_editable(1, false)


func _on_delete_pressed() -> void:
	if _document == null:
		return
	var idx := _selected_index()
	if idx >= 0:
		_document.delete_track(idx)


# --- Phase E3: Replace WAV + Add Track flows ----------------------------
#
# Both flows pop a FileDialog (ACCESS_FILESYSTEM so artists can grab WAVs
# from anywhere on disk) and feed the loaded float32 samples into the
# document. The minimal RIFF/WAV parser handles the typical NovaLogic
# pattern (22050 Hz 16-bit stereo) and skips any informational chunks
# until it finds "data".

func _on_replace_wav_pressed() -> void:
	if _document == null:
		return
	var idx := _selected_index()
	if idx < 0:
		return
	_ensure_files().open("Choose a WAV", PackedStringArray(["*.wav ; WAV audio"]),
		func(path: String): _replace_wav(idx, path))


func _replace_wav(idx: int, path: String) -> void:
	var samples := _load_wav_as_float32(path)
	if samples.is_empty():
		_report_error("WAV load failed: %s" % path)
		return
	var err: int = _document.replace_track_audio(idx, samples)
	if err != OK:
		_report_error("replace_track_audio failed: %d" % err)


func _on_add_pressed() -> void:
	if _document == null:
		return
	_ensure_files().open("Choose a WAV", PackedStringArray(["*.wav ; WAV audio"]), _on_add_path_selected)


# Console line + toast relay, one call (see error_reported).
func _report_error(message: String) -> void:
	push_error(message)
	error_reported.emit(message)


var _files: FileDialogHelper


func _ensure_files() -> FileDialogHelper:
	if _files == null:
		_files = FileDialogHelper.new(self)
	return _files


func _on_add_path_selected(path: String) -> void:
	var samples := _load_wav_as_float32(path)
	if samples.is_empty():
		_report_error("WAV load failed: %s" % path)
		return
	# Default name from filename basename, capped at 15 chars to leave the
	# null terminator slot in the entry's name[16] field.
	var basename: String = path.get_file().get_basename().substr(0, 15)
	_document.add_track(StringName(basename), samples)


# Minimal RIFF/WAV reader: 16-bit PCM only. Walks the chunk list to locate
# fmt + data because real WAVs frequently carry LIST/INFO/bext chunks
# between the header and the audio payload. Returns interleaved float32
# samples in [-1, 1].
func _load_wav_as_float32(path: String) -> PackedFloat32Array:
	var fa := FileAccess.open(path, FileAccess.READ)
	if fa == null:
		return PackedFloat32Array()
	var bytes := fa.get_buffer(fa.get_length())
	fa.close()
	if bytes.size() < 44:
		return PackedFloat32Array()
	# RIFF / WAVE preamble
	if bytes.decode_u32(0) != 0x46464952:  # "RIFF"
		return PackedFloat32Array()
	if bytes.decode_u32(8) != 0x45564157:  # "WAVE"
		return PackedFloat32Array()
	var bits: int = 0
	var data_offset: int = -1
	var data_size: int = 0
	var i: int = 12
	while i + 8 <= bytes.size():
		var tag := bytes.decode_u32(i)
		var sz := int(bytes.decode_u32(i + 4))
		if tag == 0x20746d66:  # "fmt "
			# fmt: u16 format, u16 channels, u32 rate, u32 byte_rate,
			# u16 block, u16 bits_per_sample.
			if sz >= 16 and i + 8 + sz <= bytes.size():
				bits = bytes.decode_u16(i + 8 + 14)
		elif tag == 0x61746164:  # "data"
			data_offset = i + 8
			data_size = sz
			break
		i += 8 + sz
		# Chunks pad to even length on disk.
		if sz & 1:
			i += 1
	if bits != 16:
		_report_error("only 16-bit PCM WAV supported in v1; got bits=%d" % bits)
		return PackedFloat32Array()
	if data_offset < 0 or data_offset + data_size > bytes.size():
		return PackedFloat32Array()
	var sample_count := data_size / 2
	var out := PackedFloat32Array()
	out.resize(sample_count)
	for j in range(sample_count):
		var s := bytes.decode_s16(data_offset + j * 2)
		out[j] = float(s) / 32768.0
	return out
