extends SceneTree

# Render material swatch A/B driver (REN-1, the T2 instrument — ADR 0023).
#
# Renders a deterministic grid of material swatches — one cell per unique
# object-shader key reachable from the canonical shader-tag table
# (NovaObjectShaderCache.get_known_shader_tags()) under a curated flag/emissive
# variant set — using code-generated textures only (asset-free), and saves one
# grid PNG + a JSON manifest. Compare mode diffs two captures exactly.
# Not collected by GUT (probe suffix). Asset-gated world baselines ride
# env_visual_baseline_probe.gd; ordering composites join at REN-3.
#
# POLICY (ADR 0023): baselines live in .scratch/golden/render/ (machine-local,
# never committed). The default comparison is EXACT (max channel delta 0);
# any tolerance is an investigation aid, never a gate — a real visual delta
# either carries its witness citation or is a regression.
#
# Use (windowed — screenshots need a real rasterizer, NOT --headless):
#   capture:   "$GODOT_BIN" --path godot -s res://tests/render_swatch_probe.gd -- capture <out_dir> [prefix]
#   composite: "$GODOT_BIN" --path godot -s res://tests/render_swatch_probe.gd -- composite <out_dir> [prefix]
#   compare:   "$GODOT_BIN" --path godot -s res://tests/render_swatch_probe.gd -- compare <a.png> <b.png>
#   calibrate: "$GODOT_BIN" --path godot -s res://tests/render_swatch_probe.gd -- calibrate
#
# The calibrate mode (the D-RMAT-7 identity proof) renders the full 0..255
# byte gradient through nova_gamma_to_linear() and asserts the captured bytes
# equal the input — proving the shader-side gamma encode is the exact inverse
# of THIS engine build's output blit on THIS driver (the gamma-space
# convention, godot/shaders/nova_color.gdshaderinc). Run it after any Godot
# version or renderer change.
#
# The composite mode (REN-3) renders the DRAW-ORDER scenes: overlapping
# translucent quads whose depths are arranged AGAINST the witnessed order, so
# only the ported priority ladder (libs/renderer/render_order via
# NovaObjectShaderCache) produces the correct stack — Godot's per-object
# depth sort alone would compose them backwards. Scene 1 is the water bracket
# (below-water alpha under the water surface under above-water alpha
# [orig: Terrain_RenderSceneWithReflection @ 0x5c93a0]); scene 2 is the sky
# ladder (star field under bodies, the sun glow on top of world alpha
# [orig: Terrain_RenderSkyboxPass @ 0x610ac0; render_skybox_sun_glow
# @ 0x5c9714]). docs/render/render-order-re.md.

const WINDOW_SIZE := Vector2i(1280, 1024)
const SETTLE_FRAMES := 24
const CELL_WORLD := 2.4
const GRID_COLS := 10

# Curated variants per tag: name, material_flags, emissive_type, glass, alpha byte.
const VARIANTS: Array = [
	["base", 0x00, 0, 0, 128],
	["two", 0x04, 0, 0, 128],
	["atest", 0x01, 0, 0, 128],
	["atinv", 0x03, 0, 0, 128],
	["emis", 0x00, 2, 0, 128],
]


func _initialize() -> void:
	call_deferred("_run")


func _run() -> void:
	var args := OS.get_cmdline_user_args()
	if args.size() >= 2 and args[0] == "capture":
		await _capture_mode(args[1], args[2] if args.size() >= 3 else "swatch")
	elif args.size() >= 2 and args[0] == "composite":
		await _composite_mode(args[1], args[2] if args.size() >= 3 else "composite")
	elif args.size() >= 3 and args[0] == "compare":
		_compare_mode(args[1], args[2])
	elif args.size() >= 1 and args[0] == "calibrate":
		await _calibrate_mode()
	else:
		push_error("render_swatch_probe: usage -- capture <out_dir> [prefix] | composite <out_dir> [prefix] | compare <a.png> <b.png> | calibrate")
		quit(1)


func _capture_mode(out_dir: String, prefix: String) -> void:
	DirAccess.make_dir_recursive_absolute(out_dir)
	get_root().get_window().size = WINDOW_SIZE

	var cache := NovaObjectShaderCache.get_singleton()
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
	root.add_child(scene)

	var world_env := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.12, 0.12, 0.14)
	world_env.environment = env
	scene.add_child(world_env)

	var diffuse := _make_diffuse_texture()
	var detail := _make_detail_texture()
	var normal := _make_normal_texture()

	var rows := int(ceil(float(cells.size()) / float(GRID_COLS)))
	for i in range(cells.size()):
		var cell: Dictionary = cells[i]
		var cx := (i % GRID_COLS) * CELL_WORLD
		var cy := -(i / GRID_COLS) * CELL_WORLD
		var material := _make_swatch_material(cache, cell, diffuse, detail, normal)

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

	for _i in range(SETTLE_FRAMES):
		await process_frame

	var image := root.get_viewport().get_texture().get_image()
	var png_path := out_dir.path_join("%s_grid.png" % prefix)
	var err := ERR_UNAVAILABLE
	if image != null:
		err = image.save_png(png_path)

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

	print("render_swatch_probe: cells=", cells.size(), " png=", png_path, " ok=", err == OK)
	quit(0 if err == OK else 1)


