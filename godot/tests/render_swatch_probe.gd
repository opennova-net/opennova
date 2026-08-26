extends SceneTree

# Render material swatch A/B driver (REN-1, the T2 instrument — ADR 0023).
#
# Renders a deterministic grid of material swatches — one cell per unique
# object-shader key reachable from the canonical shader-tag table
# (ObjectShaderCache.get_known_shader_tags()) under a curated flag/emissive
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
#   lighting:  "$GODOT_BIN" --path godot -s res://tests/render_swatch_probe.gd -- lighting <out_dir> [prefix]
#   channels:  "$GODOT_BIN" --path godot -s res://tests/render_swatch_probe.gd -- channels <out_dir> [prefix]
#   clip:      "$GODOT_BIN" --path godot -s res://tests/render_swatch_probe.gd -- clip <out_dir> [prefix]
#   projshadow:"$GODOT_BIN" --path godot -s res://tests/render_swatch_probe.gd -- projshadow <out_dir> [prefix]
#   matchterrain:"$GODOT_BIN" --path godot -s res://tests/render_swatch_probe.gd -- matchterrain <out_dir> [prefix]
#   glow:      "$GODOT_BIN" --path godot -s res://tests/render_swatch_probe.gd -- glow <out_dir> [prefix]
#   compare:   "$GODOT_BIN" --path godot -s res://tests/render_swatch_probe.gd -- compare <a.png> <b.png>
#   calibrate: "$GODOT_BIN" --path godot -s res://tests/render_swatch_probe.gd -- calibrate
#
# The calibrate mode renders the full 0..255 byte gradient into the retail
# gamma scene target and asserts that the terminal compositor plus display output
# returns the same bytes. It is the zero-tolerance proof for the hard cutover.
#
# The composite mode (REN-3) renders the DRAW-ORDER scenes: overlapping
# translucent quads whose depths are arranged AGAINST the witnessed order, so
# only the ported priority ladder (engine/runtime/renderer/render_order via
# ObjectShaderCache) produces the correct stack — Godot's per-object
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
	elif args.size() >= 2 and args[0] == "lighting":
		await _lighting_mode(args[1], args[2] if args.size() >= 3 else "lighting")
	elif args.size() >= 2 and args[0] == "channels":
		await _channel_mode(args[1], args[2] if args.size() >= 3 else "channels")
	elif args.size() >= 2 and args[0] == "clip":
		await _clip_mode(args[1], args[2] if args.size() >= 3 else "clip")
	elif args.size() >= 2 and args[0] == "projshadow":
		await _projshadow_mode(args[1], args[2] if args.size() >= 3 else "projshadow")
	elif args.size() >= 2 and args[0] == "matchterrain":
		await _matchterrain_mode(args[1], args[2] if args.size() >= 3 else "matchterrain")
	elif args.size() >= 2 and args[0] == "glow":
		await _glow_mode(args[1], args[2] if args.size() >= 3 else "glow")
	elif args.size() >= 3 and args[0] == "compare":
		_compare_mode(args[1], args[2])
	elif args.size() >= 1 and args[0] == "calibrate":
		await _calibrate_mode()
	else:
		push_error("render_swatch_probe: usage -- capture <out_dir> [prefix] | composite <out_dir> [prefix] | lighting <out_dir> [prefix] | channels <out_dir> [prefix] | clip <out_dir> [prefix] | projshadow <out_dir> [prefix] | matchterrain <out_dir> [prefix] | glow <out_dir> [prefix] | compare <a.png> <b.png> | calibrate")
		quit(1)


func _add_framefx(scene: Node3D, q3_enabled: bool) -> FrameFx:
	var renderer := FrameFx.new()
	renderer.visible = q3_enabled
	scene.add_child(renderer)
	return renderer


func _bind_production_object_resources(material: ShaderMaterial,
		implementation: String) -> bool:
	if implementation != "phong_map":
		return true
	# The retail 256x256 PhongMap is a generated system texture, not an authored
	# material slot. Exercise the same binding used by live object models.
	var cache := ObjectShaderCache.get_singleton()
	var key := cache.classify("VS_SKBUMPPHONGOBJ", 0, 0, 0, 128)
	var configured := ShaderMaterial.new()
	cache.configure_material_for_key(configured, key)
	var phong_map = configured.get_shader_parameter("u_phong_map")
	if phong_map == null:
		return false
	material.set_shader_parameter("u_phong_map", phong_map)
	return material.get_shader_parameter("u_phong_map") != null


func _capture_mode(out_dir: String, prefix: String) -> void:
	DirAccess.make_dir_recursive_absolute(out_dir)
	get_root().get_window().size = WINDOW_SIZE

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
	root.add_child(scene)

	var world_env := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.12, 0.12, 0.14)
	world_env.environment = env
	scene.add_child(world_env)
	_add_framefx(scene, false)

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

	var cache := ObjectShaderCache.get_singleton()
	# Exercise the real session seam: water plane at world height 0.
	cache.set_water_plane(0.0, true)

	var scene := Node3D.new()
	root.add_child(scene)
	var world_env := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.12, 0.12, 0.14)
	world_env.environment = env
	scene.add_child(world_env)
	_add_framefx(scene, false)

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
	cache.clear_water_plane()

	print("render_swatch_probe composite: scenes=", scenes.size(), " png=", png_path, " ok=", err == OK)
	quit(0 if err == OK else 1)


# Gamma-framebuffer proof (see the header). Renders 256 byte columns into the
# raw retail scene target, applies the production terminal transfer, and reads
# the display bytes back with zero tolerance. It then exercises the live
# Forward+ SRCALPHA/INVSRCALPHA and ONE/ONE paths before that transfer. The
# latter intentionally uses blend_add without writing ALPHA: every selected
# retail ONE/ONE object wrapper does the same, while the other blend_add users
# explicitly write ALPHA = 1.0.
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
	_add_framefx(scene, false)

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
	ALBEDO = nova_scene_output(vec3(b));
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
	if mismatches != 0:
		push_error("render_swatch_probe calibrate: FAIL - %d/256 bytes deviate (worst %d): %s" % [mismatches, worst, ", ".join(first_rows)])
		quit(1)
		return

	quad.visible = false
	_add_calibration_stack(scene, -0.3,
			_make_calibration_material("", 64, 1.0, false),
			_make_calibration_material("blend_mix, depth_draw_never", 192,
					128.0 / 255.0, true))
	_add_calibration_stack(scene, 0.3,
			_make_calibration_material("", 32, 1.0, false),
			_make_calibration_material("blend_add, depth_draw_never", 64,
					1.0, false))

	for _i in range(SETTLE_FRAMES):
		await process_frame
	var blend_image := root.get_viewport().get_texture().get_image()
	if blend_image == null:
		push_error("render_swatch_probe calibrate: no blend image")
		quit(1)
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
		push_error("render_swatch_probe calibrate: FAIL - gamma framebuffer blend mismatch: %s" % "; ".join(blend_rows))
		quit(1)
		return

	print("render_swatch_probe calibrate: PASS - 256/256 terminal bytes; gamma framebuffer blends SRCALPHA/INVSRCALPHA=128 and ONE/ONE=96")
	quit(0)


func _make_calibration_material(render_modes: String, byte_value: int,
		alpha: float, writes_alpha: bool) -> ShaderMaterial:
	var material := ShaderMaterial.new()
	var shader := Shader.new()
	var mode_suffix := ", " + render_modes if not render_modes.is_empty() else ""
	var alpha_line := "\n\tALPHA = u_alpha;" if writes_alpha else ""
	shader.set_code("""
shader_type spatial;
render_mode unshaded, cull_disabled%s;
#include "res://shaders/nova_color.gdshaderinc"
uniform float u_byte;
uniform float u_alpha;
void fragment() {
	ALBEDO = nova_scene_output(vec3(u_byte));%s
}
""" % [mode_suffix, alpha_line])
	material.shader = shader
	material.set_shader_parameter("u_byte", float(byte_value) / 255.0)
	material.set_shader_parameter("u_alpha", alpha)
	return material


func _add_calibration_stack(scene: Node3D, world_x: float,
		background: ShaderMaterial, foreground: ShaderMaterial) -> void:
	for layer in [[background, 0.0], [foreground, 1.0]]:
		var mesh := MeshInstance3D.new()
		var quad := QuadMesh.new()
		quad.size = Vector2(0.45, 0.45)
		mesh.mesh = quad
		mesh.material_override = layer[0]
		mesh.position = Vector3(world_x, 0.0, layer[1])
		scene.add_child(mesh)


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
	cache.configure_material_for_key(material, key)
	material.set_shader_parameter("u_diffuse", diffuse)
	material.set_shader_parameter("u_detail", detail)
	material.set_shader_parameter("u_normal_map", normal)
	if (cell["flags"] & ObjectShaderCache.MATERIAL_FLAG_ALPHA_TEST) != 0:
		material.set_shader_parameter("u_alpha_test_threshold", float(cell["atb"]) / 255.0)
		material.set_shader_parameter("u_alpha_test_invert",
				1.0 if (cell["flags"] & ObjectShaderCache.MATERIAL_FLAG_ALPHA_INVERT) != 0 else 0.0)
	else:
		material.set_shader_parameter("u_alpha_test_threshold", 0.0)
		material.set_shader_parameter("u_alpha_test_invert", 0.0)
	material.set_shader_parameter("u_reflect_color", Color(0.7, 0.8, 0.9, 0.35))
	material.set_shader_parameter("u_uv_transform_u", Vector3(1.0, 0.0, 0.0))
	material.set_shader_parameter("u_uv_transform_v", Vector3(0.0, 1.0, 0.0))
	material.set_shader_parameter("u_rgb_mod", Vector3.ONE)
	material.set_shader_parameter("u_alpha_mod", 1.0)
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


