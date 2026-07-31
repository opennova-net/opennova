class_name TerrainEditorAssetDock
extends PanelContainer

const TerrainEditorSlots = preload("res://modtools/terrain/terrain_editor_slots.gd")

@onready var _tabs: TabContainer = %Tabs
@onready var _terrain_properties_tab: Control = %TerrainPropertiesTab
@onready var _assets_box: VBoxContainer = %AssetSectionsBox
@onready var _terrain_name_edit: LineEdit = %TerrainNameEdit
@onready var _detail_density_spin: SpinBox = %DetailDensitySpin
@onready var _detail_density2_spin: SpinBox = %DetailDensity2Spin

var editor: TerrainEditor
var _syncing: bool = false
var _slot_previews: Dictionary = {}
var _slot_filename_labels: Dictionary = {}
var _slot_state_labels: Dictionary = {}
var _file_dialog: FileDialogHelper
# "Used by" (which missions sit on this terrain), mounted under the name row
# once the workspace injects the shell's reference services. Null on headless
# owners (no shell, no strip).
var _used_by_strip: ReferenceStrip


func _ready() -> void:
	_configure_tabs()
	_build_slot_sections()
	# TerrainNameEdit is read-only — renaming happens via File → Save As,
	# which derives the new terrain name from the destination directory.
	_detail_density_spin.value_changed.connect(_on_detail_density_changed)
	_detail_density2_spin.value_changed.connect(_on_detail_density2_changed)


func _configure_tabs() -> void:
	_tabs.set_tab_title(_tabs.get_tab_idx_from_control(_terrain_properties_tab), "Properties")


func set_editor(value: TerrainEditor) -> void:
	SignalRebind.rebind(editor, value, &"ui_state_changed", Callable(self, "_on_editor_ui_state_changed"))
	editor = value
	_sync_from_editor()


func sync_from_editor_state() -> void:
	_sync_from_editor()


func _on_editor_ui_state_changed(_version: int) -> void:
	_sync_from_editor()


func _sync_from_editor() -> void:
	if editor == null:
		return
	_syncing = true
	if _terrain_name_edit.text != editor.get_terrain_name_value():
		_terrain_name_edit.text = editor.get_terrain_name_value()
	_detail_density_spin.set_value_no_signal(editor.get_detail_density())
	_detail_density2_spin.set_value_no_signal(editor.get_detail_density2())
	_refresh_slot_cards()
	_refresh_used_by()
	_syncing = false


## "Used by" services from the workspace (the strip rides the shell's reference
## index, so only a shell-mounted dock ever receives this). Builds the strip once
## into the Properties tab under the terrain-name row, then retargets it from
## the editor state on every sync (open / new / save-as move the identity).
func set_reference_services(services: ReferenceServices) -> void:
	if _used_by_strip == null or not is_instance_valid(_used_by_strip):
		_used_by_strip = ReferenceStrip.new()
		_used_by_strip.name = "TerrainUsedByStrip"
		var name_row := _terrain_name_edit.get_parent()
		var box := name_row.get_parent()
		box.add_child(_used_by_strip)
		box.move_child(_used_by_strip, name_row.get_index() + 1)
	_used_by_strip.configure("terrain", services, PackedStringArray(["terrain"]))
	_refresh_used_by()


func _refresh_used_by() -> void:
	if _used_by_strip == null or editor == null:
		return
	var keys := PackedStringArray()
	# Only a saved/opened terrain has an identity missions can reference; the
	# document's name fallback ("untitled") must never be queried — referrer
	# buckets are name-keyed shared namespaces, so a collision would render
	# wrong rows. Missions reference the header name bare; the .trn spelling is
	# future-proofing the strip dedupes (the fonts both-spellings pattern).
	if not String(editor.get_current_trn_path()).is_empty():
		var terrain_name: String = editor.get_terrain_name_value()
		if not terrain_name.is_empty():
			keys.append(terrain_name)
			keys.append(terrain_name + ".trn")
	_used_by_strip.set_target(keys)


func _build_slot_sections() -> void:
	for child in _assets_box.get_children():
		child.queue_free()
	_slot_previews.clear()
	_slot_filename_labels.clear()
	_slot_state_labels.clear()

	var sections := [
		{
			"title": "Detail Layers",
			"hint": "Base terrain materials and the supporting near/far shading maps.",
			"slots": TerrainEditorSlots.get_detail_slot_ids() + TerrainEditorSlots.get_aux_slot_ids(),
		},
		{
			"title": "Map Data",
			"hint": "Shared maps that drive surface paint, foliage placement, and tile placement.",
			"slots": TerrainEditorSlots.get_map_data_slot_ids(),
		},
	]

	for section_index in sections.size():
		if section_index > 0:
			_assets_box.add_child(HSeparator.new())
		var section: Dictionary = sections[section_index]
		_assets_box.add_child(_build_section_header(String(section.get("title", "")), String(section.get("hint", ""))))
		for slot_id in section.get("slots", []):
			var slot_key := String(slot_id)
			var slot_meta: Dictionary = TerrainEditorSlots.get_slot(slot_key)
			_assets_box.add_child(_build_slot_card(slot_key, slot_meta))


