extends "res://modtools/mission/inspectors/inspector_section.gd"


# --- Mission properties (header) editable form --------------------------------
# A collapsible form for the mission-level header fields. Built ONCE and synced in
# place via _props_binder (FieldBinder), since LineEdits would lose their caret if torn
# down on every `changed`. Setters route through the controller's set_header_* (one undo
# step each). Mission-global, so it shows in both Objects and Waypoints mode.
var _props_toggle: CheckButton
var _props_box: VBoxContainer
var _props_binder: FieldBinder
# Link-widget services (resolve/pick/jump Callables from the shell). They arrive
# AFTER setup() builds the forms (the workspace injects them post-build), so the
# setter re-configures the already-built widgets.
var _ref_services: Dictionary = {}
var _terrain_ref_widget: ResourceRefWidget
var _env_ref_widget: ResourceRefWidget
# Mission-tab bulk re-ground (B8): the manual twin of the workspace's activate-time
# "terrain changed under N objects" prompt.
var _reground_button: Button


# --- Mission properties (header) editable form --------------------------------

func _build_props_panel() -> void:
	_props_binder = FieldBinder.new()
	_props_toggle = CheckButton.new()
	_props_toggle.name = "MissionPropsToggle"
	_props_toggle.text = "Mission properties"
	_props_toggle.tooltip_text = "Mission-level header: title, world, gameplay, game modes."
	_props_toggle.button_pressed = false
	_props_toggle.visible = false
	_inspector._mission_content.add_child(_props_toggle)

	_props_box = VBoxContainer.new()
	_props_box.name = "MissionPropsBox"
	_props_box.add_theme_constant_override("separation", 4)
	_props_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_props_box.visible = false
	_inspector._mission_content.add_child(_props_box)
	_props_toggle.toggled.connect(func(on: bool) -> void: _props_box.visible = on)

	_add_props_line("mission_name", "Name", "Mission title.")
	_add_props_line("designer", "Designer", "Mission author.")
	_add_props_line("briefing", "Briefing", "Mission briefing text.")
	InspectorForms.add_section_heading(_props_box, "World")
	_terrain_ref_widget = _add_props_ref("terrain", "terrain", "Terrain",
		"The ground this mission is built on. The world reloads onto the new terrain the next time the mission is opened.")
	_env_ref_widget = _add_props_ref("environment", "environment", "Environment",
		"Sky, light, and weather for this mission. Takes effect the next time the mission is opened.")
	_add_props_option("climate", "Climate", [[0, "Desert"], [1, "Jungle"], [2, "Snow"]])
	_add_props_option("weather", "Weather", [[0, "Nice day"], [1, "Rainy"], [2, "Snow"]])
	_add_props_option("mission_type", "Type", [[1, "Normal"], [2, "Combat vehicle"], [3, "Tenth Mountain"]])
	InspectorForms.add_section_heading(_props_box, "Gameplay")
	_add_props_spin("player_health", "Player health", 0.0, 1000000.0)
	_add_props_spin("minutes_per_day", "Minutes / day", 0.0, 65535.0)
	_add_props_spin("max_saves", "Max saves", 0.0, 255.0)
	_add_props_spin("start_time", "Start time", 0.0, 65535.0)
	InspectorForms.add_section_heading(_props_box, "Game mode")
	_add_props_game_mode()
	InspectorForms.add_section_heading(_props_box, "Options")
	_add_props_flag(NovaMissionData.ATTRIB_ENABLE_NVG, "Night vision")
	_add_props_flag(NovaMissionData.ATTRIB_START_WITH_NVG_ON, "Start with night vision on")
	_add_props_flag(NovaMissionData.ATTRIB_ROTATE_MAP_180, "Rotate map 180")
	InspectorForms.add_section_heading(_props_box, "Audio")
	_add_props_spin("music", "Music track", 0.0, 1000000.0)
	_add_props_spin("reverb", "Reverb", 0.0, 1000000.0)


