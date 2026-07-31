extends ObjectListDetailInspector

## Materials workflow: a left list of materials plus a right detail dock for
## the selected material's shader tag, texture slots, alpha, and UV/RGB/alpha/
## texture-animation generators.

const ShaderTagPickerScript = preload("res://modtools/object/ui/widgets/shader_tag_picker.gd")
const TextureSlotWidgetScript = preload("res://modtools/object/ui/widgets/texture_slot_widget.gd")
const AnimFramesDialogScript = preload("res://modtools/object/ui/dialogs/anim_frames_dialog.gd")

const TEXTURE_SLOTS := [
	{"slot": 1, "label": "Diffuse"},
	{"slot": 2, "label": "Detail"},
	{"slot": 3, "label": "Normal A"},
	{"slot": 4, "label": "Normal B"},
]
# Generator style names come from GeneratorStyleCatalog's consumer-specific
# views; the raw style id still passes straight through to
# set_material_*_generator.
# Material "flags" bitfield.
const MATERIAL_FLAG_ALPHA := 0x01
# Texture-slot "flags" bitfield.
const TEXTURE_FLAG_ANIMATED := 0x01
const TEXTURE_FLAG_CLAMPED := 0x02

var _material_paste_button: Button
var _material_clipboard: Dictionary = {}
# Selecting a material re-syncs these already-built controls in place (via
# _resync_detail) instead of rebuilding the whole detail dock, which was the
# selection lag. _sync_detail_fields is the build-time sync_fields closure;
# _detail_root guards against calling it after the dock was freed.
var _sync_detail_fields: Callable = Callable()
var _detail_root: Control = null
# The shader catalog is a fixed table for the open object; cache it so it is not
# re-fetched on every detail (re)build. Invalidated in refresh() on data change.
var _shader_catalog_cache: Array = []


func build_main(mount: Control) -> void:
	var box := _make_inspector_box(mount)
	_build_list_panel(box)

	var material_toolbar := HBoxContainer.new()
	material_toolbar.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_list_panel.add_child(material_toolbar)

	var copy_button := Button.new()
	copy_button.name = "MaterialCopyButton"
	copy_button.text = "Copy"
	material_toolbar.add_child(copy_button)

	var paste_button := Button.new()
	paste_button.name = "MaterialPasteButton"
	paste_button.text = "Paste"
	paste_button.disabled = _material_clipboard.is_empty()
	material_toolbar.add_child(paste_button)
	_material_paste_button = paste_button

	copy_button.pressed.connect(func() -> void:
		var current_materials := object_editor.object_data.get_materials() if object_editor and object_editor.object_data else []
		if _selected_index >= 0 and _selected_index < current_materials.size():
			_material_clipboard = (current_materials[_selected_index] as Dictionary).duplicate(true)
			paste_button.disabled = false
	)
	paste_button.pressed.connect(func() -> void:
		if _selected_index < 0 or _material_clipboard.is_empty() or object_editor == null or object_editor.object_data == null:
			return
		_paste_material_settings(_selected_index, _material_clipboard)
		_refresh_materials_left_list()
		_rebuild_detail_dock()
	)


func _list_items() -> Array:
	return object_editor.object_data.get_materials() if object_editor and object_editor.object_data else []


func _list_item_text(item, _index: int) -> String:
	return "%02d  %s" % [int(item.get("index", 0)), String(item.get("shader", ""))]


func _list_summary_text(count: int) -> String:
	return "%d materials" % count


func _list_node_name() -> StringName:
	return &"MaterialsList"


func _list_panel_node_name() -> StringName:
	return &"MaterialListPanel"


func refresh() -> void:
	_shader_catalog_cache = []
	_refresh_materials_left_list()


func _resync_detail(index: int) -> bool:
	if not _sync_detail_fields.is_valid() or _detail_root == null or not is_instance_valid(_detail_root):
		return false
	_selected_index = index
	_sync_detail_fields.call(index)
	return true


