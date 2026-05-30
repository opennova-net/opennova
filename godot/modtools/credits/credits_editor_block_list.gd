class_name CreditsEditorBlockList
extends VBoxContainer

const BlockCardScene = preload("res://modtools/credits/credits_editor_block_card.tscn")
const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")

signal entries_reordered
signal selection_changed(entry)
signal request_edit_font(font_name)

var _resource: CbinCreditsResource
var _vbox: VBoxContainer
var _font_options: PackedStringArray = PackedStringArray()
var _entry_to_card: Dictionary = {}  # CbinEntry -> CreditsEditorBlockCard
var _reconcile_pending: bool = false
var _selected_entry: CbinEntry
var _selection_emit_pending: bool = false
var _resource_root_dir: String = ""

func _ready() -> void:
	_vbox = self
	if _resource_root_dir.is_empty():
		_resource_root_dir = ResourceDirSettings.get_resource_dir()
	_refresh_font_options()

func set_resource_root_dir(path: String) -> void:
	_resource_root_dir = path.strip_edges()
	_refresh_font_options()
	_reconcile()

func set_resource(value: CbinCreditsResource) -> void:
	if _resource == value:
		return
	if _resource and _resource.entries_structure_changed.is_connected(_schedule_reconcile):
		_resource.entries_structure_changed.disconnect(_schedule_reconcile)
	_resource = value
	if _resource:
		_resource.entries_structure_changed.connect(_schedule_reconcile)
	_refresh_font_options()
	_reconcile()

func _schedule_reconcile() -> void:
	if _reconcile_pending:
		return
	_reconcile_pending = true
	call_deferred("_do_deferred_reconcile")

func _do_deferred_reconcile() -> void:
	_reconcile_pending = false
	_reconcile()

func _refresh_font_options() -> void:
	_font_options = PackedStringArray()
	var dir := DirAccess.open(_resource_root_dir) if not _resource_root_dir.is_empty() else null
	if dir != null:
		dir.list_dir_begin()
		var name := dir.get_next()
		while not name.is_empty():
			if not dir.current_is_dir() and name.get_extension().to_lower() == "fnt":
				_append_font_option(name.get_basename())
			name = dir.get_next()
		dir.list_dir_end()
	if _resource != null:
		for i in range(_resource.get_entry_count()):
			var entry := _resource.get_entry(i)
			if entry is CbinTextEntry:
				_append_font_option((entry as CbinTextEntry).get_font_name())

func _append_font_option(name: String) -> void:
	if name.is_empty():
		return
	for existing in _font_options:
		if existing == name:
			return
	_font_options.append(name)

func _reconcile() -> void:
	if _vbox == null:
		return

	var live: Dictionary = {}
	if _resource != null:
		for i in range(_resource.get_entry_count()):
			live[_resource.get_entry(i)] = true

	# Free orphans
	var orphans: Array = []
	for entry in _entry_to_card.keys():
		if not live.has(entry):
			orphans.append(entry)
	for entry in orphans:
		var card: Node = _entry_to_card[entry]
		if is_instance_valid(card):
			card.queue_free()
		_entry_to_card.erase(entry)

	if _resource == null:
		_set_selected_entry(null, false)
		return

	_refresh_font_options()

	# Clear selection if its entry was removed
	if _selected_entry != null and not live.has(_selected_entry):
		_set_selected_entry(null, true)

	# Add + reorder
	for i in range(_resource.get_entry_count()):
		var entry := _resource.get_entry(i)
		var card: CreditsEditorBlockCard
		if _entry_to_card.has(entry):
			card = _entry_to_card[entry]
		else:
			card = BlockCardScene.instantiate() as CreditsEditorBlockCard
			_vbox.add_child(card)
			card.request_delete.connect(_on_card_delete)
			card.request_select.connect(_on_card_select)
			card.request_edit_font.connect(_on_card_request_edit_font)
			_entry_to_card[entry] = card
		card.bind(entry, _font_options, _resource_root_dir)
		if _vbox.get_child(i) != card:
			_vbox.move_child(card, i)

	_refresh_selection_visual()

func add_text() -> void:
	var e := CbinTextEntry.new()
	e.set_text("New text")
	_insert_with_selection(e)

func add_image() -> void:
	var e := CbinImageEntry.new()
	e.set_advances_y(true)
	_insert_with_selection(e)

func add_newline() -> void:
	_insert_with_selection(CbinNewlineEntry.new())