# Raster proof for the D-RMAT-5 / D-RLIT object-lighting contract. One sphere
# per static technique is rendered through nine witnessed input states. This
# tests pixels rather than uniform delivery. The per-technique validation
# ledger says which authored NORMAL passes consume directional, oriented
# hemisphere, flat ambient, and gameplay point light; the probe rejects both
# missing and invented responses. The final state renders the same geometry
# through a production MultiMesh atlas row and requires exact RGBA8 parity with
# the live per-instance point-light route.
func _lighting_mode(out_dir: String, prefix: String) -> void:
	DirAccess.make_dir_recursive_absolute(out_dir)
	get_root().get_window().size = Vector2i(1280, 720)
	# Flag.fx is authored against FloatTicks. Freezing scaled time makes its
	# reconstructed normal identical across the response-state captures.
	Engine.time_scale = 0.0

	var parsed = JSON.parse_string(FileAccess.get_file_as_string(
			"res://shaders/object/pipeline_manifest.json"))
	if not parsed is Dictionary:
		push_error("render_swatch_probe lighting: object pipeline manifest did not parse")
		quit(1)
		return
	var audited = JSON.parse_string(FileAccess.get_file_as_string(
			"res://shaders/object/technique_validation.json"))
	if not audited is Dictionary:
		push_error("render_swatch_probe lighting: technique validation did not parse")
		quit(1)
		return
	var response_by_enum := {}
	var raster_checks_by_enum := {}
	for audited_entry in audited.get("techniques", []):
		var audited_enum := str(audited_entry.get("engine_enum", ""))
		response_by_enum[audited_enum] = \
				audited_entry.get("responses", {})
		raster_checks_by_enum[audited_enum] = \
				audited_entry.get("raster_checks", {})

	var scene := Node3D.new()
	root.add_child(scene)
	var world_env := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.015, 0.015, 0.02)
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	world_env.environment = env
	scene.add_child(world_env)
	_add_framefx(scene, false)

	var diffuse := _make_lighting_diffuse_texture()
	var detail := _make_lighting_detail_texture()
	var tangent_normal := _make_lighting_normal_texture()
	var object_normal := _make_lighting_object_normal_texture()
	var entries: Array[Dictionary] = []
	var techniques: Array = parsed.get("techniques", [])
	const COLS := 6
	const SPACING := 2.2
	# The world lighting block is pass-global (opennova_light_block_*), not
	# material state: publish this probe's own register before the first
	# capture so no other writer's values leak into it. The response states
	# rewrite the direction and hemisphere pair per capture.
	RenderingServer.global_shader_parameter_set("opennova_light_block_gain",
			Vector3(0.5, 0.5, 0.5))
	RenderingServer.global_shader_parameter_set("opennova_fog_enabled", false)
	for i in range(techniques.size()):
		var technique: Dictionary = techniques[i]
		var policies: Array = technique.get("policies", [])
		var policy := "opaque" if policies.has("opaque") else str(policies[0])
		var path := "res://shaders/object/%s/%s.gdshader" % [
				technique["directory"], policy]
		var shader := load(path) as Shader
		if shader == null:
			push_error("render_swatch_probe lighting: could not load %s" % path)
			quit(1)
			return
		var material := ShaderMaterial.new()
		material.shader = shader
		if not _bind_production_object_resources(material,
				str(technique["implementation"])):
			push_error("render_swatch_probe lighting: production resources unavailable for %s" % path)
			quit(1)
			return
		material.set_shader_parameter("u_diffuse", diffuse)
		material.set_shader_parameter("u_detail", detail)
		material.set_shader_parameter("u_normal_map",
				object_normal if technique["normal"] == "object_uv1" else tangent_normal)
		material.set_shader_parameter("u_uv_transform_u", Vector3(1.0, 0.0, 0.0))
		material.set_shader_parameter("u_uv_transform_v", Vector3(0.0, 1.0, 0.0))
		material.set_shader_parameter("u_rgb_mod", Vector3.ONE)
		material.set_shader_parameter("u_alpha_mod", 1.0)
		material.set_shader_parameter("u_reflect_color", Color(0.7, 0.8, 0.9, 0.25))
		material.set_shader_parameter("u_local_light_count", 0)

		var sphere := MeshInstance3D.new()
		var mesh := SphereMesh.new()
		mesh.radius = 0.72
		mesh.height = 1.44
		sphere.mesh = mesh
		sphere.material_override = material
		sphere.position = Vector3((i % COLS) * SPACING,
				-(i / COLS) * SPACING, 0.0)
		scene.add_child(sphere)

		# Static mission ROBJ draws share this exact compiled material, but carry
		# their selected light row through INSTANCE_CUSTOM.x. Keep the twin at
		# the same world transform and switch visibility between captures so the
		# comparison isolates only the light-delivery route.
		var static_instances := MultiMesh.new()
		static_instances.transform_format = MultiMesh.TRANSFORM_3D
		static_instances.use_custom_data = true
		static_instances.mesh = mesh
		static_instances.instance_count = 1
		static_instances.set_instance_transform(0,
				Transform3D(Basis.IDENTITY, sphere.position))
		static_instances.set_instance_custom_data(0,
				Color(float(i + 1), 0.0, 0.0, 0.0))
		var static_sphere := MultiMeshInstance3D.new()
		static_sphere.multimesh = static_instances
		static_sphere.material_override = material
		static_sphere.visible = false
		scene.add_child(static_sphere)
		var implementation := str(technique["implementation"])
		var engine_enum := str(technique["engine_enum"])
		var responses = response_by_enum.get(engine_enum, {})
		if not responses is Dictionary or responses.size() != 4:
			push_error("render_swatch_probe lighting: missing response audit for %s" % engine_enum)
			quit(1)
			return
		entries.append({
			"name": engine_enum,
			"implementation": implementation,
			"path": path,
			"responses": responses,
			"raster_checks": raster_checks_by_enum.get(engine_enum, {}),
			"material": material,
			"mesh": sphere,
			"static_mesh": static_sphere,
		})

	# The runtime LightScene owns and publishes this RGBAF layout. Constructing
	# the same payload here keeps the raster proof focused on the shader and
	# MultiMesh transport; LightScene selection/scatter has separate native and
	# director integration tests.
	var static_point_atlas := _make_static_point_light_atlas(entries)
	RenderingServer.global_shader_parameter_set(
			"opennova_static_point_light_rows", static_point_atlas)

	var rows := int(ceil(float(entries.size()) / float(COLS)))
	var grid_w := COLS * SPACING
	var grid_h := rows * SPACING
	var aspect := 1280.0 / 720.0
	var camera := Camera3D.new()
	camera.projection = Camera3D.PROJECTION_ORTHOGONAL
	camera.size = maxf(grid_h, grid_w / aspect) + 0.6
	camera.position = Vector3((COLS - 1) * SPACING * 0.5,
			-(rows - 1) * SPACING * 0.5, 18.0)
	camera.current = true
	scene.add_child(camera)

	var captures := {}
	for state in ["direction_a", "direction_b", "hemi_sky", "hemi_ground",
			"ambient_off", "ambient_on", "point_off", "point_on", "point_static"]:
		_apply_lighting_probe_state(entries, state)
		var frame: Image = await _capture_lighting_image()
		if frame == null:
			push_error("render_swatch_probe lighting: no viewport image for %s" % state)
			quit(1)
			return
		frame.convert(Image.FORMAT_RGBA8)
		captures[state] = frame
		var save_error := frame.save_png(out_dir.path_join("%s_%s.png" % [prefix, state]))
		if save_error != OK:
			push_error("render_swatch_probe lighting: could not save %s" % state)
			quit(1)
			return

	var pixel_scale := float((captures["direction_a"] as Image).get_height()) / camera.size
	var radius_px := maxi(8, int(0.82 * pixel_scale))
	var reports: Array[Dictionary] = []
	var failures: Array[String] = []
	for entry in entries:
		var sphere: MeshInstance3D = entry["mesh"]
		var center := camera.unproject_position(sphere.global_position)
		var rect := Rect2i(int(center.x) - radius_px, int(center.y) - radius_px,
				radius_px * 2, radius_px * 2)
		var dir_delta := _lighting_mean_delta(captures["direction_a"],
				captures["direction_b"], rect)
		var hemi_delta := _lighting_mean_delta(captures["hemi_sky"],
				captures["hemi_ground"], rect)
		var ambient_delta := _lighting_mean_delta(captures["ambient_off"],
				captures["ambient_on"], rect)
		var point_delta := _lighting_mean_delta(captures["point_off"],
				captures["point_on"], rect)
		var static_point_max_byte_delta := _lighting_max_byte_delta(
				captures["point_on"], captures["point_static"], rect)
		var dir_a_contrast := _lighting_axis_contrast(captures["direction_a"], rect, true)
		var dir_b_contrast := _lighting_axis_contrast(captures["direction_b"], rect, true)
		var sky_contrast := _lighting_axis_contrast(captures["hemi_sky"], rect, false)
		var ground_contrast := _lighting_axis_contrast(captures["hemi_ground"], rect, false)
		var report := {
			"technique": entry["name"],
			"implementation": entry["implementation"],
			"path": entry["path"],
			"expected_responses": entry["responses"],
			"direction_delta": dir_delta,
			"hemisphere_delta": hemi_delta,
			"ambient_delta": ambient_delta,
			"point_delta": point_delta,
			"static_point_max_byte_delta": static_point_max_byte_delta,
			"direction_contrast": [dir_a_contrast, dir_b_contrast],
			"hemisphere_contrast": [sky_contrast, ground_contrast],
		}
		reports.append(report)
		var responses: Dictionary = entry["responses"]
		var raster_checks: Dictionary = entry["raster_checks"]
		if bool(responses["directional"]):
			if dir_delta < 0.006:
				failures.append("%s did not react to directional reversal (%f)" % [entry["name"], dir_delta])
			if bool(raster_checks.get("directional_axis_reversal", true)) and \
					dir_a_contrast * dir_b_contrast >= -0.000025:
				failures.append("%s did not reverse its directional lobe (%f/%f)" % [entry["name"], dir_a_contrast, dir_b_contrast])
		elif dir_delta > 0.001:
			failures.append("%s invented a directional response (%f)" % [entry["name"], dir_delta])
		if bool(responses["hemisphere"]):
			if hemi_delta < 0.006:
				failures.append("%s did not react to hemisphere reversal (%f)" % [entry["name"], hemi_delta])
			if bool(raster_checks.get("hemisphere_axis_reversal", true)) and \
					sky_contrast * ground_contrast >= -0.000025:
				failures.append("%s did not reverse sky/ground response (%f/%f)" % [entry["name"], sky_contrast, ground_contrast])
		elif hemi_delta > 0.001:
			failures.append("%s invented a hemisphere response (%f)" % [entry["name"], hemi_delta])
		if bool(responses["ambient"]):
			if ambient_delta < 0.006:
				failures.append("%s did not react to flat ambient light (%f)" % [entry["name"], ambient_delta])
		elif ambient_delta > 0.001:
			failures.append("%s invented an ambient response (%f)" % [entry["name"], ambient_delta])
		if bool(responses["point"]):
			if point_delta < 0.001:
				failures.append("%s did not react to gameplay point light (%f)" % [entry["name"], point_delta])
		elif point_delta > 0.001:
			failures.append("%s invented a gameplay point-light response (%f)" % [entry["name"], point_delta])
		if static_point_max_byte_delta != 0:
			failures.append("%s static atlas point-light route was not pixel-identical (max byte delta %d)" % [entry["name"], static_point_max_byte_delta])

	var manifest := {
		"version": 2,
		"probe": "object-light-response",
		"window": [1280, 720],
		"technique_count": entries.size(),
		"states": captures.keys(),
		"thresholds": {
			"lit_direction_delta_min": 0.006,
			"lit_hemisphere_delta_min": 0.006,
			"lit_ambient_delta_min": 0.006,
			"lit_point_delta_min": 0.001,
			"unexpected_response_delta_max": 0.001,
			"static_point_max_byte_delta": 0,
		},
		"techniques": reports,
		"failures": failures,
	}
	var mf := FileAccess.open(out_dir.path_join("%s_manifest.json" % prefix), FileAccess.WRITE)
	if mf != null:
		mf.store_string(JSON.stringify(manifest, "\t"))
		mf.close()
	if failures.is_empty():
		print("render_swatch_probe lighting: PASS - ", entries.size(),
				" techniques respond according to their audited light contract")
		quit(0)
	else:
		for failure in failures:
			push_error("render_swatch_probe lighting: " + failure)
		quit(1)


