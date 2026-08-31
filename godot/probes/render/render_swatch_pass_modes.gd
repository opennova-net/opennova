class_name RenderSwatchPassModes
extends RefCounted

## The render_swatch auxiliary-pass raster proofs (modes clip, matchterrain):
## the reflection CLIP gate and the skinned MATCHTERRAIN pass, each technique
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
	RenderSwatchSupport.add_display_decode(scene)

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
		var disarmed_delta := RenderSwatchSupport.lighting_mean_delta(
				captures["inactive"], captures["disarmed"], bottom_rect)
		var wrong_eye_delta := RenderSwatchSupport.lighting_mean_delta(
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
		RenderSwatchSupport.channel_expect_delta(failures, entry["name"], "reflection CLIP",
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
	_sink.artifact("%s_manifest" % prefix, out_dir.path_join("%s_manifest.json" % prefix))
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
	RenderingServer.global_shader_parameter_set(
			"opennova_water_reflection_clip_active", false)


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
	RenderSwatchSupport.add_display_decode(scene)

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
	var mf := FileAccess.open(out_dir.path_join("%s_manifest.json" % prefix), FileAccess.WRITE)
	if mf != null:
		mf.store_string(JSON.stringify(manifest, "\t"))
		mf.close()
	_sink.artifact("%s_manifest" % prefix, out_dir.path_join("%s_manifest.json" % prefix))
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

