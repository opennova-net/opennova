extends Node

# Minimal clean first-person capture of the arms+gun viewmodel on OPEN ground (away from
# the 05TR spawn tent/foliage), to judge the viewmodel fix without occlusion. Boots the
# standalone game, walks forward to clear the tents, stays first person, and captures
# level plus a slight look-down.

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const StandaloneProbe := preload("res://tests/standalone_game_probe.gd")
const OUT_DIR := "res://../.scratch/fp"

var _play_viewport: Viewport = null
var _out_abs := ""


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
		push_error("[fp] " + String(session.error)); get_tree().quit(1); return
	_play_viewport = session.viewport

	# Walk out to open ground, stay first person.
	_hold(KEY_W, true)
	await _settle(300)
	_hold(KEY_W, false)
	await _settle(30)
	await _capture("01_fp_level.png")

	# NOVA_VM_SWEEP=1: capture the four cardinal container yaws to pin the rig->camera
	# axis map against the retail look in one run (PLAYER_VIEWMODEL_ROT is a live var).
	if OS.get_environment("NOVA_VM_SWEEP") == "1":
		var sweep_presenter := _find_by_method(get_tree().root, "set_debug_force_viewmodel")
		if sweep_presenter != null:
			var pvp: SubViewport = sweep_presenter.viewmodel_rig().get("_vm_viewport")
			var pcam: Camera3D = sweep_presenter.viewmodel_rig().get("_vm_camera")
			print("[fp] pass: vp=%s size=%s cam=%s current=%s fov=%.1f cull=%d world_shared=%s" % [
				str(pvp != null), str(pvp.size) if pvp != null else "-", str(pcam != null),
				str(pcam.current) if pcam != null else "-", pcam.fov if pcam != null else -1.0,
				pcam.cull_mask if pcam != null else -1,
				str(pvp.world_3d == pcam.get_world_3d()) if pvp != null and pcam != null else "-"])
			var vm_models := _viewmodel_models(get_tree().root)
			for m in vm_models:
				print("[fp] vm model %s visible=%s inside_tree=%s" % [m.name, str(m.visible), str(m.is_inside_tree())])
			if pvp != null:
				await RenderingServer.frame_post_draw
				var pimg: Image = pvp.get_texture().get_image()
				if pimg != null:
					pimg.save_png(_out_abs.path_join("pass_view.png"))
					print("[fp] wrote pass_view.png")
			var restore: Vector3 = sweep_presenter.viewmodel_rig().PLAYER_VIEWMODEL_ROT
			for y in [0, 90, 180, 270]:
				sweep_presenter.viewmodel_rig().PLAYER_VIEWMODEL_ROT = Vector3(0, y, 0)
				await _settle(6)
				await _capture("sweep_yaw_%03d.png" % y)
			sweep_presenter.viewmodel_rig().PLAYER_VIEWMODEL_ROT = restore
			await _settle(6)
	_look(Vector2(0, 260))   # ~30 deg down at 0.12 deg/px -- see the gun + hands
	await _settle(24)
	await _capture("02_fp_down.png")

	# NOVA_VM_FSM=1: drive the weapon action FSM live — full-auto fire (LMB held),
	# reload (R), ADS in/out (RMB) — through the REAL input path (LocalPlayerPresenter reads
	# the Input singleton while the mouse is captured), logging the FSM view at each
	# stage and capturing frames. [net-re §5.62]
	if OS.get_environment("NOVA_VM_FSM") == "1":
		await _fsm_sequence()
		print("[fp] done -> ", _out_abs)
		get_tree().quit()
		return

	# NOVA_VM_LAB=1: the viewmodel experiment matrix — {mirrored, unmirrored} x {idle, rest}.
	# Rest pose makes skinning mathematically identity (pose == bind), isolating mesh/skin
	# plumbing from pose deformation; the unmirror isolates the (-x,y,z) handedness question.
	if OS.get_environment("NOVA_VM_LAB") == "1":
		var presenter := _find_by_method(get_tree().root, "set_debug_force_viewmodel")
		var vm_models := _viewmodel_models(get_tree().root)
		print("[fp] lab: presenter=%s models=%d" % [str(presenter != null), vm_models.size()])
		if presenter != null:
			# Runtime oracle: the skeleton rest carries the bind rotations and the reset
			# clip's channels ARE the bind (bind == frame-0 channel on every shipped .bad),
			# so rest-vs-eval_pose(anim_reset) angles ~0 prove the convention holds on the
			# REAL ak47 data.
			for m in vm_models:
				var skel = m.get("_skeletal")
				if skel == null:
					continue
				var bindb: Array = skel.get_skeleton_bones()
				var posed: Array = skel.eval_pose("anim_reset", 0.0)
				var worst := 0.0
				var mean := 0.0
				var n: int = min(bindb.size(), posed.size())
				for i in range(n):
					var rb: Basis = (bindb[i] as Dictionary).get("rest", Transform3D()).basis
					var pb: Basis = (posed[i] as Transform3D).basis
					var d: float = rad_to_deg((rb.inverse() * pb).get_rotation_quaternion().get_angle())
					worst = max(worst, d)
					mean += d
				mean = mean / max(n, 1)
				print("[fp] bind-vs-reset %s: bones=%d worst=%.1f deg mean=%.1f deg" % [m.name, n, worst, mean])
			await _capture("10_idle_native.png")
			# The reset CLIP: identity deltas -> the authored (native) rig verbatim.
			for m in vm_models:
				m.play_body_clip("anim_reset")
			await _settle(6)
			await _capture("22_resetclip.png")
			for m in vm_models:
				m.play_body_clip("anim_wpn_idle")
	else:
		_look(Vector2(0, -430))  # look up a touch
		await _settle(24)
		await _capture("03_fp_up.png")

	print("[fp] done -> ", _out_abs)
	get_tree().quit()