func build_detail(box: VBoxContainer) -> void:
	# Invalidate the in-place resync seam before (re)building so a stale closure is
	# never reused against freed nodes; it is re-armed at the end on success.
	_sync_detail_fields = Callable()
	_detail_root = null
	var materials := object_editor.object_data.get_materials() if object_editor and object_editor.object_data else []
	var shader_catalog := _shader_catalog()

	var detail_box := VBoxContainer.new()
	detail_box.name = "MaterialDetailPanel"
	detail_box.add_theme_constant_override("separation", 8)
	detail_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(detail_box)
	if materials.is_empty():
		_add_empty_state(detail_box, "Select a material.", "MaterialDetailEmpty")
		return
	_selected_index = clampi(_selected_index, 0, materials.size() - 1)

	var shader_label := Label.new()
	shader_label.text = "Shader"
	detail_box.add_child(shader_label)

	var shader_option = ShaderTagPickerScript.new()
	shader_option.name = "ShaderTagPicker"
	shader_option.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	detail_box.add_child(shader_option)
	var shader_option_legacy := OptionButton.new()
	shader_option_legacy.name = "ShaderTagOption"
	shader_option_legacy.visible = false
	detail_box.add_child(shader_option_legacy)

	var shader_status := Label.new()
	shader_status.name = "MaterialShaderStatus"
	shader_status.clip_text = true
	shader_status.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	detail_box.add_child(shader_status)

	var slot_controls := {}
	for slot_def in TEXTURE_SLOTS:
		var slot := int(slot_def.get("slot", 0))
		var slot_box := VBoxContainer.new()
		slot_box.name = "TextureSlot%dBox" % slot
		slot_box.add_theme_constant_override("separation", 4)
		detail_box.add_child(slot_box)

		var label := Label.new()
		label.text = String(slot_def.get("label", "Texture"))
		slot_box.add_child(label)

		var row := HBoxContainer.new()
		row.name = "TextureSlot%dRow" % slot
		row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		slot_box.add_child(row)

		var texture_widget = TextureSlotWidgetScript.new()
		texture_widget.name = "TextureSlot%dWidget" % slot
		texture_widget.configure(slot)
		texture_widget.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		# Thumbnail loads through the object data's own resolution path (resource
		# root or the model's source folder) — the same lookup the renderer uses.
		texture_widget.set_preview_loader(func(name: String) -> Texture2D:
			if object_editor == null or object_editor.object_data == null:
				return null
			return object_editor.object_data.load_texture_name(name))
		row.add_child(texture_widget)

		var options_row := HBoxContainer.new()
		options_row.name = "TextureSlot%dOptionsRow" % slot
		options_row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		slot_box.add_child(options_row)

		var clamped := _add_checkbox(options_row, "TextureSlot%dClamped" % slot, "Clamp")

		var animated := _add_checkbox(options_row, "TextureSlot%dAnimated" % slot, "Anim")

		var frame := SpinBox.new()
		frame.name = "TextureSlot%dFrame" % slot
		frame.min_value = 0
		frame.max_value = 255
		frame.step = 1
		frame.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		options_row.add_child(frame)

		var status := Label.new()
		status.name = "TextureSlot%dStatus" % slot
		status.clip_text = true
		status.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		slot_box.add_child(status)

		slot_controls[slot] = {
			"box": slot_box,
			"widget": texture_widget,
			"edit": texture_widget.line_edit,
			"status": status,
			"browse": texture_widget.browse_button,
			"clear": texture_widget.clear_button,
			"clamped": clamped,
			"animated": animated,
			"frame": frame,
		}

	var alpha_label := Label.new()
	alpha_label.text = "Alpha threshold"
	detail_box.add_child(alpha_label)

	var alpha := SpinBox.new()
	alpha.name = "AlphaThreshold"
	alpha.min_value = 0.0
	alpha.max_value = 1.0
	alpha.step = 0.01
	alpha.custom_minimum_size = Vector2(0, 34)
	detail_box.add_child(alpha)

	var generator_controls := _build_generator_controls(detail_box)

	var edit_frames := Button.new()
	edit_frames.name = "TextureAnimationFramesButton"
	edit_frames.text = "Edit frames..."
	detail_box.add_child(edit_frames)

	var anim_frames_dialog = AnimFramesDialogScript.new()
	anim_frames_dialog.name = "MaterialAnimFramesDialog"
	box.add_child(anim_frames_dialog)

	# One shared texture picker (parented to `box` so a rebuild frees it); the
	# target slot rides each one-shot callback's capture instead of a pending dict.
	var files := FileDialogHelper.new(box)
	var texture_filters := PackedStringArray([
		"*.tga,*.TGA,*.dds,*.DDS,*.mdt,*.MDT,*.pcx,*.PCX,*.png,*.PNG ; Object textures",
		"*.jpg,*.JPG,*.jpeg,*.JPEG,*.bmp,*.BMP ; Image fallbacks",
	])

	var selected := {"index": _selected_index}
	var guard := SyncGuard.new()

	var refresh_materials := func() -> void:
		materials = object_editor.object_data.get_materials() if object_editor and object_editor.object_data else []
		_refresh_materials_left_list()

	var sync_fields := func(index: int) -> void:
		guard.active = true
		selected["index"] = index
		# Re-read fresh: this closure's captured `materials` is the build-time
		# snapshot (GDScript lambdas capture locals by value), so after an edit
		# resyncs the panel it would otherwise show stale generator/shader data.
		materials = object_editor.object_data.get_materials() if object_editor and object_editor.object_data else materials
		if index < 0 or index >= materials.size():
			guard.active = false
			return
		var material: Dictionary = materials[index]
		shader_option.setup(shader_catalog, String(material.get("shader", "")))
		_populate_shader_tags(shader_option_legacy, String(material.get("shader", "")), shader_catalog)
		var shader_info := _shader_info_for_material(material, shader_catalog)
		shader_status.text = _shader_status_text(shader_info)
		alpha.value = float(material.get("alpha_threshold", 0.0))
		alpha.editable = bool(shader_info.get("is_alpha", false)) or (int(material.get("flags", 0)) & MATERIAL_FLAG_ALPHA) != 0
		for slot_def in TEXTURE_SLOTS:
			var slot := int(slot_def.get("slot", 0))
			var controls: Dictionary = slot_controls.get(slot, {})
			var texture_info := _texture_for_slot(material, slot)
			var supported := _shader_supports_texture_slot(shader_info, slot)
			var occupied := not String(texture_info.get("name", "")).is_empty()
			var slot_box := controls.get("box") as Control
			var texture_widget = controls.get("widget")
			var edit := controls.get("edit") as LineEdit
			var status := controls.get("status") as Label
			var browse := controls.get("browse") as Button
			var clear := controls.get("clear") as Button
			var clamped := controls.get("clamped") as CheckBox
			var animated := controls.get("animated") as CheckBox
			var frame := controls.get("frame") as SpinBox
			if slot_box != null:
				slot_box.visible = supported or occupied
			if texture_widget != null:
				texture_widget.set_value(String(texture_info.get("name", "")))
				texture_widget.set_slot_enabled(supported)
				texture_widget.set_placeholder("Filename in object folder" if supported else "Unsupported by shader")
			if edit != null:
				edit.text = String(texture_info.get("name", ""))
				edit.editable = supported
				edit.placeholder_text = "Filename in object folder" if supported else "Unsupported by shader"
			if browse != null:
				browse.disabled = not supported
			if clear != null:
				clear.disabled = not supported or not occupied
			var texture_flags := int(texture_info.get("flags", 0))
			if clamped != null:
				clamped.button_pressed = (texture_flags & TEXTURE_FLAG_CLAMPED) != 0
				clamped.disabled = not supported or not occupied
			if animated != null:
				animated.button_pressed = (texture_flags & TEXTURE_FLAG_ANIMATED) != 0
				animated.disabled = not supported or not occupied
			if frame != null:
				frame.value = int(texture_info.get("frame", 0))
				frame.editable = supported and occupied
			if status != null:
				status.text = _describe_texture_status(index, texture_info, supported)
		_sync_generator_controls(generator_controls, material, shader_info)
		guard.active = false

	var resync_selected := func() -> void:
		var current_index := int(selected.get("index", -1))
		if current_index >= materials.size():
			current_index = materials.size() - 1
		_selected_index = current_index
		selected["index"] = current_index
		refresh_materials.call()
		if current_index >= 0 and current_index < materials.size():
			sync_fields.call(current_index)

	var apply_slot_name := func(slot: int, value: String) -> void:
		var current_index := int(selected.get("index", -1))
		if current_index < 0 or object_editor == null or object_editor.object_data == null:
			return
		object_editor.object_data.set_material_texture_slot(current_index, slot, value.get_file())
		resync_selected.call()

	var apply_slot_options := func(slot: int) -> void:
		var current_index := int(selected.get("index", -1))
		if current_index < 0 or object_editor == null or object_editor.object_data == null:
			return
		var controls: Dictionary = slot_controls.get(slot, {})
		var clamped := controls.get("clamped") as CheckBox
		var animated := controls.get("animated") as CheckBox
		var frame := controls.get("frame") as SpinBox
		var flags := 0
		if animated != null and animated.button_pressed:
			flags |= 0x01
		if clamped != null and clamped.button_pressed:
			flags |= 0x02
		var type := 4 if slot == 3 or slot == 4 else 0
		object_editor.object_data.set_material_texture_slot_options(current_index, slot, flags, int(frame.value) if frame != null else 0, type)
		resync_selected.call()

	var apply_uv_generator := func(axis: String, params: Dictionary) -> void:
		if guard.active or selected["index"] < 0:
			return
		object_editor.object_data.set_material_uv_generator(selected["index"], axis, params)
		resync_selected.call()

	var apply_rgb_generator := func(params: Dictionary) -> void:
		if guard.active or selected["index"] < 0:
			return
		object_editor.object_data.set_material_rgb_generator(selected["index"], params)
		resync_selected.call()

	var apply_alpha_generator := func(params: Dictionary) -> void:
		if guard.active or selected["index"] < 0:
			return
		object_editor.object_data.set_material_alpha_generator(selected["index"], params)
		resync_selected.call()

	var apply_texture_animation := func(params: Dictionary) -> void:
		if guard.active or selected["index"] < 0:
			return
		object_editor.object_data.set_material_texture_animation(selected["index"], params)
		resync_selected.call()

	var show_slot_message := func(slot: int, message: String) -> void:
		var controls: Dictionary = slot_controls.get(slot, {})
		var status := controls.get("status") as Label
		if status != null:
			status.text = message

	shader_option.value_changed.connect(func(tag: String) -> void:
		if guard.active or selected["index"] < 0:
			return
		object_editor.object_data.set_material_shader(selected["index"], tag)
		resync_selected.call()
	)

	for slot_def in TEXTURE_SLOTS:
		var slot := int(slot_def.get("slot", 0))
		var controls: Dictionary = slot_controls.get(slot, {})
		var texture_widget = controls.get("widget")
		var edit := controls.get("edit") as LineEdit
		if texture_widget != null:
			texture_widget.value_changed.connect(func(slot_id: int, value: String) -> void:
				apply_slot_name.call(slot_id, value)
			)
			texture_widget.browse_requested.connect(func(slot_id: int) -> void:
				var source_dir := object_editor.object_data.get_source_dir() if object_editor and object_editor.object_data else ""
				if source_dir.is_empty():
					show_slot_message.call(slot_id, "Open an object before choosing a texture.")
					return
				files.open("Choose a texture", texture_filters, func(path: String) -> void:
					var file_name := _object_folder_filename(path)
					if file_name.is_empty():
						show_slot_message.call(slot_id, "Choose a file in the object folder.")
						return
					apply_slot_name.call(slot_id, file_name), source_dir)
			)
		if edit != null:
			edit.text_submitted.connect(func(value: String, slot_id: int = slot) -> void:
				apply_slot_name.call(slot_id, value)
			)
		var clamped := controls.get("clamped") as CheckBox
		var animated := controls.get("animated") as CheckBox
		var frame := controls.get("frame") as SpinBox
		if clamped != null:
			clamped.toggled.connect(func(_pressed: bool, slot_id: int = slot) -> void:
				if not guard.active:
					apply_slot_options.call(slot_id)
			)
		if animated != null:
			animated.toggled.connect(func(_pressed: bool, slot_id: int = slot) -> void:
				if not guard.active:
					apply_slot_options.call(slot_id)
			)
		if frame != null:
			frame.value_changed.connect(func(_value: float, slot_id: int = slot) -> void:
				if not guard.active:
					apply_slot_options.call(slot_id)
			)

	alpha.value_changed.connect(func(value: float) -> void:
		if not guard.active and selected["index"] >= 0:
			object_editor.object_data.set_material_alpha_threshold(selected["index"], value)
			resync_selected.call()
	)
	for axis in ["u", "v"]:
		var axis_id := String(axis)
		var axis_controls: Dictionary = generator_controls.get(axis_id, {})
		for key in ["phase", "rate", "start", "end"]:
			var spin := axis_controls.get(key) as SpinBox
			if spin != null:
				_connect_uv_generator_spin(spin, axis_id, String(key), generator_controls, apply_uv_generator)
		var style_option := axis_controls.get("style_option") as OptionButton
		if style_option != null:
			style_option.item_selected.connect(func(index: int, selected_axis: String = axis_id, option: OptionButton = style_option) -> void:
				var params := _uv_generator_params(generator_controls, selected_axis)
				params["style"] = option.get_item_id(index)
				apply_uv_generator.call(selected_axis, params)
			)
		var reg_picker = axis_controls.get("reg_picker")
		if reg_picker != null:
			reg_picker.register_selected.connect(func(reg: int, selected_axis: String = axis_id) -> void:
				var params := _uv_generator_params(generator_controls, selected_axis)
				params["reg"] = reg
				apply_uv_generator.call(selected_axis, params)
			)
	for key in ["phase", "rate"]:
		var spin := generator_controls.get("rgb_%s" % key) as SpinBox
		if spin != null:
			_connect_rgb_generator_spin(spin, String(key), generator_controls, apply_rgb_generator)
	var rgb_style_option := generator_controls.get("rgb_style_option") as OptionButton
	if rgb_style_option != null:
		rgb_style_option.item_selected.connect(func(index: int) -> void:
			var params := _rgb_generator_params(generator_controls)
			params["style"] = rgb_style_option.get_item_id(index)
			apply_rgb_generator.call(params)
		)
	var rgb_reg_picker = generator_controls.get("rgb_reg_picker")
	if rgb_reg_picker != null:
		rgb_reg_picker.register_selected.connect(func(reg: int) -> void:
			var params := _rgb_generator_params(generator_controls)
			params["reg"] = reg
			apply_rgb_generator.call(params)
		)
	var rgb_start := generator_controls.get("rgb_start_color") as ColorPickerButton
	var rgb_end := generator_controls.get("rgb_end_color") as ColorPickerButton
	if rgb_start != null:
		rgb_start.color_changed.connect(func(_color: Color) -> void:
			apply_rgb_generator.call(_rgb_generator_params(generator_controls))
		)
	if rgb_end != null:
		rgb_end.color_changed.connect(func(_color: Color) -> void:
			apply_rgb_generator.call(_rgb_generator_params(generator_controls))
		)
	for key in ["phase", "rate", "start", "end"]:
		var spin := generator_controls.get("alpha_%s" % key) as SpinBox
		if spin != null:
			_connect_alpha_generator_spin(spin, String(key), generator_controls, apply_alpha_generator)
	var alpha_style_option := generator_controls.get("alpha_style_option") as OptionButton
	if alpha_style_option != null:
		alpha_style_option.item_selected.connect(func(index: int) -> void:
			var params := _alpha_generator_params(generator_controls)
			params["style"] = alpha_style_option.get_item_id(index)
			apply_alpha_generator.call(params)
		)
	var alpha_reg_picker = generator_controls.get("alpha_reg_picker")
	if alpha_reg_picker != null:
		alpha_reg_picker.register_selected.connect(func(reg: int) -> void:
			var params := _alpha_generator_params(generator_controls)
			params["reg"] = reg
			apply_alpha_generator.call(params)
		)
	for key in ["frames", "type", "time"]:
		var spin := generator_controls.get("anim_%s" % key) as SpinBox
		if spin != null:
			_connect_texture_animation_spin(spin, String(key), generator_controls, apply_texture_animation)
	edit_frames.pressed.connect(func() -> void:
		var current_index := int(selected.get("index", -1))
		if current_index < 0 or current_index >= materials.size():
			return
		var material: Dictionary = materials[current_index]
		var shader_info := _shader_info_for_material(material, shader_catalog)
		anim_frames_dialog.setup(object_editor.object_data, current_index, shader_info)
		anim_frames_dialog.popup_centered()
	)
	anim_frames_dialog.frame_changed.connect(func() -> void:
		resync_selected.call()
	)
	if not materials.is_empty():
		sync_fields.call(_selected_index)
		# Arm the in-place resync: subsequent list selections re-run sync_fields on
		# these built controls instead of rebuilding the dock.
		_sync_detail_fields = sync_fields
		_detail_root = detail_box


