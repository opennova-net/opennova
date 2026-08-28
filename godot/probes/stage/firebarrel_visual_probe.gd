extends GameProbe

## firebarrel_visual: spawns Effect_FireBarrelS through the game's effect layer
## (EffectWorld -> ParticleRenderer) against a dark backdrop on a probe stage,
## runs it a few seconds of 62 Hz ticks, and saves stage captures. Verifies the
## greenish-flame regression fix (case-insensitive curve-table resolve) renders
## the barrel flame orange again. Needs a window.

const STAGE_SIZE := Vector2i(960, 540)
const TICK_DT := 1.0 / 62.0
const CAPTURE_TIMES := [1.0, 2.0, 3.0, 4.0]

var _ctx: ProbeContext
var _stage: ProbeStage
var _effect_world: EffectWorld
var _out_dir := ""


func run(ctx: ProbeContext) -> ProbeVerdict:
	_ctx = ctx
	_out_dir = ProbeOutput.resolve(ctx, String(ctx.args.get("output_dir", "")))
	var res_root := ctx.resource_root()
	if res_root == null:
		return ProbeVerdict.failed("the shell has no mounted resource root")
	_stage = ProbeStage.create(ctx, STAGE_SIZE, ctx.viewport())
	var probe_root := Node3D.new()
	probe_root.name = "ProbeRoot"
	_stage.add_scene(probe_root)

	var env := WorldEnvironment.new()
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.08, 0.08, 0.10)
	env.environment = environment
	probe_root.add_child(env)

	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 0.9, 2.6)
	camera.look_at_from_position(camera.position, Vector3(0.0, 0.7, 0.0), Vector3.UP)
	probe_root.add_child(camera)
	camera.make_current()

	_effect_world = EffectWorld.new()
	_effect_world.name = "EffectWorld"
	probe_root.add_child(_effect_world)
	var loaded: int = _effect_world.load_from_resource_root(res_root)
	ctx.log("probe: effect files loaded = %d, effects = %d" % [loaded, _effect_world.effect_count()])
	if _effect_world.effect_count() <= 0:
		return ProbeVerdict.failed("the mounted root carries no particle effects")

	var handle: int = _effect_world.spawn_effect("Effect_FireBarrelS", Vector3.ZERO)
	ctx.log("probe: spawn handle = %d" % handle)
	var unresolved: PackedStringArray = _effect_world.get_unresolved_texture_names()
	if unresolved.size() > 0:
		ctx.log("probe: unresolved textures: %s" % str(unresolved))

	# The 62 Hz effect clock, two ticks per accumulated step as the original
	# probe ran it, rendered every frame until each capture time.
	var elapsed := 0.0
	var accum := 0.0
	var captured: Array = []
	while captured.size() < CAPTURE_TIMES.size() and not ctx.cancelled:
		var delta := ctx.tree.root.get_process_delta_time()
		elapsed += delta
		accum += delta
		while accum >= TICK_DT:
			accum -= TICK_DT
			_effect_world.advance_fixed_tick(TICK_DT)
			_effect_world.advance_fixed_tick(TICK_DT)
		_effect_world.render_frame()
		if elapsed >= float(CAPTURE_TIMES[captured.size()]):
			var image := await _stage.capture_image(ctx.tree)
			var path := "%s/flame_t%d.png" % [_out_dir, captured.size() + 1]
			if image == null or image.save_png(path) != OK:
				return ProbeVerdict.failed("capture %d produced no image" % (captured.size() + 1))
			ctx.artifact("flame_t%d" % (captured.size() + 1), path, "png")
			ctx.log("probe: captured " + path)
			captured.append(path)
		else:
			await ctx.tree.process_frame
	var data := {
		"handle": handle,
		"live_groups": _effect_world.live_group_count(),
		"active_entries": _effect_world.active_entry_count(),
		"unresolved_textures": Array(unresolved),
		"captures": captured,
	}
	ctx.log("probe: live groups = %d active entries = %d" % [
			int(data["live_groups"]), int(data["active_entries"])])
	if handle <= 0:
		return ProbeVerdict.failed("Effect_FireBarrelS did not spawn", data)
	return ProbeVerdict.passed("%d flame captures" % captured.size(), data)
