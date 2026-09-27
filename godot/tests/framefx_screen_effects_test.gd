extends GutTest

# FrameFX's screen effects on the device: the retail rows the terminal
# compositor runs over the finished frame. The engine header
# runtime/renderer/frame_fx_effects.h carries the witnesses and the CPU
# references, and ctest renderer_frame_fx_effects pins the planner; these
# cases render a controlled beauty frame through a real FrameFx node and read
# the displayed pixels back. Beauty values are gamma-domain numbers, so a
# readback pixel is the FrameFX output value.

const BEAUTY_MASK := 14126081


func _beauty_material(color: Color) -> ShaderMaterial:
	var shader := Shader.new()
	shader.code = """
shader_type spatial;
render_mode unshaded, depth_draw_opaque, cull_disabled;
uniform vec3 u_beauty;

void fragment() {
	if (CAMERA_VISIBLE_LAYERS == 14126081u) {
		ALBEDO = u_beauty;
	} else {
		discard;
	}
}
"""
	var material := ShaderMaterial.new()
	material.shader = shader
	material.set_shader_parameter("u_beauty", Vector3(color.r, color.g, color.b))
	return material


# A viewport whose orthographic camera maps one world unit to one pixel, so a
# quad spans exactly the pixel rect it is given.
func _view(size: Vector2i) -> Dictionary:
	var viewport := SubViewport.new()
	viewport.size = size
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var environment_resource := Environment.new()
	environment_resource.background_mode = Environment.BG_COLOR
	environment_resource.background_color = Color.BLACK
	environment_resource.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	environment_resource.glow_enabled = false
	var environment := WorldEnvironment.new()
	environment.environment = environment_resource
	viewport.add_child(environment)
	var camera := Camera3D.new()
	camera.current = true
	camera.projection = Camera3D.PROJECTION_ORTHOGONAL
	camera.size = float(size.y)
	viewport.add_child(camera)
	var fx := FrameFx.new()
	viewport.add_child(fx)
	return {"viewport": viewport, "fx": fx, "size": size}


func _rect(view: Dictionary, rect: Rect2, color: Color) -> void:
	var size: Vector2i = view["size"]
	var quad := MeshInstance3D.new()
	var mesh := QuadMesh.new()
	mesh.size = rect.size
	quad.mesh = mesh
	quad.material_override = _beauty_material(color)
	quad.position = Vector3(rect.position.x + rect.size.x * 0.5 - size.x * 0.5,
			size.y * 0.5 - (rect.position.y + rect.size.y * 0.5), -2.0)
	(view["viewport"] as SubViewport).add_child(quad)


func _render(view: Dictionary) -> Image:
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	return (view["viewport"] as SubViewport).get_texture().get_image()


func _device_ready(view: Dictionary) -> bool:
	for _frame in 4:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	var report: Dictionary = (view["fx"] as FrameFx).get_backend_report()
	if not bool(report.get("rd_available", false)):
		pending("RenderingDevice unavailable under this Godot renderer")
		return false
	return true


# FrameFx.set_view_effects(red_word, camera_mode, local_dead, in_session,
# death_elapsed_ticks, thermal_view, monitor_view, nvg_active, death_screen,
# binoculars_view_active, scoped_selector, sighted_selector).
func _plan(view: Dictionary, red_word: int, local_dead: bool, elapsed: int,
		thermal: bool, monitor: bool, nvg: bool, scoped := false, sighted := false) -> void:
	var fx := view["fx"] as FrameFx
	fx.set_view_effects(red_word, 0, local_dead, false, elapsed, thermal, monitor,
			nvg, false, false, scoped, sighted)
	fx.advance_screen_effects()


func _near(color: Color, expected: Vector3, tolerance: float) -> bool:
	return abs(color.r - expected.x) <= tolerance \
			and abs(color.g - expected.y) <= tolerance \
			and abs(color.b - expected.z) <= tolerance


