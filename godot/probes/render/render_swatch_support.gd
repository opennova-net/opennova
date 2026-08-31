class_name RenderSwatchSupport
extends RefCounted

## What the render_swatch modes share: the code-generated textures and
## materials, the pixel metrics over captured images, the production resource
## binding, the per-cell diff of the swatch grid, and the Sink every mode
## reports through (the exit code, the errors, the log lines, the artifacts).

const SETTLE_FRAMES := 24


## The mode's result channel: quit() records the exit code (first wins),
## error() the failures, logv() the log lines; the probe turns it into the
## verdict.
class Sink:
	extends RefCounted

	var exit_code := -1
	var errors: Array[String] = []
	var last_line := ""
	var _ctx: ProbeContext

	func _init(ctx: ProbeContext) -> void:
		_ctx = ctx

	func quit(code: int) -> void:
		if exit_code < 0:
			exit_code = code

	func error(message: String) -> void:
		errors.append(message)
		_ctx.log("ERROR: " + message)

	func logv(parts: Array) -> void:
		var text := ""
		for part in parts:
			text += str(part)
		last_line = text
		_ctx.log(text)

	func artifact(label: String, path: String) -> void:
		if FileAccess.file_exists(path):
			_ctx.artifact(label, path)

	func verdict(summary_fallback: String) -> ProbeVerdict:
		var data := {"exit_code": exit_code, "errors": errors}
		if exit_code == 0:
			return ProbeVerdict.passed(last_line if not last_line.is_empty() else summary_fallback, data)
		return ProbeVerdict.failed(errors[0] if not errors.is_empty() else
				("%s (exit %d)" % [summary_fallback, exit_code]), data)


static func add_display_decode(scene: Node3D) -> DisplayDecode:
	var decode := DisplayDecode.new()
	scene.add_child(decode)
	return decode