func _build_section_header(title: String, hint: String) -> Control:
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 3)

	var title_label := Label.new()
	title_label.theme_type_variation = &"Heading"
	title_label.text = title
	box.add_child(title_label)

	var hint_label := Label.new()
	hint_label.theme_type_variation = &"Muted"
	hint_label.text = hint
	hint_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	box.add_child(hint_label)

	return box


func _build_slot_card(slot_id: String, meta: Dictionary) -> Control:
	var card := PanelContainer.new()
	card.size_flags_horizontal = Control.SIZE_EXPAND_FILL

	var margin := MarginContainer.new()
	margin.add_theme_constant_override("margin_left", 8)
	margin.add_theme_constant_override("margin_top", 8)
	margin.add_theme_constant_override("margin_right", 8)
	margin.add_theme_constant_override("margin_bottom", 8)
	card.add_child(margin)

	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 10)
	margin.add_child(row)

	if TerrainEditorSlots.is_previewable(slot_id):
		var preview := TextureRect.new()
		preview.custom_minimum_size = Vector2(56, 56)
		preview.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_COVERED
		preview.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
		preview.size_flags_vertical = Control.SIZE_SHRINK_BEGIN
		preview.mouse_filter = Control.MOUSE_FILTER_IGNORE
		row.add_child(preview)
		_slot_previews[slot_id] = preview

	var content := VBoxContainer.new()
	content.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	content.add_theme_constant_override("separation", 4)
	row.add_child(content)

	# The title owns its own full-width row; the actions sit on a separate row
	# below. At the ~360px dock width sharing one row squeezed the title into
	# one-token-per-line wrapping and cramped the buttons.
	var title_label := Label.new()
	title_label.text = TerrainEditorSlots.get_slot_label(slot_id)
	title_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	title_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	if meta.has("tooltip"):
		title_label.tooltip_text = String(meta["tooltip"])
	content.add_child(title_label)

	var actions := HBoxContainer.new()
	actions.add_theme_constant_override("separation", 6)
	content.add_child(actions)

	var actions_spacer := Control.new()
	actions_spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	actions.add_child(actions_spacer)

	var load_btn := Button.new()
	load_btn.text = "Load"
	load_btn.pressed.connect(_on_slot_load_pressed.bind(slot_id, TerrainEditorSlots.get_slot_dialog_title(slot_id)))
	actions.add_child(load_btn)

	var reset_btn := Button.new()
	reset_btn.text = "Reset"
	reset_btn.pressed.connect(_on_slot_reset_pressed.bind(slot_id))
	actions.add_child(reset_btn)

	var filename_label := Label.new()
	filename_label.text = "(none)"
	filename_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	filename_label.clip_text = true
	filename_label.theme_type_variation = &"Muted"
	content.add_child(filename_label)
	_slot_filename_labels[slot_id] = filename_label

	var state_label := Label.new()
	state_label.theme_type_variation = &"Muted"
	state_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	content.add_child(state_label)
	_slot_state_labels[slot_id] = state_label

	return card


func _refresh_slot_cards() -> void:
	if editor == null:
		return
	for slot_id in _slot_filename_labels:
		var texture: Texture2D = editor.get_slot_texture(slot_id)
		var filename := editor.get_slot_filename(slot_id)
		var preview: TextureRect = _slot_previews.get(slot_id)
		if preview:
			preview.texture = texture

		var filename_label: Label = _slot_filename_labels[slot_id]
		if filename.is_empty():
			filename_label.text = "(none)"
			filename_label.theme_type_variation = &"Muted"
		else:
			filename_label.text = filename
			filename_label.theme_type_variation = &""

		var state_label: Label = _slot_state_labels.get(slot_id)
		if state_label:
			state_label.text = _describe_slot_state(slot_id, filename, texture)


func _describe_slot_state(slot_id: String, filename: String, texture: Texture2D) -> String:
	if not filename.is_empty():
		return "Loaded from project assets."
	if texture == null:
		return "No texture source loaded."
	match slot_id:
		"charmap":
			return "Previewing the current surface-type map."
		"foliagemap":
			return "Previewing the current foliage placement map."
		"tilestrip":
			return "Atlas is available for tile browsing and placement."
		_:
			if TerrainEditorSlots.uses_placeholder_default(slot_id):
				return "Using the default placeholder texture."
			return "Using the current in-memory texture."


func _on_slot_load_pressed(slot_id: String, dialog_title: String) -> void:
	if editor == null:
		return
	_open_file_dialog(
		dialog_title,
		TerrainEditorSlots.get_texture_file_filters(),
		func(path: String) -> void:
			editor.load_texture_slot(slot_id, path)
	)


func _on_slot_reset_pressed(slot_id: String) -> void:
	if editor:
		editor.reset_texture_slot(slot_id)


func _open_file_dialog(title: String, filters: PackedStringArray, on_pick: Callable) -> void:
	if _file_dialog == null:
		_file_dialog = FileDialogHelper.new(self)
	var dir := editor.get_last_open_dir() if editor != null else ""
	_file_dialog.open(title, filters, on_pick, dir)


func _on_detail_density_changed(value: float) -> void:
	if _syncing or editor == null:
		return
	editor.set_detail_density_value(int(value))


func _on_detail_density2_changed(value: float) -> void:
	if _syncing or editor == null:
		return
	editor.set_detail_density2_value(int(value))
