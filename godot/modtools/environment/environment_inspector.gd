class_name EnvironmentInspector
extends ScrollContainer

# TOD controls edit the keyframe records parsed by TimeOfDay_ParseProperty @ 0x57c590
# and consumed by Environment_ComputeTimeOfDayColors @ 0x57de40; EnvFile/libs/env
# owns the actual ported behavior.

# Engine-side field -> consumption status (honored/partial/unconsumed) used to
# badge rows whose edits don't reach the picture yet; statuses mirror
# docs/env/env-honored-matrix.md and flip only with grill citations.
const BADGE_PARTIAL := "◐"
const BADGE_UNCONSUMED := "○"

var _editor
var _syncing := false
var _selected_keyframe := 0
var _consumption: Dictionary = {}

var _name_edit: LineEdit
var _time_slider: HSlider
var _time_spin: SpinBox
var _fog_level: SpinBox
var _fog_type: SpinBox
var _terrain_tint: ColorPickerButton
var _water_color: ColorPickerButton
var _cloud_tint: ColorPickerButton
var _envscale: SpinBox
var _sky_speed: SpinBox
var _sky_height: SpinBox
var _sun_model: ResourceRefWidget
var _moon_model: ResourceRefWidget
var _glare_model: ResourceRefWidget
var _star_model: ResourceRefWidget
var _sky_map1: TextureRefWidget
var _sky_map2: TextureRefWidget
var _water_murk: SpinBox
var _vertex_tint: ColorPickerButton
var _lightning_color: ColorPickerButton
var _ceiling_color: ColorPickerButton
var _floor_color: ColorPickerButton
var _iris_percent: SpinBox
var _iris_center: SpinBox
# Link-widget services (resolve/pick/jump from the shell); they arrive after
# the workspace builds this inspector, so the setter re-configures live widgets.
var _ref_services: Dictionary = {}
var _keyframe_list: ItemList
var _selected_time: SpinBox
var _color_buttons: Dictionary = {}


func _ready() -> void:
	size_flags_horizontal = Control.SIZE_EXPAND_FILL
	size_flags_vertical = Control.SIZE_EXPAND_FILL
	_build_ui()
	if _editor:
		_connect_editor()
		sync_from_editor()


func set_environment_editor(value) -> void:
	if _editor and _editor.state_changed.is_connected(sync_from_editor):
		_editor.state_changed.disconnect(sync_from_editor)
	_editor = value
	if is_node_ready():
		_connect_editor()
		sync_from_editor()


func _connect_editor() -> void:
	if _editor and not _editor.state_changed.is_connected(sync_from_editor):
		_editor.state_changed.connect(sync_from_editor)


