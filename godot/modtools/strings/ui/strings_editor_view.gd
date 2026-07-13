extends Control

## Self-contained center surface for the Strings workspace: an HSplitContainer
## holding the entry table on the left and the per-entry detail editor on the
## right. Owns the table<->detail wiring directly so selecting a row and editing a
## field never bounce through the editor shell. Structural document changes trigger
## one rebuild; live field edits patch a single table row.

const StringsTableViewScript = preload("res://modtools/strings/ui/strings_table_view.gd")
const StringsDetailPanelScript = preload("res://modtools/strings/ui/strings_detail_dock.gd")

const DETAIL_MIN_WIDTH := 360

var _doc: StringsEditor
var _table: Control
var _detail: Control
var _font_service: Dictionary = {}


func _ready() -> void:
	set_anchors_preset(Control.PRESET_FULL_RECT)

	var split := HSplitContainer.new()
	split.name = "StringsSplit"
	split.set_anchors_preset(Control.PRESET_FULL_RECT)
	split.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	split.size_flags_vertical = Control.SIZE_EXPAND_FILL
	add_child(split)

	_table = StringsTableViewScript.new()
	_table.name = "StringsTableView"
	_table.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_table.size_flags_vertical = Control.SIZE_EXPAND_FILL
	split.add_child(_table)

	_detail = StringsDetailPanelScript.new()
	_detail.name = "StringsDetailPanel"
	_detail.custom_minimum_size = Vector2(DETAIL_MIN_WIDTH, 0)
	split.add_child(_detail)

	_table.entry_selected.connect(_on_table_selected)
	_table.delete_requested.connect(_on_delete_requested)
	_detail.setup(_doc, _patch_row)
	if not _font_service.is_empty():
		_detail.set_font_service(_font_service)

	if _doc != null:
		_table.set_document(_doc)
	rebuild()


func set_document(doc: StringsEditor) -> void:
	_doc = doc
	if _table != null:
		_table.set_document(doc)
	if _detail != null:
		_detail.set_document(doc)
	rebuild()


## Forward the workspace's font capability ({list, load}) to the detail panel's
## game preview; stashed when the view has not built yet (mount order).
func set_font_service(service: Dictionary) -> void:
	_font_service = service
	if _detail != null:
		_detail.set_font_service(service)


func set_filter(search: String, section_filter: int) -> void:
	if _table != null:
		_table.set_filter(search, section_filter)
		_table.select_index(_doc.selected_index if _doc != null else -1)


func rebuild() -> void:
	if _table == null or _doc == null:
		return
	_table.rebuild()
	_table.select_index(_doc.selected_index)
	if _detail != null:
		_detail.show_entry(_doc.selected_index)


func _patch_row(index: int) -> void:
	if _table != null:
		_table.update_row(index)


func _on_table_selected(index: int) -> void:
	if _doc != null:
		_doc.selected_index = index
	if _detail != null:
		_detail.show_entry(index)


func _on_delete_requested(index: int) -> void:
	# Defer so the table is not rebuilt from inside its own gui_input handler.
	if _doc != null:
		_doc.call_deferred("remove_entry", index)
