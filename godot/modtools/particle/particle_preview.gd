class_name ParticlePreview
extends Control
## SubViewport host for the particle editor. Effects are previewed as one
## NovaParticleEmitter per referenced pdef, while individual particle defs use
## a single emitter. A bottom overlay adds transport controls (play/pause,
## restart, time scale, a deterministic frame timeline) plus a live stats
## readout. Edits in the inspector ask for a debounced live refresh.

const FlyCameraScript = preload("res://engine/fly_camera.gd")

const STEP_SECONDS := 1.0 / 60.0
const PARTICLE_FLAG_FOREVER_EMIT := 1 << 18

var _viewport_container: SubViewportContainer
var _viewport: SubViewport
var _root: Node3D
var _camera: Camera3D
var _emitters_root: Node3D
var _emitters: Array[NovaParticleEmitter] = []
var _particle_file: NovaParticleFile
var _grid: MeshInstance3D
var _axes: MeshInstance3D
var _world_env: WorldEnvironment

# Playback state. Emitters auto-advance while playing; stepping/seeking takes
# manual control (pausing first) so the frame timeline stays deterministic.
var _paused := false
var _time_scale := 1.0
var _elapsed := 0.0
var _max_seconds := 4.0
var _seek_guard := false
var _stats_accum := 0.0
var _bg_level := 0

# What is currently previewed, so a live refresh can re-apply it.
var _last_mode := 0  # 0 none, 1 particle, 2 effect
var _last_def: NovaParticleDef
var _last_effect: NovaParticleEffect
var _refresh_timer: Timer

# Overlay controls.
var _overlay: Control
var _play_button: Button
var _timeline: HSlider
var _frame_label: Label
var _speed_slider: HSlider
var _speed_label: Label
var _stats_label: Label


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_STOP
	clip_contents = true
	_build_viewport()
	_build_overlay()
	_refresh_timer = Timer.new()
	_refresh_timer.one_shot = true
	_refresh_timer.wait_time = 0.15
	add_child(_refresh_timer)
	_refresh_timer.timeout.connect(_on_refresh_timeout)
	set_process(true)


func _build_viewport() -> void:
	_viewport_container = SubViewportContainer.new()
	_viewport_container.set_anchors_preset(Control.PRESET_FULL_RECT)
	_viewport_container.stretch = true
	_viewport_container.mouse_filter = Control.MOUSE_FILTER_STOP
	add_child(_viewport_container)

	_viewport = SubViewport.new()
	_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	_viewport.transparent_bg = false
	_viewport.handle_input_locally = true
	_viewport_container.add_child(_viewport)

	_root = Node3D.new()
	_viewport.add_child(_root)

	_camera = FlyCameraScript.new()
	_camera.current = true
	_camera.fov = 50.0
	_camera.near = 0.05
	_camera.far = 5000.0
	_camera.fly_speed = 10.0
	_camera.look_at_from_position(Vector3(0.0, 4.0, 12.0), Vector3.ZERO)
	_root.add_child(_camera)

	_world_env = WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = _bg_color_for_level(_bg_level)
	_world_env.environment = env
	_root.add_child(_world_env)

	_emitters_root = Node3D.new()
	_emitters_root.name = "ParticleEmitters"
	_root.add_child(_emitters_root)

	_add_grid()
	_add_axes()


func _add_grid() -> void:
	const HALF: float = 50.0
	const STEP: float = 5.0
	var lines := ImmediateMesh.new()
	var grid_material := StandardMaterial3D.new()
	grid_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	grid_material.vertex_color_use_as_albedo = true
	grid_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	lines.surface_begin(Mesh.PRIMITIVE_LINES, grid_material)
	var minor := Color(0.32, 0.32, 0.36, 0.55)
	var x: float = -HALF
	while x <= HALF + 0.001:
		var c := minor if not is_zero_approx(x) else Color(0.65, 0.32, 0.32, 0.85)
		lines.surface_set_color(c)
		lines.surface_add_vertex(Vector3(x, 0.0, -HALF))
		lines.surface_set_color(c)
		lines.surface_add_vertex(Vector3(x, 0.0, HALF))
		x += STEP
	var z: float = -HALF
	while z <= HALF + 0.001:
		var c := minor if not is_zero_approx(z) else Color(0.32, 0.32, 0.65, 0.85)
		lines.surface_set_color(c)
		lines.surface_add_vertex(Vector3(-HALF, 0.0, z))
		lines.surface_set_color(c)
		lines.surface_add_vertex(Vector3(HALF, 0.0, z))
		z += STEP
	lines.surface_end()
	_grid = MeshInstance3D.new()
	_grid.mesh = lines
	_root.add_child(_grid)