# Raster proof for material-channel ownership. This is deliberately separate
# from the lighting response matrix: the same compiled shaders must prove that
# RgbGen affects only SELFLUM, AlphaGen affects only _FFP, raw Diffuse1.a drives
# the authored Phong lobes, and each technique cuts out from its named coverage
# source (diffuse/normal/vertex/reflect/zero).
func _channel_mode(out_dir: String, prefix: String) -> void:
	DirAccess.make_dir_recursive_absolute(out_dir)
	get_root().get_window().size = Vector2i(1280, 720)
	Engine.time_scale = 0.0

	var parsed = JSON.parse_string(FileAccess.get_file_as_string(
			"res://shaders/object/pipeline_manifest.json"))
	if not parsed is Dictionary:
		push_error("render_swatch_probe channels: object pipeline manifest did not parse")
		quit(1)
		return
	var specular_alpha_contracts = parsed.get("specular_alpha_contracts", {})
	if not specular_alpha_contracts is Dictionary:
		push_error("render_swatch_probe channels: specular-alpha contract did not parse")
		quit(1)
		return

	var scene := Node3D.new()
	root.add_child(scene)
	var world_env := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.012, 0.012, 0.016)
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	world_env.environment = env
	scene.add_child(world_env)
	_add_framefx(scene, false)

	var textures := {
		"diffuse_low": _make_channel_diffuse_texture(64),
		"diffuse_high": _make_channel_diffuse_texture(192),
		"specular_low": _make_channel_specular_texture(64),
		"specular_high": _make_channel_specular_texture(192),
		"detail": _make_lighting_detail_texture(),
		"normal_low": _make_channel_normal_texture(64),
		"normal_high": _make_channel_normal_texture(192),
	}
	var entries: Array[Dictionary] = []
	var techniques: Array = parsed.get("techniques", [])
	const COLS := 6
	const SPACING := 2.2
	for i in range(techniques.size()):
		var technique: Dictionary = techniques[i]
		var policies: Array = technique.get("policies", [])
		var color_policy := "opaque" if policies.has("opaque") else str(policies[0])
		var coverage_policy := ""
		for candidate in ["cutout_mix", "cutout_alpha", "cutout_additive"]:
			if policies.has(candidate):
				coverage_policy = candidate
				break
		if coverage_policy.is_empty():
			push_error("render_swatch_probe channels: no cutout policy for %s" %
					technique["engine_enum"])
			quit(1)
			return

		var color_path := "res://shaders/object/%s/%s.gdshader" % [
				technique["directory"], color_policy]
		var coverage_path := "res://shaders/object/%s/%s.gdshader" % [
				technique["directory"], coverage_policy]
		var color_shader := load(color_path) as Shader
		var coverage_shader := load(coverage_path) as Shader
		if color_shader == null or coverage_shader == null:
			push_error("render_swatch_probe channels: could not load %s / %s" % [
					color_path, coverage_path])
			quit(1)
			return

		var color_material := ShaderMaterial.new()
		color_material.shader = color_shader
		var coverage_material := ShaderMaterial.new()
		coverage_material.shader = coverage_shader
		if not _bind_production_object_resources(color_material,
				str(technique["implementation"])) or not \
				_bind_production_object_resources(coverage_material,
				str(technique["implementation"])):
			push_error("render_swatch_probe channels: production resources unavailable for %s" % color_path)
			quit(1)
			return
		var quad := QuadMesh.new()
		quad.size = Vector2(1.55, 1.55)
		var color_mesh := MeshInstance3D.new()
		color_mesh.mesh = quad
		color_mesh.material_override = color_material
		color_mesh.position = Vector3((i % COLS) * SPACING,
				-(i / COLS) * SPACING, 0.0)
		scene.add_child(color_mesh)
		var coverage_mesh := MeshInstance3D.new()
		coverage_mesh.mesh = quad
		coverage_mesh.material_override = coverage_material
		coverage_mesh.position = color_mesh.position
		coverage_mesh.visible = false
		scene.add_child(coverage_mesh)
		entries.append({
			"name": str(technique["engine_enum"]),
			"rgb_modulation": str(technique["rgb_modulation"]),
			"alpha_modulation": str(technique["alpha_modulation"]),
			"coverage_source": str(technique["coverage_source"]),
			"specular_alpha": str(specular_alpha_contracts.get(
					str(technique["engine_enum"]), "none")),
			"color_material": color_material,
			"coverage_material": coverage_material,
			"color_mesh": color_mesh,
			"coverage_mesh": coverage_mesh,
		})

	var rows := int(ceil(float(entries.size()) / float(COLS)))
	var grid_w := COLS * SPACING
	var grid_h := rows * SPACING
	var aspect := 1280.0 / 720.0
	var camera := Camera3D.new()
	camera.projection = Camera3D.PROJECTION_ORTHOGONAL
	camera.size = maxf(grid_h, grid_w / aspect) + 0.6
	camera.position = Vector3((COLS - 1) * SPACING * 0.5,
			-(rows - 1) * SPACING * 0.5, 18.0)
	camera.current = true
	scene.add_child(camera)

	var captures := {}
	var states := ["rgb_low", "rgb_high", "specular_alpha_low",
			"specular_alpha_high", "coverage_low", "coverage_high",
			"alpha_gen_low", "alpha_gen_high"]
	for state in states:
		_apply_channel_probe_state(entries, state, textures)
		var frame: Image = await _capture_lighting_image()
		if frame == null:
			push_error("render_swatch_probe channels: no viewport image for %s" % state)
			quit(1)
			return
		frame.convert(Image.FORMAT_RGBA8)
		captures[state] = frame
		var save_error := frame.save_png(out_dir.path_join("%s_%s.png" % [prefix, state]))
		if save_error != OK:
			push_error("render_swatch_probe channels: could not save %s" % state)
			quit(1)
			return

	var pixel_scale := float((captures["rgb_high"] as Image).get_height()) / camera.size
	var radius_px := maxi(8, int(0.78 * pixel_scale))
	var reports: Array[Dictionary] = []
	var failures: Array[String] = []
	for entry in entries:
		var mesh: MeshInstance3D = entry["color_mesh"]
		var center := camera.unproject_position(mesh.global_position)
		var rect := Rect2i(int(center.x) - radius_px, int(center.y) - radius_px,
				radius_px * 2, radius_px * 2)
		var rgb_delta := _lighting_mean_delta(
				captures["rgb_low"], captures["rgb_high"], rect)
		var specular_alpha_delta := _lighting_mean_delta(
				captures["specular_alpha_low"], captures["specular_alpha_high"], rect)
		var coverage_delta := _lighting_mean_delta(
				captures["coverage_low"], captures["coverage_high"], rect)
		var alpha_gen_delta := _lighting_mean_delta(
				captures["alpha_gen_low"], captures["alpha_gen_high"], rect)
		var report := {
			"technique": entry["name"],
			"rgb_modulation": entry["rgb_modulation"],
			"alpha_modulation": entry["alpha_modulation"],
			"coverage_source": entry["coverage_source"],
			"specular_alpha": entry["specular_alpha"],
			"rgb_delta": rgb_delta,
			"specular_alpha_delta": specular_alpha_delta,
			"coverage_delta": coverage_delta,
			"alpha_gen_delta": alpha_gen_delta,
		}
		reports.append(report)
		_channel_expect_delta(failures, entry["name"], "RgbGen", rgb_delta,
				entry["rgb_modulation"] == "self_lum", 0.006, 0.001)
		_channel_expect_delta(failures, entry["name"], "specular alpha",
				specular_alpha_delta, entry["specular_alpha"] != "none", 0.003, 0.001)
		_channel_expect_delta(failures, entry["name"], "coverage source",
				coverage_delta, entry["coverage_source"] != "zero", 0.006, 0.001)
		_channel_expect_delta(failures, entry["name"], "AlphaGen",
				alpha_gen_delta, entry["alpha_modulation"] == "ffp", 0.006, 0.001)

	var manifest := {
		"version": 1,
		"probe": "object-material-channel-response",
		"window": [1280, 720],
		"technique_count": entries.size(),
		"states": states,
		"techniques": reports,
		"failures": failures,
	}
	var mf := FileAccess.open(out_dir.path_join("%s_manifest.json" % prefix), FileAccess.WRITE)
	if mf != null:
		mf.store_string(JSON.stringify(manifest, "\t"))
		mf.close()
	if failures.is_empty():
		print("render_swatch_probe channels: PASS - ", entries.size(),
				" techniques honor RGB, alpha, specular, and coverage contracts")
		quit(0)
	else:
		for failure in failures:
			push_error("render_swatch_probe channels: " + failure)
		quit(1)


func _channel_expect_delta(failures: Array[String], technique: String,
		label: String, delta: float, expected: bool, minimum: float,
		maximum: float) -> void:
	if expected and delta < minimum:
		failures.append("%s did not react to %s (%f)" % [technique, label, delta])
	elif not expected and delta > maximum:
		failures.append("%s invented a %s response (%f)" % [technique, label, delta])


