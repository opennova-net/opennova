extends "res://modtools/object/ui/inspectors/workflow_inspector.gd"

## LODs workflow: bind ASE scenes to LODs and edit per-LOD threshold,
## attributes, and render function plus the project poly-collision LOD.


func build_main(host: Control) -> void:
	var box := _make_inspector_box(host)
	var data: NovaObjectData = object_editor.object_data if object_editor else null
	var summary: Dictionary = data.get_summary() if data != null else {}
	var lods: Array = data.get_project_lods() if data != null and data.has_method("get_project_lods") else []
	var selected := {"index": 0 if not lods.is_empty() else -1}
	var syncing := {"value": false}

	var list := ItemList.new()
	list.name = "ObjectLodsList"
	list.custom_minimum_size = Vector2(0, 150)
	list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	for lod in lods:
		var info := lod as Dictionary
		list.add_item("LOD %d  %s" % [int(info.get("index", 0)), String(info.get("scene_file", ""))])
	box.add_child(list)

	var action_row := HBoxContainer.new()
	action_row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(action_row)
	var add_button := Button.new()
	add_button.name = "ObjectAddLodSceneButton"
	add_button.text = "Add ASE"
	action_row.add_child(add_button)
	var replace_button := Button.new()
	replace_button.name = "ObjectReplaceLodSceneButton"
	replace_button.text = "Replace ASE"
	replace_button.disabled = selected["index"] < 0
	action_row.add_child(replace_button)

	var scene_dialog := FileDialog.new()
	scene_dialog.name = "ObjectLodSceneFileDialog"
	scene_dialog.access = FileDialog.ACCESS_FILESYSTEM
	scene_dialog.file_mode = FileDialog.FILE_MODE_OPEN_FILE
	scene_dialog.filters = PackedStringArray(["*.ase,*.ASE ; ASE scene"])
	box.add_child(scene_dialog)

	var poly_lod := _add_spin_row(box, "ObjectPolyCollisionLod", "Collision LOD", 0, 7, 1)
	_set_spin(poly_lod, int(summary.get("poly_collision_lod", 0)))
	var threshold := _add_spin_row(box, "ObjectLodThreshold", "Threshold", 0, 100000, 0.1)
	var attributes := _add_spin_row(box, "ObjectLodAttributes", "Attributes", 0, 255, 1)
	var render_row := HBoxContainer.new()
	render_row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(render_row)
	var render_label := Label.new()
	render_label.text = "Render fn"
	render_label.custom_minimum_size = Vector2(90, 0)
	render_row.add_child(render_label)
	var render_function := LineEdit.new()
	render_function.name = "ObjectLodRenderFunction"
	render_function.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	render_row.add_child(render_function)

	var set_lod_controls_enabled := func(enabled: bool) -> void:
		threshold.editable = enabled
		attributes.editable = enabled
		render_function.editable = enabled
		replace_button.disabled = not enabled

	var sync := func(index: int) -> void:
		syncing["value"] = true
		selected["index"] = index
		if index >= 0 and index < lods.size():
			var info := lods[index] as Dictionary
			_set_spin(threshold, float(info.get("threshold", 0.0)))
			_set_spin(attributes, int(info.get("attributes", 0)))
			render_function.text = String(info.get("render_function", ""))
			set_lod_controls_enabled.call(true)
		else:
			_set_spin(threshold, 0)
			_set_spin(attributes, 0)
			render_function.text = ""
			set_lod_controls_enabled.call(false)
		syncing["value"] = false

	list.item_selected.connect(func(index: int) -> void:
		sync.call(index)
	)
	threshold.value_changed.connect(func(value: float) -> void:
		if not syncing["value"] and data != null and selected["index"] >= 0:
			data.set_lod_field(selected["index"], "threshold", value)
	)
	attributes.value_changed.connect(func(value: float) -> void:
		if not syncing["value"] and data != null and selected["index"] >= 0:
			data.set_lod_field(selected["index"], "attributes", int(value))
	)
	render_function.text_submitted.connect(func(value: String) -> void:
		if not syncing["value"] and data != null and selected["index"] >= 0:
			data.set_lod_field(selected["index"], "render_function", value)
	)
	poly_lod.value_changed.connect(func(value: float) -> void:
		if not syncing["value"] and data != null:
			data.set_project_field("poly_collision_lod", int(value))
	)

	var pending_replace := {"value": false}
	add_button.pressed.connect(func() -> void:
		pending_replace["value"] = false
		scene_dialog.popup_centered(Vector2i(760, 520))
	)
	replace_button.pressed.connect(func() -> void:
		if selected["index"] < 0:
			return
		pending_replace["value"] = true
		scene_dialog.popup_centered(Vector2i(760, 520))
	)
	scene_dialog.file_selected.connect(func(path: String) -> void:
		var lod_index: int = int(selected["index"]) if bool(pending_replace["value"]) else -1
		if _ws.add_lod_scene(path, lod_index) == OK:
			var old_children := host.get_children()
			for child in old_children:
				host.remove_child(child)
				child.queue_free()
			build_main(host)
	)

	if not lods.is_empty():
		list.select(0)
		sync.call(0)
	else:
		sync.call(-1)