func _paste_material_settings(target_index: int, material: Dictionary) -> void:
	if object_editor == null or object_editor.object_data == null:
		return
	object_editor.object_data.set_material_shader(target_index, String(material.get("shader", "")))
	object_editor.object_data.set_material_alpha_threshold(target_index, float(material.get("alpha_threshold", 0.0)))
	for slot_def in TEXTURE_SLOTS:
		var slot := int(slot_def.get("slot", 0))
		var texture := _texture_for_slot(material, slot)
		object_editor.object_data.set_material_texture_slot(target_index, slot, String(texture.get("name", "")))
		if not String(texture.get("name", "")).is_empty():
			object_editor.object_data.set_material_texture_slot_options(
					target_index,
					slot,
					int(texture.get("flags", 0)),
					int(texture.get("frame", 0)),
					int(texture.get("type", 0)))
	for axis in ["u", "v"]:
		var key := "%s_params" % axis
		if material.has(key):
			object_editor.object_data.set_material_uv_generator(target_index, axis, material.get(key, {}))
	if material.has("rgb_gen"):
		object_editor.object_data.set_material_rgb_generator(target_index, material.get("rgb_gen", {}))
	if material.has("alpha_gen"):
		object_editor.object_data.set_material_alpha_generator(target_index, material.get("alpha_gen", {}))
	if material.has("animation"):
		object_editor.object_data.set_material_texture_animation(target_index, material.get("animation", {}))