# Raster proof for retail's auxiliary CLIP technique. Every live object
# technique renders through the same mirrored-eye gate, but only effects with
# an explicit retail CLIP block discard below waterHeight-0.1. Missing CLIP
# blocks inherit NORMAL; skinned submissions were skipped by the collector.
# The wrong-eye and disarmed captures prove this is reflection-camera state,
# not a global main-view slice.
func _clip_mode(out_dir: String, prefix: String) -> void:
	DirAccess.make_dir_recursive_absolute(out_dir)
	get_root().get_window().size = Vector2i(1280, 360)
	Engine.time_scale = 0.0

	var parsed = JSON.parse_string(FileAccess.get_file_as_string(
			"res://shaders/object/pipeline_manifest.json"))
	if not parsed is Dictionary:
		push_error("render_swatch_probe clip: object pipeline manifest did not parse")
		quit(1)
		return

	var scene := Node3D.new()
	root.add_child(scene)
	var world_env := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.008, 0.008, 0.012)
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	world_env.environment = env
	scene.add_child(world_env)
	_add_framefx(scene, false)

	var diffuse := _make_lighting_diffuse_texture()
	var detail := _make_lighting_detail_texture()
	var tangent_normal := _make_lighting_normal_texture()
	var object_normal := _make_lighting_object_normal_texture()
	var techniques: Array = parsed.get("techniques", [])
	var entries: Array[Dictionary] = []
	const SPACING := 0.88
	const QUAD_SIZE := Vector2(0.72, 2.0)
	# The lighting block is pass-global: publish this probe's flat register
	# once before the first capture rather than trusting another writer's.
	RenderingServer.global_shader_parameter_set("opennova_light_block_gain", Vector3.ONE)
	RenderingServer.global_shader_parameter_set("opennova_light_block_hemi_sky",
			Vector3(0.22, 0.22, 0.22))
	RenderingServer.global_shader_parameter_set("opennova_light_block_hemi_ground",
			Vector3(0.22, 0.22, 0.22))
	RenderingServer.global_shader_parameter_set("opennova_light_block_dir",
			Vector3(0.0, 0.0, -1.0))
	RenderingServer.global_shader_parameter_set("opennova_light_block_dir_color",
			Vector3(0.16, 0.16, 0.16))
	RenderingServer.global_shader_parameter_set("opennova_fog_enabled", false)
	for i in range(techniques.size()):
		var technique: Dictionary = techniques[i]
		var policies: Array = technique.get("policies", [])
		var policy := "opaque" if policies.has("opaque") else str(policies[0])
		var path := "res://shaders/object/%s/%s.gdshader" % [
				technique["directory"], policy]
		var shader := load(path) as Shader
		if shader == null:
			push_error("render_swatch_probe clip: could not load %s" % path)
			_clip_probe_disarm()
			quit(1)
			return
		var material := ShaderMaterial.new()
		material.shader = shader
		if not _bind_production_object_resources(material,
				str(technique["implementation"])):
			push_error("render_swatch_probe clip: production resources unavailable for %s" % path)
			_clip_probe_disarm()
			quit(1)
			return
		material.set_shader_parameter("u_diffuse", diffuse)
		material.set_shader_parameter("u_detail", detail)
		material.set_shader_parameter("u_normal_map",
				object_normal if str(technique["normal"]) == "object_uv1" else tangent_normal)
		material.set_shader_parameter("u_uv_transform_u", Vector3(1.0, 0.0, 0.0))
		material.set_shader_parameter("u_uv_transform_v", Vector3(0.0, 1.0, 0.0))
		material.set_shader_parameter("u_rgb_mod", Vector3.ONE)
		material.set_shader_parameter("u_alpha_mod", 1.0)
		material.set_shader_parameter("u_reflect_color", Color(0.65, 0.72, 0.8, 0.8))
		material.set_shader_parameter("u_alpha_test_threshold", 0.5)
		material.set_shader_parameter("u_alpha_test_invert", 0.0)
		material.set_shader_parameter("u_local_light_count", 0)
		var quad := QuadMesh.new()
		quad.size = QUAD_SIZE
		var mesh := MeshInstance3D.new()
		mesh.mesh = quad
		mesh.material_override = material
		mesh.position = Vector3(float(i) * SPACING, 0.0, 0.0)
		mesh.set_instance_shader_parameter("u_point_light_count", 0.0)
		scene.add_child(mesh)
		entries.append({
			"name": str(technique["engine_enum"]),
			"clip_class": str(technique["clip_class"]),
			"mesh": mesh,
		})

	var aspect := 1280.0 / 360.0
	var span := maxf(float(techniques.size() - 1) * SPACING + QUAD_SIZE.x, 1.0)
	var camera := Camera3D.new()
	camera.projection = Camera3D.PROJECTION_ORTHOGONAL
	camera.size = maxf(3.0, span / aspect + 0.25)
	camera.position = Vector3(float(techniques.size() - 1) * SPACING * 0.5,
			0.0, 18.0)
	camera.current = true
	scene.add_child(camera)

	var rs := RenderingServer
	rs.global_shader_parameter_set("opennova_water_height", 0.1)
	rs.global_shader_parameter_set("opennova_water_reflection_eye", camera.position)
	var captures := {}
	var states := ["inactive", "disarmed", "wrong_eye", "reflection"]
	for state in states:
		rs.global_shader_parameter_set("opennova_water_active", state != "inactive")
		rs.global_shader_parameter_set("opennova_water_reflection_clip_active",
				state == "wrong_eye" or state == "reflection")
		rs.global_shader_parameter_set("opennova_water_reflection_eye",
				camera.position + (Vector3(2.0, 0.0, 0.0) if state == "wrong_eye" else Vector3.ZERO))
		var frame: Image = await _capture_lighting_image()
		if frame == null:
			push_error("render_swatch_probe clip: no viewport image for %s" % state)
			_clip_probe_disarm()
			quit(1)
			return
		frame.convert(Image.FORMAT_RGBA8)
		captures[state] = frame
		if frame.save_png(out_dir.path_join("%s_%s.png" % [prefix, state])) != OK:
			push_error("render_swatch_probe clip: could not save %s" % state)
			_clip_probe_disarm()
			quit(1)
			return

	var pixel_scale := float((captures["inactive"] as Image).get_height()) / camera.size
	var reports: Array[Dictionary] = []
	var failures: Array[String] = []
	for entry in entries:
		var mesh: MeshInstance3D = entry["mesh"]
		var center := camera.unproject_position(mesh.global_position)
		var x_radius := maxi(3, int(QUAD_SIZE.x * pixel_scale * 0.32))
		var half_y_inner := maxi(3, int(QUAD_SIZE.y * pixel_scale * 0.12))
		var half_y_outer := maxi(half_y_inner + 2,
				int(QUAD_SIZE.y * pixel_scale * 0.44))
		var top_rect := Rect2i(int(center.x) - x_radius,
				int(center.y) - half_y_outer, x_radius * 2,
				half_y_outer - half_y_inner)
		var bottom_rect := Rect2i(int(center.x) - x_radius,
				int(center.y) + half_y_inner, x_radius * 2,
				half_y_outer - half_y_inner)
		var inactive_bottom_luma := _lighting_mean_luminance(
				captures["inactive"], bottom_rect)
		var reflection_bottom_delta := _lighting_mean_delta(
				captures["inactive"], captures["reflection"], bottom_rect)
		var reflection_top_delta := _lighting_mean_delta(
				captures["inactive"], captures["reflection"], top_rect)
		var disarmed_delta := _lighting_mean_delta(
				captures["inactive"], captures["disarmed"], bottom_rect)
		var wrong_eye_delta := _lighting_mean_delta(
				captures["inactive"], captures["wrong_eye"], bottom_rect)
		var clip_expected: bool = str(entry["clip_class"]) in ["explicit",
				"explicit_unskinned_or_submit_skip_skinned"]
		reports.append({
			"technique": entry["name"],
			"clip_class": entry["clip_class"],
			"inactive_bottom_luma": inactive_bottom_luma,
			"reflection_bottom_delta": reflection_bottom_delta,
			"reflection_top_delta": reflection_top_delta,
			"disarmed_bottom_delta": disarmed_delta,
			"wrong_eye_bottom_delta": wrong_eye_delta,
		})
		if inactive_bottom_luma < 0.012:
			failures.append("%s was not visible before clipping (%f)" % [
					entry["name"], inactive_bottom_luma])
		_channel_expect_delta(failures, entry["name"], "reflection CLIP",
				reflection_bottom_delta, clip_expected, 0.012, 0.001)
		if reflection_top_delta > 0.001:
			failures.append("%s clipped above waterHeight-0.1 (%f)" % [
					entry["name"], reflection_top_delta])
		if disarmed_delta > 0.001:
			failures.append("%s clipped while reflection CLIP was disarmed (%f)" % [
					entry["name"], disarmed_delta])
		if wrong_eye_delta > 0.001:
			failures.append("%s clipped an unrelated camera (%f)" % [
					entry["name"], wrong_eye_delta])

	var manifest := {
		"version": 1,
		"probe": "object-water-reflection-clip",
		"window": [1280, 360],
		"water_height": 0.1,
		"retail_clip_plane": 0.0,
		"technique_count": entries.size(),
		"states": states,
		"techniques": reports,
		"failures": failures,
	}
	var mf := FileAccess.open(out_dir.path_join("%s_manifest.json" % prefix), FileAccess.WRITE)
	if mf != null:
		mf.store_string(JSON.stringify(manifest, "\t"))
		mf.close()
	_clip_probe_disarm()
	if failures.is_empty():
		print("render_swatch_probe clip: PASS - ", entries.size(),
				" techniques honor explicit/fallback/skinned reflection CLIP contracts")
		quit(0)
	else:
		for failure in failures:
			push_error("render_swatch_probe clip: " + failure)
		quit(1)


func _clip_probe_disarm() -> void:
	RenderingServer.global_shader_parameter_set("opennova_water_active", false)
	RenderingServer.global_shader_parameter_set(
			"opennova_water_reflection_clip_active", false)