func _build_ui() -> void:
	_consumption = EnvFile.get_field_consumption()
	var box := VBoxContainer.new()
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.size_flags_vertical = Control.SIZE_EXPAND_FILL
	box.add_theme_constant_override("separation", 10)
	add_child(box)

	var name_label := Label.new()
	name_label.text = "Environment"
	name_label.theme_type_variation = &"Heading"
	box.add_child(name_label)

	_name_edit = LineEdit.new()
	_name_edit.placeholder_text = "Name"
	_name_edit.text_changed.connect(_on_name_changed)
	_name_edit.focus_exited.connect(_commit_edit)
	_name_edit.text_submitted.connect(func(_t): _commit_edit())
	box.add_child(_name_edit)

	box.add_child(_make_separator())
	box.add_child(_make_heading("Time of Day"))
	var time_row := HBoxContainer.new()
	time_row.add_theme_constant_override("separation", 8)
	box.add_child(time_row)
	_time_slider = HSlider.new()
	_time_slider.min_value = 0
	_time_slider.max_value = 2359
	_time_slider.step = 1
	_time_slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_time_slider.value_changed.connect(_on_time_changed)
	_time_slider.drag_ended.connect(func(_c): _commit_edit())
	time_row.add_child(_time_slider)
	_time_spin = SpinBox.new()
	_time_spin.min_value = 0
	_time_spin.max_value = 2359
	_time_spin.step = 1
	_time_spin.custom_minimum_size = Vector2(84, 0)
	_time_spin.value_changed.connect(_on_time_changed)
	time_row.add_child(_time_spin)

	box.add_child(_make_separator())
	box.add_child(_make_heading("Atmosphere"))
	var atmosphere := GridContainer.new()
	atmosphere.columns = 2
	atmosphere.add_theme_constant_override("h_separation", 8)
	atmosphere.add_theme_constant_override("v_separation", 8)
	box.add_child(atmosphere)
	_envscale = _add_spin(atmosphere, "Env scale", 0.1, 4.0, 0.01, _on_envscale_changed)
	_fog_level = _add_spin(atmosphere, "Fog level", 0, 4096, 1, _on_fog_level_changed)
	_fog_type = _add_spin(atmosphere, "Fog type", 0, 3, 1, _on_fog_type_changed)
	_sky_speed = _add_spin(atmosphere, "Sky speed", 0, 100, 1, _on_sky_speed_changed)
	_sky_height = _add_spin(atmosphere, "Sky height", 10, 500, 1, _on_sky_height_changed)
	_terrain_tint = _add_color(atmosphere, "Terrain", _on_terrain_tint_changed, "terrain_tint")
	_water_color = _add_color(atmosphere, "Water", _on_water_color_changed, "water_color")
	_cloud_tint = _add_color(atmosphere, "Clouds", _on_cloud_tint_changed, "cloud_tint")

	box.add_child(_make_separator())
	box.add_child(_make_heading("Sky"))
	var sky := GridContainer.new()
	sky.columns = 2
	sky.add_theme_constant_override("h_separation", 8)
	sky.add_theme_constant_override("v_separation", 8)
	box.add_child(sky)
	_sky_map1 = _add_sky_map(sky, "Cloud map 1", func(v: String) -> void: _editor.env_file.set_sky_map1(v))
	_sky_map2 = _add_sky_map(sky, "Cloud map 2", func(v: String) -> void: _editor.env_file.set_sky_map2(v))

	box.add_child(_make_separator())
	box.add_child(_make_heading("Sky Models"))
	var models := GridContainer.new()
	models.columns = 2
	models.add_theme_constant_override("h_separation", 8)
	models.add_theme_constant_override("v_separation", 8)
	box.add_child(models)
	_sun_model = _add_model_ref(models, "Sun", func(v: String) -> void: _editor.env_file.set_sun_3di(v))
	_moon_model = _add_model_ref(models, "Moon", func(v: String) -> void: _editor.env_file.set_moon_3di(v))
	_glare_model = _add_model_ref(models, "Glare", func(v: String) -> void: _editor.env_file.set_glare_3di(v), "glare_3di")
	_star_model = _add_model_ref(models, "Star", func(v: String) -> void: _editor.env_file.set_star_3di(v))

	box.add_child(_make_separator())
	box.add_child(_make_heading("Advanced"))
	var advanced := GridContainer.new()
	advanced.columns = 2
	advanced.add_theme_constant_override("h_separation", 8)
	advanced.add_theme_constant_override("v_separation", 8)
	box.add_child(advanced)
	# Murk caps at 0.99 like the engine clamp (original has only the <= 0.99 top).
	_water_murk = _add_spin(advanced, "Water murk", 0.0, 0.99, 0.01, _on_water_murk_changed, "water_murk")
	_lightning_color = _add_color(advanced, "Lightning", _on_lightning_color_changed, "lightning_color")
	_ceiling_color = _add_color(advanced, "Ceiling", _on_ceiling_color_changed, "ceiling_color")
	_floor_color = _add_color(advanced, "Floor", _on_floor_color_changed, "floor_color")
	_vertex_tint = _add_color(advanced, "Vertex tint", _on_vertex_tint_changed, "vertex_tint")
	_iris_percent = _add_spin(advanced, "Iris percent", 0.0, 100.0, 0.1, _on_iris_percent_changed, "iris_percent")
	_iris_center = _add_spin(advanced, "Iris center", 0.0, 5.0, 0.01, _on_iris_center_changed, "iris_center")

	box.add_child(_make_separator())
	box.add_child(_make_heading("TOD Keyframes"))
	_keyframe_list = ItemList.new()
	_keyframe_list.custom_minimum_size = Vector2(0, 136)
	_keyframe_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_keyframe_list.item_selected.connect(_on_keyframe_selected)
	box.add_child(_keyframe_list)

	var keyframe_buttons := HBoxContainer.new()
	keyframe_buttons.add_theme_constant_override("separation", 6)
	box.add_child(keyframe_buttons)
	_add_button(keyframe_buttons, "Add", _on_add_keyframe)
	_add_button(keyframe_buttons, "Duplicate", _on_duplicate_keyframe)
	_add_button(keyframe_buttons, "Remove", _on_remove_keyframe)

	var keyframe_grid := GridContainer.new()
	keyframe_grid.columns = 2
	keyframe_grid.add_theme_constant_override("h_separation", 8)
	keyframe_grid.add_theme_constant_override("v_separation", 8)
	box.add_child(keyframe_grid)
	_selected_time = _add_spin(keyframe_grid, "Time", 0, 2359, 1, _on_selected_time_changed)
	_add_keyframe_color(keyframe_grid, "sun_color", "Sun")
	_add_keyframe_color(keyframe_grid, "ground_color", "Ground")
	_add_keyframe_color(keyframe_grid, "fog_color", "Fog")
	_add_keyframe_color(keyframe_grid, "sky_color", "Sky")
	_add_keyframe_color(keyframe_grid, "skyfog_color", "Sky fog")
	_add_keyframe_color(keyframe_grid, "moon_color", "Moon")
	_add_keyframe_color(keyframe_grid, "skybase_color", "Sky base")
	_add_keyframe_color(keyframe_grid, "skybright_color", "Sky bright")
	_add_keyframe_color(keyframe_grid, "skyhighlight_color", "Sky highlight")
	_add_keyframe_color(keyframe_grid, "cloudbase_color", "Cloud base")
	_add_keyframe_color(keyframe_grid, "cloudhighlight_color", "Cloud highlight")
	_add_keyframe_color(keyframe_grid, "cloudedge_color", "Cloud edge")


