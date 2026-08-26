extends SceneTree

# Manual highest-quality renderer probe. This must run with a real Forward+
# D3D12 display driver, not --headless:
#   Godot --rendering-method gl_compatibility ...  (WRONG path)
#   Godot --rendering-method forward_plus ...      (selected path)
# It validates the complete live seam: six rendered SubViewports, retail's
# render-float -> Godot X/Z axis conjugation, Image -> Cubemap layer order,
# face orientation, shader sampling, and the gamma-byte 0x60 dim multiply.
# [orig: GTexture_RenderCubeMapFace @ 0x6864d0;
# update_environment_cubemap @ 0x6106a0; face callback @ 0x5c3700].

const FACE_DIRECTIONS := [
	Vector3.LEFT, Vector3.RIGHT, Vector3.DOWN, Vector3.UP,
	Vector3.FORWARD, Vector3.BACK,
]
const FACE_UPS := [
	Vector3.UP, Vector3.UP, Vector3.RIGHT, Vector3.LEFT,
	Vector3.UP, Vector3.UP,
]
const FACE_COLORS := [
	Color(1.0, 0.0, 0.0),
	Color(0.0, 1.0, 0.0),
	Color(0.0, 0.0, 1.0),
	Color(1.0, 1.0, 0.0),
	Color(1.0, 0.0, 1.0),
	Color(0.0, 1.0, 1.0),
]
const SAMPLE_COUNT := 18
const SAMPLE_CELL_WIDTH := 8
const SAMPLE_HEIGHT := 2


func _initialize() -> void:
	call_deferred("_run")


func _fail(message: String) -> void:
	printerr("[environment-cube-probe] FAIL: %s" % message)
	quit(1)


func _gamma_to_linear(value: float) -> float:
	if value <= 0.04045:
		return value / 12.92
	return pow((value + 0.055) / 1.055, 2.4)


func _make_marker(position: Vector3, gamma_color: Color,
		radius: float) -> MeshInstance3D:
	var marker := MeshInstance3D.new()
	var sphere := SphereMesh.new()
	sphere.radius = radius
	sphere.height = radius * 2.0
	sphere.radial_segments = 24
	sphere.rings = 12
	marker.mesh = sphere
	marker.position = position
	marker.layers = Water.VISUAL_LAYER_ENVIRONMENT_CAPTURE
	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.albedo_color = Color(
			_gamma_to_linear(gamma_color.r),
			_gamma_to_linear(gamma_color.g),
			_gamma_to_linear(gamma_color.b), 1.0)
	marker.material_override = material
	return marker


func _make_sampler(cube: Cubemap) -> SubViewport:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(SAMPLE_COUNT * SAMPLE_CELL_WIDTH, SAMPLE_HEIGHT)
	viewport.transparent_bg = false
	viewport.render_target_clear_mode = SubViewport.CLEAR_MODE_ALWAYS
	viewport.render_target_update_mode = SubViewport.UPDATE_DISABLED
	var rect := ColorRect.new()
	rect.position = Vector2.ZERO
	rect.size = Vector2(viewport.size)
	var shader := Shader.new()
	shader.code = """
shader_type canvas_item;
render_mode unshaded;
uniform samplerCube u_cube : filter_linear, repeat_disable;

vec3 sample_direction(int slot) {
	if (slot == 0) return vec3(-1.0, 0.0, 0.0);
	if (slot == 1) return vec3( 1.0, 0.0, 0.0);
	if (slot == 2) return vec3( 0.0,-1.0, 0.0);
	if (slot == 3) return vec3( 0.0, 1.0, 0.0);
	if (slot == 4) return vec3( 0.0, 0.0,-1.0);
	if (slot == 5) return vec3( 0.0, 0.0, 1.0);
	if (slot == 6) return normalize(vec3(-20.0, 7.0, 0.0));
	if (slot == 7) return normalize(vec3( 20.0, 7.0, 0.0));
	if (slot == 8) return normalize(vec3( 7.0,-20.0, 0.0));
	if (slot == 9) return normalize(vec3(-7.0, 20.0, 0.0));
	if (slot == 10) return normalize(vec3(0.0, 7.0,-20.0));
	if (slot == 11) return normalize(vec3(0.0, 7.0, 20.0));
	if (slot == 12) return normalize(vec3(-20.0, 0.0,-7.0));
	if (slot == 13) return normalize(vec3( 20.0, 0.0, 7.0));
	if (slot == 14) return normalize(vec3( 0.0,-20.0, 7.0));
	if (slot == 15) return normalize(vec3( 0.0, 20.0, 7.0));
	if (slot == 16) return normalize(vec3( 7.0, 0.0,-20.0));
	return normalize(vec3(-7.0, 0.0, 20.0));
}

void fragment() {
	int slot = min(int(floor(UV.x * 18.0)), 17);
	COLOR = texture(u_cube, sample_direction(slot));
}
"""
	var material := ShaderMaterial.new()
	material.shader = shader
	material.set_shader_parameter("u_cube", cube)
	rect.material = material
	viewport.add_child(rect)
	return viewport


