extends GutTest

# TextureSlotWidget regression net: the slot contract (one value_changed with
# slot id, silent set_value, enable gating) predates this file; the thumbnail
# rows are the A7 addition.

const TextureSlotWidgetScript := preload("res://modtools/object/ui/widgets/texture_slot_widget.gd")


func _make_widget(slot := 3) -> TextureSlotWidget:
	var w: TextureSlotWidget = TextureSlotWidgetScript.new()
	w.configure(slot)
	add_child_autofree(w)
	return w


func _solid_texture() -> Texture2D:
	var img := Image.create(4, 4, false, Image.FORMAT_RGB8)
	img.fill(Color.BLUE)
	return ImageTexture.create_from_image(img)


func test_commit_emits_slot_id_and_filename_once() -> void:
	var w := _make_widget(5)
	var emissions := []
	w.value_changed.connect(func(slot: int, filename: String) -> void: emissions.append([slot, filename]))
	w.line_edit.text = "hull.tga"
	w.line_edit.text_submitted.emit("hull.tga")
	w.line_edit.text_submitted.emit("hull.tga")
	assert_eq(emissions, [[5, "hull.tga"]], "one emission carrying the slot id; repeats are no-ops")


func test_set_value_updates_thumb_without_emitting() -> void:
	var w := _make_widget()
	var calls := []
	w.set_preview_loader(func(name: String) -> Texture2D:
		calls.append(name)
		return _solid_texture())
	var emissions := []
	w.value_changed.connect(func(slot: int, filename: String) -> void: emissions.append([slot, filename]))
	w.set_value("hull.tga")
	assert_eq(calls, ["hull.tga"], "set_value feeds the thumbnail")
	assert_not_null(w.thumb.get_texture(), "thumbnail filled")
	assert_eq(emissions, [], "programmatic set_value stays silent")


func test_disabled_slot_dims_thumb_and_disables_buttons() -> void:
	var w := _make_widget()
	w.set_value("hull.tga")
	w.set_slot_enabled(false)
	assert_false(w.line_edit.editable, "name field locks")
	assert_true(w.browse_button.disabled, "browse locks")
	assert_lt(w.thumb.modulate.a, 1.0, "thumbnail dims")
	w.set_slot_enabled(true)
	assert_eq(w.thumb.modulate.a, 1.0, "thumbnail restores")


func test_clear_button_empties_value_and_thumb() -> void:
	var w := _make_widget()
	w.set_preview_loader(func(_name: String) -> Texture2D: return _solid_texture())
	w.set_value("hull.tga")
	var emissions := []
	w.value_changed.connect(func(slot: int, filename: String) -> void: emissions.append([slot, filename]))
	w.clear_button.pressed.emit()
	assert_eq(emissions.size(), 1, "clear commits once")
	assert_eq(emissions[0][1], "", "clear commits the empty filename")
	assert_null(w.thumb.get_texture(), "thumbnail empties with the value")