func _insert_with_selection(entry: CbinEntry) -> void:
	if _resource == null:
		return
	var sel_idx := _index_of(_selected_entry) if _selected_entry != null else -1
	if sel_idx < 0:
		_resource.add_entry(entry)
	else:
		_resource.insert_entry(sel_idx + 1, entry)
	_set_selected_entry(entry, false)  # chain successive adds naturally
	_schedule_selection_emit()

func _index_of(entry: CbinEntry) -> int:
	if _resource == null or entry == null:
		return -1
	for i in range(_resource.get_entry_count()):
		if _resource.get_entry(i) == entry:
			return i
	return -1

func select_entry(entry: CbinEntry) -> void:
	_set_selected_entry(entry, true)

func card_for_entry(entry: CbinEntry) -> CreditsEditorBlockCard:
	if entry == null:
		return null
	return _entry_to_card.get(entry)

# Keyboard helpers (driven from CreditsEditor._shortcut_input). Both return true
# only when they acted, so the caller can decide whether to consume the event.
func delete_selected() -> bool:
	if _selected_entry == null:
		return false
	var card := card_for_entry(_selected_entry)
	if card == null:
		return false
	_on_card_delete(card)
	return true

func move_selected(delta: int) -> bool:
	if _resource == null or _selected_entry == null:
		return false
	var src_index := _index_of(_selected_entry)
	if src_index < 0:
		return false
	# ±1 deltas map directly to a valid insert index after removal (verified against
	# the same remove_entry/insert_entry shift semantics used by _drop_data).
	var target_index := clampi(src_index + delta, 0, _resource.get_entry_count() - 1)
	if target_index == src_index:
		return false
	var entry := _selected_entry
	_resource.remove_entry(src_index)
	_resource.insert_entry(target_index, entry)
	_schedule_selection_emit()
	entries_reordered.emit()
	return true

func _on_card_select(card: CreditsEditorBlockCard) -> void:
	select_entry(card.get_entry())


func _on_card_request_edit_font(font_name: String) -> void:
	request_edit_font.emit(font_name)


func _refresh_selection_visual() -> void:
	for entry in _entry_to_card.keys():
		var card = _entry_to_card[entry]
		if is_instance_valid(card):
			card.set_selected(entry == _selected_entry)

func _on_card_delete(card: CreditsEditorBlockCard) -> void:
	if _resource == null:
		return
	var entry := card.get_entry()
	for i in range(_resource.get_entry_count()):
		if _resource.get_entry(i) == entry:
			var next_selection: CbinEntry = null
			if entry == _selected_entry:
				if i + 1 < _resource.get_entry_count():
					next_selection = _resource.get_entry(i + 1)
				elif i - 1 >= 0:
					next_selection = _resource.get_entry(i - 1)
			_resource.remove_entry(i)
			if entry == _selected_entry:
				_set_selected_entry(next_selection, true)
			break

func _can_drop_data(_pos: Vector2, data: Variant) -> bool:
	return typeof(data) == TYPE_DICTIONARY and data.has("card")

func _drop_data(pos: Vector2, data: Variant) -> void:
	var dragged_card := data["card"] as CreditsEditorBlockCard
	if dragged_card == null or _resource == null:
		return
	var dragged_entry := dragged_card.get_entry()
	var target_index := _index_at_position(pos)
	var src_index := -1
	for i in range(_resource.get_entry_count()):
		if _resource.get_entry(i) == dragged_entry:
			src_index = i
			break
	if src_index < 0 or src_index == target_index:
		return
	_resource.remove_entry(src_index)
	if target_index > src_index:
		target_index -= 1
	_resource.insert_entry(target_index, dragged_entry)
	if dragged_entry == _selected_entry:
		_schedule_selection_emit()
	entries_reordered.emit()

func _index_at_position(_pos: Vector2) -> int:
	var local_y := _vbox.get_local_mouse_position().y
	var idx := 0
	for child in _vbox.get_children():
		var card := child as Control
		if card == null:
			continue
		var rect := card.get_rect()
		if local_y < rect.position.y + rect.size.y * 0.5:
			return idx
		idx += 1
	return idx

func _set_selected_entry(entry: CbinEntry, emit_changed_signal: bool) -> void:
	if _selected_entry == entry:
		if emit_changed_signal:
			_refresh_selection_visual()
		return
	_selected_entry = entry
	_refresh_selection_visual()
	if emit_changed_signal:
		selection_changed.emit(_selected_entry)

func _schedule_selection_emit() -> void:
	if _selection_emit_pending:
		return
	_selection_emit_pending = true
	call_deferred("_emit_pending_selection")

func _emit_pending_selection() -> void:
	_selection_emit_pending = false
	_refresh_selection_visual()
	selection_changed.emit(_selected_entry)
