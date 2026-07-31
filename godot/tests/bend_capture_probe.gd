extends Node

## Visual probe for the third-person torso bend (D-INF-11 port): boots the ONED
## standalone game, opening $NOVA_MISSION_BMS (default 00TRa.bms) from the resource
## root, then drives the player deterministically — F4 to third
## person, synthesized mouse-look down / up / a fast yaw flick — capturing a PNG
## per pose into <repo>/.scratch/bend/. Needs a real rendering window (not
## --headless). Run:
##   NOVA_RESOURCE_DIR=<asset-dir> "$GODOT_BIN" --path godot \
##       res://tests/bend_capture_probe.tscn
## Expected: 03 bends the spine/head forward, 04 back (the §14 gradient), 05
## shows the body/legs lagging a fast aim flick, 06 settled.
## [orig: Entity_BuildBoneTransformMatrices @0x4b1290; world-wac-ai-re.md §14]

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const StandaloneProbe := preload("res://tests/standalone_game_probe.gd")

const OUT_DIR := "res://../.scratch/bend"

var _root := ""
var _out_abs := ""
var _play_viewport: Viewport = null


func _ready() -> void:
	_out_abs = ProjectSettings.globalize_path(OUT_DIR)
	DirAccess.make_dir_recursive_absolute(_out_abs)

	_root = OS.get_environment("NOVA_RESOURCE_DIR").strip_edges()
	if _root.is_empty():
		_root = ResourceDirSettings.get_resource_dir()
	if not ResourceDirSettings.is_valid_root(_root):
		push_error("[bend] no valid resource dir; set NOVA_RESOURCE_DIR")
		get_tree().quit(1)
		return
	print("[bend] resource dir: ", _root)

	# Fullscreen for the captures (the core-engine window mode; F11 / the MCP
	# set_fullscreen tool route here too) — the play viewport fills the display.
	if OS.get_environment("NOVA_FULLSCREEN") != "0":
		NovaWindow.set_fullscreen(get_window(), true)
		await _settle(6)

	var bms_name := OS.get_environment("NOVA_MISSION_BMS").strip_edges()
	if bms_name.is_empty():
		bms_name = "00TRa.bms"
	var session: Dictionary = await StandaloneProbe.boot(
		self, _root, bms_name, ResourceDirSettings.get_expansion())
	if not String(session.get("error", "")).is_empty():
		push_error("[bend] " + String(session.error))
		get_tree().quit(1)
		return
	_play_viewport = session.viewport

	await _capture("01_fp.png")

	# NOVA_VM_SWEEP=1: sweep candidate viewmodel facings (one capture each) to pin the
	# first-person weapon model's native orientation, then quit. Tuning aid only.
	if OS.get_environment("NOVA_VM_SWEEP") == "1":
		var presenter := _find_player_presenter(get_tree().root)
		if presenter != null:
			var candidates: Array = [
				Vector3(0, 180, 0), Vector3(0, 0, 0), Vector3(0, 90, 0), Vector3(0, -90, 0),
				Vector3(-90, 180, 0), Vector3(90, 180, 0), Vector3(-90, 0, 0), Vector3(90, 0, 0),
			]
			for i in candidates.size():
				presenter.viewmodel_rig().PLAYER_VIEWMODEL_ROT = candidates[i]
				await _settle(8)
				await _capture("vm_rot_%d_%s.png" % [i, str(candidates[i]).replace(" ", "")])
		print("[bend] sweep done -> ", _out_abs)
		get_tree().quit()
		return

	_press_key(KEY_F4)
	await _settle(20)
	await _capture("02_tp_level.png")
	# Fast 180 flick: the aim swings instantly, the body chases (~5.8 deg/tick) and the
	# legs re-plant behind it — the lag IS the twist (D-INF-12 chase math).
	_look(Vector2(1500, 0))
	await _settle(3)
	await _capture("03_tp_flick.png")
	await _settle(90)
	await _capture("04_tp_settled.png")
	# Walk out into the open, away from the spawn tent, then face the open field
	# (away from the rock wall the road runs beside).
	_hold_key(KEY_W, true)
	await _settle(260)
	_hold_key(KEY_W, false)
	await _settle(20)
	_look(Vector2(750, 0))            # fast ~90 deg flick on open ground
	await _settle(3)
	await _capture("05_tp_flick_open.png")
	await _settle(80)
	await _capture("06_tp_open_level.png")
	_look(Vector2(0, 420))            # ~50 deg down at 0.12 deg/px
	await _settle(30)
	await _capture("07_tp_down.png")
	_look(Vector2(0, -790))           # ~45 deg up (orbit stays under vertical)
	await _settle(30)
	await _capture("08_tp_up.png")

	# Debug experiments (the F3 View-tab toggles): back to first person with the
	# body forced onto the world layer — look down and find our own feet.
	var presenter := _find_player_presenter(get_tree().root)
	if presenter != null:
		_press_key(KEY_F4)            # back to first person
		presenter.set_debug_body_in_first_person(true)
		presenter.set_debug_force_viewmodel(true)
		_look(Vector2(0, 380))        # level-ish again
		await _settle(20)
		await _capture("09_fp_body_level.png")
		_look(Vector2(0, 580))        # ~70 deg down
		await _settle(30)
		await _capture("10_fp_feet.png")
	else:
		push_warning("[bend] no LocalPlayerPresenter found for the FP-body captures")

	print("[bend] done -> ", _out_abs)
	get_tree().quit()


func _settle(frames: int) -> void:
	for _i in frames:
		await get_tree().process_frame


# The game shell owns a LocalPlayerPresenter; find it by capability.
func _find_player_presenter(node: Node) -> Node:
	if node.has_method("set_debug_body_in_first_person"):
		return node
	for child in node.get_children():
		var found := _find_player_presenter(child)
		if found != null:
			return found
	return null


func _hold_key(keycode: Key, down: bool) -> void:
	var ev := InputEventKey.new()
	ev.keycode = keycode
	ev.physical_keycode = keycode
	ev.pressed = down
	Input.parse_input_event(ev)


func _press_key(keycode: Key) -> void:
	var down := InputEventKey.new()
	down.keycode = keycode
	down.physical_keycode = keycode
	down.pressed = true
	Input.parse_input_event(down)
	var up := InputEventKey.new()
	up.keycode = keycode
	up.physical_keycode = keycode
	up.pressed = false
	Input.parse_input_event(up)


# Feed relative mouse-look in small steps so per-frame handling matches a real
# drag (the presenter clamps pitch per event batch either way).
func _look(total: Vector2) -> void:
	const STEPS := 10
	for _i in STEPS:
		var mm := InputEventMouseMotion.new()
		mm.relative = total / STEPS
		Input.parse_input_event(mm)


func _capture(filename: String) -> void:
	await RenderingServer.frame_post_draw
	var vp: Viewport = _play_viewport if _play_viewport != null else get_viewport()
	var img: Image = vp.get_texture().get_image()
	if img == null:
		push_error("[bend] capture failed for %s" % filename)
		return
	img.save_png(_out_abs.path_join(filename))
	print("[bend] wrote ", filename)
