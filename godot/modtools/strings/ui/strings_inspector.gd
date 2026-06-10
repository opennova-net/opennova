extends Control

## Left pane: search, section filtering and management, a validation summary, and
## CSV import/export for the Strings workspace. Reads/writes through the workspace
## coordinator so the center table and detail stay in sync.

const OK_COLOR := Color(0.55, 0.82, 0.55)
const ISSUE_COLOR := Color(1.0, 0.7, 0.4)

var _ws: StringsEditorWorkspace
var _doc: StringsEditor
var _loading: bool = false

var _search_edit: LineEdit
var _section_filter: OptionButton
var _section_name_edit: LineEdit
var _validation_label: Label
var _normalize_button: Button
var _lookup_edit: LineEdit
var _lookup_result: RichTextLabel
var _import_dialog: FileDialog
var _export_dialog: FileDialog
var _rename_section_button: Button
var _remove_section_button: Button


func setup(workspace: StringsEditorWorkspace) -> void:
	_ws = workspace
	_doc = workspace.get_document()


func _ready() -> void:
	var box := ObjectUiHelpers.make_inspector_box(self)
	# make_inspector_box adds a MarginContainer to this bare Control, which does not
	# lay out its children — stretch that wrapper to fill the inspector host.
	(box.get_parent().get_parent() as Control).set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)

	ObjectUiHelpers.add_section_heading(box, "Find")
	_search_edit = LineEdit.new()
	_search_edit.name = "StringsSearchEdit"
	_search_edit.placeholder_text = "Search keys and text..."
	_search_edit.clear_button_enabled = true
	_search_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_search_edit.text_changed.connect(func(text): if _ws != null: _ws.set_search(text))
	box.add_child(_search_edit)

	var filter_row := ObjectUiHelpers.add_detail_field(box, "Section filter")
	_section_filter = OptionButton.new()
	_section_filter.name = "StringsSectionFilter"
	_section_filter.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_section_filter.item_selected.connect(_on_filter_selected)
	filter_row.add_child(_section_filter)

	var entry_buttons := HBoxContainer.new()
	entry_buttons.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(entry_buttons)
	_add_button(entry_buttons, "StringsAddEntryButton", "Add String", func(): if _ws != null: _ws.add_entry_default())
	_add_button(entry_buttons, "StringsRemoveEntryButton", "Remove String", func(): if _ws != null: _ws.remove_selected_entry())

	ObjectUiHelpers.add_section_heading(box, "Sections")
	_section_name_edit = LineEdit.new()
	_section_name_edit.name = "StringsSectionNameEdit"
	_section_name_edit.placeholder_text = "Section name"
	_section_name_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(_section_name_edit)

	var section_buttons := HBoxContainer.new()
	section_buttons.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(section_buttons)
	_add_button(section_buttons, "StringsAddSectionButton", "Add", _on_add_section)
	_rename_section_button = _add_button(section_buttons, "StringsRenameSectionButton", "Rename", _on_rename_section)
	_remove_section_button = _add_button(section_buttons, "StringsRemoveSectionButton", "Remove", _on_remove_section)

	ObjectUiHelpers.add_section_heading(box, "Validation")
	_validation_label = ObjectUiHelpers.add_muted_label(box, "No issues")
	_validation_label.name = "StringsValidationLabel"
	_normalize_button = _add_button(box, "StringsNormalizeButton", "Group entries by section", _on_normalize)
	_normalize_button.tooltip_text = "Reorder entries so each section's strings sit together, the layout the game requires."
	_normalize_button.visible = false

	ObjectUiHelpers.add_section_heading(box, "Lookup tester")
	_lookup_edit = LineEdit.new()
	_lookup_edit.name = "StringsLookupEdit"
	_lookup_edit.placeholder_text = "section:key  (or just key)"
	_lookup_edit.tooltip_text = "Resolve a string the way the game does: section-scoped, first match wins. A plain key searches all sections."
	_lookup_edit.clear_button_enabled = true
	_lookup_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_lookup_edit.text_changed.connect(func(_text): _refresh_lookup())
	box.add_child(_lookup_edit)
	_lookup_result = RichTextLabel.new()
	_lookup_result.name = "StringsLookupResult"
	_lookup_result.bbcode_enabled = true
	_lookup_result.fit_content = true
	_lookup_result.scroll_active = false
	_lookup_result.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(_lookup_result)

	ObjectUiHelpers.add_section_heading(box, "CSV")
	var csv_buttons := HBoxContainer.new()
	csv_buttons.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(csv_buttons)
	_add_button(csv_buttons, "StringsImportCsvButton", "Import...", _on_import_csv)
	_add_button(csv_buttons, "StringsExportCsvButton", "Export...", _on_export_csv)

	refresh()


func refresh() -> void:
	if _doc == null and _ws != null:
		_doc = _ws.get_document()
	_rebuild_section_filter()
	_refresh_validation()
	_refresh_lookup()
	_update_section_buttons()


# --- Section filter ---

