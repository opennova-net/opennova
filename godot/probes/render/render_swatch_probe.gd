extends GameProbe

## render_swatch: the render material swatch A/B driver (REN-1, the T2
## instrument, ADR 0023) as a game_probe tool. Every scene renders on a
## ProbeStage (an off-screen viewport mirroring the shell's rendering
## settings) so the live world never bleeds into a capture:
##   capture     one cell per unique object-shader key -> <prefix>_grid.png + manifest
##   composite   the DRAW-ORDER scenes (water bracket, sky ladder)
##   lighting    the object light-response matrix (RenderSwatchLightingModes)
##   channels    material-channel ownership (RenderSwatchLightingModes)
##   clip / projshadow / matchterrain / glow   the auxiliary passes (RenderSwatchPassModes)
##   compare     diff two grid captures exactly (headless is fine)
##   calibrate   the gamma-framebuffer proof: 256/256 terminal bytes, the blend domain
## POLICY (ADR 0023): baselines live under .scratch/golden/render/ (machine-
## local, never committed); the default comparison is EXACT; any tolerance
## is an investigation aid, never a gate. The composite ladder: scene 1 is the
## water bracket [orig: Terrain_RenderSceneWithReflection @ 0x5c93a0], scene 2
## the sky ladder [orig: Terrain_RenderSkyboxPass @ 0x610ac0;
## render_skybox_sun_glow @ 0x5c9714]; docs/render/render-order-re.md.

const WINDOW_SIZE := Vector2i(1280, 1024)
const CELL_WORLD := 2.4
const GRID_COLS := 10
const RENDER_MODES := ["capture", "composite", "lighting", "channels", "clip",
		"projshadow", "matchterrain", "glow", "calibrate"]

# Curated variants per tag: name, material_flags, emissive_type, glass, alpha byte.
const VARIANTS: Array = [
	["base", 0x00, 0, 0, 128],
	["two", 0x04, 0, 0, 128],
	["atest", 0x01, 0, 0, 128],
	["atinv", 0x03, 0, 0, 128],
	["emis", 0x00, 2, 0, 128],
]

var _ctx: ProbeContext
var _stage: ProbeStage
var _sink: RenderSwatchSupport.Sink


func run(ctx: ProbeContext) -> ProbeVerdict:
	_ctx = ctx
	_sink = RenderSwatchSupport.Sink.new(ctx)
	var mode := String(ctx.args.get("mode", ""))
	var out_dir := String(ctx.args.get("output_dir", "")).strip_edges()
	if out_dir.is_empty():
		out_dir = ctx.artifact_dir
	out_dir = ProjectSettings.globalize_path(out_dir).simplify_path()
	var prefix := String(ctx.args.get("prefix", "")).strip_edges()
	if prefix.is_empty():
		prefix = mode
	if mode == "compare":
		_compare_mode(String(ctx.args.get("a", "")), String(ctx.args.get("b", "")))
		return _sink.verdict("compare")
	if not RENDER_MODES.has(mode):
		return ProbeVerdict.failed("unknown mode %s" % mode)
	if DisplayServer.get_name() == "headless":
		return ProbeVerdict.failed("mode %s needs a real rasterizer; launch without --headless" % mode)
	_stage = ProbeStage.create(ctx, WINDOW_SIZE, ctx.viewport())
	match mode:
		"capture":
			await _capture_mode(out_dir, prefix)
		"composite":
			await _composite_mode(out_dir, prefix)
		"calibrate":
			await _calibrate_mode()
		"lighting":
			await RenderSwatchLightingModes.new(ctx, _stage, _sink).lighting_mode(out_dir, prefix)
		"channels":
			await RenderSwatchLightingModes.new(ctx, _stage, _sink).channel_mode(out_dir, prefix)
		"clip":
			await RenderSwatchPassModes.new(ctx, _stage, _sink).clip_mode(out_dir, prefix)
		"projshadow":
			await RenderSwatchPassModes.new(ctx, _stage, _sink).projshadow_mode(out_dir, prefix)
		"matchterrain":
			await RenderSwatchPassModes.new(ctx, _stage, _sink).matchterrain_mode(out_dir, prefix)
		"glow":
			await RenderSwatchPassModes.new(ctx, _stage, _sink).glow_mode(out_dir, prefix)
	return _sink.verdict(mode)


