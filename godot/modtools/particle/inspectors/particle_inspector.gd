class_name ParticleDefInspector
extends Control

## Code-first Details inspector for a [particledef]. A browser (list + Add /
## Duplicate / Delete) drives a per-particle form of collapsible sections built
## with InspectorForms + FieldBinder. Flag/move are edited as checkbox grids
## bound to the int bitfields (the fields the writer actually serializes), and
## graphic layers get a full per-layer editor. Every edit marks the document
## dirty and asks the preview to refresh live.
##
## The workspace instantiates this via ParticleInspectorScript.new() (a bare
## Control), so there is no companion .tscn. The class name + set_particle_editor
## / set_workspace surface are preserved for the workstation test.

const UI := preload("res://modtools/framework/inspector_forms.gd")
const FieldBinderScript := preload("res://engine/ui/field_binder.gd")

const CURVE_FIELDS := [
	["scale_func", "Size over life"],
	["alpha_func", "Opacity over life"],
	["red_func", "Red over life"],
	["green_func", "Green over life"],
	["blue_func", "Blue over life"],
	["emit_rate_func", "Emit rate over life"],
]

var _editor: ParticleEditor
var _workspace

var _list: ItemList
var _empty_label: Label
var _dup_button: Button
var _del_button: Button
var _form: VBoxContainer

var _particles: Array = []
var _binder: FieldBinder
var _id_edit: LineEdit
var _flags_label: Label
var _suppress := false


func _ready() -> void:
	_build_skeleton()
	_refresh()


func set_particle_editor(value: ParticleEditor) -> void:
	if _editor != null:
		if _editor.document_changed.is_connected(_refresh):
			_editor.document_changed.disconnect(_refresh)
		if _editor.selection_changed.is_connected(_refresh_selection):
			_editor.selection_changed.disconnect(_refresh_selection)
	_editor = value
	if _editor != null:
		_editor.document_changed.connect(_refresh)
		_editor.selection_changed.connect(_refresh_selection)
	if is_inside_tree():
		_refresh()


func set_workspace(value) -> void:
	_workspace = value


# --- Skeleton ----------------------------------------------------------------

func _build_skeleton() -> void:
	set_anchors_preset(Control.PRESET_FULL_RECT)
	var root := VBoxContainer.new()
	root.set_anchors_preset(Control.PRESET_FULL_RECT)
	root.add_theme_constant_override("separation", 6)
	add_child(root)

	UI.add_section_heading(root, "Particles")

	var toolbar := HBoxContainer.new()
	root.add_child(toolbar)
	var add_btn := Button.new()
	add_btn.text = "+ Add"
	add_btn.tooltip_text = "Add a new particle"
	toolbar.add_child(add_btn)
	add_btn.pressed.connect(_on_add_pressed)
	_dup_button = Button.new()
	_dup_button.text = "Duplicate"
	toolbar.add_child(_dup_button)
	_dup_button.pressed.connect(_on_duplicate_pressed)
	_del_button = Button.new()
	_del_button.text = "Delete"
	toolbar.add_child(_del_button)
	_del_button.pressed.connect(_on_delete_pressed)

	_list = ItemList.new()
	_list.custom_minimum_size = Vector2(0, 150)
	_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	root.add_child(_list)
	_list.item_selected.connect(_on_item_selected)

	_empty_label = UI.add_empty_state(root,
		"No particles yet. Add one to start shaping an effect.")

	_form = UI.make_inspector_box(root)


# --- Refresh -----------------------------------------------------------------

func _refresh() -> void:
	_particles.clear()
	if _list != null:
		_list.clear()
	if _editor != null and _editor.particle_file != null:
		for entry in _editor.particle_file.particles:
			if entry != null:
				_particles.append(entry)
				_list.add_item(entry.id)
	if _empty_label != null:
		_empty_label.visible = _particles.is_empty()
	_refresh_selection()