func _add_axes() -> void:
	const LEN := 2.0
	var lines := ImmediateMesh.new()
	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.vertex_color_use_as_albedo = true
	lines.surface_begin(Mesh.PRIMITIVE_LINES, mat)
	var axes := [
		[Vector3.RIGHT, Color(0.9, 0.3, 0.3)],
		[Vector3.UP, Color(0.4, 0.9, 0.4)],
		[Vector3.BACK, Color(0.4, 0.6, 0.95)],
	]
	for entry in axes:
		var dir: Vector3 = entry[0]
		var col: Color = entry[1]
		lines.surface_set_color(col)
		lines.surface_add_vertex(Vector3.ZERO)
		lines.surface_set_color(col)
		lines.surface_add_vertex(dir * LEN)
	lines.surface_end()
	_axes = MeshInstance3D.new()
	_axes.mesh = lines
	_root.add_child(_axes)


func _build_overlay() -> void:
	_overlay = Control.new()
	_overlay.set_anchors_preset(Control.PRESET_FULL_RECT)
	_overlay.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(_overlay)

	var bar := PanelContainer.new()
	bar.mouse_filter = Control.MOUSE_FILTER_STOP
	bar.anchor_left = 0.0
	bar.anchor_right = 1.0
	bar.anchor_top = 1.0
	bar.anchor_bottom = 1.0
	bar.offset_top = -64.0
	bar.offset_left = 0.0
	bar.offset_right = 0.0
	bar.offset_bottom = 0.0
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.08, 0.09, 0.11, 0.82)
	style.content_margin_left = 8.0
	style.content_margin_right = 8.0
	style.content_margin_top = 4.0
	style.content_margin_bottom = 4.0
	bar.add_theme_stylebox_override("panel", style)
	_overlay.add_child(bar)

	var rows := VBoxContainer.new()
	rows.add_theme_constant_override("separation", 2)
	bar.add_child(rows)

	# Timeline row.
	var timeline_row := HBoxContainer.new()
	rows.add_child(timeline_row)
	_frame_label = Label.new()
	_frame_label.custom_minimum_size = Vector2(110, 0)
	_frame_label.text = "frame 0 / 0"
	timeline_row.add_child(_frame_label)
	_timeline = HSlider.new()
	_timeline.min_value = 0.0
	_timeline.max_value = 1.0
	_timeline.step = 1.0
	_timeline.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	timeline_row.add_child(_timeline)
	_timeline.value_changed.connect(_on_timeline_changed)

	# Transport row.
	var transport := HBoxContainer.new()
	transport.add_theme_constant_override("separation", 4)
	rows.add_child(transport)
	_add_transport_button(transport, "|<", "Restart from frame 0", _on_restart_button)
	_add_transport_button(transport, "<", "Step back one frame", func(): step_frame(-1))
	_play_button = _add_transport_button(transport, "⏸", "Play / pause", _on_play_button)
	_add_transport_button(transport, ">", "Step forward one frame", func(): step_frame(1))

	var sep := VSeparator.new()
	transport.add_child(sep)
	var speed_caption := Label.new()
	speed_caption.text = "Speed"
	transport.add_child(speed_caption)
	_speed_slider = HSlider.new()
	_speed_slider.min_value = 0.1
	_speed_slider.max_value = 3.0
	_speed_slider.step = 0.05
	_speed_slider.value = 1.0
	_speed_slider.custom_minimum_size = Vector2(90, 0)
	transport.add_child(_speed_slider)
	_speed_slider.value_changed.connect(_on_speed_changed)
	_speed_label = Label.new()
	_speed_label.custom_minimum_size = Vector2(40, 0)
	_speed_label.text = "1.0x"
	transport.add_child(_speed_label)

	transport.add_child(VSeparator.new())
	_add_transport_button(transport, "Reset view", "Re-frame the camera", reset_view)
	_add_transport_button(transport, "BG", "Cycle background brightness", _cycle_background)

	_stats_label = Label.new()
	_stats_label.theme_type_variation = &"Muted"
	_stats_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	_stats_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_stats_label.text = "No particle selected"
	transport.add_child(_stats_label)


func _add_transport_button(parent: Control, text: String, tip: String, cb: Callable) -> Button:
	var btn := Button.new()
	btn.text = text
	btn.tooltip_text = tip
	btn.focus_mode = Control.FOCUS_NONE
	parent.add_child(btn)
	btn.pressed.connect(cb)
	return btn


func _process(delta: float) -> void:
	if not _paused and not _emitters.is_empty():
		_elapsed += clampf(delta, 0.0, 0.1) * _time_scale
	_stats_accum += delta
	if _stats_accum >= 0.2:
		_stats_accum = 0.0
		_update_overlay_readouts()


# --- Document / selection ----------------------------------------------------