# [orig: FrameFX_CreateRenderTargets @0x583c7f..0x583c97]: the capture
# is the highest power of two not above the frame side minus one.
func test_capture_is_the_power_of_two_floor_of_the_frame() -> void:
	var view := _view(Vector2i(200, 100))
	if not await _device_ready(view):
		return
	var report: Dictionary = (view["fx"] as FrameFx).get_backend_report()
	assert_eq(report.get("capture_size", Vector2i.ZERO), Vector2i(128, 64))


# Type 8 then its scanlines [orig: FrameFX_ThermalView @0x584390; ps @0x7D7C48;
# FrameFX_ScanlineOverlay @0x583a60]: a 0.5 grey becomes (L^2, L + 0.05, L^2) at
# L = 0.498 (the 8-bit capture), then even rows multiply by 2 x 0x60 texel
# x 128/255 = 0.756 and odd rows by 1.008 .. 1.188.
func test_thermal_view_inverts_the_luma_into_green_then_scans() -> void:
	var view := _view(Vector2i(128, 96))
	_rect(view, Rect2(0, 0, 128, 96), Color(0.5, 0.5, 0.5))
	if not await _device_ready(view):
		return
	var fx := view["fx"] as FrameFx
	fx.init_mission_textures()
	_plan(view, 0, false, 0, true, false, false)
	var image := _render(view)
	var even := image.get_pixel(64, 40)
	var odd := image.get_pixel(64, 41)
	assert_true(_near(even, Vector3(0.248 * 0.756, 0.548 * 0.756, 0.248 * 0.756), 0.025),
			"even rows: the thermal colour times the dim scanline: %s" % even)
	assert_between(odd.g, 0.53, 0.66, "odd rows keep or brighten the thermal green: %s" % odd)
	assert_between(odd.r, 0.24, 0.31)
	assert_gt(odd.g, even.g + 0.1, "alternate rows differ")


# Type 9 alone [orig: FrameFX_ApplyWeaponViewEffect @0x5845dd -> FrameFX_ScanlineOverlay @0x583a60].
func test_monitor_view_scans_the_frame() -> void:
	var view := _view(Vector2i(128, 96))
	_rect(view, Rect2(0, 0, 128, 96), Color(0.5, 0.5, 0.5))
	if not await _device_ready(view):
		return
	var fx := view["fx"] as FrameFx
	fx.init_mission_textures()
	_plan(view, 0, false, 0, false, true, false)
	var image := _render(view)
	assert_true(_near(image.get_pixel(30, 20), Vector3(0.378, 0.378, 0.378), 0.02),
			"even rows: 0.5 x 0.756")
	assert_between(image.get_pixel(30, 21).g, 0.49, 0.61, "odd rows: 0.5 x 1.008 .. 1.188")


# The NVG view [orig: Render_ProcessMainSceneFrame @0x5ca516..0x5ca5cd /
# @0x5ca6ab..0x5ca73a; ps @0x7DC3D8 / @0x7DC228]: a 0.3 grey tints to
# (0.12, 0.74, 0.12); the toggle frame's glow starts from the green clear, so
# the first frame is saturated green, and the glow then settles at 6.25x its
# per-pass source (0.84 decay, two passes a frame).
func test_nvg_flashes_green_on_the_toggle_frame_then_settles() -> void:
	var view := _view(Vector2i(128, 96))
	_rect(view, Rect2(0, 0, 128, 96), Color(0.3, 0.3, 0.3))
	if not await _device_ready(view):
		return
	var fx := view["fx"] as FrameFx
	_plan(view, 0, false, 0, false, false, true)
	var image := _render(view)
	var report := fx.get_backend_report()
	assert_true(bool(report.get("nvg_composited", false)))
	assert_eq(int(report.get("nvg_glow_clears", -1)), 1)
	var flash := image.get_pixel(64, 48)
	assert_true(_near(flash, Vector3(0.200, 1.0, 0.200), 0.03),
			"tint + the decayed green clear: %s" % flash)
	var edge := image.get_pixel(127, 48)
	assert_true(_near(edge, Vector3.ZERO, 0.01),
			"the composite quad stops at W - 1: the last column keeps the black clear")
	for _frame in 40:
		_plan(view, 0, false, 0, false, false, true)
		image = _render(view)
	var settled := image.get_pixel(64, 48)
	assert_true(_near(settled, Vector3(0.390, 1.0, 0.390), 0.04),
			"tint + the steady glow: %s" % settled)
	assert_eq(int(fx.get_backend_report().get("nvg_glow_clears", -1)), 1,
			"only the toggle frame clears the persistent glow")