func _refresh_selection() -> void:
	if _form == null:
		return
	_clear_form()
	var p: NovaParticleDef = _editor.current_particle if _editor != null else null
	var idx := _particles.find(p) if p != null else -1
	if idx >= 0:
		_list.select(idx)
	else:
		_list.deselect_all()
	if _dup_button != null:
		_dup_button.disabled = (p == null)
	if _del_button != null:
		_del_button.disabled = (p == null)
	if p == null:
		UI.add_muted_label(_form, "Select a particle to edit, or add a new one.")
		return
	_build_form(p)


func _clear_form() -> void:
	if _form == null:
		return
	# Free immediately (not queue_free): the form is rebuilt on every selection,
	# and deferred frees pile up as orphaned subtrees that pollute the shared
	# headless renderer across tests. _clear_form is only ever called from a
	# model signal handler, never from within these nodes' own callbacks.
	for child in _form.get_children():
		_form.remove_child(child)
		child.free()
	_binder = null
	_id_edit = null
	_flags_label = null


# --- Form build --------------------------------------------------------------

func _build_form(p: NovaParticleDef) -> void:
	_binder = FieldBinderScript.new()

	_build_identity(p)
	_build_emission(p)
	_build_appearance(p)
	_build_motion(p)
	_build_behavior(p)
	_build_graphics(p)
	_build_curves(p)
	_build_sounds(p)

	_binder.sync_from({})


func _build_identity(p: NovaParticleDef) -> void:
	var box := UI.add_foldable_section(_form, "Identity", true)
	# Name needs live list-label sync, so it is wired manually (not via binder).
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(row)
	var label := Label.new()
	label.text = "Name"
	label.custom_minimum_size = Vector2(76, 0)
	row.add_child(label)
	_id_edit = LineEdit.new()
	_id_edit.text = p.id
	_id_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(_id_edit)
	_id_edit.text_changed.connect(func(text: String) -> void: _on_id_changed(p, text))
	_id_edit.focus_exited.connect(func() -> void: _on_id_focus_exited(p))

	_row_line(box, p, "child_id", "Spawns on death",
		"Name of a child particle definition this particle spawns when it dies (optional).",
		func(value: String) -> void: _editor.set_child_id(p, value))
	_row_float(box, p, "lod", "Detail distance", 0.0, 100000.0, 1.0,
		"Distance fade hint. 0 keeps the particle always drawn.")


func _build_emission(p: NovaParticleDef) -> void:
	var box := UI.add_foldable_section(_form, "Emission", true)
	_row_float(box, p, "emit_rate", "Emit rate (/s)", 0.0, 10000.0, 0.5,
		"Particles spawned per second.")
	_row_float(box, p, "emit_burst", "Burst count", 1.0, 1000.0, 1.0,
		"Particles spawned each tick (at least 1).")
	_row_float(box, p, "emit_dur", "Burst duration (s)", 0.0, 1000.0, 0.05,
		"How long the emitter keeps spawning. 0 with Forever emit = endless.")
	_row_float(box, p, "emit_delay", "Start delay (s)", 0.0, 1000.0, 0.05,
		"Delay before the emitter starts spawning.")
	_row_option(box, p, "emit_shape", "Emit shape",
		[{"id": 0, "label": "Point"}, {"id": 1, "label": "Box"},
		{"id": 2, "label": "Sphere"}, {"id": 3, "label": "Cone"}])
	_row_vec3(box, p, "emit_shape_size", "Shape size", 0.0, 1000.0, 0.1)

	var adv := UI.add_foldable_section(box, "Emission · advanced", false)
	_row_float(adv, p, "emit_maxoverride", "Max alive", 0.0, 100000.0, 1.0,
		"Cap on live particles. 0 = no cap.")
	_row_float(adv, p, "emit_dur_adj", "Duration variance", 0.0, 1000.0, 0.05)
	_row_float(adv, p, "emit_rate_adj", "Rate variance", 0.0, 10000.0, 0.5)
	_row_vec3(adv, p, "emit_shape_size_skip", "Shape inner size", 0.0, 1000.0, 0.1)


