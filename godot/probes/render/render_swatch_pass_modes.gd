class_name RenderSwatchPassModes
extends RefCounted

## The render_swatch auxiliary-pass raster proofs (modes clip, matchterrain,
## glow and projshadow): the reflection CLIP gate, the skinned MATCHTERRAIN
## pass, the selective Q3 GLOW and the slot PROJSHAD coverage, each technique
## on the probe stage through its witnessed states.

var _ctx: ProbeContext
var _stage: ProbeStage
var _sink: RenderSwatchSupport.Sink


func _init(ctx: ProbeContext, stage: ProbeStage, sink: RenderSwatchSupport.Sink) -> void:
	_ctx = ctx
	_stage = stage
	_sink = sink


func _capture_lighting_image() -> Image:
	return await _stage.capture_image(_ctx.tree, RenderSwatchSupport.SETTLE_FRAMES)


# Raster proof for retail's auxiliary CLIP technique. Every live object
# technique renders through the same mirrored-eye gate, but only effects with
# an explicit retail CLIP block discard below waterHeight-0.1. Missing CLIP
# blocks inherit NORMAL; skinned submissions were skipped by the collector.
# The wrong-eye and disarmed captures prove this is reflection-camera state,
# not a global main-view slice.
func clip_mode(out_dir: String, prefix: String) -> void:
	DirAccess.make_dir_recursive_absolute(out_dir)
	_stage.set_stage_size(Vector2i(1280, 360))
	_ctx.set_time_scale(0.0)

	var parsed = JSON.parse_string(FileAccess.get_file_as_string(
			"res://shaders/object/pipeline_manifest.json"))
	if not parsed is Dictionary:
		_sink.error("render_swatch_probe clip: object pipeline manifest did not parse")
		_sink.quit(1)
		return

	var scene := Node3D.new()
	_stage.add_scene(scene)
	var world_env := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.008, 0.008, 0.012)
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	world_env.environment = env
	scene.add_child(world_env)
	RenderSwatchSupport.add_framefx(scene, false)

	var diffuse := RenderSwatchSupport.make_lighting_diffuse_texture()
	var detail := RenderSwatchSupport.make_lighting_detail_texture()
	var tangent_normal := RenderSwatchSupport.make_lighting_normal_texture()
	var object_normal := RenderSwatchSupport.make_lighting_object_normal_texture()
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
			_sink.error("render_swatch_probe clip: could not load %s" % path)
			_clip_probe_disarm()
			_sink.quit(1)
			return
		var material := ShaderMaterial.new()
		material.shader = shader
		if not RenderSwatchSupport.bind_production_object_resources(material,
				str(technique["implementation"])):
			_sink.error("render_swatch_probe clip: production resources unavailable for %s" % path)
			_clip_probe_disarm()
			_sink.quit(1)
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
	var beauty_mask := camera.cull_mask
	var captures := {}
	# The reflected pass is the camera whose mask omits the water layer: the
	# same water plane leaves a beauty camera unclipped.
	var states := ["inactive", "beauty", "reflection"]
	for state in states:
		rs.global_shader_parameter_set("opennova_water_active", state != "inactive")
		camera.cull_mask = Water.REFLECTION_CULL_MASK if state == "reflection" \
				else beauty_mask
		var frame: Image = await _capture_lighting_image()
		if frame == null:
			_sink.error("render_swatch_probe clip: no viewport image for %s" % state)
			_clip_probe_disarm()
			_sink.quit(1)
			return
		frame.convert(Image.FORMAT_RGBA8)
		captures[state] = frame
		if frame.save_png(out_dir.path_join("%s_%s.png" % [prefix, state])) != OK:
			_sink.error("render_swatch_probe clip: could not save %s" % state)
			_clip_probe_disarm()
			_sink.quit(1)
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
		var inactive_bottom_luma := RenderSwatchSupport.lighting_mean_luminance(
				captures["inactive"], bottom_rect)
		var reflection_bottom_delta := RenderSwatchSupport.lighting_mean_delta(
				captures["inactive"], captures["reflection"], bottom_rect)
		var reflection_top_delta := RenderSwatchSupport.lighting_mean_delta(
				captures["inactive"], captures["reflection"], top_rect)
		var beauty_delta := RenderSwatchSupport.lighting_mean_delta(
				captures["inactive"], captures["beauty"], bottom_rect)
		var clip_expected: bool = str(entry["clip_class"]) in ["explicit",
				"explicit_unskinned_or_submit_skip_skinned"]
		reports.append({
			"technique": entry["name"],
			"clip_class": entry["clip_class"],
			"inactive_bottom_luma": inactive_bottom_luma,
			"reflection_bottom_delta": reflection_bottom_delta,
			"reflection_top_delta": reflection_top_delta,
			"beauty_bottom_delta": beauty_delta,
		})
		if inactive_bottom_luma < 0.012:
			failures.append("%s was not visible before clipping (%f)" % [
					entry["name"], inactive_bottom_luma])
		RenderSwatchSupport.channel_expect_delta(failures, entry["name"], "reflection CLIP",
				reflection_bottom_delta, clip_expected, 0.012, 0.001)
		if reflection_top_delta > 0.001:
			failures.append("%s clipped above waterHeight (%f)" % [
					entry["name"], reflection_top_delta])
		if beauty_delta > 0.001:
			failures.append("%s clipped a camera that draws the water layer (%f)" % [
					entry["name"], beauty_delta])

	var manifest := {
		"version": 1,
		"probe": "object-water-reflection-clip",
		"window": [1280, 360],
		"water_height": 0.1,
		"retail_clip_plane": 0.1,
		"technique_count": entries.size(),
		"states": states,
		"techniques": reports,
		"failures": failures,
	}
	RenderSwatchSupport.write_manifest(_sink, out_dir, prefix, manifest)
	_clip_probe_disarm()
	if failures.is_empty():
		_sink.logv(["render_swatch_probe clip: PASS - ", entries.size(),
				" techniques honor explicit/fallback/skinned reflection CLIP contracts"])
		_sink.quit(0)
	else:
		for failure in failures:
			_sink.error("render_swatch_probe clip: " + failure)
		_sink.quit(1)


