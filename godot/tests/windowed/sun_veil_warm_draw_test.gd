extends GutTest

# The sun veil past the mission load's one forced frame (D-NET-1315 and its follow): the frame
# mounts the shell's HUD and submits the overlay's draw at once (HudOverlay.submit_draw_now), the
# view veil's quad among it, and its end clears the overlay's own item and takes the mount off the
# tree, so the first live frame draws the one quad retail draws [orig:
# Environment_ApplySunVeilAndExposureStopdown @ 0x5ad8b0 from Render_ProcessMainSceneFrame
# @ 0x5cac4b]. Godot clears an item at a redraw only once it drew the item itself, so a quad the
# frame left would stand under the live one: a held veil alpha of 0.5 over black would read 191,
# two quads, where one reads 128. The overlay clears the veil's item at each of its draws
# (D-NET-1384 put the veil on it). Renders through the real device: a tests/windowed/ script, run by
# `scripts/test_godot.sh --windowed` (a headless run pends it).

const VEIL_ALPHA := &"opennova_sun_veil_alpha"
const SIZE := Vector2i(64, 64)


func after_each() -> void:
	RenderingServer.global_shader_parameter_set(VEIL_ALPHA, 0.0)


func _render(viewport: SubViewport) -> Image:
	await get_tree().process_frame
	await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	return viewport.get_texture().get_image()


func test_the_first_live_frame_draws_one_veil_quad_past_the_forced_frame() -> void:
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	RenderingServer.global_shader_parameter_set(VEIL_ALPHA, 0.5)
	var viewport := SubViewport.new()
	viewport.size = SIZE
	viewport.disable_3d = true
	viewport.transparent_bg = false
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var black := ColorRect.new()
	black.color = Color.BLACK
	black.size = Vector2(SIZE)
	viewport.add_child(black)
	var hud := HudOverlay.new()
	hud.size = Vector2(SIZE)
	# The load's forced frame: the overlay mounts, its draw (the veil's quad among it) is submitted
	# at once, the frame draws, and its end clears the overlay's item and takes the mount off the
	# tree (GameHudPresenter's hud_pipeline_warm handler) before the queued redraw ever runs.
	viewport.add_child(hud)
	hud.submit_draw_now()
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	RenderingServer.canvas_item_clear(hud.get_canvas_item())
	viewport.remove_child(hud)
	await get_tree().process_frame
	# The first frame with a local player mounts it for good.
	viewport.add_child(hud)
	var image: Image = await _render(viewport)
	var veil := image.get_pixel(32, 32).r8
	assert_true(absi(veil - 128) <= 2,
			"one veil quad at alpha 0.5 over black reads 128 (two read 191): %d" % veil)
	viewport.remove_child(hud)
	hud.free()