# Raster proof for the retail skinned MATCHTERRAIN technique. Retail draws a
# terrain-colored pass first, then NORMAL over accepted alpha-test pixels.
# The folded implementation must therefore appear only in NORMAL's rejected
# pixels, only for the nine skinned highest-quality runtime techniques, only
# while crouched/prone with a resident terrain page, and must consume both the
# composed page's RGB and alpha lighting channel.
func _matchterrain_mode(out_dir: String, prefix: String) -> void:
	DirAccess.make_dir_recursive_absolute(out_dir)
	get_root().get_window().size = Vector2i(1280, 720)
	Engine.time_scale = 0.0

	var parsed = JSON.parse_string(FileAccess.get_file_as_string(
			"res://shaders/object/pipeline_manifest.json"))
	if not parsed is Dictionary:
		push_error("render_swatch_probe matchterrain: object pipeline manifest did not parse")
		quit(1)
		return
	var contracts = parsed.get("match_terrain_contracts", {})
	if not contracts is Dictionary:
		push_error("render_swatch_probe matchterrain: contracts did not parse")
		quit(1)
		return

	var scene := Node3D.new()
	root.add_child(scene)
	var world_env := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.008, 0.008, 0.012)
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	world_env.environment = env
	scene.add_child(world_env)
	_add_framefx(scene, false)

	var tile_low := _make_matchterrain_array(32)
	var tile_high := _make_matchterrain_array(224)
	var textures := {
		"diffuse_low": _make_channel_diffuse_texture(32),
		"diffuse_high": _make_channel_diffuse_texture(224),
		"detail": _make_lighting_detail_texture(),
		"normal_low": _make_channel_normal_texture(32),
		"normal_high": _make_channel_normal_texture(224),
		"tile_low": tile_low,
		"tile_high": tile_high,
	}
	var entries: Array[Dictionary] = []
	var techniques: Array = parsed.get("techniques", [])
	const COLS := 6
	const SPACING := 2.2
	for i in range(techniques.size()):
		var technique: Dictionary = techniques[i]
		var name := str(technique["engine_enum"])
		var policies: Array = technique.get("policies", [])
		var policy := ""
		for candidate in ["cutout_mix", "cutout_alpha", "cutout_additive"]:
			if policies.has(candidate):
				policy = candidate
				break
		if policy.is_empty():
			push_error("render_swatch_probe matchterrain: no cutout policy for %s" % name)
			quit(1)
			return
		var path := "res://shaders/object/%s/%s.gdshader" % [
				technique["directory"], policy]
		var shader := load(path) as Shader
		if shader == null:
			push_error("render_swatch_probe matchterrain: could not load %s" % path)
			quit(1)
			return
		var material := ShaderMaterial.new()
		material.shader = shader
		if not _bind_production_object_resources(material,
				str(technique["implementation"])):
			push_error("render_swatch_probe matchterrain: production resources unavailable for %s" % path)
			quit(1)
			return
		var quad := QuadMesh.new()
		quad.size = Vector2(1.55, 1.55)
		var mesh := MeshInstance3D.new()
		mesh.mesh = quad
		mesh.material_override = material
		mesh.position = Vector3((i % COLS) * SPACING,
				-(i / COLS) * SPACING, 0.0)
		scene.add_child(mesh)
		entries.append({
			"name": name,
			"contract": str(contracts.get(name, "missing")),
			"material": material,
			"mesh": mesh,
		})

	var rows := int(ceil(float(entries.size()) / float(COLS)))
	var grid_w := COLS * SPACING
	var grid_h := rows * SPACING
	var aspect := 1280.0 / 720.0
	var camera := Camera3D.new()
	camera.projection = Camera3D.PROJECTION_ORTHOGONAL
	camera.size = maxf(grid_h, grid_w / aspect) + 0.6
	camera.position = Vector3((COLS - 1) * SPACING * 0.5,
			-(rows - 1) * SPACING * 0.5, 18.0)
	camera.current = true
	scene.add_child(camera)

	var captures := {}
	var states := ["disabled_rejected", "missing_cache", "page_unready",
			"active_alpha_low", "active_alpha_high", "accepted_disabled",
			"accepted_active"]
	for state in states:
		_apply_matchterrain_probe_state(entries, state, textures)
		var frame: Image = await _capture_lighting_image()
		if frame == null:
			push_error("render_swatch_probe matchterrain: no viewport image for %s" % state)
			quit(1)
			return
		frame.convert(Image.FORMAT_RGBA8)
		captures[state] = frame
		if frame.save_png(out_dir.path_join("%s_%s.png" % [prefix, state])) != OK:
			push_error("render_swatch_probe matchterrain: could not save %s" % state)
			quit(1)
			return

	var pixel_scale := float((captures["active_alpha_high"] as Image).get_height()) / camera.size
	var radius_px := maxi(8, int(0.60 * pixel_scale))
	var reports: Array[Dictionary] = []
	var failures: Array[String] = []
	for entry in entries:
		var mesh: MeshInstance3D = entry["mesh"]
		var center := camera.unproject_position(mesh.global_position)
		var rect := Rect2i(int(center.x) - radius_px, int(center.y) - radius_px,
				radius_px * 2, radius_px * 2)
		var contract := str(entry["contract"])
		var has_pass := contract == "skinned_stance"
		var active_luma := _lighting_mean_luminance(
				captures["active_alpha_high"], rect)
		var activation_delta := _lighting_mean_delta(
				captures["disabled_rejected"], captures["active_alpha_high"], rect)
		var tile_alpha_delta := _lighting_mean_delta(
				captures["active_alpha_low"], captures["active_alpha_high"], rect)
		var missing_delta := _lighting_mean_delta(
				captures["disabled_rejected"], captures["missing_cache"], rect)
		var unready_delta := _lighting_mean_delta(
				captures["disabled_rejected"], captures["page_unready"], rect)
		var accepted_delta := _lighting_mean_delta(
				captures["accepted_disabled"], captures["accepted_active"], rect)
		reports.append({
			"technique": entry["name"],
			"contract": contract,
			"active_luma": active_luma,
			"activation_delta": activation_delta,
			"terrain_alpha_delta": tile_alpha_delta,
			"missing_cache_delta": missing_delta,
			"page_unready_delta": unready_delta,
			"accepted_normal_delta": accepted_delta,
		})
		if contract == "missing":
			failures.append("%s has no MATCHTERRAIN contract" % entry["name"])
		_channel_expect_delta(failures, entry["name"], "MATCHTERRAIN stance/page gate",
				activation_delta, has_pass, 0.02, 0.001)
		_channel_expect_delta(failures, entry["name"], "terrain alpha lighting",
				tile_alpha_delta, has_pass, 0.01, 0.001)
		if has_pass and active_luma < 0.04:
			failures.append("%s did not render terrain into rejected pixels (%f)" % [
					entry["name"], active_luma])
		if missing_delta > 0.001:
			failures.append("%s sampled a missing terrain cache (%f)" % [
					entry["name"], missing_delta])
		if unready_delta > 0.001:
			failures.append("%s sampled an unready terrain page (%f)" % [
					entry["name"], unready_delta])
		if accepted_delta > 0.001:
			failures.append("%s changed NORMAL-accepted pixels (%f)" % [
					entry["name"], accepted_delta])

	var manifest := {
		"version": 1,
		"probe": "object-skinned-match-terrain",
		"window": [1280, 720],
		"technique_count": entries.size(),
		"states": states,
		"techniques": reports,
		"failures": failures,
	}
	var mf := FileAccess.open(out_dir.path_join("%s_manifest.json" % prefix), FileAccess.WRITE)
	if mf != null:
		mf.store_string(JSON.stringify(manifest, "\t"))
		mf.close()
	if failures.is_empty():
		print("render_swatch_probe matchterrain: PASS - ", entries.size(),
				" techniques honor stance, residency, coverage, and tile-channel contracts")
		quit(0)
	else:
		for failure in failures:
			push_error("render_swatch_probe matchterrain: " + failure)
		quit(1)


func _apply_matchterrain_probe_state(entries: Array[Dictionary], state: String,
		textures: Dictionary) -> void:
	var accepted := state.begins_with("accepted_")
	var enabled := state not in ["disabled_rejected", "accepted_disabled"]
	var has_cache := state != "missing_cache"
	var page_ready := state != "page_unready"
	var tile = textures["tile_low"] if state == "active_alpha_low" else \
			textures["tile_high"]
	# The lighting block is pass-global: publish this state's register with
	# every capture so each starts from its own values, not a prior writer's.
	RenderingServer.global_shader_parameter_set("opennova_light_block_gain", Vector3.ONE)
	RenderingServer.global_shader_parameter_set("opennova_light_block_hemi_sky",
			Vector3(0.10, 0.18, 0.24))
	RenderingServer.global_shader_parameter_set("opennova_light_block_hemi_ground",
			Vector3(0.10, 0.18, 0.24))
	RenderingServer.global_shader_parameter_set("opennova_light_block_dir",
			Vector3(0.0, 0.0, -1.0) if accepted else Vector3(0.0, 0.0, 1.0))
	RenderingServer.global_shader_parameter_set("opennova_light_block_dir_color",
			Vector3(0.34, 0.22, 0.12))
	RenderingServer.global_shader_parameter_set("opennova_fog_enabled", false)
	for entry in entries:
		var material: ShaderMaterial = entry["material"]
		var mesh: MeshInstance3D = entry["mesh"]
		material.set_shader_parameter("u_diffuse",
				textures["diffuse_high" if accepted else "diffuse_low"])
		material.set_shader_parameter("u_detail", textures["detail"])
		material.set_shader_parameter("u_normal_map",
				textures["normal_high" if accepted else "normal_low"])
		material.set_shader_parameter("u_uv_transform_u", Vector3(1.0, 0.0, 0.0))
		material.set_shader_parameter("u_uv_transform_v", Vector3(0.0, 1.0, 0.0))
		material.set_shader_parameter("u_rgb_mod", Vector3.ONE)
		material.set_shader_parameter("u_alpha_mod", 1.0)
		material.set_shader_parameter("u_reflect_color",
				Color(0.7, 0.8, 0.9, 0.88 if accepted else 0.12))
		material.set_shader_parameter("u_alpha_test_threshold", 0.5)
		material.set_shader_parameter("u_alpha_test_invert", 0.0)
		material.set_shader_parameter("u_local_light_count", 0)
		material.set_shader_parameter("u_match_terrain_cache", tile)
		material.set_shader_parameter("u_has_match_terrain_cache", has_cache)
		mesh.set_instance_shader_parameter("u_point_light_count", 0.0)
		mesh.set_instance_shader_parameter("u_match_terrain_enabled", enabled)
		mesh.set_instance_shader_parameter("u_match_terrain_page_ready", page_ready)
		mesh.set_instance_shader_parameter("u_match_terrain_page_layer", 0.0)
		mesh.set_instance_shader_parameter("u_match_terrain_page_projection",
				Vector4(mesh.position.x - 1.0, mesh.position.z - 1.0, 0.5, 2.0))


func _make_matchterrain_array(alpha: int) -> Texture2DArray:
	var image := Image.create(256, 256, false, Image.FORMAT_RGBA8)
	image.fill(Color8(72, 132, 196, alpha))
	var texture := Texture2DArray.new()
	var error := texture.create_from_images([image])
	if error != OK:
		push_error("render_swatch_probe matchterrain: Texture2DArray creation failed (%d)" % error)
	return texture