func _refresh_materials_left_list() -> void:
	_refresh_list()
	if _material_paste_button != null and is_instance_valid(_material_paste_button):
		_material_paste_button.disabled = _material_clipboard.is_empty()


func _populate_shader_tags(option: OptionButton, current: String, shader_catalog: Array) -> void:
	option.clear()
	var known_tags := []
	for shader in shader_catalog:
		known_tags.append(String(shader.get("name", "")))
	if not current.is_empty() and not known_tags.has(current):
		option.add_item(current)
	for shader in shader_catalog:
		var shader_tag := String(shader.get("name", ""))
		if shader_tag.is_empty():
			continue
		option.add_item(shader_tag)
	var match_index := _shader_index(option, current)
	if match_index >= 0:
		option.select(match_index)
	elif option.get_item_count() > 0:
		option.select(0)


func _shader_index(option: OptionButton, tag: String) -> int:
	for i in range(option.get_item_count()):
		if option.get_item_text(i) == tag:
			return i
	return -1


func _texture_for_slot(material: Dictionary, slot: int) -> Dictionary:
	var textures: Array = material.get("textures", [])
	for i in range(textures.size()):
		var texture: Dictionary = textures[i]
		if int(texture.get("slot", 0)) == slot:
			return {
				"texture_index": i,
				"name": String(texture.get("name", "")),
				"flags": int(texture.get("flags", 0)),
				"frame": int(texture.get("frame", 0)),
				"type": int(texture.get("type", 0)),
				"resolved_path": String(texture.get("resolved_path", "")),
			}
	return {
		"texture_index": -1,
		"name": "",
		"flags": 0,
		"frame": 0,
		"type": 0,
		"resolved_path": "",
	}


