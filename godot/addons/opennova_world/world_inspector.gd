@tool
extends EditorInspectorPlugin
## Lays the WorldField table out under the scene node that presents each
## native document. Sections and captions come from the table, in its order.

const Property := preload("res://addons/opennova_world/world_property.gd")
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


## The scene node that presents each native document in the Inspector.
static func _document_for(object: Object) -> int:
	if object is GameWorld:
		return WorldField.Document.MISSION
	if object is MissionEnvironment:
		return WorldField.Document.ENVIRONMENT
	if object is FoliageDispatcher:
		return WorldField.Document.TERRAIN
	return -1


func _parse_begin(object: Object) -> void:
	var session := _session_for.call(object) as WorldEditSession
	var document := _document_for(object)
	if session == null or document < 0:
		return
	var group := ""
	var slotted: Array[WorldField] = []
	for spec in WorldField.for_document(document as WorldField.Document):
		if spec.slotted:
			slotted.append(spec)
			continue
		group = _section(session, spec, group)
		_field(session, spec.id)
	if not slotted.is_empty():
		group = _section(session, slotted[0], group)
		for slot in range(session.get_foliage_count()):
			_heading("Slot %d" % (slot + 1))
			for spec in slotted:
				_field(session, spec.id, slot)
		if session.get_foliage_count() == 0:
			_note("This terrain has no foliage slots.")
	if not session.is_editable():
		_note("Use Create Copy in the world toolbar to edit native values.")


## Opens the spec's section when it differs from the open one; returns the open section.
func _section(session: WorldEditSession, spec: WorldField, open_group: String) -> String:
	if spec.group == open_group:
		return open_group
	_heading("%s - %s" % [spec.group, session.get_file_name(spec.id)])
	if spec.document == WorldField.Document.ENVIRONMENT:
		_note(session.get_environment_note())
	return spec.group


func _field(session: WorldEditSession, field: WorldField.Id, slot: int = 0) -> void:
	var editor := Property.new()
	editor.setup(session, field, slot, _request_edit)
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