# Highest-quality GLOW proof. _FFP LUM copies NORMAL, fixed Glass uses the
# sun-rotated specular cube, and every other live runtime technique has no
# GLOW pass. Ring pixels exercise the production isolated Q3 target and exact
# FrameFX kernel; the away-sun capture proves Glass tracks MatRotSpecular.
func _glow_mode(out_dir: String, prefix: String) -> void:
	DirAccess.make_dir_recursive_absolute(out_dir)
	get_root().get_window().size = Vector2i(1280, 720)
	Engine.time_scale = 0.0
	var parsed = JSON.parse_string(FileAccess.get_file_as_string(
			"res://shaders/object/pipeline_manifest.json"))
	if not parsed is Dictionary:
		push_error("render_swatch_probe glow: object pipeline manifest did not parse")
		quit(1)
		return
	var contracts = parsed.get("glow_contracts", {})
	if not contracts is Dictionary:
		push_error("render_swatch_probe glow: contracts did not parse")
		quit(1)
		return

	var scene := Node3D.new()
	root.add_child(scene)
	var world_env := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.006, 0.006, 0.009)
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	world_env.environment = env
	scene.add_child(world_env)
	var frame_renderer := _add_framefx(scene, true)

	var diffuse := _make_channel_diffuse_texture(255)
	var detail := _make_lighting_detail_texture()
	var normal := _make_lighting_normal_texture()
	var entries: Array[Dictionary] = []
	var techniques: Array = parsed.get("techniques", [])
	const COLS := 6
	const SPACING := 2.5
	const QUAD_SIZE := Vector2(1.30, 1.30)
	# The lighting block is pass-global. A zero direction color keeps every
	# NORMAL technique flat so only the glow cube can follow the direction,
	# which each capture aims below.
	RenderingServer.global_shader_parameter_set("opennova_light_block_gain", Vector3.ONE)
	RenderingServer.global_shader_parameter_set("opennova_light_block_hemi_sky",
			Vector3(0.04, 0.04, 0.04))
	RenderingServer.global_shader_parameter_set("opennova_light_block_hemi_ground",
			Vector3(0.04, 0.04, 0.04))
	RenderingServer.global_shader_parameter_set("opennova_light_block_dir_color",
			Vector3.ZERO)
	RenderingServer.global_shader_parameter_set("opennova_fog_enabled", false)
	for i in range(techniques.size()):
		var technique: Dictionary = techniques[i]
		var name := str(technique["engine_enum"])
		var policies: Array = technique.get("policies", [])
		var policy := "opaque" if policies.has("opaque") else str(policies[0])
		var path := "res://shaders/object/%s/%s.gdshader" % [
				technique["directory"], policy]
		var shader := load(path) as Shader
		if shader == null:
			push_error("render_swatch_probe glow: could not load %s" % path)
			quit(1)
			return
		var material := ShaderMaterial.new()
		material.shader = shader
		if not _bind_production_object_resources(material,
				str(technique["implementation"])):
			push_error("render_swatch_probe glow: production resources unavailable for %s" % path)
			quit(1)
			return
		material.set_shader_parameter("u_diffuse", diffuse)
		material.set_shader_parameter("u_detail", detail)
		material.set_shader_parameter("u_normal_map", normal)
		material.set_shader_parameter("u_uv_transform_u", Vector3(1.0, 0.0, 0.0))
		material.set_shader_parameter("u_uv_transform_v", Vector3(0.0, 1.0, 0.0))
		material.set_shader_parameter("u_rgb_mod", Vector3.ONE)
		material.set_shader_parameter("u_alpha_mod", 1.0)
		material.set_shader_parameter("u_reflect_color", Color(1.0, 1.0, 1.0, 1.0))
		material.set_shader_parameter("u_local_light_count", 0)
		var quad := QuadMesh.new()
		quad.size = QUAD_SIZE
		var mesh := MeshInstance3D.new()
		mesh.mesh = quad
		mesh.material_override = material
		mesh.position = Vector3((i % COLS) * SPACING,
				-(i / COLS) * SPACING, 0.0)
		mesh.set_instance_shader_parameter("u_point_light_count", 0.0)
		scene.add_child(mesh)
		entries.append({"name": name,
			"contract": str(contracts.get(name, "missing")),
			"material": material, "mesh": mesh})

	var rows := int(ceil(float(entries.size()) / float(COLS)))
	var grid_w := COLS * SPACING
	var grid_h := rows * SPACING
	var aspect := 1280.0 / 720.0
	var camera := Camera3D.new()
	camera.projection = Camera3D.PROJECTION_ORTHOGONAL
	camera.size = maxf(grid_h, grid_w / aspect) + 0.6
	camera.position = Vector3((COLS - 1) * SPACING * 0.5,
			-(rows - 1) * SPACING * 0.5, 18.0)
	camera.current = true
	scene.add_child(camera)

	var captures := {}
	var states := ["nopass_off", "nopass_on", "glow_off_aligned",
			"glow_on_aligned", "glow_off_away", "glow_on_away"]
	for state in states:
		frame_renderer.visible = state in ["nopass_on", "glow_on_aligned", "glow_on_away"]
		var aligned: bool = not state.ends_with("_away")
		var show_glow_contracts: bool = state.begins_with("glow_")
		var reflection := Vector3(0.0, 0.0, 1.0)
		for entry in entries:
			var mesh: MeshInstance3D = entry["mesh"]
			mesh.visible = (str(entry["contract"]) != "no_pass") == show_glow_contracts
			if str(entry["contract"]) != "rotated_specular":
				continue
			# D3D's camera-space reflection coordinate changes across this
			# orthographic grid because CameraPos remains a single world point.
			# The block direction is one value per pass, so aim the very narrow
			# retail cube lobes at the rotated_specular swatch's actual center
			# reflection instead of assuming its coordinate is +/-Z; every other
			# visible swatch draws with a zero direction color and cannot follow.
			var incident := (mesh.global_position - camera.global_position).normalized()
			reflection = (incident - 2.0 * Vector3(0.0, 0.0, 1.0) *
					incident.dot(Vector3(0.0, 0.0, 1.0))).normalized()
		RenderingServer.global_shader_parameter_set("opennova_light_block_dir",
				reflection if aligned else -reflection)
		var frame: Image = await _capture_lighting_image()
		if frame == null:
			push_error("render_swatch_probe glow: no viewport image for %s" % state)
			quit(1)
			return
		frame.convert(Image.FORMAT_RGBA8)
		captures[state] = frame
		if frame.save_png(out_dir.path_join("%s_%s.png" % [prefix, state])) != OK:
			push_error("render_swatch_probe glow: could not save %s" % state)
			quit(1)
			return

	var pixel_scale := float((captures["glow_on_aligned"] as Image).get_height()) / camera.size
	var inner_radius := maxi(4, int(QUAD_SIZE.x * pixel_scale * 0.55))
	var outer_radius := maxi(inner_radius + 3, int(QUAD_SIZE.x * pixel_scale * 0.92))
	var core_radius := maxi(5, int(QUAD_SIZE.x * pixel_scale * 0.34))
	var reports: Array[Dictionary] = []
	var failures: Array[String] = []
	for entry in entries:
		var mesh: MeshInstance3D = entry["mesh"]
		var center := camera.unproject_position(mesh.global_position)
		var contract := str(entry["contract"])
		var bloom_delta := _lighting_mean_ring_delta(
				captures["glow_off_aligned"], captures["glow_on_aligned"], center,
				inner_radius, outer_radius) if contract != "no_pass" else \
				_lighting_mean_ring_delta(captures["nopass_off"],
						captures["nopass_on"], center, inner_radius, outer_radius)
		var away_bloom_delta := _lighting_mean_ring_delta(
				captures["glow_off_away"], captures["glow_on_away"], center,
				inner_radius, outer_radius)
		var core_rect := Rect2i(int(center.x) - core_radius,
				int(center.y) - core_radius, core_radius * 2, core_radius * 2)
		var sun_delta := _lighting_mean_delta(
				captures["glow_on_aligned"], captures["glow_on_away"], core_rect)
		reports.append({"technique": entry["name"], "contract": contract,
			"aligned_bloom_ring_delta": bloom_delta,
			"away_bloom_ring_delta": away_bloom_delta,
			"sun_rotation_core_delta": sun_delta})
		if contract == "missing":
			failures.append("%s has no GLOW contract" % entry["name"])
		_channel_expect_delta(failures, entry["name"], "GLOW bloom",
				bloom_delta, contract != "no_pass", 0.0015, 0.0007)
		if contract == "rotated_specular":
			if sun_delta < 0.02:
				failures.append("%s GLOW did not track MatRotSpecular (%f)" % [
						entry["name"], sun_delta])
			if away_bloom_delta > 0.001:
				failures.append("%s kept a glass bloom away from the sun (%f)" % [
						entry["name"], away_bloom_delta])
		elif contract == "normal_copy" and away_bloom_delta < 0.0015:
			failures.append("%s LUM GLOW changed with the sun (%f)" % [
					entry["name"], away_bloom_delta])

	var manifest := {"version": 2,
		"probe": "object-selective-q3-framefx", "window": [1280, 720],
		"technique_count": entries.size(), "states": states,
		"backend": frame_renderer.get_backend_report(),
		"techniques": reports, "failures": failures}
	var mf := FileAccess.open(out_dir.path_join("%s_manifest.json" % prefix), FileAccess.WRITE)
	if mf != null:
		mf.store_string(JSON.stringify(manifest, "\t"))
		mf.close()
	if failures.is_empty():
		print("render_swatch_probe glow: PASS - ", entries.size(),
				" techniques honor LUM copy, glass sun glint, and no-pass contracts")
		quit(0)
	else:
		for failure in failures:
			push_error("render_swatch_probe glow: " + failure)
		quit(1)