func sync_from_editor() -> void:
	if _editor == null or _editor.env_file == null:
		return
	_syncing = true
	var env: EnvFile = _editor.env_file
	_name_edit.text = env.get_env_name()
	_time_slider.value = _editor.time_of_day
	_time_spin.value = _editor.time_of_day
	_envscale.value = env.get_envscale()
	_fog_level.value = env.get_fog_level()
	_fog_type.value = env.get_fog_type()
	_sky_speed.value = env.get_sky_speed()
	_sky_height.value = env.get_sky_height()
	_terrain_tint.color = env.get_terrain_tint()
	_water_color.color = env.get_water_color()
	_cloud_tint.color = env.get_cloud_tint()
	_water_murk.value = env.get_water_murk()
	_lightning_color.color = env.get_lightning_color()
	_ceiling_color.color = env.get_ceiling_color()
	_floor_color.color = env.get_floor_color()
	_vertex_tint.color = env.get_vertex_tint()
	_iris_percent.value = env.get_iris_percent()
	_iris_center.value = env.get_iris_center()
	_sun_model.set_value(env.get_sun_3di())
	_moon_model.set_value(env.get_moon_3di())
	_glare_model.set_value(env.get_glare_3di())
	_star_model.set_value(env.get_star_3di())
	# Push feed: preview exactly the textures the sky renders (resolved by the
	# same C++ path — resource root or the .env's own folder).
	_sky_map1.set_value(env.get_sky_map1())
	_sky_map1.set_preview_texture(env.get_sky_map1_tex())
	_sky_map2.set_value(env.get_sky_map2())
	_sky_map2.set_preview_texture(env.get_sky_map2_tex())
	_sync_keyframe_list()
	_sync_selected_keyframe()
	_syncing = false


