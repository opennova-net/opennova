class_name ResourceRefWidget
extends HBoxContainer
## A reference to another resource, as one editable row: name field, a badge
## showing whether the name resolves in the resource folder, a browse picker,
## and a jump button that opens the target in its own workspace. Satisfies
## FieldBinder.bind_link's widget contract (set_value/get_value/value_changed;
## set_value never emits).
##
## Services arrive as Callables so the widget works anywhere: with no services
## (headless tests, runtime owners) it degrades to a plain name field — badge,
## browse, and jump simply hide. services_from_shell builds the editor trio.

signal value_changed(value: String)

## Drop-accept equivalences beyond an exact kind match: the extractors emit
## both spellings for picture files, so a row configured either way accepts
## either payload.
const _KIND_EQUIVALENTS := {"texture": ["image"], "image": ["texture"]}

var name_edit: LineEdit
var badge: Label
var browse_button: Button
var jump_button: Button
var clear_button: Button

var _kind := ""
var _label := ""
var _services: Dictionary = {}
var _current := ""
var _shown := ""
var _resolved_path := ""
# Resolution memo: (value, cache_epoch) -> resolve result, so per-sync refreshes
# stay free until the name or the resource root actually changes.
var _memo_value := ""
var _memo_epoch := -1
var _memo_result: Dictionary = {}
# Turns a picked browser path into the stored value. Header refs store bare
# names; texture-flavored adopters override to keep the extension.
var _value_from_path := func(path: String) -> String: return path.get_file().get_basename()
# Status copy overrides for adopters whose targets are not resource-folder
# files (string keys); empty = the default resource-folder wording.
var _browse_copy := ""
var _missing_copy := ""


func _init() -> void:
	add_theme_constant_override("separation", 4)

	name_edit = LineEdit.new()
	name_edit.name = "RefName"
	name_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	name_edit.text_submitted.connect(func(value: String) -> void: _commit(value))
	name_edit.focus_exited.connect(func() -> void:
		# A focus_exited fired while the row is being torn down (inspector
		# rebuild) must not commit the in-flight text - mirrors the mount
		# inspectors' _wire_text teardown skip.
		if name_edit.is_inside_tree() and name_edit.text != _shown:
			_commit(name_edit.text))
	add_child(name_edit)

	badge = Label.new()
	badge.name = "RefBadge"
	badge.visible = false
	badge.custom_minimum_size = Vector2(18, 0)
	badge.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	badge.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	badge.mouse_filter = Control.MOUSE_FILTER_STOP
	add_child(badge)

	browse_button = Button.new()
	browse_button.name = "RefBrowse"
	browse_button.text = "..."
	browse_button.custom_minimum_size = Vector2(34, 30)
	browse_button.focus_mode = Control.FOCUS_NONE
	browse_button.visible = false
	browse_button.pressed.connect(_on_browse_pressed)
	add_child(browse_button)

	jump_button = Button.new()
	jump_button.name = "RefJump"
	jump_button.text = "→"
	jump_button.custom_minimum_size = Vector2(30, 30)
	jump_button.focus_mode = Control.FOCUS_NONE
	jump_button.visible = false
	jump_button.pressed.connect(_on_jump_pressed)
	add_child(jump_button)

	clear_button = Button.new()
	clear_button.name = "RefClear"
	clear_button.text = "X"
	clear_button.tooltip_text = "Clear"
	clear_button.custom_minimum_size = Vector2(30, 30)
	clear_button.focus_mode = Control.FOCUS_NONE
	clear_button.pressed.connect(func() -> void: _commit(""))
	add_child(clear_button)

	# The GUI drop walk stops at MOUSE_FILTER_STOP children, so the row's own
	# drop virtuals only cover the gaps between them: every child forwards to
	# the same payload handlers. The field gets a dedicated pair that DEFERS
	# plain-String drops to LineEdit's native caret insert - LineEdit::drop_data
	# always runs the forwarded drop AND THEN the native insert for String data,
	# so handling Strings here would double-apply (commit + mangled re-insert,
	# then a second corrupt commit on focus-out). The field's drag callable
	# stays empty so native selected-text drags keep working; the badge's
	# reuses the drag source it already is.
	name_edit.set_drag_forwarding(Callable(), _can_drop_data_on_field, _drop_data_on_field)
	badge.set_drag_forwarding(_get_drag_data, _can_drop_data, _drop_data)
	browse_button.set_drag_forwarding(Callable(), _can_drop_data, _drop_data)
	jump_button.set_drag_forwarding(Callable(), _can_drop_data, _drop_data)
	clear_button.set_drag_forwarding(Callable(), _can_drop_data, _drop_data)


## services: { "resolve": Callable(kind, name) -> {status, path},
##             "pick":    Callable(kind, title, on_pick: Callable(path)),
##             "jump":    Callable(kind, path) }
## Any subset works; missing entries hide their affordance. Safe to call again
## when services arrive after the form was built.
func configure(kind: String, display_label: String, services: Dictionary = {}) -> void:
	_kind = kind
	_label = display_label
	_services = services
	name_edit.placeholder_text = "(none)"
	browse_button.visible = _service("pick").is_valid()
	browse_button.tooltip_text = _browse_copy if not _browse_copy.is_empty() \
			else "Choose a %s from the resource folder." % _label.to_lower()
	jump_button.tooltip_text = "Open this %s in its editor." % _label.to_lower()
	_memo_value = ""
	_memo_epoch = -1
	_refresh_status_ui()


