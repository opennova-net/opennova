class_name ParticlePreview
extends Control
## SubViewport mount for the particle editor. The preview owns one value-only
## NovaEffectScene rendered through one shared NovaParticleRenderer.
## A bottom overlay adds transport controls (play/pause, restart, time scale, a
## deterministic fixed-tick timeline) plus a live stats readout. Edits in the
## inspector ask for a debounced live refresh.

const FlyCameraScript = preload("res://engine/fly_camera.gd")

const STEP_SECONDS := 1.0 / 62.5
const PARTICLE_FLAG_FOREVER_EMIT := NovaParticleDef.FLAG_FOREVER_EMIT
const PREVIEW_EFFECT_ID := "__oned_particle_preview__"

var _viewport_container: SubViewportContainer
var _viewport: SubViewport
var _root: Node3D
var _camera: Camera3D
var _scene: NovaEffectScene
var _renderer: NovaParticleRenderer
var _particle_file: NovaParticleFile
var _preview_document: NovaParticleFile
var _preview_effect_name := ""
var _effect_handle := 0
var _spawn_receipt: Dictionary = {}
var _preview_definition_indices: Array[int] = []
var _preview_definitions: Array[NovaParticleDef] = []
var _preview_finite := true
var _texture_provider := Callable()
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

	_renderer = NovaParticleRenderer.new()
	_renderer.name = "ParticleRenderer"
	_renderer.set_procedural_fallback_enabled(true)
	_root.add_child(_renderer)
	_reset_scene()

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
	if not _paused and _has_spawn():
		var step := clampf(delta, 0.0, 0.1) * _time_scale
		_advance_scene(step, false)
		_elapsed = minf(_elapsed + step, _max_seconds)
		if _elapsed >= _max_seconds:
			set_paused(true)
	_stats_accum += delta
	if _stats_accum >= 0.2:
		_stats_accum = 0.0
		_update_overlay_readouts()


# --- Document / selection ----------------------------------------------------

func set_particle_file(file: NovaParticleFile) -> void:
	_particle_file = file
	_configure_renderer_textures()


func set_texture_provider(provider: Callable) -> void:
	_texture_provider = provider
	_configure_renderer_textures()


func get_texture_provider() -> Callable:
	return _texture_provider


func set_effect(effect: NovaParticleEffect) -> void:
	clear_preview()
	_last_mode = 2
	_last_effect = effect
	_last_def = null
	if effect == null or _particle_file == null:
		return
	var pdefs: PackedStringArray = effect.pdefs
	var max_seconds := 0.0
	_preview_finite = true
	for pdef_name in pdefs:
		var def := _particle_file.find_particle(pdef_name)
		if def != null:
			max_seconds = maxf(max_seconds, _def_window_seconds(def))
			if (int(def.flags) & PARTICLE_FLAG_FOREVER_EMIT) != 0:
				_preview_finite = false
	_max_seconds = maxf(max_seconds, 1.0)
	_start_preview(_make_preview_document(effect, null))
	_warm_all()
	_reset_timeline()


func set_particle_def(def: NovaParticleDef) -> void:
	clear_preview()
	_last_mode = 1
	_last_def = def
	_last_effect = null
	if def != null:
		_preview_finite = (int(def.flags) & PARTICLE_FLAG_FOREVER_EMIT) == 0
		_max_seconds = _def_window_seconds(def)
		_start_preview(_make_preview_document(null, def))
	_warm_all()
	_reset_timeline()


func clear_preview() -> void:
	_last_mode = 0
	_last_def = null
	_last_effect = null
	_preview_document = null
	_preview_effect_name = ""
	_effect_handle = 0
	_spawn_receipt = {}
	_preview_definition_indices.clear()
	_preview_definitions.clear()
	_preview_finite = true
	_elapsed = 0.0
	_reset_scene()
	_update_overlay_readouts()


