extends WorkflowInspector

## Preview workflow: object summary, playback controls, the export-chunk mask,
## and per-control-register sliders. The export mask itself is coordinator
## state (used by begin_export); this inspector only builds its checkboxes.

const CollisionHull = preload("res://engine/object/collision_hull.gd")


func build_main(host: Control) -> void:
	var box := _make_inspector_box(host)
	var summary := object_editor.object_data.get_summary() if object_editor and object_editor.object_data else {}
	for key in ["source_kind", "lod_count", "material_count", "light_count", "userpoint_count"]:
		var row := Label.new()
		row.text = "%s: %s" % [String(key).capitalize(), str(summary.get(key, ""))]
		row.clip_text = true
		box.add_child(row)

	var playback := HBoxContainer.new()
	playback.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(playback)

	var play := Button.new()
	play.name = "PreviewPlayButton"
	play.toggle_mode = true
	play.button_pressed = _preview == null or _preview.is_playing()
	play.text = "Pause" if play.button_pressed else "Play"
	playback.add_child(play)

	var reset := Button.new()
	reset.name = "PreviewResetButton"
	reset.text = "Reset"
	playback.add_child(reset)

	var wire := CheckBox.new()
	wire.name = "PreviewWireCheck"
	wire.text = "Wire"
	wire.button_pressed = _preview != null and _preview.is_wireframe()
	playback.add_child(wire)

	var collision := CheckBox.new()
	collision.name = "PreviewCollisionCheck"
	collision.text = "Collision"
	collision.button_pressed = _preview != null and _preview.is_collision_visible()
	# Only meaningful when the model carries collision volumes.
	collision.disabled = _preview == null or not _preview.has_collision()
	playback.add_child(collision)

	play.toggled.connect(func(pressed: bool) -> void:
		if _preview != null:
			_preview.set_playing(pressed)
		play.text = "Pause" if pressed else "Play"
	)
	reset.pressed.connect(func() -> void:
		if _preview != null:
			_preview.reset_animation_time()
	)
	wire.toggled.connect(func(pressed: bool) -> void:
		if _preview != null:
			_preview.set_wireframe(pressed)
	)
	collision.toggled.connect(func(pressed: bool) -> void:
		if _preview != null:
			_preview.set_collision_visible(pressed)
	)

	_build_animation_controls(box)
	_build_collision_legend(box)
	_build_export_mask_controls(box)

	var ctrl_regs: Array = object_editor.object_data.get_control_registers() if object_editor and object_editor.object_data and object_editor.object_data.has_method("get_control_registers") else []
	if not ctrl_regs.is_empty() and _preview != null:
		var ctrl_label := Label.new()
		ctrl_label.text = "Control registers"
		box.add_child(ctrl_label)
		for reg in ctrl_regs:
			var reg_name := String((reg as Dictionary).get("name", ""))
			if reg_name.is_empty():
				continue
			var row := HBoxContainer.new()
			row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			box.add_child(row)
			var label := Label.new()
			label.text = reg_name
			label.custom_minimum_size = Vector2(90, 0)
			row.add_child(label)
			var slider := HSlider.new()
			slider.name = "ControlRegisterSlider_%s" % reg_name.replace(" ", "_")
			slider.min_value = 0
			slider.max_value = ObjectEditorWorkspace.U16_VALUE_MAX
			slider.step = 1
			slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			row.add_child(slider)
			slider.value_changed.connect(func(value: float, name: String = reg_name) -> void:
				if _preview != null:
					_preview.set_ctrl_value(name, int(value))
			)