func _rebuild_section_filter() -> void:
	if _section_filter == null or _doc == null:
		return
	# The dropdown is keyed by ITEM INDEX, not item id: index 0 = "All sections"
	# (filter -1), index s+1 = section s. OptionButton.add_item treats a negative
	# id as "auto-assign by index", so a -1 sentinel id silently collides with real
	# section 0 — which made "All sections" actually filter to section 0.
	var previous := _ws.get_section_filter() if _ws != null else -1
	var count := _doc.string_table.get_section_count()
	# Guard the programmatic rebuild so item_selected does not re-enter.
	_loading = true
	_section_filter.clear()
	_section_filter.add_item("All sections")
	for s in count:
		_section_filter.add_item("%s (%d)" % [_doc.string_table.get_section_name(s), s])
	# Reselect the previous filter when it still exists, else fall back to All.
	var select_index := previous + 1 if previous >= 0 and previous < count else 0
	_section_filter.select(select_index)
	_loading = false
	# Apply the resolved filter once (the guarded select() emitted no signal).
	if _ws != null:
		_ws.set_section_filter(select_index - 1)


func _on_filter_selected(item_index: int) -> void:
	if _loading:
		return
	if _ws != null:
		_ws.set_section_filter(item_index - 1)
	_update_section_buttons()


# Rename/Remove act on the section chosen in the filter; disable them on "All
# sections" so the action is not a silent no-op.
func _update_section_buttons() -> void:
	var has_section := _ws != null and _ws.get_section_filter() >= 0
	if _rename_section_button != null:
		_rename_section_button.disabled = not has_section
		_rename_section_button.tooltip_text = "" if has_section else "Pick a section in the filter to rename it."
	if _remove_section_button != null:
		_remove_section_button.disabled = not has_section
		_remove_section_button.tooltip_text = "" if has_section else "Pick a section in the filter to remove it."


# --- Section management ---

func _on_add_section() -> void:
	if _doc == null:
		return
	var name := _section_name_edit.text.strip_edges()
	if name.is_empty():
		name = "section_%d" % _doc.string_table.get_section_count()
	_doc.add_section(name)
	_section_name_edit.text = ""


func _on_rename_section() -> void:
	var section := _ws.get_section_filter() if _ws != null else -1
	if _doc == null or section < 0:
		return
	var name := _section_name_edit.text.strip_edges()
	if name.is_empty():
		return
	_doc.rename_section(section, name)


func _on_remove_section() -> void:
	var section := _ws.get_section_filter() if _ws != null else -1
	if _doc == null or section < 0:
		return
	_doc.remove_section(section)


# --- Validation ---

func _refresh_validation() -> void:
	if _validation_label == null or _doc == null:
		return
	var report := _doc.validate()
	_validation_label.text = String(report.get("summary", ""))
	_validation_label.add_theme_color_override("font_color", OK_COLOR if report.get("ok", true) else ISSUE_COLOR)
	if _normalize_button != null:
		_normalize_button.visible = not bool(report.get("grouped", true))


func _on_normalize() -> void:
	if _doc != null:
		_doc.normalize_grouping()


# --- Lookup tester ---

## Resolves "section:key" (or a bare key) against the open table with the
## game's semantics — section-scoped first-match lookup, {hot} marker stripped
## for display, and the engine's visible ??section:key?? marker on a scoped
## miss.
func _refresh_lookup() -> void:
	if _lookup_edit == null or _lookup_result == null or _doc == null:
		return
	var query := _lookup_edit.text.strip_edges()
	if query.is_empty():
		_lookup_result.text = "[color=#888888]Type a lookup to preview the in-game result.[/color]"
		return
	var table := _doc.string_table
	var colon := query.find(":")
	if colon >= 0:
		var section := query.substr(0, colon).strip_edges()
		var key := query.substr(colon + 1).strip_edges()
		if table.has_string_in_section(section, key):
			var raw := String(table.get_string_in_section(section, key))
			_lookup_result.text = "In-game: %s" % _escape_bbcode(RtxtStringFile.strip_hotkey(raw))
		else:
			_lookup_result.text = "[color=#ff9966]In-game: ??%s:%s??[/color]" % [_escape_bbcode(section), _escape_bbcode(key)]
	else:
		if table.has_string(query):
			var raw := String(table.get_string(query))
			_lookup_result.text = "In-game: %s" % _escape_bbcode(RtxtStringFile.strip_hotkey(raw))
		else:
			_lookup_result.text = "[color=#ff9966]Not found in any section (the game would show empty text).[/color]"


# --- CSV ---

func _on_import_csv() -> void:
	if _import_dialog == null:
		_import_dialog = _make_csv_dialog(FileDialog.FILE_MODE_OPEN_FILE)
		_import_dialog.file_selected.connect(func(path): if _doc != null: _doc.import_csv(path))
	_import_dialog.popup_centered_ratio(0.6)


func _on_export_csv() -> void:
	if _export_dialog == null:
		_export_dialog = _make_csv_dialog(FileDialog.FILE_MODE_SAVE_FILE)
		_export_dialog.file_selected.connect(func(path): if _doc != null: _doc.export_csv(path))
	_export_dialog.current_file = "strings.csv"
	_export_dialog.popup_centered_ratio(0.6)


func _make_csv_dialog(mode: int) -> FileDialog:
	var dialog := FileDialog.new()
	dialog.file_mode = mode
	dialog.access = FileDialog.ACCESS_FILESYSTEM
	dialog.add_filter("*.csv", "CSV")
	add_child(dialog)
	return dialog


# --- Helpers ---

func _escape_bbcode(text: String) -> String:
	return text.replace("[", "[lb]")


func _add_button(parent: Control, node_name: String, text: String, handler: Callable) -> Button:
	var button := Button.new()
	button.name = node_name
	button.text = text
	button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	button.pressed.connect(handler)
	parent.add_child(button)
	return button