func _describe_texture_status(material_index: int, texture_info: Dictionary, supported: bool = true) -> String:
	var name := String(texture_info.get("name", ""))
	if name.is_empty():
		return "Empty" if supported else "Unsupported by shader"
	var texture_index := int(texture_info.get("texture_index", -1))
	if object_editor == null or object_editor.object_data == null or texture_index < 0:
		return "Present but unsupported by shader" if not supported else "Missing in object folder"
	var resolved := String(texture_info.get("resolved_path", ""))
	if resolved.is_empty():
		resolved = object_editor.object_data.resolve_material_texture_path(material_index, texture_index)
	if resolved.is_empty():
		return "Present but unsupported by shader; missing in object folder" if not supported else "Missing in object folder"
	var prefix := "Unsupported by shader; resolved" if not supported else "Resolved"
	return "%s: %s" % [prefix, resolved.get_file()]


func _shader_catalog() -> Array:
	if not _shader_catalog_cache.is_empty():
		return _shader_catalog_cache
	if object_editor != null and object_editor.object_data != null:
		var catalog: Array = object_editor.object_data.get_shader_catalog()
		if not catalog.is_empty():
			_shader_catalog_cache = catalog
			return _shader_catalog_cache
	return []


func _shader_info_for_material(material: Dictionary, shader_catalog: Array) -> Dictionary:
	var shader_name := String(material.get("shader", ""))
	for shader in shader_catalog:
		if String(shader.get("name", "")) == shader_name:
			return shader
	var info := {}
	for key in ["shader_flags", "has_diffuse", "has_secondary", "has_normal_a", "has_normal_b", "is_alpha", "is_luminance", "is_glass_shader", "is_skinned_shader", "is_blending_shader", "uses_uv_generators", "uses_environment", "uses_specular", "uses_flag_animation", "shader_family", "shader_blend", "normal_space"]:
		if material.has(key):
			info[key] = material[key]
	info["name"] = shader_name
	return info


func _shader_supports_texture_slot(shader_info: Dictionary, slot: int) -> bool:
	match slot:
		1:
			return bool(shader_info.get("has_diffuse", false))
		2:
			return bool(shader_info.get("has_secondary", false))
		3:
			return bool(shader_info.get("has_normal_a", false))
		4:
			return bool(shader_info.get("has_normal_b", false))
		_:
			return false


func _shader_status_text(shader_info: Dictionary) -> String:
	var bits := []
	if bool(shader_info.get("has_diffuse", false)):
		bits.append("diffuse")
	if bool(shader_info.get("has_secondary", false)):
		bits.append("detail")
	if bool(shader_info.get("has_normal_a", false)):
		bits.append("normal")
	if bool(shader_info.get("has_normal_b", false)):
		bits.append("normal B")
	if bool(shader_info.get("is_alpha", false)):
		bits.append("alpha")
	if bool(shader_info.get("is_luminance", false)):
		bits.append("luminance")
	if bool(shader_info.get("is_glass_shader", false)):
		bits.append("glass")
	if bool(shader_info.get("uses_uv_generators", false)):
		bits.append("UV gen")
	return "Enabled: %s" % ", ".join(bits) if not bits.is_empty() else "No material options enabled by this shader."