# Skeletal animation (.bad/.adm) preview -- the smoke test for the runtime skeletal
# system. Pick a .adm (defaults to the model's basename), Load to bind it to the model
# (builds the Skeleton3D + Skin when the model is skinned), then choose a clip to play.
# Reads the .adm + its .bad clips from the mounted resource root by name (the VFS path),
# so it works whether assets are loose or in PFF archives.
func _build_animation_controls(box: VBoxContainer) -> void:
	if _preview == null:
		return
	var label := Label.new()
	label.text = "Skeletal animation (.adm)"
	box.add_child(label)

	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(row)

	var name_edit := LineEdit.new()
	name_edit.name = "AdmNameEdit"
	name_edit.placeholder_text = "model.adm"
	name_edit.text = _default_adm_name()
	name_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(name_edit)

	var load_button := Button.new()
	load_button.name = "AdmLoadButton"
	load_button.text = "Load"
	row.add_child(load_button)

	var clip_picker := OptionButton.new()
	clip_picker.name = "AdmClipPicker"
	clip_picker.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	clip_picker.disabled = true
	box.add_child(clip_picker)

	var status := Label.new()
	status.name = "AdmStatusLabel"
	status.theme_type_variation = &"Muted"
	status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	box.add_child(status)

	load_button.pressed.connect(func() -> void:
		if _preview == null:
			return
		var root: Variant = _ws.get_resource_root() if _ws != null and _ws.has_method("get_resource_root") else null
		var keys: PackedStringArray = _preview.load_animation_set(name_edit.text.strip_edges(), root)
		clip_picker.clear()
		if keys.is_empty():
			clip_picker.disabled = true
			status.text = "No animations loaded: %s" % _preview.get_animation_error()
			return
		for k in keys:
			clip_picker.add_item(k)
		clip_picker.disabled = false
		var skinned: bool = _preview.has_skeleton()
		status.text = "%d clip(s) loaded%s" % [keys.size(), "" if skinned else "  (model has no skin to pose)"]
		clip_picker.select(0)
		_preview.play_animation(clip_picker.get_item_text(0))
	)

	clip_picker.item_selected.connect(func(index: int) -> void:
		if _preview != null:
			_preview.play_animation(clip_picker.get_item_text(index))
	)


# Best-guess .adm name for the loaded model: its basename + ".adm" (the convention an
# item .def follows -- graphic "US01" / anim_def "US01"). Blank for an unsaved model.
func _default_adm_name() -> String:
	if object_editor == null or object_editor.object_data == null:
		return ""
	var name := String(object_editor.object_data.get_object_name()).get_file().get_basename().strip_edges()
	return "" if name.is_empty() or name == "untitled" else name + ".adm"


# Color key for the collision overlay: one swatch + label per distinct collidable
# type present in the loaded model, matching CollisionHull.color_for_type. Helps the
# artist read which colored hull is which type when validating.
func _build_collision_legend(box: VBoxContainer) -> void:
	if object_editor == null or object_editor.object_data == null:
		return
	if not object_editor.object_data.has_method("get_collision_volumes"):
		return
	var volumes: Array = object_editor.object_data.get_collision_volumes()
	if volumes.is_empty():
		return
	var seen: Dictionary = {}
	for v in volumes:
		seen[int((v as Dictionary).get("type", 0))] = true
	var types := seen.keys()
	types.sort()
	var label := Label.new()
	label.text = "Collision types"
	box.add_child(label)
	for t in types:
		var row := HBoxContainer.new()
		box.add_child(row)
		var swatch := ColorRect.new()
		swatch.color = CollisionHull.color_for_type(t)
		swatch.custom_minimum_size = Vector2(16, 16)
		row.add_child(swatch)
		var name := CollisionHull.name_for_type(t)
		var caption := Label.new()
		caption.text = "Type %d (%s?)" % [t, name] if name != "" else "Type %d" % t
		row.add_child(caption)


func _build_export_mask_controls(box: VBoxContainer) -> void:
	if object_editor == null or object_editor.object_data == null or not object_editor.object_data.can_export_3di():
		return
	_ws._sync_export_update_mask_from_dirty()
	var label := Label.new()
	label.text = "Export chunks"
	box.add_child(label)

	var row := HBoxContainer.new()
	row.name = "ObjectExportMaskControls"
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(row)

	_add_export_mask_check(row, "ObjectExportMtrlCheck", "MTRL", ObjectEditorWorkspace.OED_UPDATE_MTRL)
	_add_export_mask_check(row, "ObjectExportLghtCheck", "LGHT", ObjectEditorWorkspace.OED_UPDATE_LGHT)
	_add_export_mask_check(row, "ObjectExportPanmCheck", "PANM", ObjectEditorWorkspace.OED_UPDATE_PANM)


func _add_export_mask_check(parent: HBoxContainer, node_name: String, label: String, bit: int) -> CheckBox:
	var check := CheckBox.new()
	check.name = node_name
	check.text = label
	check.button_pressed = (_ws._export_update_mask & bit) != 0
	parent.add_child(check)
	check.toggled.connect(func(pressed: bool) -> void:
		if pressed:
			_ws._export_update_mask |= bit
		else:
			_ws._export_update_mask &= ~bit
	)
	return check