func _make_preview_document(
		effect: NovaParticleEffect, selected_def: NovaParticleDef) -> NovaParticleFile:
	var document := NovaParticleFile.new()
	if _particle_file != null:
		document.source_path = _particle_file.source_path
		document.tables = _particle_file.tables
		document.table_handles = _particle_file.table_handles

	var preview_effect := NovaParticleEffect.new()
	preview_effect.id = PREVIEW_EFFECT_ID
	var particles: Array[NovaParticleDef] = []
	if selected_def != null:
		var preview_def := selected_def
		var preview_id := String(selected_def.id)
		if preview_id.is_empty():
			preview_def = selected_def.duplicate(true) as NovaParticleDef
			if preview_def != null:
				preview_id = "__oned_selected_particle_def__"
				preview_def.id = preview_id
			else:
				preview_def = selected_def
		particles.append(preview_def)
		preview_effect.pdefs = PackedStringArray([preview_id])
	else:
		preview_effect.pdefs = effect.pdefs if effect != null else PackedStringArray()

	# Keep the rest of the catalog available for child-particle references. The
	# selected pdef is first so duplicate ids resolve to the value being edited.
	if _particle_file != null:
		for entry in _particle_file.particles:
			var candidate := entry as NovaParticleDef
			if candidate == null:
				continue
			if selected_def != null \
					and String(candidate.id).nocasecmp_to(String(selected_def.id)) == 0:
				continue
			particles.append(candidate)
	document.particles = particles
	var effects: Array[NovaParticleEffect] = [preview_effect]
	document.effects = effects
	return document


func _start_preview(document: NovaParticleFile) -> void:
	_preview_document = document
	_preview_effect_name = PREVIEW_EFFECT_ID
	_preview_definitions.clear()
	var preview_effect := document.find_effect(_preview_effect_name)
	if preview_effect != null:
		for definition_name in preview_effect.pdefs:
			var preview_def := document.find_particle(definition_name)
			if preview_def != null:
				_preview_definitions.append(preview_def)
	_scene = NovaEffectScene.new()
	var files: Array[NovaParticleFile] = [document]
	_scene.open(files, {
		"simulation_tick_seconds": STEP_SECONDS,
		"random_seed": 1,
	})
	_effect_handle = int(_scene.intern(_preview_effect_name))
	_spawn_receipt = {}
	_preview_definition_indices.clear()
	if _effect_handle > 0:
		_spawn_receipt = _scene.spawn({
			"effect_handle": _effect_handle,
			"transform": Transform3D.IDENTITY,
		})
	var initial_inspection := _scene.inspect(false)
	for group_v in initial_inspection.get("groups", []):
		for emitter_v in (group_v as Dictionary).get("emitters", []):
			var emitter := emitter_v as Dictionary
			_preview_definition_indices.append(
					int(emitter.get("definition_index", -1)))
	if _renderer != null:
		_renderer.set_scene(_scene)
		_configure_renderer_textures()


func _reset_scene() -> void:
	_scene = NovaEffectScene.new()
	var files: Array[NovaParticleFile] = []
	_scene.open(files, {"simulation_tick_seconds": STEP_SECONDS})
	if _renderer != null:
		_renderer.set_scene(_scene)
		_configure_renderer_textures()


func _respawn_current() -> void:
	if _preview_document == null:
		return
	_start_preview(_preview_document)


func _configure_renderer_textures() -> void:
	if _renderer == null:
		return
	_renderer.set_texture_provider(_texture_provider)
	_renderer.set_texture_dir(_texture_dir())
	_renderer.set_procedural_fallback_enabled(true)
	_renderer.render_now()


func _has_preview() -> bool:
	return _preview_document != null


func _has_spawn() -> bool:
	return _effect_handle > 0 and bool(_spawn_receipt.get("spawned", false))


func _advance_scene(seconds: float, render_immediately := true) -> void:
	if _scene == null:
		return
	_scene.advance_in_place(maxf(seconds, 0.0))
	if render_immediately and _renderer != null:
		_renderer.render_now()