func _composite_mode(out_dir: String, prefix: String) -> void:
	DirAccess.make_dir_recursive_absolute(out_dir)
	get_root().get_window().size = WINDOW_SIZE

	var cache := NovaObjectShaderCache.get_singleton()
	# Exercise the real session seam: water plane at world height 0.
	cache.set_water_split_height(0.0)

	var scene := Node3D.new()
	root.add_child(scene)
	var world_env := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.12, 0.12, 0.14)
	world_env.environment = env
	scene.add_child(world_env)

	# Each layer: [label, color(rgba), rung, z]. Z runs TOWARD the camera
	# (+z nearer): every scene places its ladder-EARLIEST layer NEAREST, so
	# plain per-object depth sorting would compose the stack in exactly the
	# opposite order — the capture is green only through the rungs.
	var scenes: Array = [
		{
			"name": "water_bracket",
			"layers": [
				["alpha_below", Color(0.9, 0.15, 0.1, 0.75), cache.alpha_rung_for_height(-8.0), 2.0],
				["water", Color(0.1, 0.25, 0.9, 0.75), NovaObjectShaderCache.RENDER_RUNG_WATER, 0.0],
				["alpha_above", Color(0.1, 0.85, 0.2, 0.75), cache.alpha_rung_for_height(8.0), -2.0],
			],
		},
		{
			"name": "sky_ladder",
			"layers": [
				["stars", Color(0.95, 0.95, 0.95, 0.75), NovaObjectShaderCache.RENDER_RUNG_SKY_STARS, 2.0],
				["body", Color(0.95, 0.75, 0.1, 0.75), NovaObjectShaderCache.RENDER_RUNG_SKY_BODY, 0.0],
				["glow", Color(0.95, 0.4, 0.7, 0.75), NovaObjectShaderCache.RENDER_RUNG_SUN_GLOW, -2.0],
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
			quad.material_override = _make_layer_material(layer[1], int(layer[2]))
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

	for _i in range(SETTLE_FRAMES):
		await process_frame

	var image := root.get_viewport().get_texture().get_image()
	var png_path := out_dir.path_join("%s_grid.png" % prefix)
	var err := ERR_UNAVAILABLE
	if image != null:
		err = image.save_png(png_path)

	var manifest := {
		"version": 1,
		"window": [WINDOW_SIZE.x, WINDOW_SIZE.y],
		"scenes": manifest_scenes,
	}
	var mf := FileAccess.open(out_dir.path_join("%s_manifest.json" % prefix), FileAccess.WRITE)
	if mf != null:
		mf.store_string(JSON.stringify(manifest, "\t"))
		mf.close()
	cache.clear_water_split_height()

	print("render_swatch_probe composite: scenes=", scenes.size(), " png=", png_path, " ok=", err == OK)
	quit(0 if err == OK else 1)


# D-RMAT-7 identity proof (see the header). Renders 256 byte columns through
# nova_gamma_to_linear() into the default (LINEAR-tonemap) 3D pipeline and
# reads the framebuffer back: displayed byte must equal computed byte for
# every input. Zero tolerance — a deviation means the include's curve is not
# the exact inverse of this engine build's blit and must be re-derived.
func _calibrate_mode() -> void:
	get_root().get_window().size = WINDOW_SIZE

	var scene := Node3D.new()
	root.add_child(scene)
	var world_env := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.0, 0.0, 0.0)
	world_env.environment = env
	scene.add_child(world_env)

	var quad := MeshInstance3D.new()
	var quad_mesh := QuadMesh.new()
	quad_mesh.size = Vector2(1.0, 1.0)
	quad.mesh = quad_mesh
	var material := ShaderMaterial.new()
	var shader := Shader.new()
	shader.set_code("""
shader_type spatial;
render_mode unshaded;
#include "res://shaders/nova_color.gdshaderinc"
void fragment() {
	float b = floor(clamp(UV.x, 0.0, 0.999999) * 256.0) / 255.0;
	ALBEDO = nova_gamma_to_linear(vec3(b));
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

	for _i in range(SETTLE_FRAMES):
		await process_frame

	var image := root.get_viewport().get_texture().get_image()
	if image == null:
		push_error("render_swatch_probe calibrate: no viewport image (run windowed, not --headless)")
		quit(1)
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
	if mismatches == 0:
		print("render_swatch_probe calibrate: PASS - 256/256 bytes identity through nova_gamma_to_linear + output blit")
		quit(0)
	else:
		push_error("render_swatch_probe calibrate: FAIL - %d/256 bytes deviate (worst %d): %s" % [mismatches, worst, ", ".join(first_rows)])
		quit(1)


func _make_layer_material(color: Color, rung: int) -> ShaderMaterial:
	var material := ShaderMaterial.new()
	var shader := Shader.new()
	shader.set_code("""
shader_type spatial;
render_mode unshaded, blend_mix, depth_draw_never, cull_disabled;
uniform vec4 u_color;
void fragment() {
	ALBEDO = u_color.rgb;
	ALPHA = u_color.a;
}
""")
	material.shader = shader
	material.set_shader_parameter("u_color", color)
	material.render_priority = rung
	return material


func _compare_mode(path_a: String, path_b: String) -> void:
	var a := Image.load_from_file(path_a)
	var b := Image.load_from_file(path_b)
	if a == null or b == null:
		push_error("render_swatch_probe: could not load images")
		quit(1)
		return
	if a.get_size() != b.get_size():
		push_error("render_swatch_probe: size mismatch %s vs %s" % [a.get_size(), b.get_size()])
		quit(1)
		return
	var metrics: Dictionary = a.compute_image_metrics(b, false)
	print("render_swatch_probe compare: max=", metrics.get("max"),
			" mean=", metrics.get("mean"), " rms=", metrics.get("root_mean_squared"),
			" psnr=", metrics.get("peak_snr"))
	if float(metrics.get("max", 1.0)) == 0.0:
		print("render_swatch_probe compare: IDENTICAL")
		quit(0)
		return
	# Attribute the delta per manifest cell so a reviewer sees WHICH material
	# changed, not an anonymous pixel count. The manifest sits next to
	# capture A as <prefix>_manifest.json.
	var manifest := _load_manifest_for(path_a)
	if manifest.is_empty():
		print("render_swatch_probe compare: DIFFERS (no manifest found for per-cell report)")
		quit(1)
		return
	var report := _per_cell_diff(a, b, manifest)
	var total: int = report["total_diff_pixels"]
	print("render_swatch_probe compare: DIFFERS — ", total, " differing pixel(s) in ",
			report["cells"].size(), " cell(s):")
	for entry in report["cells"]:
		print("  cell %d key=%s %s/%s: %d px (max delta %d)" % [
				entry["index"], entry["key"], entry["tag"], entry["variant"],
				entry["pixels"], entry["max_delta"]])
	print("render_swatch_probe compare: a visual delta either carries its witness",
			" citation (ADR 0023) or is a regression.")
	quit(1)


func _load_manifest_for(png_path: String) -> Dictionary:
	var manifest_path := png_path.replace("_grid.png", "_manifest.json")
	if not FileAccess.file_exists(manifest_path):
		return {}
	var parsed = JSON.parse_string(FileAccess.get_file_as_string(manifest_path))
	return parsed if parsed is Dictionary else {}


func _per_cell_diff(a: Image, b: Image, manifest: Dictionary) -> Dictionary:
	# Invert the capture's orthogonal projection: vertical world span
	# grid.rows * cell_world maps onto the window height, same scale on X,
	# camera centered on the grid (see _capture_mode).
	var grid: Dictionary = manifest["grid"]
	var cells: Array = manifest["cells"]
	var cols := int(grid["cols"])
	var rows := int(grid["rows"])
	var cell_world := float(grid["cell_world"])
	var w := a.get_width()
	var h := a.get_height()
	var scale := float(h) / (rows * cell_world)
	var vis_w_world := float(w) / scale
	var grid_w := cols * cell_world
	var grid_h := rows * cell_world
	var world_left := (grid_w * 0.5 - cell_world * 0.5) - vis_w_world * 0.5
	var world_top := (-grid_h * 0.5 + cell_world * 0.5) + grid_h * 0.5

	var out_cells: Array = []
	var total := 0
	for i in range(cells.size()):
		var cx := (i % cols) * cell_world
		var cy := -(i / cols) * cell_world
		var px0 := clampi(int((cx - cell_world * 0.5 - world_left) * scale), 0, w)
		var px1 := clampi(int((cx + cell_world * 0.5 - world_left) * scale), 0, w)
		var py0 := clampi(int((world_top - (cy + cell_world * 0.5)) * scale), 0, h)
		var py1 := clampi(int((world_top - (cy - cell_world * 0.5)) * scale), 0, h)
		var pixels := 0
		var max_delta := 0
		for y in range(py0, py1):
			for x in range(px0, px1):
				var ca := a.get_pixel(x, y)
				var cb := b.get_pixel(x, y)
				var d := maxi(maxi(absi(ca.r8 - cb.r8), absi(ca.g8 - cb.g8)),
						maxi(absi(ca.b8 - cb.b8), absi(ca.a8 - cb.a8)))
				if d > 0:
					pixels += 1
					max_delta = maxi(max_delta, d)
		if pixels > 0:
			var cell: Dictionary = cells[i]
			out_cells.append({"index": i, "key": cell["key"], "tag": cell["tag"],
					"variant": cell["variant"], "pixels": pixels, "max_delta": max_delta})
			total += pixels
	out_cells.sort_custom(func(x, y): return int(x["pixels"]) > int(y["pixels"]))
	return {"total_diff_pixels": total, "cells": out_cells}


func _make_swatch_material(cache, cell: Dictionary, diffuse: Texture2D, detail: Texture2D, normal: Texture2D) -> ShaderMaterial:
	# Mirrors nova_object_model.gd _create_material's uniform setup so the
	# swatch pins the same owner state the runtime binds.
	var material := ShaderMaterial.new()
	var key: int = cache.classify(cell["tag"], cell["flags"], cell["em"], cell["gl"], cell["atb"])
	material.shader = cache.get_shader_for_key(key)
	material.set_shader_parameter("u_diffuse", diffuse)
	material.set_shader_parameter("u_detail", detail)
	material.set_shader_parameter("u_normal_map", normal)
	if (cell["flags"] & NovaObjectShaderCache.MATERIAL_FLAG_ALPHA_TEST) != 0:
		material.set_shader_parameter("u_alpha_test_threshold", float(cell["atb"]) / 255.0)
		material.set_shader_parameter("u_alpha_test_invert",
				1.0 if (cell["flags"] & NovaObjectShaderCache.MATERIAL_FLAG_ALPHA_INVERT) != 0 else 0.0)
	else:
		material.set_shader_parameter("u_alpha_test_threshold", 0.0)
		material.set_shader_parameter("u_alpha_test_invert", 0.0)
	material.set_shader_parameter("u_reflect_color", Color(0.7, 0.8, 0.9, 0.35))
	material.set_shader_parameter("u_uv_transform_u", Vector3(1.0, 0.0, 0.0))
	material.set_shader_parameter("u_uv_transform_v", Vector3(0.0, 1.0, 0.0))
	material.set_shader_parameter("u_rgb_mod", Vector3.ONE)
	material.set_shader_parameter("u_alpha_mod", 1.0)
	material.set_shader_parameter("u_emissive", 1.0 if cell["em"] == 2 else 0.0)
	material.set_shader_parameter("u_local_light_count", 0)
	# Pin the TIME-driven flag wind sway: A/B captures happen at arbitrary
	# times, and a swaying vertex displacement is capture noise, not a
	# material delta. The Flag family still renders (family lighting, key,
	# blend) — only the animation amplitude is zeroed.
	material.set_shader_parameter("u_wind_amount", 0.0)
	return material


func _make_diffuse_texture() -> ImageTexture:
	# 8px checker with a vertical alpha ramp: blend and alpha-test variants
	# read differently by construction.
	var image := Image.create(64, 64, false, Image.FORMAT_RGBA8)
	for y in range(64):
		var alpha := int(255.0 * float(y) / 63.0)
		for x in range(64):
			var checker := ((x / 8) + (y / 8)) % 2 == 0
			var rgb := Color8(200, 60, 40, alpha) if checker else Color8(240, 220, 200, alpha)
			image.set_pixel(x, y, rgb)
	return ImageTexture.create_from_image(image)


func _make_detail_texture() -> ImageTexture:
	var image := Image.create(32, 32, false, Image.FORMAT_RGBA8)
	for y in range(32):
		for x in range(32):
			var checker := ((x / 4) + (y / 4)) % 2 == 0
			var v := 128 if checker else 220
			image.set_pixel(x, y, Color8(v, v, v, 255))
	return ImageTexture.create_from_image(image)


func _make_normal_texture() -> ImageTexture:
	# Flat tangent normal with one hemispherical bump in the center.
	var image := Image.create(64, 64, false, Image.FORMAT_RGBA8)
	for y in range(64):
		for x in range(64):
			var dx := (float(x) - 32.0) / 20.0
			var dy := (float(y) - 32.0) / 20.0
			var r2 := dx * dx + dy * dy
			var n := Vector3(0, 0, 1)
			if r2 < 1.0:
				n = Vector3(dx, dy, sqrt(1.0 - r2)).normalized()
			image.set_pixel(x, y, Color(n.x * 0.5 + 0.5, n.y * 0.5 + 0.5, n.z * 0.5 + 0.5, 1.0))
	return ImageTexture.create_from_image(image)