# The NVG view's Scoped arm [orig: NVG_DrawScopedLens @0x5d1d10;
# NVG_RenderSceneToTarget @0x5d0a0e..0x5d0eb4]: the frame clears black and the
# lens draws instead of the full-screen composite. On a 256 x 96 surface the
# ring is (95 >> 3) + (95 >> 1) = 58 about (127, 47): the disc (to 0.71 x 58)
# carries the tint + the four quarter-strength glow passes -- the full
# composite's sum at the centre -- the ring (to 1.5 x 58 = 87) the doubled
# polar unwrap through the ring colours, and beyond it the black clear stays.
# A white scene unwraps to 4 x 0.247 = 0.988 grey; 50 px out the inner/outer
# colours interpolate to (0.079, 0.079, 0.107), so the ring reads
# 2 x (2 x 0.988 x d) x 0.988 = (0.31, 0.31, 0.42).
func test_nvg_scoped_arm_draws_the_lens_over_a_black_clear() -> void:
	var view := _view(Vector2i(256, 96))
	_rect(view, Rect2(0, 0, 256, 96), Color.WHITE)
	if not await _device_ready(view):
		return
	var fx := view["fx"] as FrameFx
	_plan(view, 0, false, 0, false, false, true, true)
	var image := _render(view)
	var report := fx.get_backend_report()
	assert_true(bool(report.get("nvg_lens_drawn", false)), "the Scoped arm draws the lens")
	assert_false(bool(report.get("nvg_composited", true)),
			"the lens replaces the full-screen composite")
	var centre := image.get_pixel(127, 47)
	assert_true(centre.r >= 0.38 and centre.g >= 0.97,
			"the disc: the white tint (0.4, 1, 0.4) plus the glow: %s" % centre)
	var ring := image.get_pixel(177, 47)
	assert_true(_near(ring, Vector3(0.31, 0.31, 0.42), 0.06),
			"the ring's doubled polar unwrap: %s" % ring)
	assert_gt(ring.b, ring.r + 0.05, "the ring colours lean blue")
	var outside := image.get_pixel(10, 47)
	assert_true(_near(outside, Vector3.ZERO, 0.01),
			"beyond 1.5 x the ring the black clear stays: %s" % outside)
	# Off the Scoped arm the full-screen composite returns.
	_plan(view, 0, false, 0, false, false, true, false)
	image = _render(view)
	report = fx.get_backend_report()
	assert_false(bool(report.get("nvg_lens_drawn", true)))
	assert_true(bool(report.get("nvg_composited", false)))
	assert_gt(image.get_pixel(10, 47).g, 0.9, "the composite covers the whole frame")