func _build_appearance(p: NovaParticleDef) -> void:
	var box := UI.add_foldable_section(_form, "Appearance", true)
	_row_float(box, p, "scale_value", "Size (units)", 0.0, 1000.0, 0.05,
		"Particle size in world units.")
	_row_float(box, p, "scale_adj", "Size variance", 0.0, 1000.0, 0.05)
	_row_float(box, p, "alpha", "Opacity", 0.0, 1.0, 0.01,
		"Overall transparency. 1 = opaque.")
	_row_color(box, p, "color1", "Tint A")
	_row_color(box, p, "color2", "Tint B")
	_row_color(box, p, "color3", "Tint C")
	_row_color(box, p, "color4", "Tint D")
	_row_float(box, p, "bump_scale", "Bump lighting", 0.0, 100.0, 0.05,
		"Strength of pseudo-lighting for bump blend modes.")


func _build_motion(p: NovaParticleDef) -> void:
	var box := UI.add_foldable_section(_form, "Motion", false)
	_row_float(box, p, "age", "Lifetime (s)", 0.0, 1000.0, 0.05,
		"How long each particle lives.")
	_row_float(box, p, "speed", "Speed (units/s)", -1000.0, 1000.0, 0.1)
	_row_float(box, p, "spread", "Cone spread (deg)", 0.0, 180.0, 1.0,
		"Half-angle of the emission cone.")
	_row_float(box, p, "gravity", "Gravity", -10000.0, 10000.0, 1.0)
	_row_float(box, p, "drag", "Air drag", -100.0, 1000.0, 0.05,
		"How quickly particles slow down. 0 = no slowdown.")
	_row_float(box, p, "elastic", "Bounciness", 0.0, 10.0, 0.05)
	_row_float(box, p, "orbitalspeed", "Orbit speed", -1000.0, 1000.0, 0.1)
	_row_float(box, p, "yaw_rot", "Spin yaw (deg/s)", -3600.0, 3600.0, 1.0)
	_row_float(box, p, "pitch_rot", "Spin pitch (deg/s)", -3600.0, 3600.0, 1.0)
	_row_float(box, p, "roll_rot", "Spin roll (deg/s)", -3600.0, 3600.0, 1.0)

	var adv := UI.add_foldable_section(box, "Motion · advanced", false)
	_row_float(adv, p, "age_adj", "Lifetime variance", 0.0, 1000.0, 0.05)
	_row_float(adv, p, "speed_adj", "Speed variance", 0.0, 1000.0, 0.1)
	_row_vec3(adv, p, "gravity_mask", "Gravity axes", 0.0, 1.0, 1.0)
	_row_vec3(adv, p, "orbital_axis", "Orbit axis", -1.0, 1.0, 0.1)
	_row_vec3(adv, p, "orientation", "Facing", -360.0, 360.0, 1.0)
	_row_float(adv, p, "y_offset", "Spawn offset Y", -1000.0, 1000.0, 0.1)
	_row_float(adv, p, "z_offset", "Spawn offset Z", -1000.0, 1000.0, 0.1)


func _build_behavior(p: NovaParticleDef) -> void:
	var box := UI.add_foldable_section(_form, "Behavior flags", false)

	UI.add_muted_label(box, "Movement")
	var move_grid := GridContainer.new()
	move_grid.columns = 2
	box.add_child(move_grid)
	for move_name in NovaParticleDef.get_move_flag_table():
		var bit := int(NovaParticleDef.get_move_flag_table()[move_name])
		var cb := CheckBox.new()
		cb.text = move_name.capitalize()
		cb.tooltip_text = move_name
		move_grid.add_child(cb)
		_binder.bind_checkbox(cb,
			func(_i): return (int(p.move) & bit) != 0,
			func(on): _set_bit(p, "move", bit, on))

	UI.add_muted_label(box, "Flags")
	var flag_grid := GridContainer.new()
	flag_grid.columns = 2
	box.add_child(flag_grid)
	for flag_name in NovaParticleDef.get_particle_flag_table():
		var bit := int(NovaParticleDef.get_particle_flag_table()[flag_name])
		var cb := CheckBox.new()
		cb.text = flag_name.capitalize()
		cb.tooltip_text = flag_name
		flag_grid.add_child(cb)
		_binder.bind_checkbox(cb,
			func(_i): return (int(p.flags) & bit) != 0,
			func(on): _set_bit(p, "flags", bit, on))

	_flags_label = UI.add_muted_label(box, "")
	_update_flags_label(p)