func _sync_keyframe_list() -> void:
	_keyframe_list.clear()
	var keyframes := _keyframes()
	for i in keyframes.size():
		var keyframe: NovaEnvKeyframe = keyframes[i]
		_keyframe_list.add_item("%04d" % keyframe.get_time())
	if keyframes.is_empty():
		_selected_keyframe = -1
		return
	_selected_keyframe = clampi(_selected_keyframe, 0, keyframes.size() - 1)
	_keyframe_list.select(_selected_keyframe)


func _sync_selected_keyframe() -> void:
	var keyframe := _current_keyframe()
	var disabled := keyframe == null
	_selected_time.editable = not disabled
	for button in _color_buttons.values():
		button.disabled = disabled
	if disabled:
		return
	_selected_time.value = keyframe.get_time()
	for prop in _color_buttons:
		_color_buttons[prop].color = keyframe.get(prop)


func _add_spin(parent: Control, label_text: String, min_value: float, max_value: float, step: float, callback: Callable, field: String = "") -> SpinBox:
	_add_grid_label(parent, label_text, field)
	var spin := SpinBox.new()
	spin.min_value = min_value
	spin.max_value = max_value
	spin.step = step
	spin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	spin.value_changed.connect(callback)
	# Focus-out closes the editing burst so one drag/typed value = one undo step.
	spin.get_line_edit().focus_exited.connect(_commit_edit)
	parent.add_child(spin)
	return spin


func _add_color(parent: Control, label_text: String, callback: Callable, field: String = "") -> ColorPickerButton:
	_add_grid_label(parent, label_text, field)
	var button := ColorPickerButton.new()
	button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	button.color_changed.connect(callback)
	# Closing the picker popup commits the whole color edit as one undo step.
	button.popup_closed.connect(_commit_edit)
	parent.add_child(button)
	return button


# Row labels carry the consumption badge: fields whose edits don't reach the
# picture yet get a glyph (partial ◐ / unconsumed ○) and a plain-language
# tooltip, instead of silently accepting the edit.
func _add_grid_label(parent: Control, label_text: String, field: String = "") -> void:
	var label := Label.new()
	label.text = label_text
	label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	var info: Dictionary = _consumption.get(field, {})
	if not info.is_empty():
		var status := String(info.get("status", "honored"))
		if status == "partial":
			label.text += " " + BADGE_PARTIAL
		elif status == "unconsumed":
			label.text += " " + BADGE_UNCONSUMED
		var tip := String(info.get("note", ""))
		if not tip.is_empty():
			var anchor := String(info.get("anchor", ""))
			if not anchor.is_empty():
				tip += "\n[orig: %s]" % anchor
			label.tooltip_text = tip
			label.mouse_filter = Control.MOUSE_FILTER_PASS
	parent.add_child(label)


## A 3DI model reference row (badge + browse + jump into the Object workspace).
## Link-widget commits are discrete (Enter / focus-out / pick / clear), so each
## one is its own undo step — no begin/commit burst bookkeeping.
func _add_model_ref(parent: Control, label_text: String, commit: Callable, field: String = "") -> ResourceRefWidget:
	_add_grid_label(parent, label_text, field)
	var widget := ResourceRefWidget.new()
	widget.name = "EnvModel" + label_text
	widget.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	widget.configure("object_model", label_text, _ref_services)
	widget.value_changed.connect(func(value: String) -> void:
		if _syncing or _editor == null or _editor.env_file == null:
			return
		_editor.push_undo_step(func() -> void: commit.call(value)))
	parent.add_child(widget)
	return widget


## A cloud-map texture row with an always-on preview. The preview uses the push
## feed (sync_from_editor hands over the texture the sky actually renders).
func _add_sky_map(parent: Control, label_text: String, commit: Callable) -> TextureRefWidget:
	_add_grid_label(parent, label_text)
	var widget := TextureRefWidget.new()
	widget.name = "EnvSkyMap" + label_text.replace(" ", "")
	widget.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	widget.configure("texture", label_text, _ref_services)
	widget.value_changed.connect(func(value: String) -> void:
		if _syncing or _editor == null or _editor.env_file == null:
			return
		_editor.push_undo_step(func() -> void: commit.call(value)))
	parent.add_child(widget)
	return widget