# Raster proof for the retail PROJSHAD technique used by the live 12-slot
# dynamic-shadow captures. Every technique is compiled and observed as black
# coverage over retail's white clear under the real camera gate. The matrix
# proves no-pass effects, Diffuse1.a for all
# sixteen explicit blocks, Diffuse2.a on the two _MT FFP blocks, AlphaGenValue
# on the four FFP families, and isolation from the main/unrelated camera.
func _projshadow_mode(out_dir: String, prefix: String) -> void:
	DirAccess.make_dir_recursive_absolute(out_dir)
	get_root().get_window().size = Vector2i(1280, 720)
	Engine.time_scale = 0.0

	var parsed = JSON.parse_string(FileAccess.get_file_as_string(
			"res://shaders/object/pipeline_manifest.json"))
	if not parsed is Dictionary:
		push_error("render_swatch_probe projshadow: object pipeline manifest did not parse")
		quit(1)
		return
	var contracts = parsed.get("projected_shadow_contracts", {})
	if not contracts is Dictionary:
		push_error("render_swatch_probe projshadow: projected-shadow contracts did not parse")
		quit(1)
		return

	var scene := Node3D.new()
	root.add_child(scene)
	var world_env := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color.WHITE
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	world_env.environment = env
	scene.add_child(world_env)
	_add_framefx(scene, false)

	var textures := {
		"diffuse_low": _make_channel_diffuse_texture(64),
		"diffuse_high": _make_channel_diffuse_texture(192),
		"detail_low": _make_channel_detail_texture(64),
		"detail_high": _make_channel_detail_texture(192),
		"normal": _make_lighting_normal_texture(),
	}
	var entries: Array[Dictionary] = []
	var techniques: Array = parsed.get("techniques", [])
	const COLS := 6
	const SPACING := 2.2
	for i in range(techniques.size()):
		var technique: Dictionary = techniques[i]
		var name := str(technique["engine_enum"])
		var policies: Array = technique.get("policies", [])
		# Use one authored alpha-test policy so the black pass makes each
		# PROJSHAD coverage source observable at the real discard boundary.
		var policy := ""
		for candidate in ["cutout_mix", "cutout_alpha", "cutout_additive"]:
			if policies.has(candidate):
				policy = candidate
				break
		if policy.is_empty():
			push_error("render_swatch_probe projshadow: no cutout policy for %s" % name)
			quit(1)
			return
		var path := "res://shaders/object/%s/%s.gdshader" % [
				technique["directory"], policy]
		var shader := load(path) as Shader
		if shader == null:
			push_error("render_swatch_probe projshadow: could not load %s" % path)
			_projshadow_probe_disarm()
			quit(1)
			return
		var material := ShaderMaterial.new()
		material.shader = shader
		if not _bind_production_object_resources(material,
				str(technique["implementation"])):
			push_error("render_swatch_probe projshadow: production resources unavailable for %s" % path)
			_projshadow_probe_disarm()
			quit(1)
			return
		var quad := QuadMesh.new()
		quad.size = Vector2(1.55, 1.55)
		var mesh := MeshInstance3D.new()
		mesh.mesh = quad
		mesh.material_override = material
		mesh.position = Vector3((i % COLS) * SPACING,
				-(i / COLS) * SPACING, 0.0)
		mesh.set_instance_shader_parameter("u_point_light_count", 0.0)
		scene.add_child(mesh)
		entries.append({
			"name": name,
			"contract": str(contracts.get(name, "missing")),
			"material": material,
			"mesh": mesh,
		})

	var rows := int(ceil(float(entries.size()) / float(COLS)))
	var grid_w := COLS * SPACING
	var grid_h := rows * SPACING
	var aspect := 1280.0 / 720.0
	var camera := Camera3D.new()
	camera.projection = Camera3D.PROJECTION_ORTHOGONAL
	camera.size = maxf(grid_h, grid_w / aspect) + 0.6
	camera.position = Vector3((COLS - 1) * SPACING * 0.5,
			-(rows - 1) * SPACING * 0.5, 18.0)
	camera.current = true
	scene.add_child(camera)

	var captures := {}
	var states := ["inactive", "wrong_eye", "diffuse_low", "diffuse_high",
			"detail_low", "detail_high", "alpha_gen_low", "alpha_gen_high"]
	for state in states:
		_apply_projshadow_probe_state(entries, state, textures, camera)
		var frame: Image = await _capture_lighting_image()
		if frame == null:
			push_error("render_swatch_probe projshadow: no viewport image for %s" % state)
			_projshadow_probe_disarm()
			quit(1)
			return
		frame.convert(Image.FORMAT_RGBA8)
		captures[state] = frame
		if frame.save_png(out_dir.path_join("%s_%s.png" % [prefix, state])) != OK:
			push_error("render_swatch_probe projshadow: could not save %s" % state)
			_projshadow_probe_disarm()
			quit(1)
			return

	var pixel_scale := float((captures["diffuse_high"] as Image).get_height()) / camera.size
	var radius_px := maxi(8, int(0.60 * pixel_scale))
	var reports: Array[Dictionary] = []
	var failures: Array[String] = []
	for entry in entries:
		var mesh: MeshInstance3D = entry["mesh"]
		var center := camera.unproject_position(mesh.global_position)
		var rect := Rect2i(int(center.x) - radius_px, int(center.y) - radius_px,
				radius_px * 2, radius_px * 2)
		var contract := str(entry["contract"])
		var capture_darkness := 1.0 - _lighting_mean_luminance(
				captures["diffuse_high"], rect)
		var wrong_eye_delta := _lighting_mean_delta(
				captures["inactive"], captures["wrong_eye"], rect)
		var diffuse_delta := _lighting_mean_delta(
				captures["diffuse_low"], captures["diffuse_high"], rect)
		var detail_delta := _lighting_mean_delta(
				captures["detail_low"], captures["detail_high"], rect)
		var alpha_gen_delta := _lighting_mean_delta(
				captures["alpha_gen_low"], captures["alpha_gen_high"], rect)
		var has_pass := contract != "no_pass"
		var uses_detail := contract == "diffuse_detail_alpha_ffp"
		var uses_alpha_gen := contract.ends_with("_ffp")
		reports.append({
			"technique": entry["name"],
			"contract": contract,
			"capture_darkness": capture_darkness,
			"wrong_eye_delta": wrong_eye_delta,
			"diffuse_alpha_delta": diffuse_delta,
			"detail_alpha_delta": detail_delta,
			"alpha_gen_delta": alpha_gen_delta,
		})
		if contract == "missing":
			failures.append("%s has no projected-shadow contract" % entry["name"])
		if has_pass and capture_darkness < 0.12:
			failures.append("%s did not render its PROJSHAD pass (%f)" % [
					entry["name"], capture_darkness])
		elif not has_pass and capture_darkness > 0.02:
			failures.append("%s invented a PROJSHAD pass (%f)" % [
					entry["name"], capture_darkness])
		if wrong_eye_delta > 0.001:
			failures.append("%s applied PROJSHAD to an unrelated camera (%f)" % [
					entry["name"], wrong_eye_delta])
		_channel_expect_delta(failures, entry["name"], "PROJSHAD Diffuse1.a",
				diffuse_delta, has_pass, 0.05, 0.001)
		_channel_expect_delta(failures, entry["name"], "PROJSHAD Diffuse2.a",
				detail_delta, uses_detail, 0.03, 0.001)
		_channel_expect_delta(failures, entry["name"], "PROJSHAD AlphaGenValue",
				alpha_gen_delta, uses_alpha_gen, 0.03, 0.001)

	var manifest := {
		"version": 2,
		"probe": "object-slot-projected-shadow",
		"window": [1280, 720],
		"technique_count": entries.size(),
		"states": states,
		"techniques": reports,
		"failures": failures,
	}
	var mf := FileAccess.open(out_dir.path_join("%s_manifest.json" % prefix), FileAccess.WRITE)
	if mf != null:
		mf.store_string(JSON.stringify(manifest, "\t"))
		mf.close()
	_projshadow_probe_disarm()
	if failures.is_empty():
		print("render_swatch_probe projshadow: PASS - ", entries.size(),
				" techniques render black-on-white coverage and honor no-pass, texture-alpha, AlphaGen, and camera contracts")
		quit(0)
	else:
		for failure in failures:
			push_error("render_swatch_probe projshadow: " + failure)
		quit(1)


func _apply_projshadow_probe_state(entries: Array[Dictionary], state: String,
		textures: Dictionary, camera: Camera3D) -> void:
	var diffuse = textures["diffuse_low"] if state == "diffuse_low" else \
			textures["diffuse_high"]
	var detail = textures["detail_low"] if state == "detail_low" else \
			textures["detail_high"]
	var alpha_mod := 0.25 if state == "alpha_gen_low" else 1.0
	# The lighting block is pass-global: publish this probe's flat register
	# with every state so each capture starts from its own values.
	RenderingServer.global_shader_parameter_set("opennova_light_block_gain", Vector3.ONE)
	RenderingServer.global_shader_parameter_set("opennova_light_block_hemi_sky",
			Vector3(0.2, 0.2, 0.2))
	RenderingServer.global_shader_parameter_set("opennova_light_block_hemi_ground",
			Vector3(0.2, 0.2, 0.2))
	RenderingServer.global_shader_parameter_set("opennova_light_block_dir",
			Vector3(0.0, 0.0, -1.0))
	RenderingServer.global_shader_parameter_set("opennova_light_block_dir_color",
			Vector3(0.2, 0.2, 0.2))
	RenderingServer.global_shader_parameter_set("opennova_fog_enabled", false)
	for entry in entries:
		var material: ShaderMaterial = entry["material"]
		material.set_shader_parameter("u_diffuse", diffuse)
		material.set_shader_parameter("u_detail", detail)
		material.set_shader_parameter("u_normal_map", textures["normal"])
		material.set_shader_parameter("u_uv_transform_u", Vector3(1.0, 0.0, 0.0))
		material.set_shader_parameter("u_uv_transform_v", Vector3(0.0, 1.0, 0.0))
		material.set_shader_parameter("u_rgb_mod", Vector3.ONE)
		material.set_shader_parameter("u_alpha_mod", alpha_mod)
		material.set_shader_parameter("u_reflect_color", Color(0.7, 0.8, 0.9, 0.8))
		material.set_shader_parameter("u_alpha_test_threshold", 0.5)
		material.set_shader_parameter("u_alpha_test_invert", 0.0)
		material.set_shader_parameter("u_local_light_count", 0)
	# A slot capture camera is recognised by its cull mask: exactly one of the
	# twelve capture layer bits and nothing else. "inactive" is an ordinary
	# camera; "wrong_eye" is the beauty signature (a camera that is NOT a
	# capture) and must render the NORMAL technique, not coverage.
	if state == "inactive":
		camera.cull_mask = 0xFFFFF
	elif state == "wrong_eye":
		camera.cull_mask = FrameFx.kBeautyCameraMask
	else:
		camera.cull_mask = 1 << 1
	for entry in entries:
		var mesh: MeshInstance3D = entry["mesh"]
		mesh.layers = 0xFFFFF


func _projshadow_probe_disarm() -> void:
	pass


func _apply_channel_probe_state(entries: Array[Dictionary], state: String,
		textures: Dictionary) -> void:
	var color_phase := state.begins_with("rgb_") or \
			state.begins_with("specular_alpha_")
	var high := state.ends_with("_high")
	# The lighting block is pass-global, so one direction serves every swatch
	# of a capture. The coverage states swing it for the vertex_diffuse_alpha
	# source (its coverage is the directional self-shadow, direction only);
	# with a zero direction color no other technique's lit result can follow
	# the swing, so the flip reaches exactly the swatches it did per material.
	var direction := Vector3(0.0, 0.0, -1.0)
	var direction_color := Vector3.ZERO
	if state.begins_with("specular_alpha_"):
		direction_color = Vector3(0.55, 0.55, 0.55)
	elif state.begins_with("coverage_"):
		direction = Vector3(0.0, 0.0, -1.0) if high else Vector3(0.0, 0.0, 1.0)
	RenderingServer.global_shader_parameter_set("opennova_light_block_gain", Vector3.ONE)
	RenderingServer.global_shader_parameter_set("opennova_light_block_hemi_sky",
			Vector3(0.18, 0.18, 0.18))
	RenderingServer.global_shader_parameter_set("opennova_light_block_hemi_ground",
			Vector3(0.18, 0.18, 0.18))
	RenderingServer.global_shader_parameter_set("opennova_light_block_dir", direction)
	RenderingServer.global_shader_parameter_set("opennova_light_block_dir_color",
			direction_color)
	RenderingServer.global_shader_parameter_set("opennova_fog_enabled", false)
	for entry in entries:
		var color_mesh: MeshInstance3D = entry["color_mesh"]
		var coverage_mesh: MeshInstance3D = entry["coverage_mesh"]
		color_mesh.visible = color_phase
		coverage_mesh.visible = not color_phase
		var coverage_source := str(entry["coverage_source"])
		var diffuse_high := true
		var normal_high := true
		var reflect_alpha := 0.75
		var rgb_mod := Vector3.ONE
		var alpha_mod := 1.0
		if state.begins_with("rgb_"):
			rgb_mod = Vector3.ONE if high else Vector3(0.2, 0.2, 0.2)
		elif state.begins_with("specular_alpha_"):
			diffuse_high = high
		elif state.begins_with("coverage_"):
			# vertex_diffuse_alpha rides the pass direction published above.
			if coverage_source == "diffuse_alpha":
				diffuse_high = high
			elif coverage_source == "normal_alpha":
				normal_high = high
			elif coverage_source == "reflect_alpha":
				reflect_alpha = 0.75 if high else 0.25
		elif state.begins_with("alpha_gen_"):
			alpha_mod = 1.0 if high else 0.25

		for material in [entry["color_material"], entry["coverage_material"]]:
			var shader_material := material as ShaderMaterial
			var diffuse_key := "diffuse_high" if diffuse_high else "diffuse_low"
			if state.begins_with("specular_alpha_"):
				# Keep the diffuse term below framebuffer saturation so the alpha-
				# controlled highlight remains independently observable.
				diffuse_key = "specular_high" if high else "specular_low"
			shader_material.set_shader_parameter("u_diffuse",
					textures[diffuse_key])
			shader_material.set_shader_parameter("u_detail", textures["detail"])
			shader_material.set_shader_parameter("u_normal_map",
					textures["normal_high" if normal_high else "normal_low"])
			shader_material.set_shader_parameter("u_uv_transform_u", Vector3(1.0, 0.0, 0.0))
			shader_material.set_shader_parameter("u_uv_transform_v", Vector3(0.0, 1.0, 0.0))
			shader_material.set_shader_parameter("u_rgb_mod", rgb_mod)
			shader_material.set_shader_parameter("u_alpha_mod", alpha_mod)
			shader_material.set_shader_parameter("u_reflect_color",
					Color(0.7, 0.8, 0.9, reflect_alpha))
			shader_material.set_shader_parameter("u_alpha_test_threshold", 0.5)
			shader_material.set_shader_parameter("u_alpha_test_invert", 0.0)
			shader_material.set_shader_parameter("u_local_light_count", 0)
		color_mesh.set_instance_shader_parameter("u_point_light_count", 0.0)
		coverage_mesh.set_instance_shader_parameter("u_point_light_count", 0.0)