func _build_graphics(p: NovaParticleDef) -> void:
	var box := UI.add_foldable_section(_form, "Graphic layers", true)
	var blend_names := NovaParticleDef.get_blend_mode_names()
	var blend_options: Array = []
	for i in range(blend_names.size()):
		blend_options.append({"id": i, "label": blend_names[i]})

	var graphics := p.get_graphics()
	var present := 0
	for i in range(graphics.size()):
		var layer: NovaParticleGraphicLayer = graphics[i]
		if layer == null or not layer.present:
			continue
		present += 1
		var card := UI.build_channel_card(box, "")
		var head := HBoxContainer.new()
		card.add_child(head)
		UI.add_section_heading(head, "Layer %d" % (i + 1))
		var spacer := Control.new()
		spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		head.add_child(spacer)
		var remove := Button.new()
		remove.text = "Remove"
		head.add_child(remove)
		remove.pressed.connect(func() -> void: _on_remove_layer(p, i))

		_row_line(card, layer, "texture", "Texture",
			"Texture file (e.g. dirtpuf.tga). Empty draws a soft dot.")
		_row_option(card, layer, "blend_mode", "Blend", blend_options)
		_row_float(card, layer, "flip_frames", "Flip frames", 1.0, 256.0, 1.0,
			"Frames in the flipbook strip. 1 = a single still image.")
		_row_float(card, layer, "flip_rate", "Flip rate (/s)", 0.0, 120.0, 1.0)
		_row_check(card, layer, "color_overrides_set", "Override particle tints")
		_row_color(card, layer, "color1", "Tint A")
		_row_color(card, layer, "color2", "Tint B")
		_row_color(card, layer, "color3", "Tint C")
		_row_color(card, layer, "color4", "Tint D")
		_row_float(card, layer, "alpha", "Opacity", 0.0, 1.0, 0.01)
		_row_float(card, layer, "scale_value", "Size (units)", 0.0, 1000.0, 0.05)

	if present == 0:
		UI.add_muted_label(box, "No graphic layers. Add one to give the particle a look.")
	if present < 4:
		var add := Button.new()
		add.text = "+ Add graphic layer"
		box.add_child(add)
		add.pressed.connect(func() -> void: _on_add_layer(p))


func _build_curves(p: NovaParticleDef) -> void:
	var box := UI.add_foldable_section(_form, "Curves (animate over life)", false)
	var tables: Array = []
	if _editor != null and _editor.particle_file != null:
		for t in _editor.particle_file.tables:
			if t != null:
				tables.append(t)
	if tables.is_empty():
		UI.add_muted_label(box, "Add a curve table to animate these over a particle's life.")
	for entry in CURVE_FIELDS:
		var field: String = entry[0]
		var label_text: String = entry[1]
		_build_curve_row(box, p, field, label_text, tables)


func _build_curve_row(box: VBoxContainer, p: NovaParticleDef, field: String,
		label_text: String, tables: Array) -> void:
	var curve: NovaParticleCurveRef = _curve_for(p, field)
	var row := UI.add_detail_field(box, label_text)
	var controls := HBoxContainer.new()
	controls.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(controls)

	var option := OptionButton.new()
	option.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	option.add_item("(none)")
	option.set_item_metadata(0, "")
	var current_name: String = curve.name if (curve != null and curve.present) else ""
	var selected := 0
	for j in range(tables.size()):
		option.add_item(tables[j].id)
		option.set_item_metadata(j + 1, tables[j].id)
		# Match the runtime's case-insensitive table resolve so a curve authored
		# with a case-mixed name still shows its table instead of "(none)".
		if tables[j].id.nocasecmp_to(current_name) == 0:
			selected = j + 1
	option.select(selected)
	controls.add_child(option)
	option.item_selected.connect(func(index: int) -> void:
		_on_curve_assigned(p, field, String(option.get_item_metadata(index))))

	var rev := CheckBox.new()
	rev.text = "rev"
	rev.tooltip_text = "Play the curve in reverse"
	rev.button_pressed = curve.reverse if curve != null else false
	controls.add_child(rev)
	rev.toggled.connect(func(on: bool) -> void: _on_curve_modifier(p, field, "reverse", on))

	var inv := CheckBox.new()
	inv.text = "inv"
	inv.tooltip_text = "Invert the curve values"
	inv.button_pressed = curve.inverse if curve != null else false
	controls.add_child(inv)
	inv.toggled.connect(func(on: bool) -> void: _on_curve_modifier(p, field, "inverse", on))


