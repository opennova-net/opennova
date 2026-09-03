extends GameProbe

## gore_set_visual: visual evidence for D-PTL-24, the blood puff actually
## RENDERS. The catalog ctest proves Effect_AmHitBody resolves to its five
## authored emitters instead of the one-emitter `stockeffect` clone; this one
## proves pixels: it loads the mounted catalog into an EffectWorld on a probe
## stage, captures a clean baseline, then submits Effect_AmHitBody at three
## anchors the way `_route_round_impacts` does for a flesh hit and captures
## again. A non-trivial changed-pixel count is the proof. Needs a window.

const STAGE_SIZE := Vector2i(960, 540)
const TICK_DT := 1.0 / 62.5
const CAPTURE_TICKS := 5
const EFFECT_NAME := "Effect_AmHitBody"
# Three torso-height puffs at tick 5 move a few hundred pixels of a 960x540
# stage; the invisible stockeffect clone moves none.
const MIN_CHANGED_PIXELS := 100
# Roughly torso height, spread across the frame: the anchors a burst into a
# standing target would produce.
const HIT_POINTS := [
	Vector3(-2.6, 1.4, 0.0),
	Vector3(0.0, 1.6, 0.0),
	Vector3(2.6, 1.3, 0.0),
]

var _ctx: ProbeContext
var _stage: ProbeStage
var _scene: Node3D
var _fx: EffectWorld
var _caption: Label
var _before: Image


func run(ctx: ProbeContext) -> ProbeVerdict:
	_ctx = ctx
	var out_dir := ProbeOutput.resolve(ctx, String(ctx.args.get("output_dir", "")))
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
	ctx.log("[gore-visual] catalog: %d effects across %d files (gore set: %s)"
			% [effect_count, _fx.file_count(), root.particle_extension()])
	if effect_count <= 0:
		return ProbeVerdict.failed("the mounted root carries no particle effects")

	# The authored definition the catalog resolved for the effect: its pdef
	# list is what the live emitter census below is measured against (the US
	# gore set authors four: BloodAltG + AmHitBody_Mist2G/3G/4G).
	var authored_pdefs := PackedStringArray()
	for file_value in _fx.get_files():
		var file := file_value as ParticleFile
		var effect := file.find_effect(EFFECT_NAME)
		if effect != null and authored_pdefs.is_empty():
			authored_pdefs = PackedStringArray(effect.get_pdefs())
			ctx.log("[gore-visual] definition source=%s pdefs=%s" % [
					file.get_source_path(), str(authored_pdefs)])
	if authored_pdefs.is_empty():
		return ProbeVerdict.failed("%s is not authored in the mounted catalog (the gore set did not load)" % EFFECT_NAME)
	var unresolved := _fx.get_unresolved_texture_names()
	if not unresolved.is_empty():
		ctx.log("[gore-visual] unresolved textures: %s" % str(unresolved))

	await ctx.wait_frames(8)
	_before = await _stage.capture_after_render(_ctx.tree, _fx)
	if _before == null:
		return ProbeVerdict.failed("the baseline capture produced no image")

	_caption.text = "AFTER  •  3 x %s at flesh-hit anchors" % EFFECT_NAME
	var handle := _fx.intern_effect(EFFECT_NAME)
	var spawned := 0
	for point_v in HIT_POINTS:
		if _fx.spawn_effect_by_handle(handle, point_v as Vector3, Vector3.FORWARD):
			spawned += 1
	for tick in range(CAPTURE_TICKS):
		_fx.advance_fixed_tick(TICK_DT)
		# Per-tick emitter census: which authored emitters are live when.
		var census := PackedStringArray()
		for group_v in _fx.get_debug_group_report():
			var group := group_v as EffectGroupReport
			var names := PackedStringArray()
			for emitter_v in group.emitters:
				var emitter := emitter_v as EffectEmitterReport
				names.append("%s:%d" % [emitter.name, emitter.alive])
			census.append("[%s]" % ",".join(names))
		ctx.log("[gore-visual] tick %d emitters %s" % [tick + 1, " ".join(census)])
	var after := await _stage.capture_after_render(_ctx.tree, _fx)
	if after == null:
		return ProbeVerdict.failed("the after capture produced no image")

	var emitters := 0
	for group_v in _fx.get_debug_group_report():
		emitters += (group_v as EffectGroupReport).emitters.size()
	var changed := ProbeCapture.changed_pixels(_before, after, 90)
	ctx.log("[gore-visual] handle %d, spawned %d/%d, %d live emitter(s), %d changed pixel(s)"
			% [handle, spawned, HIT_POINTS.size(), emitters, changed])

	var before_path := out_dir.path_join("gore_before.png")
	var after_path := out_dir.path_join("gore_after.png")
	_before.save_png(before_path)
	after.save_png(after_path)
	ctx.artifact("gore_before", before_path, "png")
	ctx.artifact("gore_after", after_path, "png")
	ctx.log("[gore-visual] wrote gore_before.png / gore_after.png to %s" % out_dir)

	# The stockeffect clone would be one invisible emitter per hit; the authored
	# blood is every pdef of the definition, live, in dark red (color1..4 =
	# 139,0,0). Every authored emitter per hit and real moved pixels are both
	# required (the clone moves none).
	var expected_emitters := authored_pdefs.size() * HIT_POINTS.size()
	var data := {
		"handle": handle,
		"spawned": spawned,
		"authored_pdefs": Array(authored_pdefs),
		"emitters": emitters,
		"expected_emitters": expected_emitters,
		"changed_pixels": changed,
		"gore_set": root.particle_extension(),
	}
	var ok := spawned == HIT_POINTS.size() and emitters == expected_emitters and changed > MIN_CHANGED_PIXELS
	ctx.log("[gore-visual] VERDICT: %s" % ("PASS, the blood puff renders" if ok else "FAIL"))
	if ok:
		return ProbeVerdict.passed("the blood puff renders (%d/%d emitters, %d changed pixels)" % [
				emitters, expected_emitters, changed], data)
	return ProbeVerdict.failed("spawned %d/%d, %d/%d emitters, %d changed pixels (need > %d)" % [
			spawned, HIT_POINTS.size(), emitters, expected_emitters, changed, MIN_CHANGED_PIXELS], data)


func _build_stage() -> void:
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 1.8, 7.5)
	camera.fov = 55.0
	camera.current = true
	_scene.add_child(camera)
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
	_scene.add_child(world_environment)

	var key := DirectionalLight3D.new()
	key.light_energy = 1.2
	key.rotation_degrees = Vector3(-50.0, -30.0, 0.0)
	_scene.add_child(key)

	var layer := CanvasLayer.new()
	_scene.add_child(layer)
	_caption = Label.new()
	_caption.text = "BEFORE  •  retail catalog mounted, no effect submitted"
	_caption.position = Vector2(24, 24)
	_caption.add_theme_font_size_override("font_size", 22)
	_caption.add_theme_color_override("font_color", Color.BLACK)
	layer.add_child(_caption)


