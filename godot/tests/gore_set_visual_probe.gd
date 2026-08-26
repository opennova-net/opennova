extends Node3D

## Visual evidence for D-PTL-24: the blood puff actually RENDERS.
##
## The catalog probe (`gore_set_catalog_probe.gd`) proves Effect_AmHitBody resolves to
## its five authored emitters instead of the one-emitter `stockeffect` clone. This one
## proves pixels: it mounts the retail catalog, captures a clean baseline, then submits
## Effect_AmHitBody at three anchors the way `_route_round_impacts` does for a flesh hit
## and captures again. A non-trivial changed-pixel count is the proof.
##
## Needs a real rasterizer (NOT --headless):
##   NW_RESOURCE_DIR=<retail JO install> NOVA_GORE_PROBE_DIR=<out dir> \
##     "$GODOT_BIN" --path godot res://tests/gore_set_visual_probe.tscn

const OUTPUT_ENV := "NOVA_GORE_PROBE_DIR"
const RESOURCE_ENV := "NW_RESOURCE_DIR"
const TICK_DT := 1.0 / 62.5
const CAPTURE_TICKS := 5
const EFFECT_NAME := "Effect_AmHitBody"
# Roughly torso height, spread across the frame — the anchors a burst into a
# standing target would produce.
const HIT_POINTS := [
	Vector3(-2.6, 1.4, 0.0),
	Vector3(0.0, 1.6, 0.0),
	Vector3(2.6, 1.3, 0.0),
]

var _fx: EffectWorld
var _caption: Label
var _before: Image


func _ready() -> void:
	DisplayServer.window_set_size(Vector2i(960, 540))
	_build_stage()

	var resource_dir := OS.get_environment(RESOURCE_ENV).strip_edges()
	var root := ResourceRoot.new()
	if resource_dir.is_empty() or root.mount_runtime(resource_dir, "", false, "jo") != OK:
		push_error("[gore-visual] could not mount %s=%s" % [RESOURCE_ENV, resource_dir])
		get_tree().quit(1)
		return

	_fx = EffectWorld.new()
	_fx.name = "RetailEffectWorld"
	add_child(_fx)
	var effect_count := _fx.load_from_resource_root(root)
	print("[gore-visual] catalog: %d effects across %d files (gore set: %s)"
			% [effect_count, _fx.file_count(), root.particle_extension()])
	if effect_count <= 0:
		push_error("[gore-visual] mounted no particle effects")
		get_tree().quit(1)
		return

	await ProbeClock.settle(get_tree(), 8)
	_before = await _capture()

	_caption.text = "AFTER  •  3 x %s at flesh-hit anchors" % EFFECT_NAME
	var handle := _fx.intern_effect(EFFECT_NAME)
	var spawned := 0
	for point_v in HIT_POINTS:
		if _fx.spawn_effect_by_handle(handle, point_v as Vector3, Vector3.FORWARD):
			spawned += 1
	for _i in range(CAPTURE_TICKS):
		_fx.advance_fixed_tick(TICK_DT)
	var after := await _capture()

	var emitters := 0
	for group_v in _fx.get_debug_group_report():
		emitters += ((group_v as Dictionary).get("emitters", []) as Array).size()
	var changed := _changed_pixels(_before, after)
	print("[gore-visual] handle %d, spawned %d/%d, %d live emitter(s), %d changed pixel(s)"
			% [handle, spawned, HIT_POINTS.size(), emitters, changed])

	var out_dir := OS.get_environment(OUTPUT_ENV).strip_edges()
	if not out_dir.is_empty():
		DirAccess.make_dir_recursive_absolute(out_dir)
		_before.save_png(out_dir.path_join("gore_before.png"))
		after.save_png(out_dir.path_join("gore_after.png"))
		print("[gore-visual] wrote gore_before.png / gore_after.png to %s" % out_dir)

	# The stockeffect clone would be one emitter of grey; the authored blood is five
	# emitters of dark red (color1..4 = 139,0,0). Both a real emitter count and real
	# moved pixels are required.
	var ok := spawned == HIT_POINTS.size() and emitters >= 15 and changed > 500
	print("[gore-visual] VERDICT: %s" % ("PASS — the blood puff renders" if ok else "FAIL"))
	get_tree().quit(0 if ok else 1)


func _build_stage() -> void:
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 1.8, 7.5)
	camera.fov = 55.0
	camera.current = true
	add_child(camera)
	camera.look_at(Vector3(0.0, 1.5, 0.0), Vector3.UP)

	var world_environment := WorldEnvironment.new()
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	# A light, neutral ground: dark red particles need a pale backdrop to read.
	environment.background_color = Color("c8ccd2")
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color("ffffff")
	environment.ambient_light_energy = 1.0
	world_environment.environment = environment
	add_child(world_environment)

	var key := DirectionalLight3D.new()
	key.light_energy = 1.2
	key.rotation_degrees = Vector3(-50.0, -30.0, 0.0)
	add_child(key)

	var layer := CanvasLayer.new()
	add_child(layer)
	_caption = Label.new()
	_caption.text = "BEFORE  •  retail catalog mounted, no effect submitted"
	_caption.position = Vector2(24, 24)
	_caption.add_theme_font_size_override("font_size", 22)
	_caption.add_theme_color_override("font_color", Color.BLACK)
	layer.add_child(_caption)


func _capture() -> Image:
	_fx.render_frame()
	await get_tree().process_frame
	await RenderingServer.frame_post_draw
	return get_viewport().get_texture().get_image()


func _changed_pixels(reference: Image, candidate: Image) -> int:
	if reference == null or candidate == null \
			or reference.get_size() != candidate.get_size():
		return -1
	var changed := 0
	# Skip the caption band: the BEFORE/AFTER label change is intentional and must
	# not be scored as particle visibility.
	for y_value in range(90, reference.get_height()):
		for x_value in range(reference.get_width()):
			var before := reference.get_pixel(x_value, y_value)
			var after := candidate.get_pixel(x_value, y_value)
			if absf(before.r - after.r) + absf(before.g - after.g) \
					+ absf(before.b - after.b) > 0.12:
				changed += 1
	return changed
