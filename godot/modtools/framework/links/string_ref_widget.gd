class_name StringRefWidget
extends VBoxContainer
## A string-table key reference: a ResourceRefWidget row (key field, badge,
## pick, jump, clear) above an inline resolved-text preview. Satisfies the
## FieldBinder.bind_link contract (set_value/get_value/value_changed; set_value
## never emits).
##
## Resolution is context-local: unlike file references, a key resolves against
## a specific string TABLE, so services arrive from the adopter that holds it
## (never from the shell). With no services the widget degrades to a plain key
## field - badge, pick, jump, and preview all hide.

signal value_changed(value: String)

var ref_row: ResourceRefWidget
var preview: Label

var _services: Dictionary = {}
# The resolved table path, captured from the last LIVE resolve - the jump
# service receives it so the Strings workspace opens the right table. Reuse
# hazard for future adopters: the inner row memoizes resolves on (value,
# epoch), so a widget reused across a TABLE change with the same key would
# keep a stale path - reconfigure (or rebuild) the widget when the backing
# table changes, as the MNU inspector's per-rebuild construction does.
var _table_path := ""


func _init() -> void:
	add_theme_constant_override("separation", 2)
	ref_row = ResourceRefWidget.new()
	ref_row.name = "StringRefRow"
	# Keys are identity values, never paths: a picked key like "BTN.OK" must
	# not be basename-stripped.
	ref_row.set_value_from_path(func(key: String) -> String: return key)
	ref_row.set_status_copy("Choose a string from the menu's text table.",
		"Not in the string table. The game will show ??key??.")
	ref_row.value_changed.connect(func(value: String) -> void:
		_refresh_preview()
		value_changed.emit(value))
	add_child(ref_row)

	preview = Label.new()
	preview.name = "StringRefPreview"
	preview.visible = false
	preview.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	preview.add_theme_color_override("font_color", Color(0.62, 0.62, 0.62))
	add_child(preview)


## services (string-key terms, all optional):
##   "resolve": Callable(key) -> { "status": "found"|"missing"|"",
##              "path": <table path>, "text": <display text, hotkey-stripped> }
##   "pick":    Callable(current_key, on_pick: Callable(key))
##   "jump":    Callable(key, table_path)
func configure(display_label: String, services: Dictionary = {}) -> void:
	_services = services
	var inner := {}
	var resolve := _service("resolve")
	if resolve.is_valid():
		inner["resolve"] = func(_kind: String, key: String) -> Dictionary:
			var result: Dictionary = resolve.call(key)
			_table_path = String(result.get("path", ""))
			# The inner row gates its jump on a non-empty resolved path; for
			# keys the path is auxiliary (the jump service carries the key and
			# the workspace re-resolves the table), so backfill it with the key
			# when the provider does not know the table's path. Consequence: a
			# string_id LinkPayload's path field may be the KEY, not a file
			# path - drop targets must not trust it as a location.
			if not result.is_empty() and _table_path.is_empty():
				var patched := result.duplicate()
				patched["path"] = key
				return patched
			return result
	var pick := _service("pick")
	if pick.is_valid():
		inner["pick"] = func(_kind: String, _title: String, on_pick: Callable) -> void:
			pick.call(ref_row.get_value(), on_pick)
	var jump := _service("jump")
	if jump.is_valid():
		inner["jump"] = func(_kind: String, _path: String) -> void:
			jump.call(ref_row.get_value(), _table_path)
	ref_row.configure("string_id", display_label, inner)
	_refresh_preview()


func set_value(text: String) -> void:
	ref_row.set_value(text)
	_refresh_preview()


func get_value() -> String:
	return ref_row.get_value()


func _service(service_name: String) -> Callable:
	var cb: Variant = _services.get(service_name)
	return cb if cb is Callable else Callable()


func _refresh_preview() -> void:
	var resolve := _service("resolve")
	var key := ref_row.get_value()
	if not resolve.is_valid() or key.is_empty():
		preview.visible = false
		return
	var result: Dictionary = resolve.call(key)
	match String(result.get("status", "")):
		"found":
			preview.text = "= \"%s\"" % String(result.get("text", ""))
			preview.visible = true
		"missing":
			preview.text = "Not in string table"
			preview.visible = true
		_:
			preview.visible = false