func _build_generator_controls(box: VBoxContainer) -> Dictionary:
	var controls := {}
	var title := Label.new()
	title.text = "Generators"
	box.add_child(title)

	for axis in ["u", "v"]:
		var section := VBoxContainer.new()
		section.name = "%sGeneratorSection" % axis.to_upper()
		section.add_theme_constant_override("separation", 4)
		box.add_child(section)
		var header := Label.new()
		header.text = "%s map function" % axis.to_upper()
		section.add_child(header)
		var axis_controls := {
			"box": section,
			"style_option": _add_id_option_row(section, "%sGeneratorStyleOption" % axis.to_upper(), "Style", GeneratorStyleCatalog.options_for_consumer(GeneratorStyleCatalog.CONSUMER_UV)),
			"phase": _add_spin_row(section, "%sGeneratorPhase" % axis.to_upper(), "Phase", -100000, 100000, 0.01),
			"reg_picker": _add_ctrl_reg_row(section, "%sGeneratorControlReg" % axis.to_upper(), "Control reg"),
			"rate": _add_spin_row(section, "%sGeneratorRate" % axis.to_upper(), "Rate", -100000, 100000, 0.01),
			"start": _add_spin_row(section, "%sGeneratorStart" % axis.to_upper(), "Start", -100000, 100000, 0.01),
			"end": _add_spin_row(section, "%sGeneratorEnd" % axis.to_upper(), "End", -100000, 100000, 0.01),
		}
		controls[axis] = axis_controls

	var rgb_section := VBoxContainer.new()
	rgb_section.name = "RgbGeneratorSection"
	rgb_section.add_theme_constant_override("separation", 4)
	box.add_child(rgb_section)
	var rgb_header := Label.new()
	rgb_header.text = "RGB gen"
	rgb_section.add_child(rgb_header)
	controls["rgb_box"] = rgb_section
	controls["rgb_style_option"] = _add_id_option_row(rgb_section, "RgbGeneratorStyleOption", "Style", GeneratorStyleCatalog.options_for_consumer(GeneratorStyleCatalog.CONSUMER_RGB))
	controls["rgb_phase"] = _add_spin_row(rgb_section, "RgbGeneratorPhase", "Phase", -100000, 100000, 0.01)
	controls["rgb_reg_picker"] = _add_ctrl_reg_row(rgb_section, "RgbGeneratorControlReg", "Control reg")
	controls["rgb_rate"] = _add_spin_row(rgb_section, "RgbGeneratorRate", "Rate", -100000, 100000, 0.01)
	controls["rgb_start_color"] = _add_color_row(rgb_section, "RgbGeneratorStartColor", "Start")
	controls["rgb_end_color"] = _add_color_row(rgb_section, "RgbGeneratorEndColor", "End")
	# RGB-gen colors are RGB-only: the .3di leaves their alpha byte 0 (unused), so hide the
	# alpha control and show them opaque (display alpha forced in _sync_generator_controls).
	(controls["rgb_start_color"] as ColorPickerButton).edit_alpha = false
	(controls["rgb_end_color"] as ColorPickerButton).edit_alpha = false

	var alpha_section := VBoxContainer.new()
	alpha_section.name = "AlphaGeneratorSection"
	alpha_section.add_theme_constant_override("separation", 4)
	box.add_child(alpha_section)
	var alpha_header := Label.new()
	alpha_header.text = "Alpha gen"
	alpha_section.add_child(alpha_header)
	controls["alpha_box"] = alpha_section
	controls["alpha_style_option"] = _add_id_option_row(alpha_section, "AlphaGeneratorStyleOption", "Style", GeneratorStyleCatalog.options_for_consumer(GeneratorStyleCatalog.CONSUMER_ALPHA))
	controls["alpha_phase"] = _add_spin_row(alpha_section, "AlphaGeneratorPhase", "Phase", -100000, 100000, 0.01)
	controls["alpha_reg_picker"] = _add_ctrl_reg_row(alpha_section, "AlphaGeneratorControlReg", "Control reg")
	controls["alpha_rate"] = _add_spin_row(alpha_section, "AlphaGeneratorRate", "Rate", -100000, 100000, 0.01)
	controls["alpha_start"] = _add_spin_row(alpha_section, "AlphaGeneratorStart", "Start", -32768, 32767, 1)
	controls["alpha_end"] = _add_spin_row(alpha_section, "AlphaGeneratorEnd", "End", -32768, 32767, 1)

	var anim_section := VBoxContainer.new()
	anim_section.name = "TextureAnimationSection"
	anim_section.add_theme_constant_override("separation", 4)
	box.add_child(anim_section)
	var anim_header := Label.new()
	anim_header.text = "Texture animation"
	anim_section.add_child(anim_header)
	controls["anim_box"] = anim_section
	controls["anim_frames"] = _add_spin_row(anim_section, "TextureAnimationFrames", "Frames", 0, 255, 1)
	controls["anim_type"] = _add_spin_row(anim_section, "TextureAnimationType", "Type", 0, 255, 1)
	controls["anim_time"] = _add_spin_row(anim_section, "TextureAnimationTime", "Frame time / reg", -32768, 32767, 1)
	return controls


