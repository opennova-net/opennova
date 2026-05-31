class_name MusicSectionNavigator
extends Control

# TOC for Script mode: lists every section the default script defines so
# artists can jump between them without scrolling. The workspace mounts
# this into the inspector host when SCRIPT mode activates; the panel is
# domain-of-script and never claimed by Bank or Live.

signal section_selected(name: StringName)

var _document: RefCounted

@onready var _list: ItemList = %SectionList


func bind_document(document: RefCounted) -> void:
	if _document == document:
		return
	if _document != null and _document.has_signal("changed"):
		if _document.changed.is_connected(_refresh):
			_document.changed.disconnect(_refresh)
	_document = document
	if _document != null and _document.has_signal("changed"):
		_document.changed.connect(_refresh)
	if is_node_ready():
		_refresh()


func _ready() -> void:
	if _list != null:
		_list.item_activated.connect(_on_item_activated)
	_refresh()


func _refresh() -> void:
	if _list == null:
		return
	_list.clear()
	if _document == null or not _document.script_loaded():
		return
	var script_name: String = _document.mus_script.get_default_script_name()
	var sections: PackedStringArray = _document.mus_script.get_section_names(StringName(script_name))
	for s in sections:
		_list.add_item(s)


func _on_item_activated(index: int) -> void:
	if _list == null:
		return
	if index < 0 or index >= _list.item_count:
		return
	section_selected.emit(StringName(_list.get_item_text(index)))