func _texture_dir() -> String:
	if _particle_file == null:
		return ""
	var source_path := String(_particle_file.get_source_path())
	if source_path.is_empty():
		return ""
	return source_path.get_base_dir()


# Advance the shared scene until something is alive (so a freshly selected
# particle is immediately visible), bounded to ~1.5s of warm-up.
func _warm_all() -> void:
	if not _has_spawn():
		return
	for i in range(90):
		_advance_scene(STEP_SECONDS, false)
		_elapsed += STEP_SECONDS
		if get_alive_count() > 0:
			if _renderer != null:
				_renderer.render_now()
			return
	if _renderer != null:
		_renderer.render_now()


# --- Playback API ------------------------------------------------------------

func set_paused(value: bool) -> void:
	_paused = value
	if _play_button != null:
		_play_button.text = "▶" if value else "⏸"


func is_paused() -> bool:
	return _paused


func set_time_scale(value: float) -> void:
	_time_scale = clampf(value, 0.05, 4.0)
	if _speed_label != null:
		_speed_label.text = "%.1fx" % _time_scale


func step_frame(direction: int) -> void:
	if not _has_spawn():
		return
	set_paused(true)
	if direction < 0:
		seek_seconds(_elapsed - STEP_SECONDS)
		return
	if _elapsed + STEP_SECONDS >= _max_seconds:
		seek_seconds(_max_seconds)
		return
	_advance_scene(STEP_SECONDS)
	_elapsed += STEP_SECONDS
	_update_overlay_readouts()


## Re-simulate from frame 0 to `seconds` (deterministic given the seed). Used by
## the timeline scrubber and step-back.
func seek_seconds(seconds: float) -> void:
	if not _has_spawn():
		return
	var target := clampf(seconds, 0.0, _max_seconds)
	var steps := int(round(target / STEP_SECONDS))
	_respawn_current()
	if steps > 0:
		_advance_scene(float(steps) * STEP_SECONDS)
	_elapsed = float(steps) * STEP_SECONDS
	_update_overlay_readouts()


func get_current_frame() -> int:
	return int(round(_elapsed / STEP_SECONDS))


func get_max_frames() -> int:
	return maxi(1, int(round(_max_seconds / STEP_SECONDS)))


func restart() -> void:
	if not _has_spawn():
		return
	set_paused(false)
	_respawn_current()
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
	set_paused(true)
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
		if not _has_preview():
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
	return int(_scene.get_live_counts().get("particle_count", 0)) \
			if _scene != null else 0


func get_emitter_count() -> int:
	return _preview_definition_indices.size()


func get_visual_layer_count() -> int:
	if _preview_document == null:
		return 0
	var total := 0
	for definition in _preview_definitions:
		var present := 0
		var graphics: Array = definition.get_graphics()
		for graphic_v in graphics:
			var graphic := graphic_v as NovaParticleGraphicLayer
			if graphic != null and graphic.get_present():
				present += 1
		# The editor deliberately supplies one procedural diagnostic layer when
		# a pdef has no authored graphics, matching the old preview behavior.
		total += maxi(1, present)
	return total


func get_rendered_instance_count() -> int:
	return int(_renderer.get_rendered_quad_count()) if _renderer != null else 0


func get_render_batch_count() -> int:
	return int(_renderer.get_draw_command_count()) if _renderer != null else 0