func _sync_generator_controls(controls: Dictionary, material: Dictionary, shader_info: Dictionary) -> void:
	var u_params: Dictionary = material.get("u_params", {})
	var v_params: Dictionary = material.get("v_params", {})
	_sync_uv_axis_controls(controls.get("u", {}), u_params)
	_sync_uv_axis_controls(controls.get("v", {}), v_params)

	var uses_uv := bool(shader_info.get("uses_uv_generators", false))
	var has_uv_data := int(u_params.get("style", 0)) > 0 or int(v_params.get("style", 0)) > 0
	_set_section_visible_enabled(controls.get("u", {}).get("box"), uses_uv or has_uv_data, uses_uv)
	_set_section_visible_enabled(controls.get("v", {}).get("box"), uses_uv or has_uv_data, uses_uv)

	var rgb_gen: Dictionary = material.get("rgb_gen", {})
	var rgb_style := int(rgb_gen.get("style", 0))
	_populate_id_option(controls.get("rgb_style_option") as OptionButton, GeneratorStyleCatalog.options_for_consumer(GeneratorStyleCatalog.CONSUMER_RGB), rgb_style)
	_set_spin(controls.get("rgb_phase"), float(rgb_gen.get("phase", 0.0)))
	var rgb_reg_picker = controls.get("rgb_reg_picker")
	if rgb_reg_picker != null:
		rgb_reg_picker.setup(_control_registers(), int(rgb_gen.get("reg", -1)))
	_set_spin(controls.get("rgb_rate"), float(rgb_gen.get("rate", 0.0)))
	var rgb_start := controls.get("rgb_start_color") as ColorPickerButton
	var rgb_end := controls.get("rgb_end_color") as ColorPickerButton
	if rgb_start != null:
		var start_rgb: Color = rgb_gen.get("start_color", Color.WHITE)
		rgb_start.color = Color(start_rgb.r, start_rgb.g, start_rgb.b, 1.0)
	if rgb_end != null:
		var end_rgb: Color = rgb_gen.get("end_color", Color.WHITE)
		rgb_end.color = Color(end_rgb.r, end_rgb.g, end_rgb.b, 1.0)
	_apply_generator_row_visibility(rgb_style, controls.get("rgb_phase"), [
		controls.get("rgb_rate"),
		controls.get("rgb_start_color"), controls.get("rgb_end_color"),
	], rgb_reg_picker)
	var rgb_enabled := bool(shader_info.get("is_luminance", false))
	var rgb_visible := rgb_enabled or rgb_style > 0
	_set_section_visible_enabled(controls.get("rgb_box"), rgb_visible, rgb_enabled)

	var alpha_gen: Dictionary = material.get("alpha_gen", {})
	var alpha_style := int(alpha_gen.get("style", 0))
	_populate_id_option(controls.get("alpha_style_option") as OptionButton, GeneratorStyleCatalog.options_for_consumer(GeneratorStyleCatalog.CONSUMER_ALPHA), alpha_style)
	_set_spin(controls.get("alpha_phase"), float(alpha_gen.get("phase", 0.0)))
	var alpha_reg_picker = controls.get("alpha_reg_picker")
	if alpha_reg_picker != null:
		alpha_reg_picker.setup(_control_registers(), int(alpha_gen.get("reg", -1)))
	_set_spin(controls.get("alpha_rate"), float(alpha_gen.get("rate", 0.0)))
	_set_spin(controls.get("alpha_start"), int(alpha_gen.get("start", 0)))
	_set_spin(controls.get("alpha_end"), int(alpha_gen.get("end", 0)))
	_apply_generator_row_visibility(alpha_style, controls.get("alpha_phase"), [
		controls.get("alpha_rate"),
		controls.get("alpha_start"), controls.get("alpha_end"),
	], alpha_reg_picker)
	var alpha_enabled := bool(shader_info.get("is_alpha", false))
	var alpha_visible := alpha_enabled or alpha_style > 0
	_set_section_visible_enabled(controls.get("alpha_box"), alpha_visible, alpha_enabled)

	var animation: Dictionary = material.get("animation", {})
	_set_spin(controls.get("anim_frames"), int(animation.get("num_frames", 0)))
	_set_spin(controls.get("anim_type"), int(animation.get("animation_type", 0)))
	_set_spin(controls.get("anim_time"), int(animation.get("cycle_frame_time", 0)))
	var anim_enabled := bool(shader_info.get("has_diffuse", false))
	var anim_visible := anim_enabled or int(animation.get("num_frames", 0)) > 0
	_set_section_visible_enabled(controls.get("anim_box"), anim_visible, anim_enabled)


func _sync_uv_axis_controls(axis_controls: Dictionary, params: Dictionary) -> void:
	var style := int(params.get("style", 0))
	_populate_id_option(axis_controls.get("style_option") as OptionButton, GeneratorStyleCatalog.options_for_consumer(GeneratorStyleCatalog.CONSUMER_UV), style)
	_set_spin(axis_controls.get("phase"), float(params.get("phase", 0.0)))
	var reg_picker = axis_controls.get("reg_picker")
	if reg_picker != null:
		reg_picker.setup(_control_registers(), int(params.get("reg", -1)))
	_set_spin(axis_controls.get("rate"), float(params.get("rate", 0.0)))
	_set_spin(axis_controls.get("start"), float(params.get("start", 0.0)))
	_set_spin(axis_controls.get("end"), float(params.get("end", 0.0)))
	_apply_generator_row_visibility(style, axis_controls.get("phase"), [
		axis_controls.get("rate"),
		axis_controls.get("start"), axis_controls.get("end"),
	], reg_picker)


func _set_section_visible_enabled(node, visible: bool, enabled: bool) -> void:
	var control := node as Control
	if control == null:
		return
	control.visible = visible
	_set_controls_enabled(control, enabled)


func _set_controls_enabled(node: Node, enabled: bool) -> void:
	if node is SpinBox:
		(node as SpinBox).editable = enabled
	elif node is LineEdit:
		(node as LineEdit).editable = enabled
	elif node is Button:
		(node as Button).disabled = not enabled
	for child in node.get_children():
		_set_controls_enabled(child, enabled)


