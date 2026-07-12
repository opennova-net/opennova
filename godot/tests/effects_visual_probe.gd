extends Node3D

# Environment-particle visual probe: loads the mounted .ptl set (NOVA_RESOURCE_DIR)
# into a bare NovaEffectWorld, spawns a spread of representative effects — explosion,
# smoke column, brazier fire, muzzle flash, dirt impact, casing — into an empty dark
# scene, and captures frames over their lifetimes. Verifies the RUNTIME effect path
# (textures via the provider, blend draw, cadence, motion) without mission hunting.
# Output: .scratch/fx_probe/*.png + per-capture live-group/particle counters.

const OUT_DIR := "res://../.scratch/fx_probe"

var _fx: NovaEffectWorld
var _out_abs := ""

const SPAWNS := [
	{"name": "Effect_AirExp", "pos": Vector3(-12, 2, -20)},
	{"name": "Effect_BlackSmoke", "pos": Vector3(-4, 0, -20)},
	{"name": "Effect_Brazier", "pos": Vector3(2, 0, -18)},
	{"name": "Effect_M82_Muz", "pos": Vector3(6, 2, -14)},
	{"name": "Effect_AmHitDirt", "pos": Vector3(10, 0, -18)},
	{"name": "Effect_50BMGCas", "pos": Vector3(8, 1.5, -12)},
]


func _ready() -> void:
	_out_abs = ProjectSettings.globalize_path(OUT_DIR)
	DirAccess.make_dir_recursive_absolute(_out_abs)
	var dir := OS.get_environment("NOVA_RESOURCE_DIR").strip_edges()
	var root := NovaResourceRoot.new()
	if root.set_root_dir(dir) != OK:
		push_error("[fx] mount failed for %s" % dir)
		get_tree().quit(1)
		return

	var cam := Camera3D.new()
	cam.position = Vector3(0, 3, 0)
	cam.current = true
	add_child(cam)
	var light := DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-45, 30, 0)
	add_child(light)
	var env := WorldEnvironment.new()
	var e := Environment.new()
	e.background_mode = Environment.BG_COLOR
	e.background_color = Color(0.12, 0.13, 0.16)
	env.environment = e
	add_child(env)

	_fx = NovaEffectWorld.new()
	add_child(_fx)
	var n := _fx.load_from_resource_root(root)
	print("[fx] loaded: files=%d effects=%d" % [_fx.file_count(), n])
	if n == 0:
		push_error("[fx] no effects loaded")
		get_tree().quit(1)
		return

	for s in SPAWNS:
		var handle: int = _fx.spawn_effect(String(s["name"]), s["pos"])
		print("[fx] spawn %s -> handle %d" % [s["name"], handle])

	await _capture("fx_020.png", 0.2)
	await _capture("fx_100.png", 0.8)
	await _capture("fx_250.png", 1.5)
	print("[fx] done -> ", _out_abs)
	get_tree().quit(0)


func _capture(name: String, wait_s: float) -> void:
	var t := 0.0
	while t < wait_s:
		t += get_process_delta_time()
		await get_tree().process_frame
	await RenderingServer.frame_post_draw
	var img: Image = get_viewport().get_texture().get_image()
	if img != null:
		img.save_png(_out_abs.path_join(name))
	var particles := 0
	for c in _fx.get_children():
		if c.has_method("get_alive_count"):
			particles += int(c.get_alive_count())
	print("[fx] wrote %s live_groups=%d emitters=%d alive=%s" % [
		name, _fx.live_group_count(), _fx.get_child_count(), str(particles)])