static func bind_production_object_resources(material: ShaderMaterial,
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


static func load_manifest_for(png_path: String) -> Dictionary:
	var manifest_path := png_path.replace("_grid.png", "_manifest.json")
	if not FileAccess.file_exists(manifest_path):
		return {}
	var parsed = JSON.parse_string(FileAccess.get_file_as_string(manifest_path))
	return parsed if parsed is Dictionary else {}


static func per_cell_diff(a: Image, b: Image, manifest: Dictionary) -> Dictionary:
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


static func make_swatch_material(cache, cell: Dictionary, diffuse: Texture2D, detail: Texture2D, normal: Texture2D) -> ShaderMaterial:
	# Mirrors object_model.gd _create_material's uniform setup so the
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


static func make_diffuse_texture() -> ImageTexture:
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


static func make_detail_texture() -> ImageTexture:
	var image := Image.create(32, 32, false, Image.FORMAT_RGBA8)
	for y in range(32):
		for x in range(32):
			var checker := ((x / 4) + (y / 4)) % 2 == 0
			var v := 128 if checker else 220
			image.set_pixel(x, y, Color8(v, v, v, 255))
	return ImageTexture.create_from_image(image)


static func make_normal_texture() -> ImageTexture:
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


static func make_calibration_material(render_modes: String, byte_value: int,
		alpha: float, writes_alpha: bool) -> ShaderMaterial:
	var material := ShaderMaterial.new()
	var shader := Shader.new()
	var mode_suffix := ", " + render_modes if not render_modes.is_empty() else ""
	var alpha_line := "\n\tALPHA = u_alpha;" if writes_alpha else ""
	shader.set_code("""
shader_type spatial;
render_mode unshaded, cull_disabled%s;
#include "res://shaders/color.gdshaderinc"
uniform float u_byte;
uniform float u_alpha;
void fragment() {
	ALBEDO = scene_output(vec3(u_byte));%s
}
""" % [mode_suffix, alpha_line])
	material.shader = shader
	material.set_shader_parameter("u_byte", float(byte_value) / 255.0)
	material.set_shader_parameter("u_alpha", alpha)
	return material


static func add_calibration_stack(scene: Node3D, world_x: float,
		background: ShaderMaterial, foreground: ShaderMaterial) -> void:
	for layer in [[background, 0.0], [foreground, 1.0]]:
		var mesh := MeshInstance3D.new()
		var quad := QuadMesh.new()
		quad.size = Vector2(0.45, 0.45)
		mesh.mesh = quad
		mesh.material_override = layer[0]
		mesh.position = Vector3(world_x, 0.0, layer[1])
		scene.add_child(mesh)


static func make_layer_material(color: Color, rung: int) -> ShaderMaterial:
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


static func channel_expect_delta(failures: Array[String], technique: String,
		label: String, delta: float, expected: bool, minimum: float,
		maximum: float) -> void:
	if expected and delta < minimum:
		failures.append("%s did not react to %s (%f)" % [technique, label, delta])
	elif not expected and delta > maximum:
		failures.append("%s invented a %s response (%f)" % [technique, label, delta])


static func make_matchterrain_array(alpha: int) -> Texture2DArray:
	var image := Image.create(256, 256, false, Image.FORMAT_RGBA8)
	image.fill(Color8(72, 132, 196, alpha))
	var texture := Texture2DArray.new()
	var error := texture.create_from_images([image])
	if error != OK:
		push_error("render_swatch_probe matchterrain: Texture2DArray creation failed (%d)" % error)
	return texture


static func lighting_clip_rect(image: Image, rect: Rect2i) -> Rect2i:
	return rect.intersection(Rect2i(Vector2i.ZERO, image.get_size()))


static func lighting_mean_delta(a: Image, b: Image, rect: Rect2i) -> float:
	var clipped := lighting_clip_rect(a, rect)
	if clipped.get_area() <= 0:
		return 0.0
	var total := 0.0
	for y in range(clipped.position.y, clipped.end.y):
		for x in range(clipped.position.x, clipped.end.x):
			var ca := a.get_pixel(x, y)
			var cb := b.get_pixel(x, y)
			total += (absf(ca.r - cb.r) + absf(ca.g - cb.g) + absf(ca.b - cb.b)) / 3.0
	return total / float(clipped.get_area())


static func lighting_mean_luminance(image: Image, rect: Rect2i) -> float:
	var clipped := lighting_clip_rect(image, rect)
	if clipped.get_area() <= 0:
		return 0.0
	var total := 0.0
	for y in range(clipped.position.y, clipped.end.y):
		for x in range(clipped.position.x, clipped.end.x):
			var color := image.get_pixel(x, y)
			total += color.r * 0.2126 + color.g * 0.7152 + color.b * 0.0722
	return total / float(clipped.get_area())


static func lighting_mean_ring_delta(a: Image, b: Image, center: Vector2,
		inner_radius: int, outer_radius: int) -> float:
	var bounds := Rect2i(int(center.x) - outer_radius,
			int(center.y) - outer_radius, outer_radius * 2, outer_radius * 2)
	var clipped := lighting_clip_rect(a, bounds)
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


static func lighting_axis_contrast(image: Image, rect: Rect2i, horizontal: bool) -> float:
	var clipped := lighting_clip_rect(image, rect)
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


static func make_lighting_diffuse_texture() -> ImageTexture:
	var image := Image.create(8, 8, false, Image.FORMAT_RGBA8)
	image.fill(Color8(184, 168, 152, 255))
	return ImageTexture.create_from_image(image)


static func make_lighting_detail_texture() -> ImageTexture:
	var image := Image.create(8, 8, false, Image.FORMAT_RGBA8)
	image.fill(Color8(128, 128, 128, 255))
	return ImageTexture.create_from_image(image)


static func make_lighting_normal_texture() -> ImageTexture:
	var image := Image.create(8, 8, false, Image.FORMAT_RGBA8)
	image.fill(Color8(128, 128, 255, 255))
	return ImageTexture.create_from_image(image)


static func make_lighting_object_normal_texture() -> ImageTexture:
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


static func make_channel_diffuse_texture(alpha: int) -> ImageTexture:
	var image := Image.create(8, 8, false, Image.FORMAT_RGBA8)
	image.fill(Color8(184, 168, 152, alpha))
	return ImageTexture.create_from_image(image)


static func make_channel_specular_texture(alpha: int) -> ImageTexture:
	var image := Image.create(8, 8, false, Image.FORMAT_RGBA8)
	image.fill(Color8(16, 16, 16, alpha))
	return ImageTexture.create_from_image(image)


static func make_channel_normal_texture(alpha: int) -> ImageTexture:
	var image := Image.create(8, 8, false, Image.FORMAT_RGBA8)
	image.fill(Color8(128, 128, 255, alpha))
	return ImageTexture.create_from_image(image)


static func make_channel_detail_texture(alpha: int) -> ImageTexture:
	var image := Image.create(8, 8, false, Image.FORMAT_RGBA8)
	image.fill(Color8(128, 128, 128, alpha))
	return ImageTexture.create_from_image(image)
