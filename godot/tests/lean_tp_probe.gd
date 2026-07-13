extends Node

# Diagnostic probe for the #239 runtime reports: (A) FP lean not moving the camera,
# (B) prone roll never rolling the camera, (C) FP->TP crosshair aim discontinuity.
# Boots ONED play-in-editor on open ground and prints the sim view (lean_deg etc.),
# the camera basis roll, the head-bone eye, and the aim projections at each stage.

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const EditorScene := preload("res://modtools/editor/editor_main.tscn")
const OUT_DIR := "res://../.scratch/leantp"

var _out_abs := ""
var _world: Node = null
var _host: Node = null
var _cam: Camera3D = null


func _ready() -> void:
	_out_abs = ProjectSettings.globalize_path(OUT_DIR)
	DirAccess.make_dir_recursive_absolute(_out_abs)
	var root := OS.get_environment("NOVA_RESOURCE_DIR").strip_edges()
	if root.is_empty():
		root = ResourceDirSettings.get_resource_dir()
	ResourceDirSettings.set_resource_dir(root)

	var app = EditorScene.instantiate()
	add_child(app)
	await get_tree().process_frame
	for _i in 8:
		await get_tree().process_frame
	var ws_station = app.workstation
	ws_station.set_resource_root_dir(root)
	NovaWindow.set_fullscreen(get_window(), true)
	await _settle(6)

	var bms := OS.get_environment("NOVA_MISSION_BMS").strip_edges()
	if bms.is_empty():
		bms = "05TR.bms"
	var ws = ws_station.get_workspace_adapter(EditorWorkstation.Workspace.MISSION)
	var path := NovaPaths.resolve_file(root, bms)
	if ws.open_file(path) != OK:
		push_error("[leantp] open failed"); get_tree().quit(1); return
	ws_station.set_active_workspace(EditorWorkstation.Workspace.MISSION)
	await _settle(30)
	if int(ws.play_mission()) != OK:
		push_error("[leantp] play failed"); get_tree().quit(1); return
	await _settle(120)

	_world = _find_by_method(get_tree().root, "local_player_view")
	_host = _find_by_method(get_tree().root, "set_debug_force_viewmodel")
	_cam = _find_play_camera(get_tree().root)
	print("[leantp] world=%s host=%s cam=%s" % [str(_world != null), str(_host != null), str(_cam != null)])
	if _world == null or _host == null or _cam == null:
		get_tree().quit(1); return

	# Walk clear of the spawn tents.
	_hold(KEY_W, true)
	await _settle(200)
	_hold(KEY_W, false)
	await _settle(30)

	# --- Phase A: standing FP lean (hold Q) --------------------------------
	print("[leantp] --- A: standing lean, holding Q ---")
	_dump("A pre")
	_hold(KEY_Q, true)
	for i in 5:
		await _settle(10)
		_dump("A hold+%d" % ((i + 1) * 10))
	await _capture("A_lean_fp.png")
	_hold(KEY_Q, false)
	await _settle(40)
	_dump("A released")

	# --- Phase B: prone roll (Z then hold Q) --------------------------------
	print("[leantp] --- B: prone, then hold Q (roll) ---")
	_press(KEY_Z)
	await _settle(100)
	_dump("B prone settled")
	_hold(KEY_Q, true)
	for i in 6:
		await _settle(10)
		_dump("B roll+%d" % ((i + 1) * 10))
	await _capture("B_prone_roll_fp.png")
	_hold(KEY_Q, false)
	await _settle(60)
	_press(KEY_C)
	await _settle(120)
	_dump("B stood back up")

	# --- Phase C: FP->TP aim continuity -------------------------------------
	print("[leantp] --- C: FP aim -> F4 -> TP aim ---")
	# Pitch down a touch so the ray hits terrain at a testable distance.
	_look(Vector2(0, 120))
	await _settle(20)
	_dump("C fp aimed")
	_project("C fp", [10.0, 50.0, 1000.0])
	await _capture("C_fp_aim.png")
	_press(KEY_F4)
	await _settle(60)
	_dump("C tp settled")
	_project("C tp", [10.0, 50.0, 1000.0])
	var aim: Vector2 = _host.aim_screen_point()
	var vp_size: Vector2 = _cam.get_viewport().get_visible_rect().size
	print("[leantp] C tp aim_screen_point=%s viewport=%s center=%s" % [str(aim), str(vp_size), str(vp_size * 0.5)])
	await _capture("C_tp_aim.png")
	_press(KEY_F4)
	await _settle(30)

	ws.stop_play_mission()
	print("[leantp] done -> ", _out_abs)
	get_tree().quit()


# One diagnostic line: the sim view's fp_roll_deg, the camera's roll indicator
# (right-axis vertical component; 0 = level), positions, and the body anim key.
func _dump(tag: String) -> void:
	var view = _world.local_player_view()
	var roll: float = view.fp_roll_deg if view != null else -999.0
	var anim := String(_world.local_player_anim_key()) if _world.has_method("local_player_anim_key") else "?"
	var right: Vector3 = _cam.global_transform.basis.x
	var roll_deg := rad_to_deg(asin(clampf(right.y, -1.0, 1.0)))
	var pos: Vector3 = _world.local_player_position()
	print("[leantp] %s: fp_roll_deg=%.2f cam_roll=%.2f cam_pos=%s player=%s anim=%s yaw=%.1f pitch=%.1f third=%s" % [
		tag, roll, roll_deg, str(_cam.global_position), str(pos), anim,
		_world.local_player_yaw_deg(), _world.local_player_pitch_deg(),
		str(_host.is_third_person())])


# Project the aim ray's points at the given ranges through the live camera —
# the FP center-vs-TP crosshair continuity check.
func _project(tag: String, ranges: Array) -> void:
	var yr := deg_to_rad(_world.local_player_yaw_deg())
	var pr := deg_to_rad(_world.local_player_pitch_deg())
	var forward := Vector3(sin(yr) * cos(pr), sin(pr), -cos(yr) * cos(pr))
	var eye: Vector3 = _host._eye_position(_world.local_player_position())
	for r in ranges:
		var p: Vector3 = eye + forward * float(r)
		var s := "BEHIND" if _cam.is_position_behind(p) else str(_cam.unproject_position(p))
		print("[leantp] %s ray@%.0f -> screen %s" % [tag, float(r), s])


func _find_by_method(node: Node, method: String) -> Node:
	if node.has_method(method):
		return node
	for ch in node.get_children():
		var f := _find_by_method(ch, method)
		if f != null:
			return f
	return null


func _find_play_camera(node: Node) -> Camera3D:
	if node.has_method("get_play_camera"):
		var c = node.call("get_play_camera")
		if c is Camera3D:
			return c
	for ch in node.get_children():
		var f := _find_play_camera(ch)
		if f != null:
			return f
	return null


func _settle(n: int) -> void:
	for _i in n:
		await get_tree().process_frame


func _capture(name: String) -> void:
	await RenderingServer.frame_post_draw
	var img: Image = get_viewport().get_texture().get_image()
	if img != null:
		img.save_png(_out_abs.path_join(name))
		print("[leantp] wrote ", name)


func _hold(k: Key, down: bool) -> void:
	var e := InputEventKey.new()
	e.keycode = k
	e.physical_keycode = k
	e.pressed = down
	Input.parse_input_event(e)


func _press(k: Key) -> void:
	_hold(k, true)
	_hold(k, false)


func _look(total: Vector2) -> void:
	for _i in 10:
		var mm := InputEventMouseMotion.new()
		mm.relative = total / 10.0
		Input.parse_input_event(mm)
