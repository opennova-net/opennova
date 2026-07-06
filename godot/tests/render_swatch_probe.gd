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
#   capture: "$GODOT_BIN" --path godot -s res://tests/render_swatch_probe.gd -- capture <out_dir> [prefix]
#   compare: "$GODOT_BIN" --path godot -s res://tests/render_swatch_probe.gd -- compare <a.png> <b.png>

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
	elif args.size() >= 3 and args[0] == "compare":
		_compare_mode(args[1], args[2])
	else:
		push_error("render_swatch_probe: usage -- capture <out_dir> [prefix] | compare <a.png> <b.png>")
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
	# swatch pins the same host state the runtime binds.
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
	material.set_shader_parameter("u_uv_offset", Vector2.ZERO)
	material.set_shader_parameter("u_uv_scale", Vector2.ONE)
	material.set_shader_parameter("u_uv_rotation", 0.0)
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
