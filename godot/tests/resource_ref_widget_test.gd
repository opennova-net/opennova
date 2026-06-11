extends GutTest

# ResourceRefWidget: the framework link row (name + validity badge + browse +
# jump + clear). Driven with fake service Callables — no resource root, no
# shell — to pin the degrade-without-services contract, the badge states, the
# silent set_value (bind_link contract), and the one-emission commit rules.

const ResourceRefWidgetScript := preload("res://modtools/framework/links/resource_ref_widget.gd")
const LinkPayloadScript := preload("res://modtools/framework/links/link_payload.gd")


func _make_widget() -> ResourceRefWidget:
	var w: ResourceRefWidget = ResourceRefWidgetScript.new()
	add_child_autofree(w)
	return w


func _services(resolve_map: Dictionary, log: Dictionary = {}) -> Dictionary:
	# resolve_map: name -> {status, path}; log records pick/jump invocations.
	return {
		"resolve": func(kind: String, name: String) -> Dictionary:
			log["resolved"] = [kind, name]
			return resolve_map.get(name, {"status": "missing", "path": ""}),
		"pick": func(kind: String, title: String, on_pick: Callable) -> void:
			log["pick"] = [kind, title]
			on_pick.call(String(log.get("pick_result", ""))),
		"jump": func(kind: String, path: String) -> void:
			log["jump"] = [kind, path],
	}


func test_without_services_degrades_to_plain_name_field() -> void:
	var w := _make_widget()
	w.configure("terrain", "Terrain")
	w.set_value("dvxi5")
	assert_false(w.badge.visible, "no resolve service: badge hides")
	assert_false(w.browse_button.visible, "no pick service: browse hides")
	assert_false(w.jump_button.visible, "no jump service: jump hides")
	assert_eq(w.name_edit.text, "dvxi5", "the name field still works")


func test_badge_states_follow_resolve() -> void:
	var w := _make_widget()
	w.configure("terrain", "Terrain", _services({
		"good": {"status": "found", "path": "C:/res/good.trn"},
		"odd": {"status": "unprobed", "path": ""},
	}))
	assert_false(w.badge.visible, "empty value: no badge")

	w.set_value("good")
	assert_true(w.badge.visible, "found: badge shows")
	assert_eq(w.badge.text, "●", "found badge glyph")
	assert_false(w.jump_button.disabled, "found: jump enabled")

	w.set_value("nope")
	assert_eq(w.badge.text, "!", "missing badge glyph")
	assert_true(w.jump_button.disabled, "missing: jump disabled")

	w.set_value("odd")
	assert_eq(w.badge.text, "?", "unprobed badge glyph")
	assert_true(w.jump_button.disabled, "unprobed: jump disabled")


func test_set_value_does_not_emit_value_changed() -> void:
	var w := _make_widget()
	var emissions := []
	w.value_changed.connect(func(v: String) -> void: emissions.append(v))
	w.configure("terrain", "Terrain")
	w.set_value("dvxi5")
	w.set_value("other")
	assert_eq(emissions, [], "programmatic set_value must stay silent (bind_link contract)")


func test_pick_commits_through_value_changed_exactly_once() -> void:
	var w := _make_widget()
	var log := {"pick_result": "C:/res/dvxi5.trn"}
	var emissions := []
	w.value_changed.connect(func(v: String) -> void: emissions.append(v))
	w.configure("terrain", "Terrain", _services({}, log))
	w.browse_button.pressed.emit()
	assert_eq(emissions, ["dvxi5"], "a pick commits the basename once")
	assert_eq(log["pick"][0], "terrain", "the picker receives the widget's kind")


func test_jump_invokes_service_with_kind_and_resolved_path() -> void:
	var w := _make_widget()
	var log := {}
	w.configure("terrain", "Terrain", _services({
		"dvxi5": {"status": "found", "path": "C:/res/dvxi5.trn"},
	}, log))
	w.set_value("dvxi5")
	w.jump_button.pressed.emit()
	assert_eq(log["jump"], ["terrain", "C:/res/dvxi5.trn"], "jump carries kind + resolved path")


func test_clear_commits_empty_once_and_repeat_clear_is_noop() -> void:
	var w := _make_widget()
	var emissions := []
	w.value_changed.connect(func(v: String) -> void: emissions.append(v))
	w.configure("terrain", "Terrain")
	w.set_value("dvxi5")
	w.clear_button.pressed.emit()
	w.clear_button.pressed.emit()
	assert_eq(emissions, [""], "clear emits the empty value exactly once")


