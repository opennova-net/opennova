class_name HostSettingsPanel
extends ConfirmationDialog

# The authored MULTI_PLAYER_HOST dialog owns these controls. The supplemental
# editor exposes the remaining file options and restrictions through the shared
# engine schema, without a second set of rules or defaults in GDScript.
const AUTHORED_KEYS := ["game_name", "game_password", "max_players", "time_limit",
	"kill_limit", "max_score", "respawn", "start_delay", "team_ff", "server_message",
	"country", "side_a_password", "side_b_password", "spectator_password",
	"spectator_slots", "replay", "koth_limit", "max_friendly_kills", "capture_duration",
	"capture_speed", "friendly_tags", "ff_warning", "team_choose", "team_trigger_claymore",
	"tracers", "lan_only", "dedicated"]

var _fields: Dictionary = {}
var _classes: Dictionary = {}
var _weapons: Dictionary = {}
var _row_search: Dictionary = {}  # row Control -> its lowercased label, for the find box
var _error: Label


# Whether the panel shows an editor for the schema field `key`.
func has_field(key: String) -> bool:
	return _fields.has(key)


# How many schema fields the panel shows (the classes and weapons apart).
func field_count() -> int:
	return _fields.size()


func _ready() -> void:
	title = "All server options"
	size = Vector2i(860, 650).min(get_tree().root.size - Vector2i(40, 40))
	min_size = Vector2i(540, 380)
	dialog_hide_on_ok = false
	get_ok_button().text = "Apply"
	var content := VBoxContainer.new()
	add_child(content)
	var search := LineEdit.new()
	search.placeholder_text = "Find a setting"
	content.add_child(search)
	var scroll := ScrollContainer.new()
	scroll.custom_minimum_size = Vector2(500, mini(480, size.y - 150))
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	content.add_child(scroll)
	var fields := VBoxContainer.new()
	fields.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(fields)
	var document: Dictionary = JSON.parse_string(GameCfgStore.block().server_configuration_json())
	var values: Dictionary = document.values
	for spec: Dictionary in document.schema:
		var key := String(spec.key)
		if key in AUTHORED_KEYS:
			continue
		var row := _row(fields, "%s · %s" % [spec.group, spec.label])
		var value: Variant = values.get(key, spec.default)
		var widget: Control
		match String(spec.kind):
			"boolean":
				var check := CheckBox.new()
				check.button_pressed = bool(value)
				widget = check
			"integer", "number":
				var number := SpinBox.new()
				number.min_value = float(spec.min)
				number.max_value = float(spec.max)
				number.step = 1.0 if spec.kind == "integer" else 0.001
				number.value = float(value)
				widget = number
			_:
				var text := LineEdit.new()
				text.text = String(value)
				text.secret = spec.kind == "password"
				text.max_length = int(spec.max)
				widget = text
		widget.custom_minimum_size.x = 220
		widget.tooltip_text = String(spec.note)
		row.add_child(widget)
		_fields[key] = widget
	for key: String in values.classes:
		var row := _row(fields, "Classes · " + key.capitalize())
		var check := CheckBox.new()
		check.button_pressed = bool(values.classes[key])
		row.add_child(check)
		_classes[key] = check
	for name: String in document.weapons:
		var row := _row(fields, "Weapons · " + name)
		var choice := OptionButton.new()
		choice.add_item("Always available")
		choice.add_item("Unavailable")
		choice.add_item("Armory only")
		choice.select(["always", "never", "armory"].find(values.weapons.get(name, "always")))
		row.add_child(choice)
		_weapons[name] = choice
	_error = Label.new()
	_error.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	content.add_child(_error)
	search.text_changed.connect(func(text: String) -> void:
		for row: Control in _row_search:
			row.visible = text.is_empty() or String(_row_search[row]).contains(text.to_lower()))
	confirmed.connect(_apply)
	canceled.connect(queue_free)
	close_requested.connect(queue_free)


func _row(parent: Control, text: String) -> HBoxContainer:
	var row := HBoxContainer.new()
	var label := Label.new()
	label.text = text
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(label)
	parent.add_child(row)
	_row_search[row] = text.to_lower()
	return row


func _apply() -> void:
	var patch: Dictionary = {}
	for key: String in _fields:
		var widget: Control = _fields[key]
		if widget is CheckBox:
			patch[key] = widget.button_pressed
		elif widget is SpinBox:
			patch[key] = widget.value
		else:
			patch[key] = (widget as LineEdit).text
	var classes: Dictionary = {}
	for key: String in _classes:
		classes[key] = (_classes[key] as CheckBox).button_pressed
	patch.classes = classes
	var weapons: Dictionary = {}
	for name: String in _weapons:
		weapons[name] = ["always", "never", "armory"][(_weapons[name] as OptionButton).selected]
	patch.weapons = weapons
	var problem := GameCfgStore.block().apply_server_configuration_json(JSON.stringify(patch))
	if not problem.is_empty():
		_error.text = problem
		return
	queue_free()