func _service(service_name: String) -> Callable:
	var cb: Variant = _services.get(service_name)
	return cb if cb is Callable else Callable()


func set_value(text: String) -> void:
	_current = text
	# Mirror bind_line's focused-skip: a model change landing mid-typing must not
	# clobber the in-flight keystrokes; commit-on-Enter/focus-out reconciles.
	if not name_edit.has_focus():
		name_edit.text = text
		_shown = text
	_refresh_status_ui()


func get_value() -> String:
	return _current


func set_value_from_path(cb: Callable) -> void:
	_value_from_path = cb


## Override the resource-folder-flavored tooltips for adopters whose targets
## live elsewhere (string keys in a table). Call before configure().
func set_status_copy(browse_tooltip: String, missing_tooltip: String) -> void:
	_browse_copy = browse_tooltip
	_missing_copy = missing_tooltip


static func services_from_shell(shell: Object) -> Dictionary:
	return {
		"resolve": func(kind: String, name: String) -> Dictionary:
			return shell.get_reference_index().resolve(kind, name),
		"pick": func(kind: String, title: String, on_pick: Callable) -> void:
			shell.open_kind_picker(kind, title, on_pick),
		"jump": func(kind: String, path: String) -> void:
			shell.open_in_workspace(ResourceKinds.jump_kind(kind), path),
	}


func _get_drag_data(_at: Vector2) -> Variant:
	if _current.is_empty():
		return null
	# Previews only attach during a live GUI drag; tests call this directly.
	if get_viewport() != null and get_viewport().gui_is_dragging():
		var preview := Label.new()
		preview.text = _current
		set_drag_preview(preview)
	return LinkPayload.make(_kind, _current, _resolved_path).to_drag_data()


## Payload drops only: plain text belongs to the name field's native caret
## insert (committing on Enter/focus-out like typing), never to these handlers.
func _can_drop_data(_at: Vector2, data: Variant) -> bool:
	if not name_edit.editable:
		return false
	var payload: LinkPayload = LinkPayload.from_drag_data(data)
	return payload != null and _accepts_payload_kind(payload.kind)


func _drop_data(_at: Vector2, data: Variant) -> void:
	if not name_edit.editable:
		return
	var payload: LinkPayload = LinkPayload.from_drag_data(data)
	if payload == null or not _accepts_payload_kind(payload.kind):
		return
	# The payload name is the identity a drop commits (file name for file
	# kinds); path is advisory-only. Routing through _value_from_path makes a
	# drop behave exactly like a browse pick.
	var source := payload.name if not payload.name.is_empty() else payload.path
	if source.is_empty():
		return
	_commit(_value_from_path.call(source))


# The field's forwarded pair: String drops return false so LineEdit's native
# can_drop fallback accepts them, and no-op the drop because LineEdit runs the
# forwarded drop unconditionally before its native insert.
func _can_drop_data_on_field(at: Vector2, data: Variant) -> bool:
	if data is String:
		return false
	return _can_drop_data(at, data)


func _drop_data_on_field(at: Vector2, data: Variant) -> void:
	if data is String:
		return
	_drop_data(at, data)


func _accepts_payload_kind(payload_kind: String) -> bool:
	# An empty kind never matches - not even an unconfigured widget's empty
	# _kind (a kindless payload is malformed, not a wildcard).
	if payload_kind.is_empty():
		return false
	if payload_kind == _kind:
		return true
	var equivalents: Array = _KIND_EQUIVALENTS.get(_kind, [])
	return equivalents.has(payload_kind)


func _commit(text: String) -> void:
	if text == _current:
		# Reconcile the field display even when the value is unchanged.
		name_edit.text = text
		_shown = text
		return
	_current = text
	name_edit.text = text
	_shown = text
	_refresh_status_ui()
	value_changed.emit(text)


func _on_browse_pressed() -> void:
	var pick := _service("pick")
	if not pick.is_valid():
		return
	pick.call(_kind, "Choose a %s" % _label.to_lower(), func(path: String) -> void:
		_commit(_value_from_path.call(path)))


func _on_jump_pressed() -> void:
	var jump := _service("jump")
	if not jump.is_valid() or _resolved_path.is_empty():
		return
	jump.call(_kind, _resolved_path)


func _resolve() -> Dictionary:
	var resolve := _service("resolve")
	if not resolve.is_valid() or _current.is_empty():
		return {}
	var epoch := -1
	if ClassDB.class_exists("NovaResourceRoot"):
		epoch = NovaResourceRoot.cache_epoch()
	if _current == _memo_value and epoch == _memo_epoch:
		return _memo_result
	_memo_value = _current
	_memo_epoch = epoch
	_memo_result = resolve.call(_kind, _current)
	return _memo_result


func _refresh_status_ui() -> void:
	clear_button.disabled = _current.is_empty()
	var result := _resolve()
	_resolved_path = String(result.get("path", ""))
	var has_resolve := not result.is_empty()
	var has_jump := _service("jump").is_valid()
	badge.visible = has_resolve
	jump_button.visible = has_jump and has_resolve
	if not has_resolve:
		return
	var status := String(result.get("status", ""))
	match status:
		"found":
			badge.text = "●"
			badge.tooltip_text = "Found: %s" % _resolved_path
			jump_button.disabled = false
		"missing":
			badge.text = "!"
			badge.tooltip_text = _missing_copy if not _missing_copy.is_empty() \
					else "Not in the resource folder. The game won't find this."
			jump_button.disabled = true
		_:
			badge.text = "?"
			badge.tooltip_text = "Can't be checked from here."
			jump_button.disabled = true
