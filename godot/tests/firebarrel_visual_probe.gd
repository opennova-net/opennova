extends SceneTree
## Manual probe (not collected by GUT): spawns Effect_FireBarrelS through the
## game's effect layer (NovaEffectWorld -> NovaParticleRenderer) against a dark
## backdrop, runs it a few seconds of 62 Hz ticks, and saves viewport captures.
## Verifies the greenish-flame regression fix (case-insensitive curve-table
## resolve) renders the barrel flame orange again.
## Run windowed (rendering required):
##   GODOT_BIN --path godot -s res://tests/firebarrel_visual_probe.gd
## Output: NOVA_PROBE_OUT (dir) or user://firebarrel_probe/

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const TICK_DT := 1.0 / 62.0
const CAPTURE_TIMES := [1.0, 2.0, 3.0, 4.0]

var _effect_world: Node3D
var _elapsed := 0.0
var _accum := 0.0
var _captured := 0
var _out_dir := ""


func _init() -> void:
	_out_dir = OS.get_environment("NOVA_PROBE_OUT")
	if _out_dir.is_empty():
		_out_dir = "user://firebarrel_probe"
	DirAccess.make_dir_recursive_absolute(_out_dir)

	var probe_root := Node3D.new()
	probe_root.name = "ProbeRoot"
	root.add_child(probe_root)

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

	var dir := ResourceDirSettings.get_resource_dir()
	if dir.is_empty():
		dir = OS.get_environment("NOVA_RESOURCE_DIR")
	print("probe: resource dir = ", dir)
	var res_root := NovaResourceRoot.new()
	var expansion := NovaLaunchFlags.expansion(ResourceDirSettings.get_expansion())
	var game := NovaLaunchFlags.game(ResourceDirSettings.get_game())
	if res_root.mount_runtime(dir, expansion, NovaLaunchFlags.loose_override_enabled(), game) != OK:
		print("probe: mount FAILED: ", res_root.get_last_error())
		quit(1)
		return

	_effect_world = NovaEffectWorld.new()
	_effect_world.name = "EffectWorld"
	probe_root.add_child(_effect_world)
	var loaded: int = _effect_world.load_from_resource_root(res_root)
	print("probe: effect files loaded = %d, effects = %d" % [loaded, _effect_world.effect_count()])

	var handle: int = _effect_world.spawn_effect("Effect_FireBarrelS", Vector3.ZERO)
	print("probe: spawn handle = ", handle)
	var unresolved: PackedStringArray = _effect_world.get_unresolved_texture_names()
	if unresolved.size() > 0:
		print("probe: unresolved textures: ", unresolved)


func _process(delta: float) -> bool:
	if _effect_world == null:
		return true
	_elapsed += delta
	_accum += delta
	while _accum >= TICK_DT:
		_accum -= TICK_DT
		_effect_world.advance_fixed_tick(TICK_DT)
		_effect_world.sweep(TICK_DT)
	if _captured < CAPTURE_TIMES.size() and _elapsed >= float(CAPTURE_TIMES[_captured]):
		var image := root.get_viewport().get_texture().get_image()
		var path := "%s/flame_t%d.png" % [_out_dir, _captured + 1]
		image.save_png(path)
		print("probe: captured ", path)
		_captured += 1
	if _captured >= CAPTURE_TIMES.size():
		print("probe: live groups = ", _effect_world.live_group_count(),
				" active entries = ", _effect_world.active_entry_count())
		quit(0)
		return true
	return false
