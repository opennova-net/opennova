class_name ParticlePreview
extends Control
## SubViewport host for the particle editor. Effects are previewed as one
## NovaParticleEmitter per referenced pdef, while individual particle defs use
## a single emitter.

const FlyCameraScript = preload("res://engine/fly_camera.gd")
const PARTICLE_SHADER = preload("res://modtools/particle/shaders/particle_billboard.gdshader")

var _viewport_container: SubViewportContainer
var _viewport: SubViewport
var _root: Node3D
var _camera: Camera3D
var _emitters_root: Node3D
var _emitters: Array[NovaParticleEmitter] = []
var _particle_file: NovaParticleFile
var _grid: MeshInstance3D
var _shader_material: ShaderMaterial


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_STOP
	clip_contents = true
	_build_viewport()
	set_process(true)


func _process(_delta: float) -> void:
	for emitter in _emitters:
		if emitter != null and emitter.get_alive_count() == 0:
			emitter.restart()
			_warm_emitter(emitter)


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
	_camera.look_at_from_position(Vector3(0.0, 4.0, 12.0), Vector3.ZERO)
	_root.add_child(_camera)

	_shader_material = ShaderMaterial.new()
	_shader_material.shader = PARTICLE_SHADER

	_emitters_root = Node3D.new()
	_emitters_root.name = "ParticleEmitters"
	_root.add_child(_emitters_root)

	_add_grid()


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
	if effect == null or _particle_file == null:
		return
	var pdefs: PackedStringArray = effect.pdefs
	for i in range(pdefs.size()):
		var def := _particle_file.find_particle(pdefs[i])
		if def != null:
			_add_emitter(def, i)


func set_particle_def(def: NovaParticleDef) -> void:
	clear_preview()
	if def != null:
		_add_emitter(def, 0)


func clear_preview() -> void:
	for emitter in _emitters:
		if emitter == null:
			continue
		if emitter.get_parent() != null:
			emitter.get_parent().remove_child(emitter)
		emitter.queue_free()
	_emitters.clear()


func _add_emitter(def: NovaParticleDef, index: int) -> NovaParticleEmitter:
	if _emitters_root == null:
		return null
	var emitter := NovaParticleEmitter.new()
	emitter.name = "ParticleEmitter%d" % index
	emitter.shader_material = _shader_material
	emitter.auto_advance = true
	emitter.seed = 1 + index * 101
	emitter.texture_dir = _texture_dir()
	if _particle_file != null:
		emitter.set_tables(_particle_file.get_tables())
	emitter.def = def
	_emitters_root.add_child(emitter)
	emitter.play()
	_warm_emitter(emitter)
	_emitters.append(emitter)
	return emitter


func _texture_dir() -> String:
	if _particle_file == null:
		return ""
	var source_path := String(_particle_file.get_source_path())
	if source_path.is_empty():
		return ""
	return source_path.get_base_dir()


func _warm_emitter(emitter: NovaParticleEmitter) -> void:
	if emitter == null:
		return
	for i in range(90):
		emitter.advance(1.0 / 60.0)
		if emitter.get_alive_count() > 0:
			return


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


func get_textured_layer_count() -> int:
	var total := 0
	for emitter in _emitters:
		if emitter != null:
			total += emitter.get_textured_layer_count()
	return total


func restart() -> void:
	for emitter in _emitters:
		if emitter != null:
			emitter.restart()
			_warm_emitter(emitter)


func get_emitter() -> NovaParticleEmitter:
	if _emitters.is_empty():
		return null
	return _emitters[0]