func _sample_cell(image: Image, cell: int) -> Color:
	return image.get_pixel(cell * SAMPLE_CELL_WIDTH + (SAMPLE_CELL_WIDTH >> 1),
			SAMPLE_HEIGHT >> 1)


func _dominant_channel_matches(actual: Color, expected: Color) -> bool:
	var peak: float = maxf(actual.r, maxf(actual.g, actual.b))
	if peak < 0.08:
		return false
	if expected.r > 0.5 and actual.r < peak * 0.75:
		return false
	if expected.g > 0.5 and actual.g < peak * 0.75:
		return false
	if expected.b > 0.5 and actual.b < peak * 0.75:
		return false
	return true


func _channel_near(actual: float, expected: float) -> bool:
	return absf(actual - expected) <= 2.0 / 255.0


func _dimmed_color_matches(actual: Color, source_gamma: Color) -> bool:
	var dim: float = float(EnvironmentCubeCapture.SKY_DIM_BYTE) / 255.0
	return _channel_near(actual.r, source_gamma.r * dim) and \
			_channel_near(actual.g, source_gamma.g * dim) and \
			_channel_near(actual.b, source_gamma.b * dim)


func _run() -> void:
	if DisplayServer.get_name() == "headless":
		_fail("requires a real Forward+ RenderingDevice display driver")
		return
	if RenderingServer.get_current_rendering_method() != "forward_plus" or \
			RenderingServer.get_rendering_device() == null:
		_fail("requires Forward+ RenderingDevice, got %s/%s" % [
				RenderingServer.get_current_rendering_method(),
				RenderingServer.get_current_rendering_driver_name()])
		return

	var root := get_root()
	var marker_root := Node3D.new()
	root.add_child(marker_root)
	var origin := Vector3(0.0, 1.0, 0.0)
	for face in EnvironmentCubeCapture.FACE_COUNT:
		var direction: Vector3 = FACE_DIRECTIONS[face]
		var up: Vector3 = FACE_UPS[face]
		var right: Vector3 = direction.cross(up)
		marker_root.add_child(_make_marker(
				origin + direction * 20.0, FACE_COLORS[face], 2.2))
		marker_root.add_child(_make_marker(
				origin + direction * 20.0 + up * 7.0, Color.WHITE, 1.8))
		marker_root.add_child(_make_marker(
				origin + direction * 20.0 + right * 7.0,
				Color(1.0, 0.45, 0.0), 1.8))

	var capture := EnvironmentCubeCapture.new()
	root.add_child(capture)
	await process_frame
	capture.advance_frame(Vector3.ZERO)
	# The six UPDATE_ONCE faces render with this frame; the readbacks publish
	# on the next advance.
	await process_frame
	capture.advance_frame(Vector3.ZERO)
	if not capture.is_cube_ready():
		_fail("six rendered faces did not publish on the following advance")
		return
	var cube := capture.get_environment_cube()
	if cube == null or cube.get_layers() != EnvironmentCubeCapture.FACE_COUNT:
		_fail("published resource is not a six-layer Cubemap")
		return

	var sampler := _make_sampler(cube)
	root.add_child(sampler)
	sampler.render_target_update_mode = SubViewport.UPDATE_ONCE
	await RenderingServer.frame_post_draw
	var image := sampler.get_texture().get_image()
	if image == null or image.get_size() != Vector2i(
			SAMPLE_COUNT * SAMPLE_CELL_WIDTH, SAMPLE_HEIGHT):
		_fail("sampler viewport readback is unavailable (%s)" %
				[image.get_size() if image != null else Vector2i.ZERO])
		return

	for face in EnvironmentCubeCapture.FACE_COUNT:
		var center := _sample_cell(image, face)
		if not _dominant_channel_matches(center, FACE_COLORS[face]):
			_fail("face %d center axis/layer mismatch: %s" % [face, center])
			return
		if not _dimmed_color_matches(center, FACE_COLORS[face]):
			_fail("face %d center is not the exact 0x60 gamma-byte multiply: %s" %
					[face, center])
			return
		var up_sample := _sample_cell(image, 6 + face)
		if min(up_sample.r, min(up_sample.g, up_sample.b)) < 0.08:
			_fail("face %d vertical orientation mismatch: %s" % [face, up_sample])
			return
		if not _dimmed_color_matches(up_sample, Color.WHITE):
			_fail("face %d white marker changed color domain: %s" %
					[face, up_sample])
			return
		var right_sample := _sample_cell(image, 12 + face)
		if right_sample.r < 0.08 or right_sample.r < right_sample.g * 1.25 or \
				right_sample.b > right_sample.g * 0.5:
			_fail("face %d horizontal orientation mismatch: %s" % [face, right_sample])
			return
		if not _dimmed_color_matches(right_sample, Color(1.0, 0.45, 0.0)):
			_fail("face %d midtone is not gamma-byte dimmed: %s" %
					[face, right_sample])
			return

	print("[environment-cube-probe] PASS: six D3D-style faces, X/Z axis map, " +
			"Cubemap sampling orientation, and 0x60 gamma-byte dim are live")
	root.remove_child(sampler)
	sampler.free()
	root.remove_child(capture)
	capture.free()
	root.remove_child(marker_root)
	marker_root.free()
	quit(0)