func _build_sounds(p: NovaParticleDef) -> void:
	var box := UI.add_foldable_section(_form, "Collision sounds", false)
	UI.add_muted_label(box, "One sound file per line (played on impact).")
	var edit := TextEdit.new()
	edit.custom_minimum_size = Vector2(0, 90)
	edit.placeholder_text = "splash.wav"
	edit.text = "\n".join(p.collide_sounds)
	box.add_child(edit)
	edit.text_changed.connect(func() -> void: _on_sounds_changed(p, edit))


# --- Row binders -------------------------------------------------------------

func _row_float(parent: Control, target: Object, prop: String, label_text: String,
		mn: float, mx: float, step: float, tip := "") -> void:
	var spin := UI.add_spin_row(parent, "", label_text, mn, mx, step)
	if not tip.is_empty():
		spin.tooltip_text = tip
	_binder.bind_spin(spin,
		func(_i): return float(target.get(prop)),
		func(v): _commit(target, prop, v))


func _row_color(parent: Control, target: Object, prop: String, label_text: String) -> void:
	var picker := UI.add_color_row(parent, "", label_text)
	picker.edit_alpha = false
	_binder.bind_color(picker,
		func(_i): return target.get(prop),
		func(v): _commit(target, prop, v))


func _row_line(parent: Control, target: Object, prop: String, label_text: String,
		tip := "", on_commit := Callable()) -> void:
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	parent.add_child(row)
	var label := Label.new()
	label.text = label_text
	label.custom_minimum_size = Vector2(76, 0)
	label.clip_text = true
	row.add_child(label)
	var line := LineEdit.new()
	line.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	if not tip.is_empty():
		line.tooltip_text = tip
	row.add_child(line)
	_binder.bind_line(line,
		func(_i): return String(target.get(prop)),
		func(v):
			if on_commit.is_valid():
				on_commit.call(v)
			else:
				_commit(target, prop, v))


func _row_check(parent: Control, target: Object, prop: String, label_text: String) -> void:
	var cb := UI.add_checkbox(parent, "", label_text)
	_binder.bind_checkbox(cb,
		func(_i): return bool(target.get(prop)),
		func(v): _commit(target, prop, v))


func _row_option(parent: Control, target: Object, prop: String, label_text: String,
		options: Array) -> void:
	var option := UI.add_id_option_row(parent, "", label_text, options)
	_binder.bind_option(option,
		func(_i): return int(target.get(prop)),
		func(v): _commit(target, prop, v))


func _row_vec3(parent: Control, target: Object, prop: String, label_text: String,
		mn: float, mx: float, step: float) -> void:
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	parent.add_child(row)
	var label := Label.new()
	label.text = label_text
	label.custom_minimum_size = Vector2(76, 0)
	label.clip_text = true
	row.add_child(label)
	for axis in range(3):
		var spin := SpinBox.new()
		spin.min_value = mn
		spin.max_value = mx
		spin.step = step
		spin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		row.add_child(spin)
		_binder.bind_spin(spin,
			func(_i): return float(target.get(prop)[axis]),
			func(v): _commit_vec3(target, prop, axis, v))


# --- Commit helpers ----------------------------------------------------------

func _commit(target: Object, prop: String, value) -> void:
	target.set(prop, value)
	_touch()


func _commit_vec3(target: Object, prop: String, axis: int, value: float) -> void:
	var cur: Vector3 = target.get(prop)
	cur[axis] = value
	target.set(prop, cur)
	_touch()