func set_particle_file(file: NovaParticleFile) -> void:
	_particle_file = file
	var tables: Array = []
	if _particle_file != null:
		tables = _particle_file.get_tables()
	var texture_dir := _texture_dir()
	for emitter in _emitters:
		if emitter != null:
			emitter.set_tables(tables)
			emitter.texture_dir = texture_dir


func set_effect(effect: NovaParticleEffect) -> void:
	clear_preview()
	_last_mode = 2
	_last_effect = effect
	_last_def = null
	if effect == null or _particle_file == null:
		return
	var pdefs: PackedStringArray = effect.pdefs
	var max_seconds := 0.0
	for i in range(pdefs.size()):
		var def := _particle_file.find_particle(pdefs[i])
		if def != null:
			_add_emitter(def, i)
			max_seconds = maxf(max_seconds, _def_window_seconds(def))
	_max_seconds = maxf(max_seconds, 1.0)
	_warm_all()
	_reset_timeline()


func set_particle_def(def: NovaParticleDef) -> void:
	clear_preview()
	_last_mode = 1
	_last_def = def
	_last_effect = null
	if def != null:
		_add_emitter(def, 0)
		_max_seconds = _def_window_seconds(def)
	_warm_all()
	_reset_timeline()


func clear_preview() -> void:
	for emitter in _emitters:
		if emitter == null:
			continue
		if emitter.get_parent() != null:
			emitter.get_parent().remove_child(emitter)
		emitter.queue_free()
	_emitters.clear()
	_last_mode = 0
	_last_def = null
	_last_effect = null
	_elapsed = 0.0
	_update_overlay_readouts()


func _add_emitter(def: NovaParticleDef, index: int) -> NovaParticleEmitter:
	if _emitters_root == null:
		return null
	var emitter := NovaParticleEmitter.new()
	emitter.name = "ParticleEmitter%d" % index
	emitter.auto_advance = not _paused
	emitter.time_scale = _time_scale
	emitter.seed = 1 + index * 101
	emitter.texture_dir = _texture_dir()
	if _particle_file != null:
		emitter.set_tables(_particle_file.get_tables())
	emitter.def = def
	_emitters_root.add_child(emitter)
	emitter.play()
	_emitters.append(emitter)
	return emitter


func _texture_dir() -> String:
	if _particle_file == null:
		return ""
	var source_path := String(_particle_file.get_source_path())
	if source_path.is_empty():
		return ""
	return source_path.get_base_dir()


# Advance all emitters in lockstep until something is alive (so a freshly
# selected particle is immediately visible), bounded to ~1.5s of warm-up.
func _warm_all() -> void:
	if _emitters.is_empty():
		return
	for i in range(90):
		var any_alive := false
		for emitter in _emitters:
			if emitter != null:
				emitter.advance(STEP_SECONDS)
				if emitter.get_alive_count() > 0:
					any_alive = true
		_elapsed += STEP_SECONDS
		if any_alive:
			return


# --- Playback API ------------------------------------------------------------

func set_paused(value: bool) -> void:
	_paused = value
	for emitter in _emitters:
		if emitter != null:
			emitter.auto_advance = not value
	if _play_button != null:
		_play_button.text = "▶" if value else "⏸"


func is_paused() -> bool:
	return _paused


func set_time_scale(value: float) -> void:
	_time_scale = clampf(value, 0.05, 4.0)
	for emitter in _emitters:
		if emitter != null:
			emitter.time_scale = _time_scale
	if _speed_label != null:
		_speed_label.text = "%.1fx" % _time_scale


func step_frame(direction: int) -> void:
	if _emitters.is_empty():
		return
	set_paused(true)
	if direction < 0:
		seek_seconds(_elapsed - STEP_SECONDS)
		return
	for emitter in _emitters:
		if emitter != null:
			emitter.advance(STEP_SECONDS)
	_elapsed += STEP_SECONDS
	_update_overlay_readouts()


## Re-simulate from frame 0 to `seconds` (deterministic given the seed). Used by
## the timeline scrubber and step-back.
func seek_seconds(seconds: float) -> void:
	if _emitters.is_empty():
		return
	var target := clampf(seconds, 0.0, _max_seconds)
	var steps := int(round(target / STEP_SECONDS))
	for emitter in _emitters:
		if emitter == null:
			continue
		emitter.restart()
		for i in range(steps):
			emitter.advance(STEP_SECONDS)
	_elapsed = float(steps) * STEP_SECONDS
	_update_overlay_readouts()


func get_current_frame() -> int:
	return int(round(_elapsed / STEP_SECONDS))


func get_max_frames() -> int:
	return maxi(1, int(round(_max_seconds / STEP_SECONDS)))


func restart() -> void:
	set_paused(false)
	for emitter in _emitters:
		if emitter != null:
			emitter.restart()
	_elapsed = 0.0
	_warm_all()
	_reset_timeline()