# DOCK: Mission — the manual twin of the workspace's activate-time re-ground prompt
# (terrain heights edited under the mission, undo of an applied re-ground, declined
# prompt: this button reaches the same one-undo-step bulk re-ground any time).
func _build_reground_button() -> void:
	_reground_button = Button.new()
	_reground_button.name = "MissionRegroundAll"
	_reground_button.text = "Re-ground objects"
	_reground_button.tooltip_text = "Snap objects the terrain moved out from under back onto the surface (one undo step). Objects placed above the ground on purpose are left alone."
	_reground_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_reground_button.visible = false
	_inspector._mission_content.add_child(_reground_button)
	_reground_button.pressed.connect(func() -> void:
		if _inspector._controller != null and _inspector._controller.has_method("reground_drifted"):
			_inspector._controller.reground_drifted())


func _refresh_reground_button() -> void:
	if _reground_button == null:
		return
	var mission: NovaMissionData = _inspector._controller.get_mission() if _inspector._controller != null else null
	_reground_button.visible = mission != null and _inspector._controller.has_method("reground_drifted")


func _add_props_line(field: String, label: String, tooltip: String = "") -> LineEdit:
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_props_box.add_child(row)
	var lbl := Label.new()
	lbl.text = label
	lbl.tooltip_text = tooltip if not tooltip.is_empty() else label
	lbl.clip_text = true
	lbl.custom_minimum_size = Vector2(96, 0)
	row.add_child(lbl)
	var line := LineEdit.new()
	line.name = "MissionProp_" + field
	line.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(line)
	_props_binder.bind_line(line,
		func(info) -> String: return String(info.get(field, "")),
		func(text: String) -> void: _set_header_string(field, text))
	return line


func _add_props_ref(field: String, kind: String, label: String, tooltip: String = "") -> ResourceRefWidget:
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_props_box.add_child(row)
	var lbl := Label.new()
	lbl.text = label
	lbl.tooltip_text = tooltip if not tooltip.is_empty() else label
	lbl.clip_text = true
	lbl.custom_minimum_size = Vector2(96, 0)
	row.add_child(lbl)
	var widget := ResourceRefWidget.new()
	widget.name = "MissionProp_" + field
	widget.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	widget.configure(kind, label, _ref_services)
	row.add_child(widget)
	_props_binder.bind_link(widget,
		func(info) -> String: return String(info.get(field, "")),
		func(text: String) -> void: _set_header_string(field, text))
	return widget


## Wires the link widgets' resolve/pick/jump Callables (see
## ResourceRefWidget.services_from_shell). Idempotent; safe before or after
## the form is built.
func set_reference_services(services: Dictionary) -> void:
	_ref_services = services
	_inspector._configure_selected_graphic_ref()
	if _terrain_ref_widget != null and is_instance_valid(_terrain_ref_widget):
		_terrain_ref_widget.configure("terrain", "Terrain", services)
	if _env_ref_widget != null and is_instance_valid(_env_ref_widget):
		_env_ref_widget.configure("environment", "Environment", services)


func _add_props_option(field: String, label: String, choices: Array) -> OptionButton:
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_props_box.add_child(row)
	var lbl := Label.new()
	lbl.text = label
	lbl.custom_minimum_size = Vector2(96, 0)
	row.add_child(lbl)
	var option := OptionButton.new()
	option.name = "MissionProp_" + field
	option.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	for choice in choices:
		var idx := option.item_count
		option.add_item(String(choice[1]))
		option.set_item_id(idx, int(choice[0]))
	row.add_child(option)
	_props_binder.bind_option(option,
		func(info) -> int: return int(info.get(field, 0)),
		func(value: int) -> void: _set_header_int(field, value))
	return option


