extends GameProbe

## environment_cube_capture: the highest-quality renderer proof for the live
## environment cube. Needs a real Forward+ D3D12 display driver (not
## --headless, not gl_compatibility): it validates the complete live seam,
## six rendered SubViewports, retail's render-float -> Godot X/Z axis
## conjugation, the RenderingDevice copy of each face into its cubemap layer
## (layer order, face orientation and the gamma-byte 0x60 dim multiply are
## applied by that blit, with no CPU readback), and shader sampling of the
## published TextureCubemapRD, on a ProbeStage of its own so the live world
## never enters the faces.
## [orig: GTexture_RenderCubeMapFace @ 0x6864d0;
## EnvCube_Update @ 0x6106a0; face callback @ 0x5c3700].

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
const STAGE_SIZE := Vector2i(256, 256)


func run(ctx: ProbeContext) -> ProbeVerdict:
	if GameRuntimeRoot.is_headless():
		return ProbeVerdict.failed("requires a real Forward+ RenderingDevice display driver")
	if RenderingServer.get_current_rendering_method() != "forward_plus" or \
			RenderingServer.get_rendering_device() == null:
		return ProbeVerdict.failed("requires Forward+ RenderingDevice, got %s/%s" % [
				RenderingServer.get_current_rendering_method(),
				RenderingServer.get_current_rendering_driver_name()])

	# A plain stage (no window settings mirrored): the six face viewports and
	# the cube conversion are the seam under test, at their own defaults.
	var stage := ProbeStage.create(ctx, STAGE_SIZE)
	var marker_root := Node3D.new()
	stage.add_scene(marker_root)
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
	stage.add_child(capture)
	await ctx.tree.process_frame
	capture.advance_frame(Vector3.ZERO)
	# The six UPDATE_ONCE faces render with this frame; the device copy
	# publishes on the following advance.
	await ctx.tree.process_frame
	capture.advance_frame(Vector3.ZERO)
	if not capture.is_cube_ready():
		return ProbeVerdict.failed("six rendered faces did not publish on the following advance (%s)" %
				capture.get_device_failure())
	var cube := capture.get_environment_cube()
	if cube == null or not (cube is TextureCubemapRD) or \
			cube.get_layers() != EnvironmentCubeCapture.FACE_COUNT or \
			not cube.texture_rd_rid.is_valid():
		return ProbeVerdict.failed("published resource is not a six-layer TextureCubemapRD")
	if capture.get_device_publish_count() != 1:
		return ProbeVerdict.failed("expected exactly one device publication, got %d" %
				capture.get_device_publish_count())

	var sampler := _make_sampler(cube)
	ctx.tree.root.add_child(sampler)
	ctx.defer_restore(func() -> void:
		if is_instance_valid(sampler):
			sampler.get_parent().remove_child(sampler)
			sampler.free())
	sampler.render_target_update_mode = SubViewport.UPDATE_ONCE
	await RenderingServer.frame_post_draw
	var image := sampler.get_texture().get_image()
	if image == null or image.get_size() != Vector2i(
			SAMPLE_COUNT * SAMPLE_CELL_WIDTH, SAMPLE_HEIGHT):
		return ProbeVerdict.failed("sampler viewport readback is unavailable (%s)" %
				[image.get_size() if image != null else Vector2i.ZERO])
	var sampler_png := ctx.artifact_dir.path_join("cube_samples.png")
	if image.save_png(sampler_png) == OK:
		ctx.artifact("cube_samples", sampler_png, "png")

	var faces: Array = []
	for face in EnvironmentCubeCapture.FACE_COUNT:
		var center := _sample_cell(image, face)
		var up_sample := _sample_cell(image, 6 + face)
		var right_sample := _sample_cell(image, 12 + face)
		faces.append({"face": face, "center": center, "up": up_sample, "right": right_sample})
		if not _dominant_channel_matches(center, FACE_COLORS[face]):
			return ProbeVerdict.failed("face %d center axis/layer mismatch: %s" % [face, center], {"faces": faces})
		if not _dimmed_color_matches(center, FACE_COLORS[face]):
			return ProbeVerdict.failed("face %d center is not the exact 0x60 gamma-byte multiply: %s" %
					[face, center], {"faces": faces})
		if min(up_sample.r, min(up_sample.g, up_sample.b)) < 0.08:
			return ProbeVerdict.failed("face %d vertical orientation mismatch: %s" % [face, up_sample], {"faces": faces})
		if not _dimmed_color_matches(up_sample, Color.WHITE):
			return ProbeVerdict.failed("face %d white marker changed color domain: %s" %
					[face, up_sample], {"faces": faces})
		if right_sample.r < 0.08 or right_sample.r < right_sample.g * 1.25 or \
				right_sample.b > right_sample.g * 0.5:
			return ProbeVerdict.failed("face %d horizontal orientation mismatch: %s" % [face, right_sample], {"faces": faces})
		if not _dimmed_color_matches(right_sample, Color(1.0, 0.45, 0.0)):
			return ProbeVerdict.failed("face %d midtone is not gamma-byte dimmed: %s" %
					[face, right_sample], {"faces": faces})

	ctx.log("PASS: six D3D-style faces, X/Z axis map, Cubemap sampling orientation, "
			+ "and 0x60 gamma-byte dim are live")
	return ProbeVerdict.passed("six faces, axis map, sampling orientation and the 0x60 gamma-byte dim are live",
			{"faces": faces})


# The face targets are HDR 2D: the texture stores exactly the numbers the
# admitted sky/celestial shaders write, which are retail's gamma-domain
# values (no terminal encode on this path). A marker therefore writes its
# gamma bytes raw, the way those shaders do; a StandardMaterial3D albedo
# would be linearized by the engine and come back as a double decode.
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
	var material := ShaderMaterial.new()
	var shader := Shader.new()
	shader.code = """
shader_type spatial;
render_mode unshaded, cull_disabled;
uniform vec3 u_gamma_color;
void fragment() {
	ALBEDO = u_gamma_color;
}
"""
	material.shader = shader
	material.set_shader_parameter("u_gamma_color",
			Vector3(gamma_color.r, gamma_color.g, gamma_color.b))
	marker.material_override = material
	return marker


func _make_sampler(cube: TextureCubemapRD) -> SubViewport:
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
