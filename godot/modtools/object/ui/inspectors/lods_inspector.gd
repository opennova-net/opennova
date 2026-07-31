extends ObjectInspector

## LODs workflow: bind ASE scenes to LODs and edit per-LOD threshold,
## attributes, and render function plus the project poly-collision LOD.


func build_main(mount: Control) -> void:
	var box := _make_inspector_box(mount)
	var data: NovaObjectData = object_editor.object_data if object_editor else null
	var summary: Dictionary = data.get_summary() if data != null else {}
	var lods: Array = data.get_project_lods() if data != null else []
	var selected := {"index": 0 if not lods.is_empty() else -1}
	var binder := FieldBinder.new()

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

	var set_field := func(key: String, value: Variant) -> void:
		if data != null and int(selected["index"]) >= 0:
			data.set_lod_field(int(selected["index"]), key, value)

	binder.bind_spin(threshold,
		func(info): return float(info.get("threshold", 0.0)),
		func(value): set_field.call("threshold", value))
	binder.bind_spin(attributes,
		func(info): return float(info.get("attributes", 0)),
		func(value): set_field.call("attributes", int(value)))
	binder.bind_line(render_function,
		func(info): return String(info.get("render_function", "")),
		func(value): set_field.call("render_function", value))

	var set_lod_controls_enabled := func(enabled: bool) -> void:
		threshold.editable = enabled
		attributes.editable = enabled
		render_function.editable = enabled
		replace_button.disabled = not enabled

	var sync := func(index: int) -> void:
		selected["index"] = index
		var valid := index >= 0 and index < lods.size()
		var info: Dictionary = (lods[index] as Dictionary) if valid else {}
		binder.sync_from(info)
		set_lod_controls_enabled.call(valid)

	list.item_selected.connect(func(index: int) -> void:
		sync.call(index)
	)
	poly_lod.value_changed.connect(func(value: float) -> void:
		if data != null:
			data.set_project_field("poly_collision_lod", int(value))
	)

	# One shared picker; the replace-vs-add mode rides the one-shot callback's
	# capture instead of a pending flag. Parented to `box` so a rebuild frees it.
	var files := FileDialogHelper.new(box)
	var pick_scene := func(replace_index: int) -> void:
		files.open("Choose an ASE scene", PackedStringArray(["*.ase,*.ASE ; ASE scene"]),
			func(path: String) -> void:
				if _ws.add_lod_scene(path, replace_index) == OK:
					var old_children := mount.get_children()
					for child in old_children:
						mount.remove_child(child)
						child.queue_free()
					build_main(mount))
	add_button.pressed.connect(func() -> void:
		pick_scene.call(-1)
	)
	replace_button.pressed.connect(func() -> void:
		if selected["index"] < 0:
			return
		pick_scene.call(int(selected["index"]))
	)

	if not lods.is_empty():
		list.select(0)
		sync.call(0)
	else:
		sync.call(-1)
