class_name MusicBankMode
extends Control

# View of the current document's NovaSbfBank. The track Tree lists every
# entry (#, name, preview button column); the toolbar above hosts the
# reorder / rename / replace / add / delete affordances. The Replace WAV
# and Add Track flows arrive in Phase E3.

const MusicAudioPreviewClass = preload("res://modtools/music/music_audio_preview.gd")

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
	# Column widths: # column fixed narrow, name expands, play column fixed
	# narrow on the right. Without expand_ratio(2, 0), Godot would let the
	# play column eat ~1/3 of the table because all columns default to
	# expand_ratio == 1.
	_track_tree.set_column_expand(0, false)
	_track_tree.set_column_expand_ratio(0, 0)
	_track_tree.set_column_custom_minimum_width(0, 40)
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
		_refresh_toolbar_state()
		return
	var bank: NovaSbfBank = _document.bank
	var entries: Array = bank.get_entries()
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


# Selection-dependent toolbar buttons gray out when there is no row selected.
# Add and Stop stay enabled regardless. Called on _ready, document binding,
# table refresh, and cell selection changes.
func _refresh_toolbar_state() -> void:
	var idx: int = _selected_index() if _track_tree != null else -1
	var has_selection: bool = idx >= 0
	if _move_up_button != null:
		_move_up_button.disabled = not has_selection
	if _move_down_button != null:
		_move_down_button.disabled = not has_selection
	if _rename_button != null:
		_rename_button.disabled = not has_selection
	if _delete_button != null:
		_delete_button.disabled = not has_selection
	if _replace_button != null:
		_replace_button.disabled = not has_selection


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
	var dialog := FileDialog.new()
	dialog.access = FileDialog.ACCESS_FILESYSTEM
	dialog.file_mode = FileDialog.FILE_MODE_OPEN_FILE
	dialog.add_filter("*.wav", "WAV audio")
	dialog.file_selected.connect(func(path: String): _replace_wav(idx, path); dialog.queue_free())
	dialog.canceled.connect(dialog.queue_free)
	dialog.close_requested.connect(dialog.queue_free)
	add_child(dialog)
	dialog.popup_centered_ratio(0.6)


func _replace_wav(idx: int, path: String) -> void:
	var samples := _load_wav_as_float32(path)
	if samples.is_empty():
		push_error("WAV load failed: %s" % path)
		return
	var err: int = _document.replace_track_audio(idx, samples)
	if err != OK:
		push_error("replace_track_audio failed: %d" % err)


func _on_add_pressed() -> void:
	if _document == null:
		return
	var dialog := FileDialog.new()
	dialog.access = FileDialog.ACCESS_FILESYSTEM
	dialog.file_mode = FileDialog.FILE_MODE_OPEN_FILE
	dialog.add_filter("*.wav", "WAV audio")
	dialog.file_selected.connect(func(path: String): _on_add_path_selected(path); dialog.queue_free())
	dialog.canceled.connect(dialog.queue_free)
	dialog.close_requested.connect(dialog.queue_free)
	add_child(dialog)
	dialog.popup_centered_ratio(0.6)


func _on_add_path_selected(path: String) -> void:
	var samples := _load_wav_as_float32(path)
	if samples.is_empty():
		push_error("WAV load failed: %s" % path)
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
		push_error("only 16-bit PCM WAV supported in v1; got bits=%d" % bits)
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
