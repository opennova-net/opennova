extends GutTest

# StringRefWidget: the framework link widget for string-table keys. Services
# speak string-key terms (resolve(key) -> {status, path, text}; pick(current,
# on_pick); jump(key, table_path)); without them the widget degrades to a
# plain key field. The inner row's badge/commit semantics are pinned by
# resource_ref_widget_test; these cover the key-flavored composition.

const StringRefWidgetScript = preload("res://modtools/framework/links/string_ref_widget.gd")

var _log: Array = []


func _make_widget(services: Dictionary = {}) -> StringRefWidget:
	var widget: StringRefWidget = StringRefWidgetScript.new()
	add_child_autofree(widget)
	widget.configure("string", services)
	return widget


func _table_services() -> Dictionary:
	# A two-entry fake table: ALPHA -> "Alpha", BRAVO -> "Bravo".
	return {
		"resolve": func(key: String) -> Dictionary:
			_log.append(["resolve", key])
			if key.is_empty():
				return {}
			if key == "ALPHA" or key == "BRAVO":
				return {"status": "found", "path": "/root/menutxt.BIN", "text": key.capitalize()}
			return {"status": "missing", "path": "/root/menutxt.BIN", "text": ""},
		"pick": func(current_key: String, on_pick: Callable) -> void:
			_log.append(["pick", current_key])
			on_pick.call("BRAVO"),
		"jump": func(key: String, table_path: String) -> void:
			_log.append(["jump", key, table_path]),
	}


func before_each() -> void:
	_log = []


func test_degrades_to_plain_key_field_without_services() -> void:
	var widget := _make_widget()
	widget.set_value("ALPHA")
	assert_false(widget.ref_row.badge.visible, "no resolve service - no badge")
	assert_false(widget.ref_row.browse_button.visible, "no pick service - no browse")
	assert_false(widget.ref_row.jump_button.visible, "no jump service - no jump")
	assert_false(widget.preview.visible, "no resolve service - no preview")
	assert_eq(widget.get_value(), "ALPHA", "the key field still works")


func test_found_key_shows_badge_and_resolved_text() -> void:
	var widget := _make_widget(_table_services())
	widget.set_value("ALPHA")
	assert_eq(widget.ref_row.badge.text, "●", "a known key reads found")
	assert_true(widget.preview.visible)
	assert_eq(widget.preview.text, "= \"Alpha\"", "the preview shows the resolved display text")


func test_missing_key_shows_cue() -> void:
	var widget := _make_widget(_table_services())
	widget.set_value("NO_SUCH_KEY")
	assert_eq(widget.ref_row.badge.text, "!", "an unknown key reads missing")
	assert_true(widget.preview.visible)
	assert_eq(widget.preview.text, "Not in string table")
	assert_string_contains(widget.ref_row.badge.tooltip_text, "string table",
		"the missing copy speaks string-table terms, not resource-folder terms")


func test_empty_value_hides_preview() -> void:
	var widget := _make_widget(_table_services())
	widget.set_value("ALPHA")
	widget.set_value("")
	assert_false(widget.preview.visible, "an empty key shows no preview")


func test_set_value_never_emits() -> void:
	var widget := _make_widget(_table_services())
	watch_signals(widget)
	widget.set_value("ALPHA")
	assert_signal_emit_count(widget, "value_changed", 0, "bind_link contract: set_value is silent")


func test_pick_passes_current_key_and_commits_identity() -> void:
	var widget := _make_widget(_table_services())
	widget.set_value("BTN.OK")  # a dotted key must not be basename-stripped
	var emitted: Array = []
	widget.value_changed.connect(func(v: String) -> void: emitted.append(v))
	widget.ref_row.browse_button.pressed.emit()
	assert_eq(_log.filter(func(e): return e[0] == "pick").size(), 1, "one pick call")
	assert_eq(_log.filter(func(e): return e[0] == "pick")[0][1], "BTN.OK",
		"the pick service receives the CURRENT key")
	assert_eq(emitted, ["BRAVO"], "the picked key commits exactly once, identity-valued")
	assert_eq(widget.preview.text, "= \"Bravo\"", "the preview follows the pick")


func test_jump_receives_key_and_table_path() -> void:
	var widget := _make_widget(_table_services())
	widget.set_value("ALPHA")
	assert_true(widget.ref_row.jump_button.visible, "a resolvable key offers the jump")
	assert_false(widget.ref_row.jump_button.disabled)
	widget.ref_row.jump_button.pressed.emit()
	var jumps := _log.filter(func(e): return e[0] == "jump")
	assert_eq(jumps.size(), 1)
	assert_eq(jumps[0][1], "ALPHA", "the jump carries the key")
	assert_eq(jumps[0][2], "/root/menutxt.BIN", "...and the resolved table path")


func test_clear_emits_once_and_resets_preview() -> void:
	var widget := _make_widget(_table_services())
	widget.set_value("ALPHA")
	var emitted: Array = []
	widget.value_changed.connect(func(v: String) -> void: emitted.append(v))
	widget.ref_row.clear_button.pressed.emit()
	assert_eq(emitted, [""], "clear emits the empty value exactly once")
	assert_false(widget.preview.visible, "the preview clears with the key")


func test_value_changed_forwards_once_from_inner_row() -> void:
	var widget := _make_widget(_table_services())
	var emitted: Array = []
	widget.value_changed.connect(func(v: String) -> void: emitted.append(v))
	widget.ref_row.name_edit.text = "BRAVO"
	widget.ref_row.name_edit.text_submitted.emit("BRAVO")
	assert_eq(emitted, ["BRAVO"], "an inner-row commit forwards exactly once")


func test_drop_rejects_table_file_payloads() -> void:
	# The browser pane drags string TABLES (kind "strings", a file); a key row
	# is kind "string_id", which the pane never produces - so file drops are
	# rejected by kind matching alone, with no string-widget drop code.
	var widget := _make_widget(_table_services())
	var emitted: Array = []
	widget.value_changed.connect(func(v: String) -> void: emitted.append(v))
	var table_file := LinkPayload.make("strings", "menutxt.BIN", "C:/res/menutxt.BIN").to_drag_data()
	assert_false(widget.ref_row._can_drop_data(Vector2.ZERO, table_file),
		"a strings-table payload misses a string-key row")
	widget.ref_row._drop_data(Vector2.ZERO, table_file)
	assert_eq(emitted, [], "and never commits")


func test_key_payloads_drop_between_string_widgets_keeping_dots() -> void:
	# Widget-to-widget key drags ride the inherited handler. The committed
	# value must be the KEY verbatim: the identity value_from_path override is
	# load-bearing here - the default basename strip would mangle a dotted key
	# like "BTN.OK" into "BTN".
	var widget := _make_widget(_table_services())
	var emitted: Array = []
	widget.value_changed.connect(func(v: String) -> void: emitted.append(v))
	# Per the contract, a string_id payload's path may carry the KEY (the
	# jump-gate backfill), never trust it as a location.
	var key_payload := LinkPayload.make("string_id", "BTN.OK", "BTN.OK").to_drag_data()
	assert_true(widget.ref_row._can_drop_data(Vector2.ZERO, key_payload),
		"key payloads match the string_id row")
	widget.ref_row._drop_data(Vector2.ZERO, key_payload)
	assert_eq(emitted, ["BTN.OK"], "the key commits verbatim, dots intact")
	assert_eq(widget.preview.text, "Not in string table",
		"the preview re-resolves the dropped key against the TARGET's table")
