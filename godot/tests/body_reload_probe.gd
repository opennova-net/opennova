extends Node

# Live probe for the 3P body's upper-body weapon channel (D-INF-11 §14.8): boots ONED
# standalone game (JOX root, 05TR), switches to THIRD person (F4), fires a short burst and
# reloads through the REAL input path (R while the mouse is captured), then verifies the
# sim exposes body_anim_key="anim_reload" with an advancing playhead, the avatar model
# carries the weapon channel, and a mask bone (R forearm) visibly leaves the locomotion
# pose while the reload clip plays. Waits reload completion BY STATE (the FSM span is
# ~220 ticks of 62.5 Hz — frame counts would race it). Screenshots land in .scratch/body.
# [orig: producer @0x4b5dad + WeaponSlot_ReloadAmmo @0x54173c; world-wac-ai-re.md §14.8]

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const StandaloneProbe := preload("res://tests/standalone_game_probe.gd")
const OUT_DIR := "res://../.scratch/body"

var _play_viewport: Viewport = null
var _out_abs := ""
var _fail := 0


func _ready() -> void:
	_out_abs = ProjectSettings.globalize_path(OUT_DIR)
	DirAccess.make_dir_recursive_absolute(_out_abs)
	var root := OS.get_environment("NOVA_RESOURCE_DIR").strip_edges()
	if root.is_empty():
		root = ResourceDirSettings.get_resource_dir()
	NovaWindow.set_fullscreen(get_window(), true)
	await _settle(6)

	var bms := OS.get_environment("NOVA_MISSION_BMS").strip_edges()
	if bms.is_empty():
		bms = "05TR.bms"
	var session: Dictionary = await StandaloneProbe.boot(
		self, root, bms, ResourceDirSettings.get_expansion())
	if not String(session.get("error", "")).is_empty():
		push_error("[body] " + String(session.error)); get_tree().quit(1); return
	_play_viewport = session.viewport

	var world: GameWorld = session.world
	var presenter := _find_by_method(get_tree().root, "set_debug_force_viewmodel")
	if world == null or presenter == null:
		push_error("[body] no weapon world/presenter"); get_tree().quit(1); return

	# Third person (F4 through the real key path), clear the spawn tents.
	_hold(KEY_F4, true); await _settle(2); _hold(KEY_F4, false)
	_hold(KEY_W, true)
	await _settle(240)
	_hold(KEY_W, false)
	await _settle(40)
	await _capture("01_tp_idle.png")

	var avatar: Node = presenter.get("_avatar")
	_check(avatar != null, "avatar model present")
	var fore_idle := _mask_bone_rot(avatar)
	print("[body] idle: ", _body_str(world.local_player_weapon_view()))
	_check(String(world.local_player_weapon_view().body_anim_key) ==
			String(world.get_sim().get_local_player_anim_key()),
			"idle: body channel mirrors the state with its own playhead")

	# Short burst so the magazine is not full (the reload input gate), then reload.
	_mouse_btn(MOUSE_BUTTON_LEFT, true)
	await _settle(20)
	_mouse_btn(MOUSE_BUTTON_LEFT, false)
	await _settle(12)
	_hold(KEY_R, true)
	await _settle(3)
	_hold(KEY_R, false)
	await _settle(20)
	var mid = world.local_player_weapon_view()
	print("[body] mid-reload: ", _body_str(mid))
	_check(mid.current_action == 4, "mid-reload: FSM in RELOAD")
	_check(String(mid.body_anim_key) == "anim_reload",
			"mid-reload: body channel plays anim_reload")
	var phase_a := int(mid.body_anim_phase)
	await _settle(10)
	var phase_b := int(world.local_player_weapon_view().body_anim_phase)
	_check(phase_b > phase_a, "mid-reload: body playhead advances (%d -> %d)" % [phase_a, phase_b])
	if avatar != null:
		_check(String(avatar.get("_wpn_key")) == "anim_reload", "avatar carries the weapon channel")
		var fore_reload := _mask_bone_rot(avatar)
		var delta := rad_to_deg((fore_idle.inverse() * fore_reload).get_angle())
		print("[body] R-forearm pose delta vs idle: %.1f deg" % delta)
		_check(delta > 5.0, "mask bone left the locomotion pose (%.1f deg)" % delta)
	await _capture("02_tp_reload.png")

	# Completion BY STATE (~220 ticks), then the channel mirrors the primary state
	# again while retaining its independent playhead.
	var waits := 0
	while world.local_player_weapon_view().current_action != 0 and waits < 60:
		await _settle(10)
		waits += 1
	await _settle(90)  # the 80-tick window outlives the FSM exit briefly; let it drain
	var post = world.local_player_weapon_view()
	print("[body] post-reload: ", _body_str(post))
	_check(post.current_action == 0, "post-reload: FSM back to IDLE")
	_check(String(post.body_anim_key) == String(world.get_sim().get_local_player_anim_key()),
			"post-reload: body channel mirrors the primary state again")
	await _capture("03_tp_post.png")

	print("[body] done -> ", _out_abs, "  failures=", _fail)
	get_tree().quit(1 if _fail > 0 else 0)


func _check(ok: bool, what: String) -> void:
	if ok:
		print("[body] OK: ", what)
	else:
		_fail += 1
		push_error("[body] FAIL: " + what)


# Global rotation of the R forearm mask bone (BN10) on the avatar's skeleton.
func _mask_bone_rot(avatar: Node) -> Quaternion:
	if avatar == null or not avatar.has_method("get_skeleton"):
		return Quaternion.IDENTITY
	var skel: Skeleton3D = avatar.get_skeleton()
	if skel == null:
		return Quaternion.IDENTITY
	for i in skel.get_bone_count():
		if skel.get_bone_name(i).begins_with("BN10"):
			return skel.get_bone_global_pose(i).basis.get_rotation_quaternion()
	return Quaternion.IDENTITY


func _body_str(v) -> String:
	if v == null:
		return "<null>"
	return "act=%d clip=%d res=%d body_key=%s body_phase=%d" % [
		v.current_action, v.clip, v.reserve, v.body_anim_key, v.body_anim_phase]


func _mouse_btn(b: MouseButton, down: bool) -> void:
	var e := InputEventMouseButton.new()
	e.button_index = b
	e.pressed = down
	Input.parse_input_event(e)


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
	var e := InputEventKey.new(); e.keycode = k; e.physical_keycode = k; e.pressed = down
	Input.parse_input_event(e)


func _capture(name: String) -> void:
	await RenderingServer.frame_post_draw
	var vp: Viewport = _play_viewport if _play_viewport != null else get_viewport()
	var img: Image = vp.get_texture().get_image()
	if img != null:
		img.save_png(_out_abs.path_join(name))
		print("[body] wrote ", name)
