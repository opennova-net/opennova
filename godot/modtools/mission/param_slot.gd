class_name MissionParamSlot
extends HBoxContainer

## One trigger/action parameter row: a label plus a stack of {SpinBox, OptionButton}, exactly one visible
## at a time, chosen by configure() from a MissionParamSchema slot. Built once and never freed, so a focused
## edit survives an external refresh (mirrors mission_inspector's _sync_spin / _populate_sc_option guards).
## Whatever the kind, the stored/returned value is always a raw int32, so unmapped params round-trip exactly.

signal committed  ## Emitted on a real user edit (spin value_changed or option item_selected).

const SchemaScript = preload("res://modtools/mission/mission_param_schema.gd")

var _label: Label
var _spin: SpinBox
var _option: OptionButton
var _kind: int = SchemaScript.Kind.RAW
var _items: Array = []  ## [{value:int, label:String}] for picker kinds; built by the inspector.
var _default_label: String = "Param"  ## shown for raw/unmapped slots that the schema doesn't name
var _spin_min: float = 0.0  ## raw-int spin bounds captured in setup(); restored for non-FIXED_SECONDS kinds
var _spin_max: float = 0.0


func setup(node_name: String, default_label: String, spin_min: float, spin_max: float) -> void:
	name = node_name
	_default_label = default_label
	size_flags_horizontal = Control.SIZE_EXPAND_FILL

	_label = Label.new()
	_label.clip_text = true
	_label.custom_minimum_size = Vector2(ObjectUiHelpers.LABEL_COL_WIDTH, 0)
	add_child(_label)

	_spin = SpinBox.new()
	_spin.name = "Spin"
	_spin_min = spin_min
	_spin_max = spin_max
	_spin.min_value = spin_min
	_spin.max_value = spin_max
	_spin.step = 1.0
	_spin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	add_child(_spin)

	_option = OptionButton.new()
	_option.name = "Option"
	_option.fit_to_longest_item = false
	_option.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	add_child(_option)

	_spin.value_changed.connect(func(_v): committed.emit())
	_option.item_selected.connect(func(_i): committed.emit())


# Structural reconfigure for the selected type's slot. items: [{value,label}] for picker kinds (ignored for
# RAW). Does not touch values — call set_value() afterwards. Flipping visibility here is safe: a kind change
# only follows a type-dropdown selection, which has already moved focus off the param row.
func configure(slot: Dictionary, items: Array) -> void:
	_kind = int(slot.get("kind", SchemaScript.Kind.RAW))
	_items = items
	var lbl := String(slot.get("label", ""))
	_label.text = lbl if lbl != "" else _default_label
	var tip := String(slot.get("tip", ""))
	_label.tooltip_text = tip
	_spin.tooltip_text = tip
	_option.tooltip_text = tip
	var picker := SchemaScript.is_picker(_kind)
	_spin.visible = not picker
	_option.visible = picker
	# FIXED_SECONDS shows a seconds spin (the model stores raw 16.16 = seconds * 65536); every other
	# kind uses the raw-int bounds captured in setup().
	if _kind == SchemaScript.Kind.FIXED_SECONDS:
		# The model stores raw 16.16 (= seconds * 65536). The original editor (Med_ParamAnimTime
		# @0x449ff0) steps ANIMTIME by 256 raw units, so the seconds step is 256/65536; this keeps
		# every value the original can produce exact through the seconds<->raw conversion (Range
		# snaps the spin value to step, so a coarser/rounder step would not round-trip).
		_spin.min_value = -32768.0
		_spin.max_value = 32767.0
		_spin.step = 256.0 / 65536.0
	else:
		_spin.min_value = _spin_min
		_spin.max_value = _spin_max
		_spin.step = 1.0


# Sync the active control to a raw int. Focus-guarded for the spin so a programmatic refresh never clobbers a
# value the user is mid-typing. For options, rebuilds the item list (same pattern as _populate_sc_option):
# an out-of-range / unknown value is shown as its own "Value N" row instead of snapping to the first entry.
func set_value(raw: int) -> void:
	if SchemaScript.is_picker(_kind):
		_option.clear()
		var sel := -1
		for it in _items:
			var v := int((it as Dictionary).get("value", 0))
			_option.add_item(String((it as Dictionary).get("label", str(v))))
			_option.set_item_id(_option.item_count - 1, v)
			if v == raw:
				sel = _option.item_count - 1
		if sel >= 0:
			_option.select(sel)
		else:
			_option.add_item("Value %d" % raw)
			_option.set_item_id(_option.item_count - 1, raw)
			_option.select(_option.item_count - 1)
	else:
		var shown := (float(raw) / 65536.0) if _kind == SchemaScript.Kind.FIXED_SECONDS else float(raw)
		if not _spin.get_line_edit().has_focus() and _spin.value != shown:
			_spin.value = shown


# Always returns the raw int, whichever control is active.
func read_value() -> int:
	if SchemaScript.is_picker(_kind):
		return _option.get_selected_id()
	if _kind == SchemaScript.Kind.FIXED_SECONDS:
		return int(round(_spin.value * 65536.0))
	return int(_spin.value)


func set_editable(on: bool) -> void:
	_spin.editable = on
	_option.disabled = not on


# True when the active control accepts edits (the inspector disables slots a type doesn't use).
func is_editable() -> bool:
	return _option.disabled == false if SchemaScript.is_picker(_kind) else _spin.editable


# --- Accessors (used by tests and for focus handling) -------------------------
func is_picker() -> bool:
	return SchemaScript.is_picker(_kind)


func get_spin() -> SpinBox:
	return _spin


func get_option() -> OptionButton:
	return _option
