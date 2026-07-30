extends Node

# Renderer-alignment capture probe: boots the standalone game on a retail
# mission, then captures a
# closed-loop yaw sweep of viewport PNGs at the player spawn, plus pitch
# up/down shots, for side-by-side comparison against retail screenshots.
# Prints player pos/yaw/pitch per shot so each PNG maps back to a viewpoint.
# Not collected by GUT (probe suffix).
#
# Use (windowed — screenshots need a real rasterizer, NOT --headless; run the
# SCENE so autoloads register — a bare -s script boot skips them and
# game_world.gd fails to compile):
#   NOVA_MISSION_BMS=00TRe.bms NOVA_SHOT_PREFIX=sniper \
#     "$GODOT_BIN" --path godot res://tests/render_align_probe.tscn
# Env: NOVA_RESOURCE_DIR (defaults to the persisted ONED root),
#      NOVA_MISSION_BMS (default 00TRe.bms), NOVA_SHOT_PREFIX (default = bms
#      base name), NOVA_WALK_FRAMES (optional W-hold frames before the sweep).

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const StandaloneProbe := preload("res://tests/standalone_game_probe.gd")
const OUT_DIR := "res://../.scratch/rendercmp"

var _out_abs := ""
var _world: Node = null
var _cam: Camera3D = null
var _yaw_gain := 0.0
var _pitch_gain := 0.0


func _ready() -> void:
	var bms := OS.get_environment("NOVA_MISSION_BMS").strip_edges()
	if bms.is_empty():
		bms = "00TRe.bms"
	var prefix := OS.get_environment("NOVA_SHOT_PREFIX").strip_edges()
	if prefix.is_empty():
		prefix = bms.get_basename().to_lower()
	_out_abs = ProjectSettings.globalize_path(OUT_DIR).path_join(prefix)
	DirAccess.make_dir_recursive_absolute(_out_abs)

	var root_dir := OS.get_environment("NOVA_RESOURCE_DIR").strip_edges()
	if root_dir.is_empty():
		root_dir = ResourceDirSettings.get_resource_dir()
	get_window().mode = Window.MODE_WINDOWED
	get_window().size = Vector2i(1920, 1080)

	var path := NovaPaths.resolve_file(root_dir, bms)
	print("[rendercmp] mission=%s path=%s out=%s" % [bms, path, _out_abs])
	var session: Dictionary = await StandaloneProbe.boot(
		self, root_dir, bms, ResourceDirSettings.get_expansion())
	if not String(session.get("error", "")).is_empty():
		push_error("[rendercmp] " + String(session.error))
		get_tree().quit(1)
		return

	_world = session.world
	_cam = session.camera
	print("[rendercmp] world=%s cam=%s" % [str(_world != null), str(_cam != null)])
	if _world == null or _cam == null:
		get_tree().quit(1)
		return
	_dump_env_state()

	var walk := int(OS.get_environment("NOVA_WALK_FRAMES"))
	if walk > 0:
		_hold(KEY_W, true)
		await _settle(walk)
		_hold(KEY_W, false)
		await _settle(30)

	await _measure_gains()

	# Eight-way yaw sweep at eye level.
	for yaw in [0.0, 45.0, 90.0, 135.0, 180.0, 225.0, 270.0, 315.0]:
		await _aim(yaw, 0.0)
		await _capture("%s_yaw%03d.png" % [prefix, int(yaw)])

	# Sky and ground at the spawn's forward heading.
	await _aim(0.0, 35.0)
	await _capture("%s_pitchA.png" % prefix)
	await _aim(0.0, -35.0)
	await _capture("%s_pitchB.png" % prefix)

	print("[rendercmp] done -> ", _out_abs)
	await _settle(10)
	get_tree().quit(0)


