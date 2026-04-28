class_name ParticlePreview
extends Control
## SubViewport host for the particle editor — mirrors objects-codex
## ObjectPreview's structure (SubViewportContainer + SubViewport + FlyCamera).
## Owns one NovaParticleEmitter that we re-bind each time the user picks a
## different ParticleDef in the inspector.

const FlyCameraScript = preload("res://engine/fly_camera.gd")
const PARTICLE_SHADER = preload("res://modtools/particle/shaders/particle_billboard.gdshader")

var _viewport_container: SubViewportContainer
var _viewport: SubViewport
var _root: Node3D
var _camera: Camera3D
var _emitter: NovaParticleEmitter
var _grid: MeshInstance3D
var _shader_material: ShaderMaterial


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_STOP
	clip_contents = true
	_build_viewport()


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

	_emitter = NovaParticleEmitter.new()
	_emitter.shader_material = _shader_material
	_emitter.auto_advance = true
	_root.add_child(_emitter)

	_add_grid()


func _add_grid() -> void:
	# Simple xz-plane reference grid centered at origin.
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


func set_particle_def(def: NovaParticleDef) -> void:
	if _emitter == null:
		return
	_emitter.def = def
	if def != null:
		_emitter.play()
	else:
		_emitter.stop()


func get_alive_count() -> int:
	if _emitter == null:
		return 0
	return _emitter.get_alive_count()


func restart() -> void:
	if _emitter != null:
		_emitter.restart()


func get_emitter() -> NovaParticleEmitter:
	return _emitter
