class_name CtrlRegPicker
extends OptionButton

signal register_selected(reg: int)

var _current := -1
var _syncing := false


func _init() -> void:
	item_selected.connect(_on_item_selected)


func setup(registers: Array, current_reg: int) -> void:
	_syncing = true
	clear()
	add_item("(none)", 0)
	set_item_metadata(0, -1)
	var selected_index := 0
	var seen_current := current_reg < 0
	for reg in registers:
		var reg_index := int(reg.get("index", -1))
		var reg_name := String(reg.get("name", ""))
		if reg_index < 0:
			continue
		var label := "%02d  %s" % [reg_index, reg_name] if not reg_name.is_empty() else "%02d" % reg_index
		add_item(label, reg_index)
		var item_index := get_item_count() - 1
		set_item_metadata(item_index, reg_index)
		if reg_index == current_reg:
			selected_index = item_index
			seen_current = true
	if not seen_current:
		add_item("Custom %d" % current_reg, current_reg)
		selected_index = get_item_count() - 1
		set_item_metadata(selected_index, current_reg)
	_current = current_reg
	select(selected_index)
	_syncing = false


func get_selected_register() -> int:
	if selected < 0 or selected >= get_item_count():
		return -1
	return int(get_item_metadata(selected))


func _on_item_selected(index: int) -> void:
	if _syncing:
		return
	var reg := int(get_item_metadata(index))
	if reg == _current:
		return
	_current = reg
	register_selected.emit(reg)