func reset_view() -> void:
	if _camera != null and _camera.has_method("frame_bounds_custom"):
		_camera.frame_bounds_custom(Vector3.ZERO, 9.0, 1.4, 80.0, 0.0, -0.3)
	elif _camera != null:
		_camera.look_at_from_position(Vector3(0.0, 4.0, 12.0), Vector3.ZERO)


func set_grid_visible(value: bool) -> void:
	if _grid != null:
		_grid.visible = value


func set_axes_visible(value: bool) -> void:
	if _axes != null:
		_axes.visible = value


func set_background_brightness(level: int) -> void:
	_bg_level = clampi(level, 0, 2)
	if _world_env != null and _world_env.environment != null:
		_world_env.environment.background_color = _bg_color_for_level(_bg_level)


# --- Live refresh ------------------------------------------------------------

## Debounced re-apply of the current selection so inspector edits show live
## without thrashing on every keystroke / spinbox drag.
func request_live_refresh() -> void:
	if _refresh_timer != null:
		_refresh_timer.stop()
		_refresh_timer.start()


func _on_refresh_timeout() -> void:
	var prev_elapsed := _elapsed
	var was_paused := _paused
	match _last_mode:
		1:
			if _last_def != null:
				set_particle_def(_last_def)
		2:
			if _last_effect != null:
				set_effect(_last_effect)
		_:
			return
	# Restore the viewing position so edits don't yank the timeline back to 0.
	if prev_elapsed > 0.0:
		seek_seconds(prev_elapsed)
	set_paused(was_paused)


# --- Overlay glue ------------------------------------------------------------

func _on_play_button() -> void:
	set_paused(not _paused)


func _on_restart_button() -> void:
	restart()


func _on_speed_changed(value: float) -> void:
	set_time_scale(value)


func _on_timeline_changed(value: float) -> void:
	if _seek_guard:
		return
	seek_seconds(value * STEP_SECONDS)


func _cycle_background() -> void:
	set_background_brightness((_bg_level + 1) % 3)


func _reset_timeline() -> void:
	if _timeline == null:
		return
	_seek_guard = true
	_timeline.max_value = float(get_max_frames())
	_timeline.value = float(get_current_frame())
	_seek_guard = false
	set_paused(_paused)
	_update_overlay_readouts()


func _update_overlay_readouts() -> void:
	if _frame_label != null:
		_frame_label.text = "frame %d / %d" % [get_current_frame(), get_max_frames()]
	if _timeline != null and not _seek_guard:
		_seek_guard = true
		_timeline.value = clampf(float(get_current_frame()), _timeline.min_value, _timeline.max_value)
		_seek_guard = false
	if _stats_label != null:
		if _emitters.is_empty():
			_stats_label.text = "No particle selected"
		else:
			_stats_label.text = "Alive %d · Instances %d · Layers %d · Batches %d" % [
				get_alive_count(), get_rendered_instance_count(),
				get_visual_layer_count(), get_render_batch_count()]


func _bg_color_for_level(level: int) -> Color:
	match level:
		1: return Color(0.22, 0.23, 0.26)
		2: return Color(0.78, 0.80, 0.84)
		_: return Color(0.09, 0.10, 0.12)


func _def_window_seconds(def: NovaParticleDef) -> float:
	if def == null:
		return 4.0
	var forever := (int(def.flags) & PARTICLE_FLAG_FOREVER_EMIT) != 0
	var life := def.emit_delay + def.age + 0.5
	if forever:
		life = def.emit_delay + def.age * 2.0 + 2.0
	else:
		life += def.emit_dur
	return clampf(life, 1.0, 12.0)


# --- Accessors (used by tests + overlay) -------------------------------------

func get_alive_count() -> int:
	var total := 0
	for emitter in _emitters:
		if emitter != null:
			total += emitter.get_alive_count()
	return total


func get_emitter_count() -> int:
	return _emitters.size()


func get_visual_layer_count() -> int:
	var total := 0
	for emitter in _emitters:
		if emitter != null:
			total += emitter.get_visual_layer_count()
	return total


func get_rendered_instance_count() -> int:
	var total := 0
	for emitter in _emitters:
		if emitter != null:
			total += emitter.get_rendered_instance_count()
	return total


func get_render_batch_count() -> int:
	var total := 0
	for emitter in _emitters:
		if emitter != null and emitter.has_method("get_render_batch_count"):
			total += emitter.get_render_batch_count()
	return total


func get_textured_layer_count() -> int:
	var total := 0
	for emitter in _emitters:
		if emitter != null:
			total += emitter.get_textured_layer_count()
	return total


func get_preview_camera() -> Camera3D:
	return _camera


func get_emitter() -> NovaParticleEmitter:
	if _emitters.is_empty():
		return null
	return _emitters[0]