func _capture_mode(out_dir: String, prefix: String) -> void:
	DirAccess.make_dir_recursive_absolute(out_dir)
	_stage.set_stage_size(WINDOW_SIZE)

	var cache := ObjectShaderCache.get_singleton()
	var tags: PackedStringArray = cache.get_known_shader_tags()
	tags.append("VS_LEAVESWIND") # unknown-tag fallback swatch

	# One cell per unique shader key; first (tag, variant) reaching a key names it.
	var cells: Array[Dictionary] = []
	var seen_keys := {}
	for tag in tags:
		for variant in VARIANTS:
			var flags: int = variant[1]
			var em: int = variant[2]
			var gl: int = variant[3]
			var atb: int = variant[4]
			var key: int = cache.classify(tag, flags, em, gl, atb)
			if seen_keys.has(key):
				continue
			seen_keys[key] = true
			cells.append({
				"key": "%08x" % key, "tag": tag, "variant": variant[0],
				"flags": flags, "em": em, "gl": gl, "atb": atb,
			})

	var scene := Node3D.new()
	_stage.add_scene(scene)

	var world_env := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.12, 0.12, 0.14)
	world_env.environment = env
	scene.add_child(world_env)
	RenderSwatchSupport.add_framefx(scene, false)

	var diffuse := RenderSwatchSupport.make_diffuse_texture()
	var detail := RenderSwatchSupport.make_detail_texture()
	var normal := RenderSwatchSupport.make_normal_texture()

	var rows := int(ceil(float(cells.size()) / float(GRID_COLS)))
	for i in range(cells.size()):
		var cell: Dictionary = cells[i]
		var cx := (i % GRID_COLS) * CELL_WORLD
		var cy := -(i / GRID_COLS) * CELL_WORLD
		var material := RenderSwatchSupport.make_swatch_material(cache, cell, diffuse, detail, normal)

		var sphere := MeshInstance3D.new()
		var sphere_mesh := SphereMesh.new()
		sphere_mesh.radius = 0.62
		sphere_mesh.height = 1.24
		sphere.mesh = sphere_mesh
		sphere.material_override = material
		sphere.position = Vector3(cx - 0.55, cy, 0.0)
		scene.add_child(sphere)

		var quad := MeshInstance3D.new()
		var quad_mesh := QuadMesh.new()
		quad_mesh.size = Vector2(1.15, 1.15)
		quad.mesh = quad_mesh
		quad.material_override = material
		quad.position = Vector3(cx + 0.55, cy, 0.0)
		scene.add_child(quad)

	# Orthogonal camera framing the grid exactly: cell -> pixel rects stay a
	# linear function of the indices (the manifest records the mapping).
	var grid_w := GRID_COLS * CELL_WORLD
	var grid_h := rows * CELL_WORLD
	var camera := Camera3D.new()
	camera.projection = Camera3D.PROJECTION_ORTHOGONAL
	camera.size = grid_h
	camera.position = Vector3(grid_w * 0.5 - CELL_WORLD * 0.5, -grid_h * 0.5 + CELL_WORLD * 0.5, 20.0)
	camera.current = true
	scene.add_child(camera)

	for _i in range(RenderSwatchSupport.SETTLE_FRAMES):
		await _ctx.tree.process_frame

	var image := await _stage.capture_image(_ctx.tree)
	var png_path := out_dir.path_join("%s_grid.png" % prefix)
	var err := ERR_UNAVAILABLE
	if image != null:
		err = image.save_png(png_path)
	_sink.artifact("%s_grid" % prefix, png_path)

	var manifest := {
		"version": 1,
		"window": [WINDOW_SIZE.x, WINDOW_SIZE.y],
		"grid": {"cols": GRID_COLS, "rows": rows, "cell_world": CELL_WORLD},
		"cells": cells,
	}
	var mf := FileAccess.open(out_dir.path_join("%s_manifest.json" % prefix), FileAccess.WRITE)
	if mf != null:
		mf.store_string(JSON.stringify(manifest, "\t"))
		mf.close()
	_sink.artifact("%s_manifest" % prefix, out_dir.path_join("%s_manifest.json" % prefix))

	_sink.logv(["render_swatch_probe: cells=", cells.size(), " png=", png_path, " ok=", err == OK])
	_sink.quit(0 if err == OK else 1)