func get_textured_layer_count() -> int:
	if _scene == null or _renderer == null:
		return 0
	var unresolved := {}
	for name in _renderer.get_unresolved_texture_names():
		unresolved[String(name).to_lower()] = true
	# Unlike the always-visible layer total, this diagnostic describes the
	# renderer's last immutable catalog. Read definitions from that same frame so
	# an inspector edit cannot temporarily pair new authored values with stale
	# atlas resolution state.
	var frame := _scene.get_frame_snapshot()
	var definitions := frame.get("definitions", []) as Array
	var total := 0
	for definition_index in _preview_definition_indices:
		if definition_index < 0 or definition_index >= definitions.size():
			continue
		var definition := definitions[definition_index] as Dictionary
		var graphics := definition.get("graphics", []) as Array
		for graphic_v in graphics:
			var graphic := graphic_v as Dictionary
			if not bool(graphic.get("present", false)):
				continue
			var texture_name := String(graphic.get("texture", ""))
			if texture_name.is_empty():
				continue
			var resolved := true
			for frame_name in _retail_frame_names(
					texture_name, int(graphic.get("flip_frames", 1))):
				if unresolved.has(String(frame_name).to_lower()):
					resolved = false
					break
			if resolved:
				total += 1
	return total


func _retail_frame_names(authored: String, frame_count: int) -> PackedStringArray:
	var result := PackedStringArray()
	if frame_count <= 1:
		result.append(authored)
		return result
	var base := authored.to_lower()
	var extension := base.find(".tga")
	if extension >= 0:
		base = base.substr(0, extension)
	for frame in range(1, frame_count + 1):
		result.append("%s_%02d.tga" % [base, frame])
	return result


func is_finite() -> bool:
	return _preview_finite


func is_finished() -> bool:
	if not _has_preview():
		return true
	if not _preview_finite:
		return false
	return int(_scene.get_live_counts().get("group_count", 0)) == 0


## Value-only emitter diagnostics. Entries join the portable scene snapshot,
## lifetime inspection, and renderer packet bounds by stable emitter id.
func get_emitter_diagnostics() -> Array:
	var result: Array = []
	if _scene == null:
		return result
	var frame := _scene.get_frame_snapshot()
	var frame_by_id := {}
	var frame_emitters := frame.get("emitters", []) as Array
	for emitter_v in frame_emitters:
		var emitter := emitter_v as Dictionary
		frame_by_id[int(emitter.get("emitter_id", 0))] = emitter
	var bounds_by_id := {}
	if _renderer != null:
		for bounds_v in _renderer.get_debug_emitter_bounds():
			var bounds := bounds_v as Dictionary
			bounds_by_id[int(bounds.get("emitter_id", 0))] = bounds
	var definitions := frame.get("definitions", []) as Array
	var inspection := _scene.inspect()
	var groups := inspection.get("groups", []) as Array
	for group_v in groups:
		var group := group_v as Dictionary
		var group_emitters := group.get("emitters", []) as Array
		for emitter_v in group_emitters:
			var value := (emitter_v as Dictionary).duplicate(true)
			var emitter_id := int(value.get("emitter_id", 0))
			var frame_value := frame_by_id.get(emitter_id, {}) as Dictionary
			var definition_index := int(frame_value.get("definition_index", -1))
			var finite := true
			if definition_index >= 0 and definition_index < definitions.size():
				var definition := definitions[definition_index] as Dictionary
				finite = (int(definition.get("flags", 0)) \
						& PARTICLE_FLAG_FOREVER_EMIT) == 0
			value["group_id"] = int(group.get("group_id", 0))
			value["effect_name"] = String(group.get("effect_name", ""))
			value["render_domain"] = int(group.get("render_domain", 0))
			value["frame"] = frame_value
			value["render"] = bounds_by_id.get(emitter_id, {})
			value["finite"] = finite
			value["finished"] = not bool(value.get("emitting", false)) \
					and int(value.get("alive_particle_count", 0)) == 0
			result.append(value)
	return result

func get_unresolved_texture_names() -> PackedStringArray:
	if _renderer == null:
		return PackedStringArray()
	return _renderer.get_unresolved_texture_names()


func get_texture_dir() -> String:
	return _renderer.get_texture_dir() if _renderer != null else _texture_dir()


func get_preview_camera() -> Camera3D:
	return _camera


func get_emitter() -> Variant:
	var emitters := get_emitter_diagnostics()
	return emitters[0] if not emitters.is_empty() else null