func test_drag_data_carries_kind_name_and_path() -> void:
	var w := _make_widget()
	w.configure("terrain", "Terrain", _services({
		"dvxi5": {"status": "found", "path": "C:/res/dvxi5.trn"},
	}))
	w.set_value("dvxi5")
	var data: Variant = w._get_drag_data(Vector2.ZERO)
	var payload := LinkPayloadScript.from_drag_data(data)
	assert_not_null(payload, "the badge drag packs a LinkPayload")
	assert_eq(payload.kind, "terrain")
	assert_eq(payload.name, "dvxi5")
	assert_eq(payload.path, "C:/res/dvxi5.trn")
	assert_null(LinkPayloadScript.from_drag_data({"type": "something_else"}),
		"foreign drag data unpacks to null, never a half-filled payload")


func test_focus_out_with_unchanged_text_does_not_commit() -> void:
	var w := _make_widget()
	var emissions := []
	w.value_changed.connect(func(v: String) -> void: emissions.append(v))
	w.configure("terrain", "Terrain")
	w.set_value("dvxi5")
	w.name_edit.focus_exited.emit()
	assert_eq(emissions, [], "leaving the field without editing must not re-commit")


func _terrain_payload(name: String, path := "") -> Dictionary:
	return LinkPayloadScript.make("terrain", name, path).to_drag_data()


func test_drop_with_matching_kind_commits_like_a_pick() -> void:
	var w := _make_widget()
	var emissions := []
	w.value_changed.connect(func(v: String) -> void: emissions.append(v))
	w.configure("terrain", "Terrain")
	var data := _terrain_payload("bravo.trn", "C:/res/bravo.trn")
	assert_true(w._can_drop_data(Vector2.ZERO, data), "a matching-kind payload is accepted")
	w._drop_data(Vector2.ZERO, data)
	assert_eq(emissions, ["bravo"],
		"the drop commits through value_from_path exactly once (basename, like a pick)")


func test_drop_rejects_foreign_kind_and_foreign_data() -> void:
	var w := _make_widget()
	var emissions := []
	w.value_changed.connect(func(v: String) -> void: emissions.append(v))
	w.configure("terrain", "Terrain")
	var foreign_kind := LinkPayloadScript.make("font", "Arial12b.fnt", "").to_drag_data()
	assert_false(w._can_drop_data(Vector2.ZERO, foreign_kind), "a font payload misses a terrain row")
	w._drop_data(Vector2.ZERO, foreign_kind)
	assert_false(w._can_drop_data(Vector2.ZERO, {"card": 3}),
		"non-payload dictionaries (e.g. card reorder drags) are rejected")
	assert_eq(emissions, [], "rejected drops never commit")


func test_drop_with_the_same_value_stays_silent() -> void:
	var w := _make_widget()
	var emissions := []
	w.value_changed.connect(func(v: String) -> void: emissions.append(v))
	w.configure("terrain", "Terrain")
	w.set_value("bravo")
	w._drop_data(Vector2.ZERO, _terrain_payload("bravo.trn"))
	assert_eq(emissions, [], "a drop that resolves to the current value must not emit")


func test_drop_honors_value_from_path_override() -> void:
	var w := _make_widget()
	var emissions := []
	w.value_changed.connect(func(v: String) -> void: emissions.append(v))
	w.set_value_from_path(func(path: String) -> String: return path.get_file())
	w.configure("texture", "Cloud map")
	w._drop_data(Vector2.ZERO, LinkPayloadScript.make("texture", "cloud01.pcx", "").to_drag_data())
	assert_eq(emissions, ["cloud01.pcx"], "extension-keeping adopters keep the extension on drops too")


func test_drop_accepts_image_texture_equivalence() -> void:
	var w := _make_widget()
	w.configure("texture", "Cloud map")
	assert_true(w._can_drop_data(Vector2.ZERO, LinkPayloadScript.make("image", "a.tga", "").to_drag_data()),
		"a texture row accepts the extractors' 'image' spelling")
	w.configure("image", "Picture")
	assert_true(w._can_drop_data(Vector2.ZERO, LinkPayloadScript.make("texture", "a.tga", "").to_drag_data()),
		"and the reverse")


func test_dropped_plain_text_commits_into_the_field() -> void:
	var w := _make_widget()
	var emissions := []
	w.value_changed.connect(func(v: String) -> void: emissions.append(v))
	w.configure("terrain", "Terrain")
	assert_true(w._can_drop_data(Vector2.ZERO, "bravo"), "plain text drops are accepted")
	w._drop_data(Vector2.ZERO, "bravo")
	assert_eq(emissions, ["bravo"], "the text commits through the same path as a typed value")


func test_drop_respects_read_only_field() -> void:
	var w := _make_widget()
	var emissions := []
	w.value_changed.connect(func(v: String) -> void: emissions.append(v))
	w.configure("terrain", "Terrain")
	w.name_edit.editable = false
	assert_false(w._can_drop_data(Vector2.ZERO, _terrain_payload("bravo.trn")),
		"a read-only field accepts nothing")
	w._drop_data(Vector2.ZERO, _terrain_payload("bravo.trn"))
	assert_eq(emissions, [], "and never commits")