func _composite_mode(out_dir: String, prefix: String) -> void:
	DirAccess.make_dir_recursive_absolute(out_dir)
	_stage.set_stage_size(WINDOW_SIZE)

	var cache := ObjectShaderCache.get_singleton()
	# Exercise the real session seam: water plane at world height 0.
	cache.set_water_plane(0.0, true)

	var scene := Node3D.new()
	_stage.add_scene(scene)
	var world_env := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.12, 0.12, 0.14)
	world_env.environment = env
	scene.add_child(world_env)
	RenderSwatchSupport.add_framefx(scene, false)

	# Each layer: [label, color(rgba), rung, z]. Z runs TOWARD the camera
	# (+z nearer): every scene places its ladder-EARLIEST layer NEAREST, so
	# plain per-object depth sorting would compose the stack in exactly the
	# opposite order — the capture is green only through the rungs.
	var scenes: Array = [
		{
			"name": "water_bracket",
			"layers": [
				["alpha_below", Color(0.9, 0.15, 0.1, 0.75), cache.alpha_rung_for_height(-8.0), 2.0],
				["water", Color(0.1, 0.25, 0.9, 0.75), ObjectShaderCache.RENDER_RUNG_WATER, 0.0],
				["alpha_above", Color(0.1, 0.85, 0.2, 0.75), cache.alpha_rung_for_height(8.0), -2.0],
			],
		},
		{
			"name": "sky_ladder",
			"layers": [
				["stars", Color(0.95, 0.95, 0.95, 0.75), ObjectShaderCache.RENDER_RUNG_SKY_STARS, 2.0],
				["body", Color(0.95, 0.75, 0.1, 0.75), ObjectShaderCache.RENDER_RUNG_SKY_BODY, 0.0],
				["glow", Color(0.95, 0.4, 0.7, 0.75), ObjectShaderCache.RENDER_RUNG_SUN_GLOW, -2.0],
			],
		},
	]

	var manifest_scenes: Array = []
	const SCENE_SPACING := 4.0
	for s in range(scenes.size()):
		var spec: Dictionary = scenes[s]
		var base_x := s * SCENE_SPACING
		var recorded: Array = []
		for li in range(spec["layers"].size()):
			var layer: Array = spec["layers"][li]
			var quad := MeshInstance3D.new()
			var quad_mesh := QuadMesh.new()
			quad_mesh.size = Vector2(2.6, 2.6)
			quad.mesh = quad_mesh
			quad.material_override = RenderSwatchSupport.make_layer_material(layer[1], int(layer[2]))
			# Stagger so every pairwise overlap region is visible.
			quad.position = Vector3(base_x + li * 0.55, -li * 0.55, float(layer[3]))
			scene.add_child(quad)
			recorded.append({"label": layer[0], "rung": int(layer[2]), "z": float(layer[3])})
		manifest_scenes.append({"name": spec["name"], "layers": recorded})

	var grid_w := scenes.size() * SCENE_SPACING
	var camera := Camera3D.new()
	camera.projection = Camera3D.PROJECTION_ORTHOGONAL
	camera.size = 6.0
	camera.position = Vector3(grid_w * 0.5 - SCENE_SPACING * 0.5 + 0.55, -0.55, 20.0)
	camera.current = true
	scene.add_child(camera)

	for _i in range(RenderSwatchSupport.SETTLE_FRAMES):
		await _ctx.tree.process_frame

	var image := await _stage.capture_image(_ctx.tree)
	var png_path := out_dir.path_join("%s_grid.png" % prefix)
	var err := ERR_UNAVAILABLE
	if image != null:
		err = image.save_png(png_path)
	_sink.artifact("%s_grid" % prefix, png_path)

	var manifest := {
		"version": 1,
		"window": [WINDOW_SIZE.x, WINDOW_SIZE.y],
		"scenes": manifest_scenes,
	}
	var mf := FileAccess.open(out_dir.path_join("%s_manifest.json" % prefix), FileAccess.WRITE)
	if mf != null:
		mf.store_string(JSON.stringify(manifest, "\t"))
		mf.close()
	_sink.artifact("%s_manifest" % prefix, out_dir.path_join("%s_manifest.json" % prefix))
	cache.clear_water_plane()

	_sink.logv(["render_swatch_probe composite: scenes=", scenes.size(), " png=", png_path, " ok=", err == OK])
	_sink.quit(0 if err == OK else 1)