func _set_bit(p: NovaParticleDef, prop: String, bit: int, on: bool) -> void:
	var bits := int(p.get(prop))
	bits = (bits | bit) if on else (bits & ~bit)
	p.set(prop, bits)
	if prop == "flags":
		p.flags_raw = NovaParticleDef.format_particle_flags(bits)
		_update_flags_label(p)
	else:
		p.move_raw = NovaParticleDef.format_move_flags(bits)
	_touch()


func _touch() -> void:
	if _editor != null:
		_editor.notify_particle_changed()


func _update_flags_label(p: NovaParticleDef) -> void:
	if _flags_label == null:
		return
	var raw := NovaParticleDef.format_particle_flags(int(p.flags)).strip_edges()
	_flags_label.text = "flags = %s" % raw if not raw.is_empty() else "flags = (none)"


# --- Event handlers ----------------------------------------------------------

func _on_id_changed(p: NovaParticleDef, text: String) -> void:
	if _suppress:
		return
	if text.strip_edges().is_empty():
		return  # don't commit an empty id; reverts on focus loss
	if _editor != null:
		_editor.set_particle_id(p, text)
	var idx := _particles.find(p)
	if idx >= 0:
		_list.set_item_text(idx, text)


func _on_id_focus_exited(p: NovaParticleDef) -> void:
	if _id_edit != null and _id_edit.text.strip_edges().is_empty():
		_suppress = true
		_id_edit.text = p.id
		_suppress = false


func _on_item_selected(idx: int) -> void:
	if idx < 0 or idx >= _particles.size():
		return
	if _workspace != null and _workspace.has_method("select_particle"):
		_workspace.select_particle(_particles[idx])


func _on_add_pressed() -> void:
	if _workspace != null and _workspace.has_method("add_particle"):
		_workspace.add_particle()


func _on_duplicate_pressed() -> void:
	if _editor == null or _editor.current_particle == null:
		return
	if _workspace != null and _workspace.has_method("duplicate_particle"):
		_workspace.duplicate_particle(_editor.current_particle)


func _on_delete_pressed() -> void:
	if _editor == null or _editor.current_particle == null:
		return
	if _workspace != null and _workspace.has_method("remove_particle"):
		_workspace.remove_particle(_editor.current_particle)


func _on_add_layer(p: NovaParticleDef) -> void:
	if _workspace != null and _workspace.has_method("add_graphic_layer"):
		_workspace.add_graphic_layer(p)
	_refresh_selection()


func _on_remove_layer(p: NovaParticleDef, slot: int) -> void:
	if _workspace != null and _workspace.has_method("remove_graphic_layer"):
		_workspace.remove_graphic_layer(p, slot)
	_refresh_selection()


func _on_curve_assigned(p: NovaParticleDef, field: String, table_id: String) -> void:
	if _workspace != null and _workspace.has_method("assign_curve"):
		_workspace.assign_curve(p, field, table_id)


func _on_curve_modifier(p: NovaParticleDef, field: String, modifier: String, on: bool) -> void:
	var curve: NovaParticleCurveRef = _curve_for(p, field)
	if curve == null:
		return
	if modifier == "reverse":
		curve.reverse = on
	else:
		curve.inverse = on
	_touch()


func _on_sounds_changed(p: NovaParticleDef, edit: TextEdit) -> void:
	var sounds := PackedStringArray()
	for raw_line in edit.text.split("\n"):
		var trimmed := raw_line.strip_edges()
		if not trimmed.is_empty() and sounds.size() < 20:
			sounds.append(trimmed)
	p.collide_sounds = sounds
	if _editor != null:
		_editor.notify_particle_changed()


# --- Helpers -----------------------------------------------------------------

func _curve_for(p: NovaParticleDef, field: String) -> NovaParticleCurveRef:
	match field:
		"scale_func": return p.get_scale_func()
		"alpha_func": return p.get_alpha_func()
		"red_func": return p.get_red_func()
		"green_func": return p.get_green_func()
		"blue_func": return p.get_blue_func()
		"emit_rate_func": return p.get_emit_rate_func()
	return null
