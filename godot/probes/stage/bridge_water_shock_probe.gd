extends GameProbe

## bridge_water_shock: deterministic visual evidence for D-ITEM-19. Loads the
## mounted particle catalog into an EffectWorld on a probe stage, captures the
## bridge/water stage before destruction, then submits the exact unowned
## Effect_ShockWaterBrdg family used by the destruction presentation drain at
## three representative DEAD anchors and keeps the best of four tick captures.
## Needs a window.

const STAGE_SIZE := Vector2i(960, 540)
const TICK_DT := 1.0 / 62.5
const CAPTURE_TICKS := [6, 14, 26, 42]
const SHOCK_POINTS := [
	Vector3(-4.6, 0.04, -0.4),
	Vector3(0.0, 0.04, -0.4),
	Vector3(4.6, 0.04, -0.4),
]

var _ctx: ProbeContext
var _stage: ProbeStage
var _scene: Node3D
var _fx: EffectWorld
var _caption: Label
var _detail: Label
var _before: Image
var _effect_authored := false


func run(ctx: ProbeContext) -> ProbeVerdict:
	_ctx = ctx
	var output_dir := ProbeOutput.resolve(ctx, String(ctx.args.get("output_dir", "")))
	var root := ctx.resource_root()
	if root == null:
		return ProbeVerdict.failed("the shell has no mounted resource root")
	_stage = ProbeStage.create(ctx, STAGE_SIZE, ctx.viewport())
	_scene = Node3D.new()
	_stage.add_scene(_scene)
	_build_stage()

	_fx = EffectWorld.new()
	_fx.name = "RetailEffectWorld"
	_scene.add_child(_fx)
	var effect_count := _fx.load_from_resource_root(root)
	if effect_count <= 0:
		return ProbeVerdict.failed("the mounted root carries no particle effects")
	_describe_effect_definition()

	await _settle_frames(8)
	_before = await _capture_image()
	if _before == null:
		return ProbeVerdict.failed("could not capture the baseline frame")

	_caption.text = "AFTER  •  3 × Effect_ShockWaterBrdg at transformed DEAD anchors"
	_detail.text = ("D-ITEM-19 event-position overlay  •  authored retail particles"
			if _effect_authored else
			"D-ITEM-19 event-position overlay  •  retail stockeffect is intentionally invisible")
	for point in SHOCK_POINTS:
		var handle := _fx.spawn_effect("Effect_ShockWaterBrdg", point)
		if handle <= 0:
			return ProbeVerdict.failed("Effect_ShockWaterBrdg did not spawn")
		_add_event_marker(point, SHOCK_POINTS.find(point) + 1)

	var best_image: Image
	var best_score := -1
	var best_tick := 0
	var elapsed_ticks := 0
	var ticks: Array = []
	for capture_tick in CAPTURE_TICKS:
		while elapsed_ticks < capture_tick:
			_fx.advance_fixed_tick(TICK_DT)
			_fx.render_frame()
			elapsed_ticks += 1
			await ctx.tree.process_frame
		var candidate := await _capture_image()
		var score := _changed_pixels(_before, candidate)
		var draw_report := _fx.get_debug_draw_list_report()
		var far_draw: Dictionary = draw_report.get("world_far_side", {})
		var camera_draw: Dictionary = draw_report.get("world_camera_side", {})
		var row := {
			"tick": capture_tick,
			"changed_pixels": score,
			"groups": _fx.get_debug_group_report().size(),
			"particles": int(far_draw.get("input_particles", 0)),
			"draws": int(far_draw.get("draw_command_count", 0))
					+ int(camera_draw.get("draw_command_count", 0)),
		}
		ticks.append(row)
		ctx.log("[bridge-shock] tick=%d changed_pixels=%d groups=%d particles=%d draws=%d" % [
				capture_tick, score, int(row["groups"]), int(row["particles"]), int(row["draws"])])
		if score > best_score:
			best_image = candidate
			best_score = score
			best_tick = capture_tick

	var before_path := output_dir.path_join("before.png")
	var after_path := output_dir.path_join("after.png")
	var before_error := _before.save_png(before_path)
	var after_error := best_image.save_png(after_path) \
			if best_image != null else ERR_CANT_CREATE
	if before_error != OK or after_error != OK:
		return ProbeVerdict.failed("could not write comparison PNGs")
	ctx.artifact("before", before_path, "png")
	ctx.artifact("after", after_path, "png")
	var data := {
		"files": _fx.file_count(),
		"effects": effect_count,
		"effect_authored": _effect_authored,
		"best_tick": best_tick,
		"changed_pixels": best_score,
		"ticks": ticks,
	}
	ctx.log("[bridge-shock] PASS files=%d effects=%d best_tick=%d changed_pixels=%d" % [
			_fx.file_count(), effect_count, best_tick, best_score])
	return ProbeVerdict.passed("best tick %d changed %d pixels" % [best_tick, best_score], data)


func _describe_effect_definition() -> void:
	var related := PackedStringArray()
	for file_value in _fx.get_files():
		var file := file_value as ParticleFile
		var effect := file.find_effect("Effect_ShockWaterBrdg")
		if effect != null:
			_effect_authored = true
			_ctx.log("[bridge-shock] definition source=%s pdefs=%s" % [
					file.get_source_path(), str(effect.get_pdefs())])
			return
		for effect_value in file.get_effects():
			var candidate := effect_value as ParticleEffect
			var folded := candidate.get_id().to_lower()
			if "shock" in folded or "water" in folded or "brdg" in folded \
					or "bridge" in folded:
				related.append(candidate.get_id())
	_ctx.log("[bridge-shock] definition not authored; stockeffect fallback would be used; related=%s" % [
			str(related)])


