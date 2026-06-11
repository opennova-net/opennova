extends GutTest

# TextureRefWidget / TexturePreviewBox: the always-on texture preview over a
# link row. Fake loaders pin the memoization contract (texture decode is NOT
# cached upstream, so the box must load once per name), the missing-texture
# placeholder, and the push-overrides-pull rule.

const TextureRefWidgetScript := preload("res://modtools/framework/links/texture_ref_widget.gd")
const LinkPayloadScript := preload("res://modtools/framework/links/link_payload.gd")


func _make_widget() -> TextureRefWidget:
	var w: TextureRefWidget = TextureRefWidgetScript.new()
	add_child_autofree(w)
	return w


func _solid_texture() -> Texture2D:
	var img := Image.create(4, 4, false, Image.FORMAT_RGB8)
	img.fill(Color.RED)
	return ImageTexture.create_from_image(img)


func test_preview_loader_called_once_per_name_change() -> void:
	var w := _make_widget()
	var calls := []
	var tex := _solid_texture()
	w.set_preview_loader(func(name: String) -> Texture2D:
		calls.append(name)
		return tex)
	w.set_value("rock.tga")
	w.set_value("rock.tga")
	w.refresh_preview()
	assert_eq(calls, ["rock.tga"], "an unchanged name must not reload (decode is uncached upstream)")
	w.set_value("sand.tga")
	assert_eq(calls, ["rock.tga", "sand.tga"], "a new name loads exactly once")


func test_missing_texture_shows_placeholder() -> void:
	var w := _make_widget()
	w.set_preview_loader(func(_name: String) -> Texture2D: return null)
	w.set_value("gone.tga")
	assert_null(w.preview.get_texture(), "no texture resolved")
	assert_true(w.preview._missing_label.visible, "the not-found mark shows for a named-but-missing texture")


func test_push_texture_overrides_loader() -> void:
	var w := _make_widget()
	w.set_preview_loader(func(_name: String) -> Texture2D: return null)
	w.set_value("sky.pcx")
	var tex := _solid_texture()
	w.set_preview_texture(tex)
	assert_eq(w.preview.get_texture(), tex, "the push feed displays the caller's texture")
	assert_false(w.preview._missing_label.visible, "a pushed texture clears the not-found mark")


func test_value_changed_forwards_once_from_inner_row() -> void:
	var w := _make_widget()
	var emissions := []
	w.value_changed.connect(func(v: String) -> void: emissions.append(v))
	w.configure("texture", "Cloud map")
	w.ref_row.name_edit.text = "cloud02.pcx"
	w.ref_row.name_edit.text_submitted.emit("cloud02.pcx")
	assert_eq(emissions, ["cloud02.pcx"], "the inner row's commit forwards exactly once")
	assert_eq(w.get_value(), "cloud02.pcx", "get_value reads through to the row")


func test_clear_resets_preview() -> void:
	var w := _make_widget()
	w.set_preview_loader(func(_name: String) -> Texture2D: return _solid_texture())
	w.set_value("cloud01.pcx")
	assert_not_null(w.preview.get_texture(), "preview filled")
	w.ref_row.clear_button.pressed.emit()
	assert_null(w.preview.get_texture(), "clearing the value empties the preview")
	assert_false(w.preview._missing_label.visible, "an empty value shows the bare checker, not a missing mark")


func test_drop_accepts_image_flavored_payloads_and_keeps_extension() -> void:
	var w := _make_widget()
	var emissions := []
	w.value_changed.connect(func(v: String) -> void: emissions.append(v))
	w.configure("texture", "Cloud map")
	var data := LinkPayloadScript.make("image", "cloud02.pcx", "C:/res/cloud02.pcx").to_drag_data()
	assert_true(w._can_drop_data(Vector2.ZERO, data),
		"the widget-level handlers accept image payloads")
	w._drop_data(Vector2.ZERO, data)
	assert_eq(emissions, ["cloud02.pcx"], "the drop commits the file name with extension, once")
	assert_eq(w.get_value(), "cloud02.pcx")


func test_preview_box_lets_the_drop_walk_through() -> void:
	# The handlers above are only reachable over the preview image if the box's
	# display layers don't eat the walk: the TextureRects must IGNORE mouse,
	# the missing-mark keeps its tooltip via PASS (not STOP), and the panel
	# itself forwards to the row's handlers.
	var w := _make_widget()
	assert_eq(w.preview._checker.mouse_filter, Control.MOUSE_FILTER_IGNORE,
		"the checkerboard never takes mouse events")
	assert_eq(w.preview._preview.mouse_filter, Control.MOUSE_FILTER_IGNORE,
		"the image layer never takes mouse events")
	assert_eq(w.preview._missing_label.mouse_filter, Control.MOUSE_FILTER_PASS,
		"the not-found mark keeps its tooltip but passes drops through")


func test_configure_strips_jump_but_keeps_resolve_and_pick() -> void:
	var w := _make_widget()
	w.configure("texture", "Cloud map", {
		"resolve": func(_kind: String, _name: String) -> Dictionary:
			return {"status": "found", "path": "C:/res/cloud01.pcx"},
		"pick": func(_kind: String, _title: String, _on_pick: Callable) -> void:
			pass,
		"jump": func(_kind: String, _path: String) -> void:
			pass,
	})
	w.set_value("cloud01.pcx")
	assert_true(w.ref_row.browse_button.visible, "pick survives")
	assert_true(w.ref_row.badge.visible, "resolve survives")
	assert_false(w.ref_row.jump_button.visible, "jump is stripped — there is no texture workspace")