# Gamma-framebuffer proof (see the header). Renders 256 byte columns into the
# raw retail scene target, applies the production terminal transfer, and reads
# the display bytes back with zero tolerance. It then exercises the live
# Forward+ SRCALPHA/INVSRCALPHA and ONE/ONE paths before that transfer. The
# latter intentionally uses blend_add without writing ALPHA: every selected
# retail ONE/ONE object wrapper does the same, while the other blend_add users
# explicitly write ALPHA = 1.0.
func _calibrate_mode() -> void:
	_stage.set_stage_size(WINDOW_SIZE)

	var scene := Node3D.new()
	_stage.add_scene(scene)
	var world_env := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.0, 0.0, 0.0)
	world_env.environment = env
	scene.add_child(world_env)
	RenderSwatchSupport.add_framefx(scene, false)

	var quad := MeshInstance3D.new()
	var quad_mesh := QuadMesh.new()
	quad_mesh.size = Vector2(1.0, 1.0)
	quad.mesh = quad_mesh
	var material := ShaderMaterial.new()
	var shader := Shader.new()
	shader.set_code("""
shader_type spatial;
render_mode unshaded;
#include "res://shaders/color.gdshaderinc"
void fragment() {
	float b = floor(clamp(UV.x, 0.0, 0.999999) * 256.0) / 255.0;
	ALBEDO = scene_output(vec3(b));
}
""")
	material.shader = shader
	quad.material_override = material
	scene.add_child(quad)

	var camera := Camera3D.new()
	camera.projection = Camera3D.PROJECTION_ORTHOGONAL
	camera.size = 1.0
	camera.position = Vector3(0.0, 0.0, 10.0)
	camera.current = true
	scene.add_child(camera)

	for _i in range(RenderSwatchSupport.SETTLE_FRAMES):
		await _ctx.tree.process_frame

	var image := await _stage.capture_image(_ctx.tree)
	if image == null:
		_sink.error("render_swatch_probe calibrate: no viewport image (run windowed, not --headless)")
		_sink.quit(1)
		return
	image.convert(Image.FORMAT_RGBA8)

	# Ortho size is the VERTICAL extent; the 1x1 quad spans world x [-0.5, 0.5]
	# inside a horizontal extent of size * aspect.
	var w := image.get_width()
	var h := image.get_height()
	var aspect := float(w) / float(h)
	var half_extent_x := 0.5 * aspect
	var mid_y := h / 2
	var worst := 0
	var mismatches := 0
	var first_rows: Array[String] = []
	for b in range(256):
		var u := (float(b) + 0.5) / 256.0
		var world_x := -0.5 + u
		var px := int(round((world_x + half_extent_x) / (2.0 * half_extent_x) * float(w) - 0.5))
		var got := int(round(image.get_pixel(px, mid_y).r * 255.0))
		var dev := absi(got - b)
		if dev > 0:
			mismatches += 1
			if first_rows.size() < 16:
				first_rows.append("byte %d -> %d (dev %d)" % [b, got, dev])
			worst = maxi(worst, dev)
	if mismatches != 0:
		_sink.error("render_swatch_probe calibrate: FAIL - %d/256 bytes deviate (worst %d): %s" % [mismatches, worst, ", ".join(first_rows)])
		_sink.quit(1)
		return

	quad.visible = false
	RenderSwatchSupport.add_calibration_stack(scene, -0.3,
			RenderSwatchSupport.make_calibration_material("", 64, 1.0, false),
			RenderSwatchSupport.make_calibration_material("blend_mix, depth_draw_never", 192,
					128.0 / 255.0, true))
	RenderSwatchSupport.add_calibration_stack(scene, 0.3,
			RenderSwatchSupport.make_calibration_material("", 32, 1.0, false),
			RenderSwatchSupport.make_calibration_material("blend_add, depth_draw_never", 64,
					1.0, false))

	for _i in range(RenderSwatchSupport.SETTLE_FRAMES):
		await _ctx.tree.process_frame
	var blend_image := await _stage.capture_image(_ctx.tree)
	if blend_image == null:
		_sink.error("render_swatch_probe calibrate: no blend image")
		_sink.quit(1)
		return
	blend_image.convert(Image.FORMAT_RGBA8)
	var blend_rows: Array[String] = []
	var cases := [
		["SRCALPHA/INVSRCALPHA", -0.3, 128],
		["ONE/ONE", 0.3, 96],
	]
	for test_case in cases:
		var world_x: float = test_case[1]
		var px := int(round((world_x + half_extent_x) /
				(2.0 * half_extent_x) * float(w) - 0.5))
		var got := int(round(blend_image.get_pixel(px, mid_y).r * 255.0))
		var expected: int = test_case[2]
		if got != expected:
			blend_rows.append("%s expected %d, got %d" % [
					test_case[0], expected, got])
	if not blend_rows.is_empty():
		_sink.error("render_swatch_probe calibrate: FAIL - gamma framebuffer blend mismatch: %s" % "; ".join(blend_rows))
		_sink.quit(1)
		return

	_sink.logv(["render_swatch_probe calibrate: PASS - 256/256 terminal bytes; gamma framebuffer blends SRCALPHA/INVSRCALPHA=128 and ONE/ONE=96"])
	_sink.quit(0)


