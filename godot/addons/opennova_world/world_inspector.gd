@tool
extends EditorInspectorPlugin

const Property := preload("res://addons/opennova_world/world_property.gd")
const Field := WorldEditSession.Field
signal pending_changed
var _properties: Array[WeakRef] = []
var _session_for: Callable
var _request_edit: Callable


func setup(session_for: Callable, request_edit: Callable) -> void:
	_session_for = session_for
	_request_edit = request_edit


func get_pending_files() -> PackedStringArray:
	var files := PackedStringArray()
	for property: EditorProperty in _live_properties():
		var filename: String = property.get_pending_file()
		if not filename.is_empty() and not files.has(filename):
			files.append(filename)
	return files


func flush_pending_edits() -> PackedStringArray:
	var errors := PackedStringArray()
	for property: EditorProperty in _live_properties():
		var reason: String = property.flush_pending_edit()
		if not reason.is_empty():
			errors.append(reason)
	return errors


func _live_properties() -> Array[EditorProperty]:
	var live: Array[EditorProperty] = []
	var refs: Array[WeakRef] = []
	for reference in _properties:
		var property := reference.get_ref() as EditorProperty
		if property != null and property.is_inside_tree():
			live.append(property)
			refs.append(reference)
	_properties = refs
	return live


func _can_handle(object: Object) -> bool:
	return object is GameWorld or object is MissionEnvironment or object is FoliageDispatcher


func _parse_begin(object: Object) -> void:
	var session := _session_for.call(object) as WorldEditSession
	if session == null:
		return
	if object is GameWorld:
		_heading("Mission - " + session.get_file_name(Field.START_TIME))
		_field(session, Field.START_TIME, "Start time")
	elif object is MissionEnvironment:
		_heading("Environment - " + session.get_file_name(Field.SKY_HEIGHT))
		_note(session.get_environment_note())
		_field(session, Field.SKY_MAP_1, "Sky map 1")
		_field(session, Field.SKY_MAP_2, "Sky map 2")
		_field(session, Field.SKY_HEIGHT, "Sky height")
	elif object is FoliageDispatcher:
		_heading("Foliage - " + session.get_file_name(Field.FOLIAGE_GRAPHIC))
		for slot in range(session.get_foliage_count()):
			_heading("Slot %d" % (slot + 1))
			_field(session, Field.FOLIAGE_GRAPHIC, "Graphic", slot)
			_field(session, Field.FOLIAGE_MATCH, "Map match", slot)
			_field(session, Field.FOLIAGE_SHADOW, "Cast shadow", slot)
		if session.get_foliage_count() == 0:
			_note("This terrain has no foliage slots.")
	if not session.is_editable():
		_note("Use Create Copy in the world toolbar to edit native values.")


func _field(session: WorldEditSession, field: WorldEditSession.Field, caption: String, slot: int = 0) -> void:
	var editor := Property.new()
	editor.setup(session, field, slot, caption, _request_edit)
	editor.pending_changed.connect(pending_changed.emit)
	_properties.append(weakref(editor))
	add_custom_control(editor)


func _heading(text: String) -> void:
	var heading := Label.new()
	heading.text = text
	heading.add_theme_font_override("font", EditorInterface.get_base_control().get_theme_font("bold", "EditorFonts"))
	add_custom_control(heading)


func _note(text: String) -> void:
	var note := Label.new()
	note.text = text
	note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_custom_control(note)
