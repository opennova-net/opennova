class_name ShaderTagPicker
extends OptionButton

signal value_changed(tag: String)

var _current := ""
var _syncing := false


func _init() -> void:
	item_selected.connect(_on_item_selected)


func setup(shader_catalog: Array, current: String) -> void:
	_syncing = true
	clear()
	var seen := {}
	if not current.is_empty():
		seen[current] = true
		add_item(current)
	for shader in shader_catalog:
		var tag := String(shader.get("name", ""))
		if tag.is_empty() or seen.has(tag):
			continue
		seen[tag] = true
		add_item(tag)
	if get_item_count() == 0:
		add_item("")
	_current = current
	var match_index := _index_for_tag(current)
	select(match_index if match_index >= 0 else 0)
	_syncing = false


func _index_for_tag(tag: String) -> int:
	for i in range(get_item_count()):
		if get_item_text(i) == tag:
			return i
	return -1


func _on_item_selected(index: int) -> void:
	if _syncing:
		return
	var tag := get_item_text(index)
	if tag == _current:
		return
	_current = tag
	value_changed.emit(tag)