func _add_props_spin(field: String, label: String, min_value: float, max_value: float) -> SpinBox:
	var spin := InspectorForms.add_spin_row(_props_box, "MissionProp_" + field, label, min_value, max_value, 1.0)
	_props_binder.bind_spin(spin,
		func(info) -> float: return float(int(info.get(field, 0))),
		func(value: float) -> void: _set_header_int(field, int(value)))
	return spin


func _add_props_flag(bit: int, label: String) -> CheckBox:
	var check := CheckBox.new()
	check.name = "MissionFlag_%d" % bit
	check.text = label
	_props_box.add_child(check)
	_props_binder.bind_checkbox(check,
		func(info) -> bool: return (int(info.get("attrib_flags", 0)) & bit) != 0,
		func(on: bool) -> void: _set_header_flag(bit, on))
	return check


# Engine combobox order [orig: sub_402770 @0x404eff dfx2med.exe]. Index 0 = no mode bits (Single
# Player). The dropdown uses the list INDEX as the item id (Godot ids are 32-bit, but the high modes
# like Search & Destroy = 0x80000000 are not), mapping index <-> attrib_flags bit through this table.
const _GAME_MODE_BITS := [
	0,          # Single player (no mode bits)
	0x1000000,  # Co-op
	0x2000000,  # Deathmatch
	0x20000000, # Team deathmatch
	0x4000000,  # King of the hill
	0x40000000, # Team king of the hill
	0x10000000, # Capture the flag
	0x800000,   # Attack & defend
	0x80000000, # Search & destroy
	0x8000000,  # Flagball
	0x10000,    # Advance & secure
	0x20000,    # Conquer & control
]
const _GAME_MODE_LABELS := [
	"Single player", "Co-op", "Deathmatch", "Team deathmatch", "King of the hill",
	"Team king of the hill", "Capture the flag", "Attack & defend", "Search & destroy",
	"Flagball", "Advance & secure", "Conquer & control",
]


func _add_props_game_mode() -> OptionButton:
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_props_box.add_child(row)
	var lbl := Label.new()
	lbl.text = "Game mode"
	lbl.custom_minimum_size = Vector2(96, 0)
	row.add_child(lbl)
	var option := OptionButton.new()
	option.name = "MissionProp_game_mode"
	option.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	for i in _GAME_MODE_LABELS.size():
		option.add_item(String(_GAME_MODE_LABELS[i]))
		option.set_item_id(i, i)
	row.add_child(option)
	# The mission stores the active mode as one attrib_flags bit; get_game_mode() returns it (0 = SP).
	# Map bit -> index for selection, index -> bit on edit.
	_props_binder.bind_option(option,
		func(info) -> int: return maxi(0, _GAME_MODE_BITS.find(int(info.get("game_mode", 0)))),
		func(id: int) -> void:
			# Bounds-check: the item id equals the list index today (no fallback row), but guard so a
			# future out-of-range fallback id cannot index _GAME_MODE_BITS out of bounds.
			if id >= 0 and id < _GAME_MODE_BITS.size():
				_set_game_mode(int(_GAME_MODE_BITS[id])))
	return option


func _set_game_mode(bit: int) -> void:
	if _inspector._controller != null:
		_inspector._controller.set_game_mode(bit)


func _set_header_string(field: String, value: String) -> void:
	if _inspector._controller != null:
		_inspector._controller.set_header_string(field, value)


func _set_header_int(field: String, value: int) -> void:
	if _inspector._controller != null:
		_inspector._controller.set_header_int(field, value)


func _set_header_flag(bit: int, on: bool) -> void:
	if _inspector._controller != null:
		_inspector._controller.set_header_flag(bit, on)


func _refresh_props_panel() -> void:
	if _props_toggle == null:
		return
	var mission: NovaMissionData = _inspector._controller.get_mission() if _inspector._controller != null else null
	if mission == null:
		_props_toggle.visible = false
		_props_box.visible = false
		return
	_props_toggle.visible = true
	_props_binder.sync_from(mission.get_info())