func _apply_lighting_probe_state(entries: Array[Dictionary], state: String) -> void:
	var hemi_sky := Vector3(0.04, 0.04, 0.04)
	var hemi_ground := hemi_sky
	var direction := Vector3(0.8, 0.0, -0.6)
	var direction_color := Vector3(0.42, 0.42, 0.42)
	var point_on := false
	if state == "direction_b":
		direction = Vector3(-0.8, 0.0, -0.6)
	elif state == "hemi_sky":
		hemi_sky = Vector3(0.34, 0.34, 0.34)
		hemi_ground = Vector3(0.025, 0.025, 0.025)
		direction_color = Vector3.ZERO
	elif state == "hemi_ground":
		hemi_sky = Vector3(0.025, 0.025, 0.025)
		hemi_ground = Vector3(0.34, 0.34, 0.34)
		direction_color = Vector3.ZERO
	elif state == "ambient_off" or state == "ambient_on":
		var ambient := 0.34 if state == "ambient_on" else 0.025
		hemi_sky = Vector3(ambient, ambient, ambient)
		hemi_ground = hemi_sky
		direction_color = Vector3.ZERO
	elif state == "point_off" or state == "point_on" or state == "point_static":
		hemi_sky = Vector3(0.025, 0.025, 0.025)
		hemi_ground = hemi_sky
		direction_color = Vector3.ZERO
		point_on = state == "point_on"

	# One pass-global lighting block per capture, shared by the sphere and its
	# MultiMesh twin exactly as a live world shares it across every draw.
	RenderingServer.global_shader_parameter_set("opennova_light_block_hemi_sky", hemi_sky)
	RenderingServer.global_shader_parameter_set("opennova_light_block_hemi_ground",
			hemi_ground)
	RenderingServer.global_shader_parameter_set("opennova_light_block_dir", direction)
	RenderingServer.global_shader_parameter_set("opennova_light_block_dir_color",
			direction_color)
	for entry in entries:
		var mesh: MeshInstance3D = entry["mesh"]
		var static_mesh: MultiMeshInstance3D = entry["static_mesh"]
		mesh.visible = state != "point_static"
		static_mesh.visible = state == "point_static"
		mesh.set_instance_shader_parameter("u_point_light_count", 1.0 if point_on else 0.0)
		var point_position := mesh.global_position + Vector3(0.0, 0.0, 2.0)
		mesh.set_instance_shader_parameter("u_point_light_posr_0",
				Vector4(point_position.x, point_position.y, point_position.z, 15.0 / 36.0))
		mesh.set_instance_shader_parameter("u_point_light_color_0",
				Vector4(0.8, 0.55, 0.3, 6.0))


func _make_static_point_light_atlas(entries: Array[Dictionary]) -> ImageTexture:
	var image := Image.create(9, entries.size(), false, Image.FORMAT_RGBAF)
	image.fill(Color(0.0, 0.0, 0.0, 0.0))
	for row in range(entries.size()):
		var mesh: MeshInstance3D = entries[row]["mesh"]
		var point_position := mesh.global_position + Vector3(0.0, 0.0, 2.0)
		image.set_pixel(0, row, Color(1.0, 0.0, 0.0, 0.0))
		image.set_pixel(1, row, Color(point_position.x, point_position.y,
				point_position.z, 15.0 / 36.0))
		image.set_pixel(2, row, Color(0.8, 0.55, 0.3, 6.0))
	return ImageTexture.create_from_image(image)


func _capture_lighting_image() -> Image:
	for _i in range(SETTLE_FRAMES):
		await process_frame
	return root.get_viewport().get_texture().get_image()


func _lighting_clip_rect(image: Image, rect: Rect2i) -> Rect2i:
	return rect.intersection(Rect2i(Vector2i.ZERO, image.get_size()))


func _lighting_mean_delta(a: Image, b: Image, rect: Rect2i) -> float:
	var clipped := _lighting_clip_rect(a, rect)
	if clipped.get_area() <= 0:
		return 0.0
	var total := 0.0
	for y in range(clipped.position.y, clipped.end.y):
		for x in range(clipped.position.x, clipped.end.x):
			var ca := a.get_pixel(x, y)
			var cb := b.get_pixel(x, y)
			total += (absf(ca.r - cb.r) + absf(ca.g - cb.g) + absf(ca.b - cb.b)) / 3.0
	return total / float(clipped.get_area())


func _lighting_max_byte_delta(a: Image, b: Image, rect: Rect2i) -> int:
	var clipped := _lighting_clip_rect(a, rect)
	var maximum := 0
	for y in range(clipped.position.y, clipped.end.y):
		for x in range(clipped.position.x, clipped.end.x):
			var ca := a.get_pixel(x, y)
			var cb := b.get_pixel(x, y)
			maximum = maxi(maximum, absi(ca.r8 - cb.r8))
			maximum = maxi(maximum, absi(ca.g8 - cb.g8))
			maximum = maxi(maximum, absi(ca.b8 - cb.b8))
			maximum = maxi(maximum, absi(ca.a8 - cb.a8))
	return maximum


func _lighting_mean_luminance(image: Image, rect: Rect2i) -> float:
	var clipped := _lighting_clip_rect(image, rect)
	if clipped.get_area() <= 0:
		return 0.0
	var total := 0.0
	for y in range(clipped.position.y, clipped.end.y):
		for x in range(clipped.position.x, clipped.end.x):
			var color := image.get_pixel(x, y)
			total += color.r * 0.2126 + color.g * 0.7152 + color.b * 0.0722
	return total / float(clipped.get_area())


func _lighting_mean_ring_delta(a: Image, b: Image, center: Vector2,
		inner_radius: int, outer_radius: int) -> float:
	var bounds := Rect2i(int(center.x) - outer_radius,
			int(center.y) - outer_radius, outer_radius * 2, outer_radius * 2)
	var clipped := _lighting_clip_rect(a, bounds)
	var inner_sq := float(inner_radius * inner_radius)
	var outer_sq := float(outer_radius * outer_radius)
	var total := 0.0
	var count := 0
	for y in range(clipped.position.y, clipped.end.y):
		for x in range(clipped.position.x, clipped.end.x):
			var dx := float(x) + 0.5 - center.x
			var dy := float(y) + 0.5 - center.y
			var radius_sq := dx * dx + dy * dy
			if radius_sq < inner_sq or radius_sq > outer_sq:
				continue
			var ca := a.get_pixel(x, y)
			var cb := b.get_pixel(x, y)
			total += (absf(ca.r - cb.r) + absf(ca.g - cb.g) +
					absf(ca.b - cb.b)) / 3.0
			count += 1
	return total / maxf(float(count), 1.0)


func _lighting_axis_contrast(image: Image, rect: Rect2i, horizontal: bool) -> float:
	var clipped := _lighting_clip_rect(image, rect)
	if clipped.get_area() <= 0:
		return 0.0
	var negative := 0.0
	var positive := 0.0
	var negative_count := 0
	var positive_count := 0
	var midpoint := (clipped.position.x + clipped.end.x) * 0.5 if horizontal else \
			(clipped.position.y + clipped.end.y) * 0.5
	for y in range(clipped.position.y, clipped.end.y):
		for x in range(clipped.position.x, clipped.end.x):
			var color := image.get_pixel(x, y)
			var luminance := color.r * 0.2126 + color.g * 0.7152 + color.b * 0.0722
			var coordinate := x if horizontal else y
			if coordinate < midpoint:
				negative += luminance
				negative_count += 1
			else:
				positive += luminance
				positive_count += 1
	return negative / maxf(float(negative_count), 1.0) - \
			positive / maxf(float(positive_count), 1.0)


func _make_lighting_diffuse_texture() -> ImageTexture:
	var image := Image.create(8, 8, false, Image.FORMAT_RGBA8)
	image.fill(Color8(184, 168, 152, 255))
	return ImageTexture.create_from_image(image)


func _make_lighting_detail_texture() -> ImageTexture:
	var image := Image.create(8, 8, false, Image.FORMAT_RGBA8)
	image.fill(Color8(128, 128, 128, 255))
	return ImageTexture.create_from_image(image)


func _make_lighting_normal_texture() -> ImageTexture:
	var image := Image.create(8, 8, false, Image.FORMAT_RGBA8)
	image.fill(Color8(128, 128, 255, 255))
	return ImageTexture.create_from_image(image)


func _make_lighting_object_normal_texture() -> ImageTexture:
	# Unlike a tangent-space map, (0,0,1) is not a neutral object-space map:
	# it really makes every texel face object +Z. Supply a normalized object
	# hemisphere varying over UV so direction and sky/ground response are both
	# observable without borrowing the mesh's tangent basis.
	var image := Image.create(64, 64, false, Image.FORMAT_RGBA8)
	for y in range(64):
		for x in range(64):
			var nx := (float(x) + 0.5) / 32.0 - 1.0
			var ny := 1.0 - (float(y) + 0.5) / 32.0
			var nz := sqrt(maxf(1.0 - minf(nx * nx + ny * ny, 1.0), 0.0))
			var n := Vector3(nx, ny, nz).normalized()
			# object_uv1.gdshaderinc applies the witnessed loader X reflection.
			image.set_pixel(x, y, Color(-n.x * 0.5 + 0.5,
					n.y * 0.5 + 0.5, n.z * 0.5 + 0.5, 1.0))
	return ImageTexture.create_from_image(image)


func _make_channel_diffuse_texture(alpha: int) -> ImageTexture:
	var image := Image.create(8, 8, false, Image.FORMAT_RGBA8)
	image.fill(Color8(184, 168, 152, alpha))
	return ImageTexture.create_from_image(image)


func _make_channel_specular_texture(alpha: int) -> ImageTexture:
	var image := Image.create(8, 8, false, Image.FORMAT_RGBA8)
	image.fill(Color8(16, 16, 16, alpha))
	return ImageTexture.create_from_image(image)


func _make_channel_normal_texture(alpha: int) -> ImageTexture:
	var image := Image.create(8, 8, false, Image.FORMAT_RGBA8)
	image.fill(Color8(128, 128, 255, alpha))
	return ImageTexture.create_from_image(image)


func _make_channel_detail_texture(alpha: int) -> ImageTexture:
	var image := Image.create(8, 8, false, Image.FORMAT_RGBA8)
	image.fill(Color8(128, 128, 128, alpha))
	return ImageTexture.create_from_image(image)