func _compare_mode(path_a: String, path_b: String) -> void:
	var a := Image.load_from_file(path_a)
	var b := Image.load_from_file(path_b)
	if a == null or b == null:
		_sink.error("render_swatch_probe: could not load images")
		_sink.quit(1)
		return
	if a.get_size() != b.get_size():
		_sink.error("render_swatch_probe: size mismatch %s vs %s" % [a.get_size(), b.get_size()])
		_sink.quit(1)
		return
	var metrics: Dictionary = a.compute_image_metrics(b, false)
	_sink.logv(["render_swatch_probe compare: max=", metrics.get("max"),
			" mean=", metrics.get("mean"), " rms=", metrics.get("root_mean_squared"),
			" psnr=", metrics.get("peak_snr")])
	if float(metrics.get("max", 1.0)) == 0.0:
		_sink.logv(["render_swatch_probe compare: IDENTICAL"])
		_sink.quit(0)
		return
	# Attribute the delta per manifest cell so a reviewer sees WHICH material
	# changed, not an anonymous pixel count. The manifest sits next to
	# capture A as <prefix>_manifest.json.
	var manifest := RenderSwatchSupport.load_manifest_for(path_a)
	if manifest.is_empty():
		_sink.logv(["render_swatch_probe compare: DIFFERS (no manifest found for per-cell report)"])
		_sink.quit(1)
		return
	var report := RenderSwatchSupport.per_cell_diff(a, b, manifest)
	var total: int = report["total_diff_pixels"]
	_sink.logv(["render_swatch_probe compare: DIFFERS — ", total, " differing pixel(s) in ",
			report["cells"].size(), " cell(s):"])
	for entry in report["cells"]:
		_sink.logv(["  cell %d key=%s %s/%s: %d px (max delta %d)" % [
				entry["index"], entry["key"], entry["tag"], entry["variant"],
				entry["pixels"], entry["max_delta"]]])
	_sink.logv(["render_swatch_probe compare: a visual delta either carries its witness",
			" citation (ADR 0023) or is a regression."])
	_sink.quit(1)