## Wires the link widgets' resolve/pick/jump Callables (see
## ResourceRefWidget.services_from_shell). Idempotent; safe before or after
## the form is built.
func set_reference_services(services: Dictionary) -> void:
	_ref_services = services
	if _sun_model != null and is_instance_valid(_sun_model):
		_sun_model.configure("object_model", "Sun", services)
		_moon_model.configure("object_model", "Moon", services)
		_glare_model.configure("object_model", "Glare", services)
		_star_model.configure("object_model", "Star", services)
		_sky_map1.configure("texture", "Cloud map 1", services)
		_sky_map2.configure("texture", "Cloud map 2", services)


func _add_keyframe_color(parent: Control, property_name: String, label_text: String) -> void:
	var button := _add_color(parent, label_text, _on_keyframe_color_changed.bind(property_name))
	_color_buttons[property_name] = button


func _add_button(parent: Control, text: String, callback: Callable) -> void:
	var button := Button.new()
	button.text = text
	button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	button.pressed.connect(callback)
	parent.add_child(button)


func _make_heading(text: String) -> Label:
	var label := Label.new()
	label.text = text
	label.theme_type_variation = &"Heading"
	return label


func _make_separator() -> HSeparator:
	return HSeparator.new()


func _keyframes() -> Array:
	if _editor == null or _editor.env_file == null:
		return []
	return _editor.env_file.get_tod_keyframes()


func _current_keyframe() -> NovaEnvKeyframe:
	var keyframes := _keyframes()
	if _selected_keyframe < 0 or _selected_keyframe >= keyframes.size():
		return null
	return keyframes[_selected_keyframe]


func _mark_env_changed() -> void:
	if _editor and _editor.env_file:
		_editor.environment_changed.emit(_editor.env_file, _editor.time_of_day)
		_editor.state_changed.emit()


# Open the editing burst before mutating env_file; the commit triggers wired in
# _build_ui (focus-out / picker-close / slider drag-end) close it as one step.
func _begin_edit() -> void:
	if _editor and _editor.has_method("begin_edit"):
		_editor.begin_edit()


func _commit_edit() -> void:
	if _editor and _editor.has_method("commit_edit"):
		_editor.commit_edit()


func _on_name_changed(text: String) -> void:
	if _syncing or _editor == null:
		return
	_begin_edit()
	_editor.env_file.set_env_name(text)


func _on_time_changed(value: float) -> void:
	if _syncing or _editor == null:
		return
	_begin_edit()
	_editor.set_time_of_day(value)


func _on_envscale_changed(value: float) -> void:
	if not _syncing:
		_begin_edit()
		_editor.env_file.set_envscale(value)


func _on_fog_level_changed(value: float) -> void:
	if not _syncing:
		_begin_edit()
		_editor.env_file.set_fog_level(value)


func _on_fog_type_changed(value: float) -> void:
	if not _syncing:
		_begin_edit()
		_editor.env_file.set_fog_type(int(value))


func _on_sky_speed_changed(value: float) -> void:
	if not _syncing:
		_begin_edit()
		_editor.env_file.set_sky_speed(value)


func _on_sky_height_changed(value: float) -> void:
	if not _syncing:
		_begin_edit()
		_editor.env_file.set_sky_height(value)


func _on_terrain_tint_changed(color: Color) -> void:
	if not _syncing:
		_begin_edit()
		_editor.env_file.set_terrain_tint(color)


func _on_water_color_changed(color: Color) -> void:
	if not _syncing:
		_begin_edit()
		_editor.env_file.set_water_color(color)


func _on_cloud_tint_changed(color: Color) -> void:
	if not _syncing:
		_begin_edit()
		_editor.env_file.set_cloud_tint(color)


func _on_water_murk_changed(value: float) -> void:
	if not _syncing:
		_begin_edit()
		_editor.env_file.set_water_murk(value)


