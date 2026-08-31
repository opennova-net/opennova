class_name RenderSwatchLightingModes
extends RefCounted

## The render_swatch lighting-response and material-channel raster proofs
## (modes lighting and channels): one sphere or quad per technique on the
## probe stage, rendered through the audited input states.

var _ctx: ProbeContext
var _stage: ProbeStage
var _sink: RenderSwatchSupport.Sink


func _init(ctx: ProbeContext, stage: ProbeStage, sink: RenderSwatchSupport.Sink) -> void:
	_ctx = ctx
	_stage = stage
	_sink = sink


func _capture_lighting_image() -> Image:
	return await _stage.capture_image(_ctx.tree, RenderSwatchSupport.SETTLE_FRAMES)


# Raster proof for the object-lighting contract under ADR 0043: one sphere per
# static technique rendered through eight scene-lighting states — the real
# DirectionalLight3D sun reversed, hemisphere sky ambient reversed through the
# gradient Sky, flat ambient on/off, and a real OmniLight3D per sphere on/off
# (the production delivery for every route). The per-technique validation
# ledger says which techniques are lit and which stay unshaded islands; the
# probe rejects both missing and invented responses.
func lighting_mode(out_dir: String, prefix: String) -> void:
	DirAccess.make_dir_recursive_absolute(out_dir)
	_stage.set_stage_size(Vector2i(1280, 720))
	# Flag.fx is authored against FloatTicks. Freezing scaled time makes its
	# reconstructed normal identical across the response-state captures.
	_ctx.set_time_scale(0.0)

	var parsed = JSON.parse_string(FileAccess.get_file_as_string(
			"res://shaders/object/pipeline_manifest.json"))
	if not parsed is Dictionary:
		_sink.error("render_swatch_probe lighting: object pipeline manifest did not parse")
		_sink.quit(1)
		return
	var audited = JSON.parse_string(FileAccess.get_file_as_string(
			"res://shaders/object/technique_validation.json"))
	if not audited is Dictionary:
		_sink.error("render_swatch_probe lighting: technique validation did not parse")
		_sink.quit(1)
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
	_stage.add_scene(scene)
	var world_env := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.015, 0.015, 0.02)
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.04, 0.04, 0.04)
	env.ambient_light_energy = 2.0
	world_env.environment = env
	scene.add_child(world_env)

	# The scene's light delivery IS production delivery (ADR 0043): one
	# casting-off sun for the directional states, the gradient hemisphere Sky
	# for the sky-ambient states, and one OmniLight3D per sphere for the
	# gameplay point states — all at the MODULATE2X energy 2.0 convention.
	var sun := DirectionalLight3D.new()
	sun.light_energy = 2.0
	sun.shadow_enabled = false
	sun.visible = false
	scene.add_child(sun)
	var hemi_material := ShaderMaterial.new()
	hemi_material.shader = load("res://shaders/hemisphere_sky.gdshader") as Shader
	var hemi_sky_resource := Sky.new()
	hemi_sky_resource.sky_material = hemi_material
	hemi_sky_resource.radiance_size = Sky.RADIANCE_SIZE_64
	env.sky = hemi_sky_resource

	var diffuse := RenderSwatchSupport.make_lighting_diffuse_texture()
	var detail := RenderSwatchSupport.make_lighting_detail_texture()
	var tangent_normal := RenderSwatchSupport.make_lighting_normal_texture()
	var object_normal := RenderSwatchSupport.make_lighting_object_normal_texture()
	var entries: Array[Dictionary] = []
	var techniques: Array = parsed.get("techniques", [])
	const COLS := 6
	const SPACING := 2.2
	# The modulator gain and fog enable are the two surviving pass globals the
	# object family reads (ADR 0043): publish this probe's own values before
	# the first capture so no other writer's leak into it.
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
			_sink.error("render_swatch_probe lighting: could not load %s" % path)
			_sink.quit(1)
			return
		var material := ShaderMaterial.new()
		material.shader = shader
		if not RenderSwatchSupport.bind_production_object_resources(material,
				str(technique["implementation"])):
			_sink.error("render_swatch_probe lighting: production resources unavailable for %s" % path)
			_sink.quit(1)
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

		var sphere := MeshInstance3D.new()
		var mesh := SphereMesh.new()
		mesh.radius = 0.72
		mesh.height = 1.44
		sphere.mesh = mesh
		sphere.material_override = material
		sphere.position = Vector3((i % COLS) * SPACING,
				-(i / COLS) * SPACING, 0.0)
		scene.add_child(sphere)

		# One tight-range production omni per sphere for the point states —
		# the same OmniLight3D delivery every gameplay point light uses.
		var point_light := OmniLight3D.new()
		point_light.position = sphere.position + Vector3(0.0, 0.0, 1.2)
		point_light.omni_range = 2.0
		point_light.light_color = Color(0.8, 0.55, 0.3)
		point_light.light_energy = 2.0
		point_light.shadow_enabled = false
		point_light.visible = false
		scene.add_child(point_light)
		var implementation := str(technique["implementation"])
		var engine_enum := str(technique["engine_enum"])
		var responses = response_by_enum.get(engine_enum, {})
		if not responses is Dictionary or responses.size() != 4:
			_sink.error("render_swatch_probe lighting: missing response audit for %s" % engine_enum)
			_sink.quit(1)
			return
		entries.append({
			"name": engine_enum,
			"implementation": implementation,
			"path": path,
			"responses": responses,
			"raster_checks": raster_checks_by_enum.get(engine_enum, {}),
			"material": material,
			"mesh": sphere,
			"point_light": point_light,
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
	for state in ["direction_a", "direction_b", "hemi_sky", "hemi_ground",
			"ambient_off", "ambient_on", "point_off", "point_on"]:
		_apply_lighting_probe_state(entries, state, env, sun, hemi_material)
		var frame: Image = await _capture_lighting_image()
		if frame == null:
			_sink.error("render_swatch_probe lighting: no viewport image for %s" % state)
			_sink.quit(1)
			return
		frame.convert(Image.FORMAT_RGBA8)
		captures[state] = frame
		var save_error := frame.save_png(out_dir.path_join("%s_%s.png" % [prefix, state]))
		if save_error != OK:
			_sink.error("render_swatch_probe lighting: could not save %s" % state)
			_sink.quit(1)
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
		var dir_delta := RenderSwatchSupport.lighting_mean_delta(captures["direction_a"],
				captures["direction_b"], rect)
		var hemi_delta := RenderSwatchSupport.lighting_mean_delta(captures["hemi_sky"],
				captures["hemi_ground"], rect)
		var ambient_delta := RenderSwatchSupport.lighting_mean_delta(captures["ambient_off"],
				captures["ambient_on"], rect)
		var point_delta := RenderSwatchSupport.lighting_mean_delta(captures["point_off"],
				captures["point_on"], rect)
		var dir_a_contrast := RenderSwatchSupport.lighting_axis_contrast(captures["direction_a"], rect, true)
		var dir_b_contrast := RenderSwatchSupport.lighting_axis_contrast(captures["direction_b"], rect, true)
		var sky_contrast := RenderSwatchSupport.lighting_axis_contrast(captures["hemi_sky"], rect, false)
		var ground_contrast := RenderSwatchSupport.lighting_axis_contrast(captures["hemi_ground"], rect, false)
		var report := {
			"technique": entry["name"],
			"implementation": entry["implementation"],
			"path": entry["path"],
			"expected_responses": entry["responses"],
			"direction_delta": dir_delta,
			"hemisphere_delta": hemi_delta,
			"ambient_delta": ambient_delta,
			"point_delta": point_delta,
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
		},
		"techniques": reports,
		"failures": failures,
	}
	var mf := FileAccess.open(out_dir.path_join("%s_manifest.json" % prefix), FileAccess.WRITE)
	if mf != null:
		mf.store_string(JSON.stringify(manifest, "\t"))
		mf.close()
	_sink.artifact("%s_manifest" % prefix, out_dir.path_join("%s_manifest.json" % prefix))
	if failures.is_empty():
		_sink.logv(["render_swatch_probe lighting: PASS - ", entries.size(),
				" techniques respond according to their audited light contract"])
		_sink.quit(0)
	else:
		for failure in failures:
			_sink.error("render_swatch_probe lighting: " + failure)
		_sink.quit(1)


# Raster proof for material-channel ownership. This is deliberately separate
# from the lighting response matrix: the same compiled shaders must prove that
# RgbGen affects only SELFLUM, AlphaGen affects only _FFP, raw Diffuse1.a drives
# the authored Phong lobes, and each technique cuts out from its named coverage
# source (diffuse/normal/vertex/reflect/zero).
func channel_mode(out_dir: String, prefix: String) -> void:
	DirAccess.make_dir_recursive_absolute(out_dir)
	_stage.set_stage_size(Vector2i(1280, 720))
	_ctx.set_time_scale(0.0)

	var parsed = JSON.parse_string(FileAccess.get_file_as_string(
			"res://shaders/object/pipeline_manifest.json"))
	if not parsed is Dictionary:
		_sink.error("render_swatch_probe channels: object pipeline manifest did not parse")
		_sink.quit(1)
		return
	var specular_alpha_contracts = parsed.get("specular_alpha_contracts", {})
	if not specular_alpha_contracts is Dictionary:
		_sink.error("render_swatch_probe channels: specular-alpha contract did not parse")
		_sink.quit(1)
		return

	var scene := Node3D.new()
	_stage.add_scene(scene)
	var world_env := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.012, 0.012, 0.016)
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.18, 0.18, 0.18)
	env.ambient_light_energy = 2.0
	world_env.environment = env
	scene.add_child(world_env)
	# ADR 0043: the specular states light the authored Phong lobes through the
	# production delivery — one real camera-axis sun.
	var channel_sun := DirectionalLight3D.new()
	channel_sun.basis = Basis.looking_at(Vector3(0.0, 0.0, -1.0), Vector3.UP)
	channel_sun.light_color = Color(0.55, 0.55, 0.55)
	channel_sun.light_energy = 2.0
	channel_sun.shadow_enabled = false
	channel_sun.visible = false
	scene.add_child(channel_sun)

	var textures := {
		"diffuse_low": RenderSwatchSupport.make_channel_diffuse_texture(64),
		"diffuse_high": RenderSwatchSupport.make_channel_diffuse_texture(192),
		"specular_low": RenderSwatchSupport.make_channel_specular_texture(64),
		"specular_high": RenderSwatchSupport.make_channel_specular_texture(192),
		"detail": RenderSwatchSupport.make_lighting_detail_texture(),
		"normal_low": RenderSwatchSupport.make_channel_normal_texture(64),
		"normal_high": RenderSwatchSupport.make_channel_normal_texture(192),
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
			_sink.error("render_swatch_probe channels: no cutout policy for %s" %
					technique["engine_enum"])
			_sink.quit(1)
			return

		var color_path := "res://shaders/object/%s/%s.gdshader" % [
				technique["directory"], color_policy]
		var coverage_path := "res://shaders/object/%s/%s.gdshader" % [
				technique["directory"], coverage_policy]
		var color_shader := load(color_path) as Shader
		var coverage_shader := load(coverage_path) as Shader
		if color_shader == null or coverage_shader == null:
			_sink.error("render_swatch_probe channels: could not load %s / %s" % [
					color_path, coverage_path])
			_sink.quit(1)
			return

		var color_material := ShaderMaterial.new()
		color_material.shader = color_shader
		var coverage_material := ShaderMaterial.new()
		coverage_material.shader = coverage_shader
		if not RenderSwatchSupport.bind_production_object_resources(color_material,
				str(technique["implementation"])) or not \
				RenderSwatchSupport.bind_production_object_resources(coverage_material,
				str(technique["implementation"])):
			_sink.error("render_swatch_probe channels: production resources unavailable for %s" % color_path)
			_sink.quit(1)
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
		_apply_channel_probe_state(entries, state, textures, channel_sun)
		var frame: Image = await _capture_lighting_image()
		if frame == null:
			_sink.error("render_swatch_probe channels: no viewport image for %s" % state)
			_sink.quit(1)
			return
		frame.convert(Image.FORMAT_RGBA8)
		captures[state] = frame
		var save_error := frame.save_png(out_dir.path_join("%s_%s.png" % [prefix, state]))
		if save_error != OK:
			_sink.error("render_swatch_probe channels: could not save %s" % state)
			_sink.quit(1)
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
		var rgb_delta := RenderSwatchSupport.lighting_mean_delta(
				captures["rgb_low"], captures["rgb_high"], rect)
		var specular_alpha_delta := RenderSwatchSupport.lighting_mean_delta(
				captures["specular_alpha_low"], captures["specular_alpha_high"], rect)
		var coverage_delta := RenderSwatchSupport.lighting_mean_delta(
				captures["coverage_low"], captures["coverage_high"], rect)
		var alpha_gen_delta := RenderSwatchSupport.lighting_mean_delta(
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
		RenderSwatchSupport.channel_expect_delta(failures, entry["name"], "RgbGen", rgb_delta,
				entry["rgb_modulation"] == "self_lum", 0.006, 0.001)
		RenderSwatchSupport.channel_expect_delta(failures, entry["name"], "specular alpha",
				specular_alpha_delta, entry["specular_alpha"] != "none", 0.003, 0.001)
		RenderSwatchSupport.channel_expect_delta(failures, entry["name"], "coverage source",
				coverage_delta, entry["coverage_source"] != "zero", 0.006, 0.001)
		RenderSwatchSupport.channel_expect_delta(failures, entry["name"], "AlphaGen",
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
	_sink.artifact("%s_manifest" % prefix, out_dir.path_join("%s_manifest.json" % prefix))
	if failures.is_empty():
		_sink.logv(["render_swatch_probe channels: PASS - ", entries.size(),
				" techniques honor RGB, alpha, specular, and coverage contracts"])
		_sink.quit(0)
	else:
		for failure in failures:
			_sink.error("render_swatch_probe channels: " + failure)
		_sink.quit(1)


func _apply_lighting_probe_state(entries: Array[Dictionary], state: String,
		env: Environment, sun: DirectionalLight3D,
		hemi_material: ShaderMaterial) -> void:
	var ambient := 0.04
	var ambient_from_sky := false
	var sun_on := false
	var point_on := false
	var sun_direction := Vector3(0.8, 0.0, -0.6)
	var hemi_sky := Vector3(0.025, 0.025, 0.025)
	var hemi_ground := hemi_sky
	if state == "direction_a":
		sun_on = true
	elif state == "direction_b":
		sun_on = true
		sun_direction = Vector3(-0.8, 0.0, -0.6)
	elif state == "hemi_sky":
		ambient_from_sky = true
		hemi_sky = Vector3(0.34, 0.34, 0.34)
	elif state == "hemi_ground":
		ambient_from_sky = true
		hemi_ground = Vector3(0.34, 0.34, 0.34)
	elif state == "ambient_on":
		ambient = 0.34
	elif state == "ambient_off":
		ambient = 0.025
	elif state == "point_off" or state == "point_on":
		ambient = 0.025
		point_on = state == "point_on"

	sun.visible = sun_on
	if sun_on:
		sun.basis = Basis.looking_at(sun_direction.normalized(), Vector3.UP)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY \
			if ambient_from_sky else Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(ambient, ambient, ambient)
	hemi_material.set_shader_parameter("u_hemi_sky", hemi_sky)
	hemi_material.set_shader_parameter("u_hemi_ground", hemi_ground)
	for entry in entries:
		(entry["point_light"] as OmniLight3D).visible = point_on


func _apply_channel_probe_state(entries: Array[Dictionary], state: String,
		textures: Dictionary, sun: DirectionalLight3D) -> void:
	var color_phase := state.begins_with("rgb_") or \
			state.begins_with("specular_alpha_")
	var high := state.ends_with("_high")
	# The scene sun lights only the specular states: the alpha-controlled
	# highlight must be independently observable over the flat ambient every
	# other channel state keeps.
	sun.visible = state.begins_with("specular_alpha_")
	RenderingServer.global_shader_parameter_set("opennova_light_block_gain", Vector3.ONE)
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