func _clip_probe_disarm() -> void:
	RenderingServer.global_shader_parameter_set("opennova_water_active", false)


# Raster proof for the retail skinned MATCHTERRAIN technique. Retail draws a
# terrain-colored pass first, then NORMAL over accepted alpha-test pixels.
# The folded implementation must therefore appear only in NORMAL's rejected
# pixels, only for the nine skinned highest-quality runtime techniques, only
# while crouched/prone with a resident terrain page, and must consume both the
# composed page's RGB and alpha lighting channel.
func matchterrain_mode(out_dir: String, prefix: String) -> void:
	DirAccess.make_dir_recursive_absolute(out_dir)
	_stage.set_stage_size(Vector2i(1280, 720))
	_ctx.set_time_scale(0.0)

	var parsed = JSON.parse_string(FileAccess.get_file_as_string(
			"res://shaders/object/pipeline_manifest.json"))
	if not parsed is Dictionary:
		_sink.error("render_swatch_probe matchterrain: object pipeline manifest did not parse")
		_sink.quit(1)
		return
	var contracts = parsed.get("match_terrain_contracts", {})
	if not contracts is Dictionary:
		_sink.error("render_swatch_probe matchterrain: contracts did not parse")
		_sink.quit(1)
		return

	var scene := Node3D.new()
	_stage.add_scene(scene)
	var world_env := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.008, 0.008, 0.012)
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	world_env.environment = env
	scene.add_child(world_env)
	RenderSwatchSupport.add_framefx(scene, false)

	var tile_low := RenderSwatchSupport.make_matchterrain_array(32)
	var tile_high := RenderSwatchSupport.make_matchterrain_array(224)
	var textures := {
		"diffuse_low": RenderSwatchSupport.make_channel_diffuse_texture(32),
		"diffuse_high": RenderSwatchSupport.make_channel_diffuse_texture(224),
		"detail": RenderSwatchSupport.make_lighting_detail_texture(),
		"normal_low": RenderSwatchSupport.make_channel_normal_texture(32),
		"normal_high": RenderSwatchSupport.make_channel_normal_texture(224),
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
			_sink.error("render_swatch_probe matchterrain: no cutout policy for %s" % name)
			_sink.quit(1)
			return
		var path := "res://shaders/object/%s/%s.gdshader" % [
				technique["directory"], policy]
		var shader := load(path) as Shader
		if shader == null:
			_sink.error("render_swatch_probe matchterrain: could not load %s" % path)
			_sink.quit(1)
			return
		var material := ShaderMaterial.new()
		material.shader = shader
		if not RenderSwatchSupport.bind_production_object_resources(material,
				str(technique["implementation"])):
			_sink.error("render_swatch_probe matchterrain: production resources unavailable for %s" % path)
			_sink.quit(1)
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

	var camera := RenderSwatchSupport.add_grid_camera(scene, entries.size(), COLS, SPACING)

	var captures := {}
	var states := ["disabled_rejected", "missing_cache", "page_unready",
			"active_alpha_low", "active_alpha_high", "accepted_disabled",
			"accepted_active"]
	for state in states:
		_apply_matchterrain_probe_state(entries, state, textures)
		var frame: Image = await _capture_lighting_image()
		if frame == null:
			_sink.error("render_swatch_probe matchterrain: no viewport image for %s" % state)
			_sink.quit(1)
			return
		frame.convert(Image.FORMAT_RGBA8)
		captures[state] = frame
		if frame.save_png(out_dir.path_join("%s_%s.png" % [prefix, state])) != OK:
			_sink.error("render_swatch_probe matchterrain: could not save %s" % state)
			_sink.quit(1)
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
		var active_luma := RenderSwatchSupport.lighting_mean_luminance(
				captures["active_alpha_high"], rect)
		var activation_delta := RenderSwatchSupport.lighting_mean_delta(
				captures["disabled_rejected"], captures["active_alpha_high"], rect)
		var tile_alpha_delta := RenderSwatchSupport.lighting_mean_delta(
				captures["active_alpha_low"], captures["active_alpha_high"], rect)
		var missing_delta := RenderSwatchSupport.lighting_mean_delta(
				captures["disabled_rejected"], captures["missing_cache"], rect)
		var unready_delta := RenderSwatchSupport.lighting_mean_delta(
				captures["disabled_rejected"], captures["page_unready"], rect)
		var accepted_delta := RenderSwatchSupport.lighting_mean_delta(
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
		RenderSwatchSupport.channel_expect_delta(failures, entry["name"], "MATCHTERRAIN stance/page gate",
				activation_delta, has_pass, 0.02, 0.001)
		RenderSwatchSupport.channel_expect_delta(failures, entry["name"], "terrain alpha lighting",
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
	RenderSwatchSupport.write_manifest(_sink, out_dir, prefix, manifest)
	if failures.is_empty():
		_sink.logv(["render_swatch_probe matchterrain: PASS - ", entries.size(),
				" techniques honor stance, residency, coverage, and tile-channel contracts"])
		_sink.quit(0)
	else:
		for failure in failures:
			_sink.error("render_swatch_probe matchterrain: " + failure)
		_sink.quit(1)


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


# Highest-quality GLOW proof. _FFP LUM copies NORMAL, fixed Glass uses the
# sun-rotated specular cube, and every other live runtime technique has no
# GLOW pass. Ring pixels exercise the compositor-owned typed Q3 target and exact
# FrameFX kernel; the away-sun capture proves Glass tracks MatRotSpecular.
func glow_mode(out_dir: String, prefix: String) -> void:
	DirAccess.make_dir_recursive_absolute(out_dir)
	_stage.set_stage_size(Vector2i(1280, 720))
	_ctx.set_time_scale(0.0)
	var parsed = JSON.parse_string(FileAccess.get_file_as_string(
			"res://shaders/object/pipeline_manifest.json"))
	if not parsed is Dictionary:
		_sink.error("render_swatch_probe glow: object pipeline manifest did not parse")
		_sink.quit(1)
		return
	var contracts = parsed.get("glow_contracts", {})
	if not contracts is Dictionary:
		_sink.error("render_swatch_probe glow: contracts did not parse")
		_sink.quit(1)
		return

	var scene := Node3D.new()
	_stage.add_scene(scene)
	var world_env := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.006, 0.006, 0.009)
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	world_env.environment = env
	scene.add_child(world_env)
	var frame_renderer := RenderSwatchSupport.add_framefx(scene, true)

	var diffuse := RenderSwatchSupport.make_channel_diffuse_texture(255)
	var detail := RenderSwatchSupport.make_lighting_detail_texture()
	var normal := RenderSwatchSupport.make_lighting_normal_texture()
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
			_sink.error("render_swatch_probe glow: could not load %s" % path)
			_sink.quit(1)
			return
		var material := ShaderMaterial.new()
		material.shader = shader
		if not RenderSwatchSupport.bind_production_object_resources(material,
				str(technique["implementation"])):
			_sink.error("render_swatch_probe glow: production resources unavailable for %s" % path)
			_sink.quit(1)
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

	var camera := RenderSwatchSupport.add_grid_camera(scene, entries.size(), COLS, SPACING)

	var captures := {}
	var states := ["nopass_off", "nopass_on", "glow_off_aligned",
			"glow_on_aligned", "glow_off_away", "glow_on_away"]
	for state in states:
		frame_renderer.visible = state in ["nopass_on", "glow_on_aligned", "glow_on_away"]
		# In play the GameWorld leg table compiles focused Q3 from the final beauty
		# camera every frame; this stage drives that leg explicitly.
		frame_renderer.advance_frame()
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
			_sink.error("render_swatch_probe glow: no viewport image for %s" % state)
			_sink.quit(1)
			return
		frame.convert(Image.FORMAT_RGBA8)
		captures[state] = frame
		if frame.save_png(out_dir.path_join("%s_%s.png" % [prefix, state])) != OK:
			_sink.error("render_swatch_probe glow: could not save %s" % state)
			_sink.quit(1)
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
		var bloom_delta := RenderSwatchSupport.lighting_mean_ring_delta(
				captures["glow_off_aligned"], captures["glow_on_aligned"], center,
				inner_radius, outer_radius) if contract != "no_pass" else \
				RenderSwatchSupport.lighting_mean_ring_delta(captures["nopass_off"],
						captures["nopass_on"], center, inner_radius, outer_radius)
		var away_bloom_delta := RenderSwatchSupport.lighting_mean_ring_delta(
				captures["glow_off_away"], captures["glow_on_away"], center,
				inner_radius, outer_radius)
		var core_rect := Rect2i(int(center.x) - core_radius,
				int(center.y) - core_radius, core_radius * 2, core_radius * 2)
		var sun_delta := RenderSwatchSupport.lighting_mean_delta(
				captures["glow_on_aligned"], captures["glow_on_away"], core_rect)
		reports.append({"technique": entry["name"], "contract": contract,
			"aligned_bloom_ring_delta": bloom_delta,
			"away_bloom_ring_delta": away_bloom_delta,
			"sun_rotation_core_delta": sun_delta})
		if contract == "missing":
			failures.append("%s has no GLOW contract" % entry["name"])
		RenderSwatchSupport.channel_expect_delta(failures, entry["name"], "GLOW bloom",
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
	RenderSwatchSupport.write_manifest(_sink, out_dir, prefix, manifest)
	if failures.is_empty():
		_sink.logv(["render_swatch_probe glow: PASS - ", entries.size(),
				" techniques honor LUM copy, glass sun glint, and no-pass contracts"])
		_sink.quit(0)
	else:
		for failure in failures:
			_sink.error("render_swatch_probe glow: " + failure)
		_sink.quit(1)


# Raster proof for the render-slot PROJSHAD capture: SlotShadow's typed
# requests drawn by the SlotCaptureAdapter RenderingDevice pass over the
# engine's per-technique coverage and blend tables (which ctest
# renderer_material_classify pins per technique). The synthetic 3DI fixtures
# stand in for the technique matrix, each caster alone in its own slot: the
# opaque FFP crate (black replace), the mount's alpha-tested FFP sight
# (Diffuse1.a x AlphaGen at the discard boundary), the pump's alpha-tested
# _MT post (x Diffuse2.a), the shed's alpha-blend FFP bulb (black blended by
# its coverage), and the two no-pass surfaces (the mount's additive LUM slab,
# the armory's glass). Every capture is read back from its resolve target
# under paired texture-alpha / detail-alpha / AlphaGen states, and the stage's
# beauty frame proves the NORMAL technique stays where the capture is black.
# Run without a mission: the caster group is scene-tree wide.
const PROJSHADOW_CASTERS: Array[Dictionary] = [
	{"name": "crate_ffp_opaque", "fixture": "crate", "surfaces": "", "kind": "opaque"},
	{"name": "mount_ffp_alpha_test", "fixture": "mount", "surfaces": "/fixed/cutout",
		"kind": "alpha_test", "detail": false},
	{"name": "pump_mt_alpha_test", "fixture": "pump_mtrl1_mt_alphatest",
		"surfaces": "/fixed/cutout", "kind": "alpha_test", "detail": true},
	{"name": "shed_ffp_alpha_blend", "fixture": "shed", "surfaces": "/fixed/alpha",
		"kind": "blend"},
	{"name": "mount_additive_lum", "fixture": "mount", "surfaces": "/self_lit/additive",
		"kind": "no_pass"},
	{"name": "armory_glass", "fixture": "armory", "surfaces": "/glass/", "kind": "no_pass"},
]
const PROJSHADOW_STATES: Array[String] = ["diffuse_low", "diffuse_high",
		"detail_low", "detail_high", "alpha_gen_low", "alpha_gen_high"]
# The alpha test ref of the cutout fixtures is byte 32 (0.125): the "high"
# texture alpha 192 (0.753) passes, the "low" alpha 16 (0.063) fails, and
# AlphaGen 0.1 pulls the high alpha (0.075) under the ref while 1.0 keeps it.
const PROJSHADOW_ALPHA_LOW := 16
const PROJSHADOW_ALPHA_HIGH := 192
const PROJSHADOW_ALPHA_GEN_LOW := 0.1


func projshadow_mode(out_dir: String, prefix: String) -> void:
	DirAccess.make_dir_recursive_absolute(out_dir)
	_stage.set_stage_size(Vector2i(1280, 720))
	_ctx.set_time_scale(0.0)

	var scene := Node3D.new()
	_stage.add_scene(scene)
	var world_env := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.1, 0.1, 0.12)
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	env.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	world_env.environment = env
	scene.add_child(world_env)
	RenderSwatchSupport.add_framefx(scene, false)
	var env_data := EnvFile.new()
	env_data.reset_to_default()
	env_data.set_curtime(1200)
	var mission_env := MissionEnvironment.new()
	mission_env.environment_data = env_data
	scene.add_child(mission_env)
	var camera := Camera3D.new()
	camera.current = true
	scene.add_child(camera)
	var shadow := SlotShadow.new()
	scene.add_child(shadow)
	shadow.set_environment_node(mission_env)
	shadow.set_shadow_detail(4)  # mask 0: every slot re-captures every frame

	var textures := {
		"diffuse_low": RenderSwatchSupport.make_channel_diffuse_texture(PROJSHADOW_ALPHA_LOW),
		"diffuse_high": RenderSwatchSupport.make_channel_diffuse_texture(PROJSHADOW_ALPHA_HIGH),
		"detail_low": RenderSwatchSupport.make_channel_detail_texture(PROJSHADOW_ALPHA_LOW),
		"detail_high": RenderSwatchSupport.make_channel_detail_texture(PROJSHADOW_ALPHA_HIGH),
	}
	const SPACING := 8.0
	var casters: Array[Dictionary] = []
	for i in range(PROJSHADOW_CASTERS.size()):
		var recipe: Dictionary = PROJSHADOW_CASTERS[i]
		var path := "res://../fixtures/threedi/synth/%s.3di" % String(recipe["fixture"])
		var data := ObjectData.new()
		if data.open_file(ProjectSettings.globalize_path(path)) != OK:
			_sink.error("render_swatch_probe projshadow: could not open %s" % path)
			_sink.quit(1)
			return
		var model := ObjectModel.new()
		scene.add_child(model)
		model.set_process(false)
		model.set_object_data(data)
		model.position = Vector3(float(i) * SPACING, 0.0, 0.0)
		model.set_shadow_caster_enabled(true)
		# The whole model inside its capture (the placer's sphere stamp is the
		# fallback law: half the model-bounds diagonal).
		var radius := maxf(0.5, model.get_model_bounds().size.length() * 0.5)
		model.set_shadow_bound_radii(radius, radius)
		var surfaces := String(recipe["surfaces"])
		if not surfaces.is_empty():
			_keep_only_shader_surfaces(model, surfaces)
		model.advance_runtime_frame(1.0 / 62.0)
		var materials: Array[ShaderMaterial] = []
		for row in model.get_surface_materials():
			var material := row as ShaderMaterial
			if material != null:
				materials.append(material)
		casters.append({"recipe": recipe, "model": model, "materials": materials})
	var row_center := Vector3(float(PROJSHADOW_CASTERS.size() - 1) * SPACING * 0.5, 0.5, 0.0)
	camera.position = row_center + Vector3(0.0, 6.0, 16.0)
	camera.look_at(row_center, Vector3.UP)

	var captures := {}
	var beauty := {}
	var reports := {}
	for state in PROJSHADOW_STATES:
		_apply_projshadow_probe_state(casters, state, textures)
		shadow.advance_frame()
		var frame: Image = await _capture_lighting_image()
		if frame == null:
			_sink.error("render_swatch_probe projshadow: no stage image for %s" % state)
			_sink.quit(1)
			return
		RenderingServer.force_sync()
		frame.convert(Image.FORMAT_RGBA8)
		beauty[state] = frame
		reports[state] = shadow.get_report()
		var per_caster := {}
		for entry in casters:
			var name := String(entry["recipe"]["name"])
			var order: int = shadow.get_capture_order_of(entry["model"])
			if order < 0:
				_sink.error("render_swatch_probe projshadow: %s took no slot in %s" % [name, state])
				_sink.quit(1)
				return
			var image: Image = shadow.get_capture_image(order)
			if image == null:
				_sink.error("render_swatch_probe projshadow: %s has no capture in %s" % [name, state])
				_sink.quit(1)
				return
			per_caster[name] = image
			if image.save_png(out_dir.path_join("%s_%s_%s.png" % [prefix, name, state])) != OK:
				_sink.error("render_swatch_probe projshadow: could not save %s %s" % [name, state])
				_sink.quit(1)
				return
		captures[state] = per_caster
		if frame.save_png(out_dir.path_join("%s_beauty_%s.png" % [prefix, state])) != OK:
			_sink.error("render_swatch_probe projshadow: could not save the %s beauty frame" % state)
			_sink.quit(1)
			return

	var failures: Array[String] = []
	var rows: Array[Dictionary] = []
	for entry in casters:
		var recipe: Dictionary = entry["recipe"]
		var name := String(recipe["name"])
		var kind := String(recipe["kind"])
		var black := {}
		var darkest := {}
		for state in PROJSHADOW_STATES:
			var image: Image = captures[state][name]
			black[state] = _projshadow_black_count(image)
			darkest[state] = _projshadow_darkest(image)
		rows.append({"caster": name, "kind": kind, "black_pixels": black, "darkest": darkest})
		match kind:
			"opaque":
				for state in PROJSHADOW_STATES:
					if int(black[state]) < 50:
						failures.append("%s did not render its PROJSHAD pass in %s" % [name, state])
				if absi(int(black["diffuse_low"]) - int(black["diffuse_high"])) > 2 \
						or absi(int(black["alpha_gen_low"]) - int(black["alpha_gen_high"])) > 2:
					failures.append("%s (opaque replace) reacted to a coverage change" % name)
			"alpha_test":
				var uses_detail := bool(recipe.get("detail", false))
				_projshadow_expect_present(failures, name, "diffuse_high", black, true)
				_projshadow_expect_present(failures, name, "diffuse_low", black, false)
				_projshadow_expect_present(failures, name, "alpha_gen_high", black, true)
				_projshadow_expect_present(failures, name, "alpha_gen_low", black, false)
				_projshadow_expect_present(failures, name, "detail_high", black, true)
				_projshadow_expect_present(failures, name, "detail_low", black, not uses_detail)
			"blend":
				if float(darkest["diffuse_high"]) > 0.35 or float(darkest["diffuse_high"]) < 0.12:
					failures.append("%s did not blend its coverage (darkest %f, expected ~0.25)" % [
							name, darkest["diffuse_high"]])
				if float(darkest["diffuse_low"]) < 0.85:
					failures.append("%s did not react to Diffuse1.a (darkest %f)" % [name, darkest["diffuse_low"]])
				if float(darkest["alpha_gen_low"]) < 0.85:
					failures.append("%s did not react to AlphaGenValue (darkest %f)" % [name, darkest["alpha_gen_low"]])
				if absf(float(darkest["detail_low"]) - float(darkest["detail_high"])) > 0.02:
					failures.append("%s (single-texture) invented a Diffuse2.a response" % name)
			"no_pass":
				for state in PROJSHADOW_STATES:
					if int(black[state]) != 0 or float(darkest[state]) < 0.98:
						failures.append("%s invented a PROJSHAD pass in %s" % [name, state])
	# The adapter's own accounting of the walk: two no-pass surfaces (the
	# additive LUM slab and the glass) and one blended command per state.
	for state in PROJSHADOW_STATES:
		var report: Dictionary = reports[state]
		if String(report.get("slot_status", "")) != "drawn":
			failures.append("the slot pass did not draw in %s: %s" % [state, report.get("slot_failure", "")])
		if int(report.get("slot_no_pass_surfaces", 0)) < 2:
			failures.append("fewer than two no-pass surfaces counted in %s" % state)
		if int(report.get("slot_blended_commands", 0)) < 1:
			failures.append("no blended command compiled in %s" % state)
		if int(report.get("slot_unclassified_surfaces", 0)) != 0:
			failures.append("an unclassified surface reached the walk in %s" % state)
	# Beauty isolation: the crate's top reads its NORMAL shading in the stage
	# frame while its capture is black.
	var crate: ObjectModel = casters[0]["model"]
	var crate_pixel := Vector2i(camera.unproject_position(crate.global_position + Vector3(0.0, 0.5, 0.0)))
	var beauty_frame: Image = beauty["diffuse_high"]
	var beauty_color := beauty_frame.get_pixel(
			clampi(crate_pixel.x, 0, beauty_frame.get_width() - 1),
			clampi(crate_pixel.y, 0, beauty_frame.get_height() - 1))
	if maxf(beauty_color.r, maxf(beauty_color.g, beauty_color.b)) < 0.1:
		failures.append("the beauty frame is black at the crate (%s): the NORMAL technique was replaced" % beauty_color)

	var manifest := {
		"version": 3,
		"probe": "object-slot-projected-shadow",
		"window": [1280, 720],
		"backend": String(reports["diffuse_high"].get("slot_backend", "")),
		"caster_count": casters.size(),
		"states": PROJSHADOW_STATES,
		"casters": rows,
		"beauty_at_crate": [beauty_color.r, beauty_color.g, beauty_color.b],
		"failures": failures,
	}
	RenderSwatchSupport.write_manifest(_sink, out_dir, prefix, manifest)
	if failures.is_empty():
		_sink.logv(["render_swatch_probe projshadow: PASS - ", casters.size(),
				" casters through the RenderingDevice slot pass honor black-over-white, texture-alpha, detail-alpha, AlphaGen, blend and no-pass contracts"])
		_sink.quit(0)
	else:
		for failure in failures:
			_sink.error("render_swatch_probe projshadow: " + failure)
		_sink.quit(1)


func _keep_only_shader_surfaces(root: Node, shader_fragment: String) -> void:
	if root is MeshInstance3D:
		var mesh_instance := root as MeshInstance3D
		var material := mesh_instance.get_active_material(0) as ShaderMaterial
		var shader_path := material.shader.resource_path \
				if material != null and material.shader != null else ""
		mesh_instance.visible = shader_fragment in shader_path
	for child in root.get_children():
		_keep_only_shader_surfaces(child, shader_fragment)


func _projshadow_black_count(image: Image) -> int:
	var count := 0
	for y in range(image.get_height()):
		for x in range(image.get_width()):
			var p := image.get_pixel(x, y)
			if maxf(p.r, maxf(p.g, p.b)) < 0.5:
				count += 1
	return count


func _projshadow_darkest(image: Image) -> float:
	var darkest := 1.0
	for y in range(image.get_height()):
		for x in range(image.get_width()):
			var p := image.get_pixel(x, y)
			darkest = minf(darkest, maxf(p.r, maxf(p.g, p.b)))
	return darkest


func _projshadow_expect_present(failures: Array[String], name: String, state: String,
		black: Dictionary, present: bool) -> void:
	var count := int(black[state])
	if present and count < 20:
		failures.append("%s was discarded in %s (%d black pixels): the coverage fell under the alpha ref" % [
				name, state, count])
	elif not present and count != 0:
		failures.append("%s survived %s (%d black pixels): the coverage did not fall under the alpha ref" % [
				name, state, count])


func _apply_projshadow_probe_state(casters: Array[Dictionary], state: String,
		textures: Dictionary) -> void:
	var diffuse = textures["diffuse_low"] if state == "diffuse_low" else \
			textures["diffuse_high"]
	var detail = textures["detail_low"] if state == "detail_low" else \
			textures["detail_high"]
	var alpha_mod := PROJSHADOW_ALPHA_GEN_LOW if state == "alpha_gen_low" else 1.0
	# The lighting block is pass-global: publish this probe's flat register
	# with every state so the beauty frame starts from its own values.
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
	for entry in casters:
		for material in entry["materials"]:
			material.set_shader_parameter("u_diffuse", diffuse)
			material.set_shader_parameter("u_detail", detail)
			material.set_shader_parameter("u_uv_transform_u", Vector3(1.0, 0.0, 0.0))
			material.set_shader_parameter("u_uv_transform_v", Vector3(0.0, 1.0, 0.0))
			material.set_shader_parameter("u_rgb_mod", Vector3.ONE)
			material.set_shader_parameter("u_alpha_mod", alpha_mod)