func _reg_value(picker) -> int:
	if picker != null and picker.has_method("get_selected_register"):
		return int(picker.get_selected_register())
	return -1


func _set_row_visible(control, visible: bool) -> void:
	if control == null:
		return
	var row := (control as Node).get_parent() as Control
	if row != null:
		row.visible = visible


# Every style > 0x70 stores a model-local CTRL reference in the packed parameter
# byte and is rewritten to a global ordinal by the loader. A consumer may later
# use that ordinal as waveform phase rather than read the CTRL value, but the
# authored field is still the register picker; exposing `phase` there would be a
# no-op. [orig: loader fixup sub_5B4640 @ 0x5B4640]
func _apply_generator_row_visibility(style: int, phase_control, param_controls: Array, reg_picker) -> void:
	var active := style != 0
	var parameter_is_reference := GeneratorStyleCatalog.parameter_is_ctrl_reference(style)
	_set_row_visible(phase_control, active and not parameter_is_reference)
	for control in param_controls:
		_set_row_visible(control, active)
	_set_row_visible(reg_picker, active and parameter_is_reference)


func _connect_uv_generator_spin(spin: SpinBox, axis: String, key: String, controls: Dictionary, apply: Callable) -> void:
	spin.value_changed.connect(func(value: float) -> void:
		var params := _uv_generator_params(controls, axis)
		if key == "style" or key == "reg":
			params[key] = int(value)
		else:
			params[key] = value
		apply.call(axis, params)
	)


func _connect_rgb_generator_spin(spin: SpinBox, key: String, controls: Dictionary, apply: Callable) -> void:
	spin.value_changed.connect(func(value: float) -> void:
		var params := _rgb_generator_params(controls)
		if key == "style" or key == "reg":
			params[key] = int(value)
		else:
			params[key] = value
		apply.call(params)
	)


func _connect_alpha_generator_spin(spin: SpinBox, key: String, controls: Dictionary, apply: Callable) -> void:
	spin.value_changed.connect(func(value: float) -> void:
		var params := _alpha_generator_params(controls)
		if key == "phase" or key == "rate":
			params[key] = value
		else:
			params[key] = int(value)
		apply.call(params)
	)


func _connect_texture_animation_spin(spin: SpinBox, key: String, controls: Dictionary, apply: Callable) -> void:
	spin.value_changed.connect(func(value: float) -> void:
		var params := _texture_animation_params(controls)
		match key:
			"frames":
				params["num_frames"] = int(value)
			"type":
				params["animation_type"] = int(value)
			"time":
				params["cycle_frame_time"] = int(value)
		apply.call(params)
	)


func _uv_generator_params(controls: Dictionary, axis: String) -> Dictionary:
	var axis_controls: Dictionary = controls.get(axis, {})
	return {
		"style": _selected_option_id(axis_controls.get("style_option") as OptionButton),
		"phase": float((axis_controls.get("phase") as SpinBox).value),
		"reg": _reg_value(axis_controls.get("reg_picker")),
		"rate": float((axis_controls.get("rate") as SpinBox).value),
		"start": float((axis_controls.get("start") as SpinBox).value),
		"end": float((axis_controls.get("end") as SpinBox).value),
	}


func _rgb_generator_params(controls: Dictionary) -> Dictionary:
	return {
		"style": _selected_option_id(controls.get("rgb_style_option") as OptionButton),
		"phase": float((controls.get("rgb_phase") as SpinBox).value),
		"reg": _reg_value(controls.get("rgb_reg_picker")),
		"rate": float((controls.get("rgb_rate") as SpinBox).value),
		"start_color": _rgb_gen_color(controls.get("rgb_start_color")),
		"end_color": _rgb_gen_color(controls.get("rgb_end_color")),
	}


func _rgb_gen_color(picker) -> Color:
	# RGB-gen colors are RGB-only; submit alpha 0 to keep the .3di byte WriteMTRL writes
	# (the picker displays them opaque, but the stored alpha byte stays the canonical 0).
	var color := (picker as ColorPickerButton).color
	return Color(color.r, color.g, color.b, 0.0)


func _alpha_generator_params(controls: Dictionary) -> Dictionary:
	return {
		"style": _selected_option_id(controls.get("alpha_style_option") as OptionButton),
		"phase": float((controls.get("alpha_phase") as SpinBox).value),
		"reg": _reg_value(controls.get("alpha_reg_picker")),
		"rate": float((controls.get("alpha_rate") as SpinBox).value),
		"start": int((controls.get("alpha_start") as SpinBox).value),
		"end": int((controls.get("alpha_end") as SpinBox).value),
	}


func _texture_animation_params(controls: Dictionary) -> Dictionary:
	return {
		"num_frames": int((controls.get("anim_frames") as SpinBox).value),
		"animation_type": int((controls.get("anim_type") as SpinBox).value),
		"cycle_frame_time": int((controls.get("anim_time") as SpinBox).value),
	}


func _object_folder_filename(path: String) -> String:
	if object_editor == null or object_editor.object_data == null:
		return ""
	var source_dir := object_editor.object_data.get_source_dir()
	if source_dir.is_empty() or path.is_empty():
		return ""
	var source := ProjectSettings.globalize_path(source_dir).replace("\\", "/").trim_suffix("/")
	var selected := ProjectSettings.globalize_path(path).replace("\\", "/")
	var selected_lower := selected.to_lower()
	var source_lower := source.to_lower()
	if not selected_lower.begins_with(source_lower + "/"):
		return ""
	return selected.get_file()