func _build_stage() -> void:
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 8.2, 14.5)
	camera.fov = 52.0
	camera.current = true
	_scene.add_child(camera)
	camera.look_at(Vector3(0.0, 1.0, -1.8), Vector3.UP)

	var world_environment := WorldEnvironment.new()
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color("101925")
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color("9eb6cc")
	environment.ambient_light_energy = 0.65
	environment.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	world_environment.environment = environment
	_scene.add_child(world_environment)

	var key := DirectionalLight3D.new()
	key.light_color = Color("dcecff")
	key.light_energy = 1.35
	key.shadow_enabled = true
	key.rotation_degrees = Vector3(-52.0, -28.0, 0.0)
	_scene.add_child(key)

	var water := MeshInstance3D.new()
	var water_mesh := PlaneMesh.new()
	water_mesh.size = Vector2(34.0, 22.0)
	water.mesh = water_mesh
	water.material_override = _material(Color("1d6685"), 0.23, 0.45)
	_scene.add_child(water)

	_add_box("BridgeDeck", Vector3(15.0, 0.65, 3.8),
			Vector3(0.0, 2.25, -2.0), Color("4c5966"))
	_add_box("LeftPier", Vector3(1.0, 4.4, 2.7),
			Vector3(-5.7, 1.8, -2.0), Color("35414b"))
	_add_box("RightPier", Vector3(1.0, 4.4, 2.7),
			Vector3(5.7, 1.8, -2.0), Color("35414b"))
	for z_value in [-3.55, -0.45]:
		_add_box("Rail", Vector3(15.0, 0.16, 0.16),
				Vector3(0.0, 3.35, z_value), Color("9aa7b2"))
		for x_value in [-6.7, -4.5, -2.25, 0.0, 2.25, 4.5, 6.7]:
			_add_box("RailPost", Vector3(0.14, 1.55, 0.14),
					Vector3(x_value, 2.85, z_value), Color("8996a1"))

	var panel := ColorRect.new()
	panel.position = Vector2(24.0, 22.0)
	panel.size = Vector2(665.0, 70.0)
	panel.color = Color(0.025, 0.045, 0.07, 0.88)
	_scene.add_child(panel)
	_caption = Label.new()
	_caption.position = Vector2(24.0, 12.0)
	_caption.text = "BEFORE  •  bridge water plane, no destruction effects"
	_caption.add_theme_font_size_override("font_size", 20)
	_caption.add_theme_color_override("font_color", Color("edf7ff"))
	panel.add_child(_caption)
	_detail = Label.new()
	_detail.position = Vector2(24.0, 40.0)
	_detail.text = "D-ITEM-19 deterministic runtime probe  •  retail PTL assets"
	_detail.add_theme_font_size_override("font_size", 14)
	_detail.add_theme_color_override("font_color", Color("94b9d2"))
	panel.add_child(_detail)


func _add_event_marker(pos: Vector3, index: int) -> void:
	var ring := MeshInstance3D.new()
	ring.name = "ShockEventMarker%d" % index
	var torus := TorusMesh.new()
	torus.inner_radius = 0.58
	torus.outer_radius = 0.82
	torus.rings = 32
	torus.ring_segments = 12
	ring.mesh = torus
	ring.position = pos + Vector3(0.0, 0.1, 0.0)
	var marker_material := StandardMaterial3D.new()
	marker_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	marker_material.albedo_color = Color("64e8ff")
	marker_material.emission_enabled = true
	marker_material.emission = Color("36cdea")
	marker_material.emission_energy_multiplier = 3.0
	ring.material_override = marker_material
	_scene.add_child(ring)

	var label := Label3D.new()
	label.text = "DEAD %d\nevent @ water Z" % index
	label.position = pos + Vector3(0.0, 0.9, 0.0)
	label.font_size = 36
	label.pixel_size = 0.011
	label.modulate = Color("d9f9ff")
	label.outline_size = 8
	label.no_depth_test = true
	_scene.add_child(label)


func _add_box(node_name: String, size: Vector3, pos: Vector3, color: Color) -> void:
	var instance := MeshInstance3D.new()
	instance.name = node_name
	var mesh := BoxMesh.new()
	mesh.size = size
	instance.mesh = mesh
	instance.position = pos
	instance.material_override = _material(color, 0.78, 0.08)
	_scene.add_child(instance)


func _material(color: Color, roughness: float, metallic: float) -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.albedo_color = color
	material.roughness = roughness
	material.metallic = metallic
	return material


func _settle_frames(count: int) -> void:
	for _index in range(count):
		if _fx != null:
			_fx.render_frame()
		await _ctx.tree.process_frame


func _capture_image() -> Image:
	_fx.render_frame()
	await _ctx.tree.process_frame
	return await _stage.capture_image(_ctx.tree)


func _changed_pixels(reference: Image, candidate: Image) -> int:
	if reference == null or candidate == null \
			or reference.get_size() != candidate.get_size():
		return -1
	var changed := 0
	# Exclude the caption band so this score selects particle visibility rather
	# than the intentional BEFORE/AFTER label change.
	for y_value in range(100, reference.get_height()):
		for x_value in range(reference.get_width()):
			var before := reference.get_pixel(x_value, y_value)
			var after := candidate.get_pixel(x_value, y_value)
			if absf(before.r - after.r) + absf(before.g - after.g) \
					+ absf(before.b - after.b) > 0.12:
				changed += 1
	return changed