# The NVG view's Sighted arm [orig: NVG_RenderSceneToTarget @0x5d08cb..0x5d0952 ->
# HUD_DrawWeaponSightOverlays @0x4dce00]: the SIGHTS card draws INTO the 512
# scene before its glow and tint, so a white row over the scene's left half
# tints to (0.4, 1, 0.4) where the 0.3 grey right half tints to
# (0.12, 0.74, 0.12); off the arm the published card does not draw.
func test_nvg_sighted_arm_draws_the_card_into_the_scene() -> void:
	var view := _view(Vector2i(128, 96))
	_rect(view, Rect2(0, 0, 128, 96), Color(0.3, 0.3, 0.3))
	if not await _device_ready(view):
		return
	var fx := view["fx"] as FrameFx
	var image := Image.create(8, 8, false, Image.FORMAT_RGBA8)
	image.fill(Color.WHITE)
	var textures: Array[Texture2D] = [ImageTexture.create_from_image(image)]
	fx.set_nvg_sights_card(textures, PackedFloat32Array([0.0, 0.0, 256.0, 512.0]),
			PackedInt32Array([0]))
	_plan(view, 0, false, 0, false, false, true, false, true)
	var frame := _render(view)
	var report := fx.get_backend_report()
	assert_eq(int(report.get("nvg_sights_drawn", -1)), 1, "the card's one row draws")
	assert_true(bool(report.get("nvg_composited", false)), "the full-screen composite")
	var left := frame.get_pixel(20, 48)
	var right := frame.get_pixel(100, 48)
	assert_gt(left.r, 0.38, "the card tints white: %s" % left)
	assert_gt(left.r, right.r + 0.15, "the card sits in the scene, left of centre: %s / %s"
			% [left, right])
	_plan(view, 0, false, 0, false, false, true)
	frame = _render(view)
	assert_eq(int(fx.get_backend_report().get("nvg_sights_drawn", -1)), 0,
			"off the Sighted arm the card stays out of the scene")


# Type 1 [orig: FrameFX_DamageBlur @0x5830f0]: one hit (red 120, p = 1)
# replaces the frame with the 256-square downsample through a half-texel
# 60-degree blur, softening a hard edge; red 30 (p = 0.25) cross-fades that
# image over the frame at alpha 0.5.
func test_damage_blur_softens_the_frame_then_cross_fades() -> void:
	var view := _view(Vector2i(1024, 512))
	_rect(view, Rect2(0, 0, 512, 512), Color.WHITE)
	if not await _device_ready(view):
		return
	_plan(view, 0, false, 0, false, false, false)
	var sharp := _render(view)
	assert_almost_eq(sharp.get_pixel(509, 256).r, 1.0, 0.01)
	assert_almost_eq(sharp.get_pixel(514, 256).r, 0.0, 0.01)
	_plan(view, 120, false, 0, false, false, false)
	var blurred := _render(view)
	var inside: float = blurred.get_pixel(509, 256).r
	var outside: float = blurred.get_pixel(514, 256).r
	assert_lt(inside, 0.97, "the edge's lit side softens")
	assert_gt(outside, 0.03, "the edge's dark side picks up light")
	assert_almost_eq(blurred.get_pixel(100, 256).r, 1.0, 0.02)
	assert_almost_eq(blurred.get_pixel(900, 256).r, 0.0, 0.02)
	_plan(view, 30, false, 0, false, false, false)
	var faded := _render(view)
	assert_almost_eq(faded.get_pixel(509, 256).r, 0.5 * inside + 0.5, 0.03,
			"alpha 2p = 0.5 over the sharp frame")
	assert_almost_eq(faded.get_pixel(514, 256).r, 0.5 * outside, 0.03)


# Type 4 [orig: FrameFX_DeathBlur @0x5833a0]: 200 ticks past the
# 30-tick hold (d = 13.3, three fan passes) the radial fan leaves the centre
# column sharp and smears a stripe at the border.
func test_death_blur_keeps_the_centre_and_blurs_the_border() -> void:
	var view := _view(Vector2i(1024, 512))
	_rect(view, Rect2(504, 0, 16, 512), Color.WHITE)
	_rect(view, Rect2(996, 0, 16, 512), Color.WHITE)
	if not await _device_ready(view):
		return
	_plan(view, 0, true, 230, false, false, false)
	var image := _render(view)
	assert_gt(image.get_pixel(512, 256).r, 0.9, "the centre keeps its stripe")
	assert_lt(image.get_pixel(1004, 256).r, 0.5, "the border stripe smears inward")