func _on_lightning_color_changed(color: Color) -> void:
	if not _syncing:
		_begin_edit()
		_editor.env_file.set_lightning_color(color)


func _on_ceiling_color_changed(color: Color) -> void:
	if not _syncing:
		_begin_edit()
		_editor.env_file.set_ceiling_color(color)


func _on_floor_color_changed(color: Color) -> void:
	if not _syncing:
		_begin_edit()
		_editor.env_file.set_floor_color(color)


func _on_vertex_tint_changed(color: Color) -> void:
	if not _syncing:
		_begin_edit()
		_editor.env_file.set_vertex_tint(color)


func _on_iris_percent_changed(value: float) -> void:
	if not _syncing:
		_begin_edit()
		_editor.env_file.set_iris_percent(value)


func _on_iris_center_changed(value: float) -> void:
	if not _syncing:
		_begin_edit()
		_editor.env_file.set_iris_center(value)


func _on_keyframe_selected(index: int) -> void:
	_selected_keyframe = index
	_syncing = true
	_sync_selected_keyframe()
	_syncing = false


func _on_selected_time_changed(value: float) -> void:
	if _syncing:
		return
	var keyframe := _current_keyframe()
	if keyframe:
		_begin_edit()
		keyframe.set_time(int(value))
		sync_from_editor()


func _on_keyframe_color_changed(color: Color, property_name: String) -> void:
	if _syncing:
		return
	var keyframe := _current_keyframe()
	if keyframe:
		_begin_edit()
		keyframe.set(property_name, color)
		_mark_env_changed()


func _on_add_keyframe() -> void:
	if _editor == null:
		return
	_editor.push_undo_step(func():
		var keyframes := _keyframes()
		var keyframe := NovaEnvKeyframe.new()
		keyframe.set_time(int(_editor.time_of_day))
		if not keyframes.is_empty():
			_copy_keyframe(keyframes[clampi(_selected_keyframe, 0, keyframes.size() - 1)], keyframe)
			keyframe.set_time(int(_editor.time_of_day))
		keyframes.append(keyframe)
		_selected_keyframe = keyframes.size() - 1
		_editor.env_file.set_tod_keyframes(keyframes))
	sync_from_editor()


func _on_duplicate_keyframe() -> void:
	var source := _current_keyframe()
	if source == null or _editor == null:
		return
	_editor.push_undo_step(func():
		var keyframes := _keyframes()
		var duplicate := NovaEnvKeyframe.new()
		_copy_keyframe(source, duplicate)
		duplicate.set_time(clampi(source.get_time() + 100, 0, 2359))
		keyframes.append(duplicate)
		_selected_keyframe = keyframes.size() - 1
		_editor.env_file.set_tod_keyframes(keyframes))
	sync_from_editor()


func _copy_keyframe(source: NovaEnvKeyframe, target: NovaEnvKeyframe) -> void:
	target.set_time(source.get_time())
	target.set_sun_color(source.get_sun_color())
	target.set_ground_color(source.get_ground_color())
	target.set_fog_color(source.get_fog_color())
	target.set_sky_color(source.get_sky_color())
	target.set_moon_color(source.get_moon_color())
	target.set_skyfog_color(source.get_skyfog_color())
	target.set_skybase_color(source.get_skybase_color())
	target.set_skybright_color(source.get_skybright_color())
	target.set_skyhighlight_color(source.get_skyhighlight_color())
	target.set_cloudbase_color(source.get_cloudbase_color())
	target.set_cloudhighlight_color(source.get_cloudhighlight_color())
	target.set_cloudedge_color(source.get_cloudedge_color())


func _on_remove_keyframe() -> void:
	var keyframes := _keyframes()
	if keyframes.size() <= 1 or _selected_keyframe < 0 or _editor == null:
		return
	_editor.push_undo_step(func():
		var frames := _keyframes()
		frames.remove_at(_selected_keyframe)
		_selected_keyframe = mini(_selected_keyframe, frames.size() - 1)
		_editor.env_file.set_tod_keyframes(frames))
	sync_from_editor()
