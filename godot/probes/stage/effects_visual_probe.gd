extends GameProbe

## effects_visual: the environment-particle visual probe. Loads the mounted
## .ptl set (the launch's resource root) into a bare EffectWorld on a probe
## stage, spawns a spread of representative effects (explosion, smoke column,
## brazier fire, muzzle flash, dirt impact, casing) into an empty dark scene,
## and captures frames over their lifetimes: the RUNTIME effect path (textures
## via the provider, blend draw, cadence, motion) without mission hunting.
## Needs a window.

const STAGE_SIZE := Vector2i(960, 540)
const SPAWNS := [
	{"name": "Effect_AirExp", "pos": Vector3(-12, 2, -20)},
	{"name": "Effect_BlackSmoke", "pos": Vector3(-4, 0, -20)},
	{"name": "Effect_Brazier", "pos": Vector3(2, 0, -18)},
	{"name": "Effect_M82_Muz", "pos": Vector3(6, 2, -14)},
	{"name": "Effect_AmHitDirt", "pos": Vector3(10, 0, -18)},
	{"name": "Effect_50BMGCas", "pos": Vector3(8, 1.5, -12)},
]
const CAPTURES := [["fx_020.png", 0.2], ["fx_100.png", 0.8], ["fx_250.png", 1.5]]

var _ctx: ProbeContext
var _stage: ProbeStage
var _fx: EffectWorld
var _out_abs := ""


func run(ctx: ProbeContext) -> ProbeVerdict:
	_ctx = ctx
	_out_abs = ProbeOutput.resolve(ctx, String(ctx.args.get("output_dir", "")))
	var root := ctx.resource_root()
	if root == null:
		return ProbeVerdict.failed("the shell has no mounted resource root")
	_stage = ProbeStage.create(ctx, STAGE_SIZE, ctx.viewport())
	var scene := Node3D.new()
	_stage.add_scene(scene)

	var cam := Camera3D.new()
	cam.position = Vector3(0, 3, 0)
	cam.current = true
	scene.add_child(cam)
	var light := DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-45, 30, 0)
	scene.add_child(light)
	var env := WorldEnvironment.new()
	var e := Environment.new()
	e.background_mode = Environment.BG_COLOR
	e.background_color = Color(0.12, 0.13, 0.16)
	env.environment = e
	scene.add_child(env)

	_fx = EffectWorld.new()
	scene.add_child(_fx)
	var n := _fx.load_from_resource_root(root)
	ctx.log("[fx] loaded: files=%d effects=%d" % [_fx.file_count(), n])
	if n == 0:
		return ProbeVerdict.failed("no effects loaded from the mounted root")

	var handles := {}
	for s in SPAWNS:
		var handle: int = _fx.spawn_effect(String(s["name"]), s["pos"])
		handles[String(s["name"])] = handle
		ctx.log("[fx] spawn %s -> handle %d" % [s["name"], handle])

	var captures: Array = []
	for capture in CAPTURES:
		var row := await _capture(String(capture[0]), float(capture[1]))
		if row.is_empty():
			return ProbeVerdict.failed("capture %s produced no image" % String(capture[0]),
					{"loaded": n, "handles": handles, "captures": captures})
		captures.append(row)
	ctx.log("[fx] done -> " + _out_abs)
	return ProbeVerdict.passed("%d effects loaded, %d captures" % [n, captures.size()],
			{"loaded": n, "files": _fx.file_count(), "handles": handles, "captures": captures})


## Render the effect world every frame for `wait_s`, then read the stage.
func _capture(file_name: String, wait_s: float) -> Dictionary:
	var t := 0.0
	while t < wait_s and not _ctx.cancelled:
		_fx.render_frame(GameWorld.current_frame_clock_ms())
		t += _ctx.tree.root.get_process_delta_time()
		await _ctx.tree.process_frame
	_fx.render_frame(GameWorld.current_frame_clock_ms())
	var img := await _stage.capture_image(_ctx.tree)
	if img == null:
		return {}
	var path := _out_abs.path_join(file_name)
	if img.save_png(path) != OK:
		return {}
	_ctx.artifact(file_name.get_basename(), path, "png")
	var particles := 0
	var emitters := 0
	for group_v in _fx.get_debug_group_report():
		for emitter_v in (group_v as EffectGroupReport).emitters:
			emitters += 1
			particles += (emitter_v as EffectEmitterReport).alive
	var row := {
		"file": file_name,
		"live_groups": _fx.live_group_count(),
		"emitters": emitters,
		"alive": particles,
	}
	_ctx.log("[fx] wrote %s live_groups=%d emitters=%d alive=%d" % [
			file_name, int(row["live_groups"]), int(row["emitters"]), particles])
	return row