# The env node lives in the play camera's world; report what drives the look.
func _dump_env_state() -> void:
	for node in get_tree().root.find_children("*", "", true, false):
		if node.has_method("get_sky_ambient") and "time_of_day" in node:
			if _cam != null and node.get_viewport() != _cam.get_viewport():
				continue
			print("[rendercmp] env node=%s time_of_day=%s weather_driven=%s" % [
				node.name, str(node.time_of_day),
				str(node.is_weather_driven()) if node.has_method("is_weather_driven") else "?"])
			if node.has_method("get_color_src_gain"):
				print("[rendercmp] gain=%s" % str(node.get_color_src_gain()))
			print("[rendercmp] sun_light=%s fill=%s sky_amb=%s" % [
				str(node.get_sun_light()), str(node.get_fill_light()), str(node.get_sky_ambient())])
			print("[rendercmp] fog_color=%s fog_level=%.1f" % [
				str(node.get_fog_color()), node.get_fog_level()])
			if node.has_method("get_sun_light_target"):
				print("[rendercmp] targets sun=%s fill=%s sky=%s" % [
					str(node.get_sun_light_target()) if node.has_method("get_sun_light_target") else "?",
					str(node.get_fill_light_target()) if node.has_method("get_fill_light_target") else "?",
					str(node.get_sky_ambient_target()) if node.has_method("get_sky_ambient_target") else "?"])
			if node.has_method("get_sky_base"):
				print("[rendercmp] dome sky_base=%s bright=%s cloud_base=%s edge=%s" % [
					str(node.get_sky_base()), str(node.get_sky_bright()),
					str(node.get_cloud_base()), str(node.get_cloud_edge())])
	for light in get_tree().root.find_children("*", "", true, false):
		if not light.has_method("resync_colors"):
			continue
		if _cam != null and light is CanvasItem:
			continue
		var core = light.get("_core")
		if core != null and core.has_method("get_color_src_gain"):
			print("[rendercmp] weather core gain=%s" % str(core.get_color_src_gain()))


# Mouse-motion yaw/pitch response measured live: sign and scale both come from
# the sim, so the aim loop needs no convention assumptions.
func _measure_gains() -> void:
	var y0: float = _world.get_sim().get_local_player_yaw_deg()
	_look(Vector2(200, 0))
	await _settle(6)
	var y1: float = _world.get_sim().get_local_player_yaw_deg()
	_yaw_gain = _wrap_deg(y1 - y0) / 200.0
	var p0: float = _world.get_sim().get_local_player_pitch_deg()
	_look(Vector2(0, 200))
	await _settle(6)
	var p1: float = _world.get_sim().get_local_player_pitch_deg()
	_pitch_gain = (p1 - p0) / 200.0
	print("[rendercmp] yaw_gain=%.5f pitch_gain=%.5f" % [_yaw_gain, _pitch_gain])
	if absf(_yaw_gain) < 0.0001 or absf(_pitch_gain) < 0.0001:
		push_error("[rendercmp] look gain measurement failed — aim loop cannot converge")


func _aim(target_yaw: float, target_pitch: float) -> void:
	for _i in 24:
		var yaw_err := _wrap_deg(target_yaw - float(_world.get_sim().get_local_player_yaw_deg()))
		var pitch_err := target_pitch - float(_world.get_sim().get_local_player_pitch_deg())
		if absf(yaw_err) < 0.5 and absf(pitch_err) < 0.5:
			break
		var dx := clampf(yaw_err / _yaw_gain, -400.0, 400.0) if absf(yaw_err) >= 0.5 else 0.0
		var dy := clampf(pitch_err / _pitch_gain, -400.0, 400.0) if absf(pitch_err) >= 0.5 else 0.0
		_look(Vector2(dx, dy))
		await _settle(4)


func _wrap_deg(a: float) -> float:
	return wrapf(a, -180.0, 180.0)


func _capture(name: String) -> void:
	await _settle(6)
	await RenderingServer.frame_post_draw
	var img: Image = get_viewport().get_texture().get_image()
	if img != null:
		img.save_png(_out_abs.path_join(name))
	print("[rendercmp] wrote %s yaw=%.1f pitch=%.1f pos=%s" % [
		name, _world.get_sim().get_local_player_yaw_deg(), _world.get_sim().get_local_player_pitch_deg(),
		str(_world.get_sim().get_local_player_position())])


func _find_by_method(node: Node, method: String) -> Node:
	if node.has_method(method):
		return node
	for ch in node.get_children():
		var f := _find_by_method(ch, method)
		if f != null:
			return f
	return null


func _settle(n: int) -> void:
	for _i in n:
		await get_tree().process_frame


func _hold(k: Key, down: bool) -> void:
	var e := InputEventKey.new()
	e.keycode = k
	e.physical_keycode = k
	e.pressed = down
	Input.parse_input_event(e)


func _look(total: Vector2) -> void:
	for _i in 10:
		var mm := InputEventMouseMotion.new()
		mm.relative = total / 10.0
		Input.parse_input_event(mm)