func _fsm_sequence() -> void:
	var world := _find_by_method(get_tree().root, "local_player_weapon_view")
	if world == null:
		push_error("[fp] fsm: no weapon world")
		return
	await _settle(12)
	print("[fp] fsm idle: ", _fsm_str(world.local_player_weapon_view()))
	await _capture("30_fsm_idle.png")
	# Full-auto burst: hold LMB ~40 frames (the AK is flags auto).
	_mouse_btn(MOUSE_BUTTON_LEFT, true)
	await _settle(12)
	await _capture("31_fsm_firing.png")
	await _settle(28)
	_mouse_btn(MOUSE_BUTTON_LEFT, false)
	await _settle(10)
	print("[fp] fsm after burst: ", _fsm_str(world.local_player_weapon_view()))
	# Reload (R edge).
	_hold(KEY_R, true)
	await _settle(3)
	_hold(KEY_R, false)
	await _settle(20)
	print("[fp] fsm mid-reload: ", _fsm_str(world.local_player_weapon_view()))
	await _capture("32_fsm_reload.png")
	# The reload's baked span is TICKS (delaystart 100 + delayend auto from the clip,
	# ~3.5 s of 62.5 Hz wall-clock), while _settle counts FRAMES — wait by STATE so
	# the probe is fps-independent instead of racing the reload at high frame rates.
	var reload_waits := 0
	while world.local_player_weapon_view().current_action != 0 and reload_waits < 60:
		await _settle(10)
		reload_waits += 1
	print("[fp] fsm post-reload: ", _fsm_str(world.local_player_weapon_view()))
	# ADS in (RMB edge), hold for the eased tpos view + zoom, then back to hip.
	_mouse_btn(MOUSE_BUTTON_RIGHT, true)
	await _settle(3)
	_mouse_btn(MOUSE_BUTTON_RIGHT, false)
	await _settle(30)
	await _capture("33_fsm_ads.png")
	var presenter := _find_by_method(get_tree().root, "set_debug_force_viewmodel")
	if presenter != null:
		var cam: Camera3D = presenter.get("_camera")
		var pv: PlayerLocalView = world.local_player_view() \
				if world.has_method("local_player_view") else null
		print("[fp] fsm ads: engaged=%s fraction=%.2f cam_fov=%.1f" % [
			str(pv.scope_engaged) if pv != null else "<null>",
			pv.scope_fraction if pv != null else -1.0,
			cam.fov if cam != null else -1.0])
	_mouse_btn(MOUSE_BUTTON_RIGHT, true)
	await _settle(3)
	_mouse_btn(MOUSE_BUTTON_RIGHT, false)
	await _settle(30)
	await _capture("34_fsm_hip.png")
	print("[fp] fsm final: ", _fsm_str(world.local_player_weapon_view()))


func _fsm_str(v) -> String:
	if v == null:
		return "<null>"
	return "act=%d clip=%d res=%d fired=%d dry=%d rel=%d play=%d key=%s" % [
		v.current_action, v.clip, v.reserve, v.fired_serial, v.dry_serial,
		v.reload_serial, v.play_serial, v.anim_key]


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


# The two NovaObjectModels under the PlayerViewmodel container (arms + gun).
func _viewmodel_models(node: Node) -> Array:
	if node.name == "PlayerViewmodel":
		var out: Array = []
		for ch in node.get_children():
			if ch.has_method("play_body_clip"):
				out.append(ch)
		return out
	for ch in node.get_children():
		var f := _viewmodel_models(ch)
		if not f.is_empty():
			return f
	return []


func _settle(n: int) -> void:
	for _i in n:
		await get_tree().process_frame


func _hold(k: Key, down: bool) -> void:
	var e := InputEventKey.new(); e.keycode = k; e.physical_keycode = k; e.pressed = down
	Input.parse_input_event(e)


func _look(total: Vector2) -> void:
	for _i in 10:
		var mm := InputEventMouseMotion.new(); mm.relative = total / 10.0
		Input.parse_input_event(mm)


func _capture(name: String) -> void:
	await RenderingServer.frame_post_draw
	# The standalone window carries both the world and shared HUD presenter.
	var img: Image = get_viewport().get_texture().get_image()
	if img != null:
		img.save_png(_out_abs.path_join(name))
		print("[fp] wrote ", name)
