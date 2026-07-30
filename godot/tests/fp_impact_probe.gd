extends Node

# Impact-position probe: boots the standalone game on the requested mount, walks to open
# ground, aims down, fires a burst through the real input path, then dumps the
# camera aim ray against every live effect-world group (name / sim position /
# rendered bounds) to localize "impacts spawn in the wrong place" reports.
# Run windowed:
#   GODOT_BIN --path godot res://tests/fp_impact_probe.tscn  (or -s wrapper)
# Output: res://../.scratch/fp_impact/

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const StandaloneProbe := preload("res://tests/standalone_game_probe.gd")
const OUT_DIR := "res://../.scratch/fp_impact"

var _out_abs := ""


func _ready() -> void:
	_out_abs = ProjectSettings.globalize_path(OUT_DIR)
	DirAccess.make_dir_recursive_absolute(_out_abs)
	# The resource dir is persisted user:// state SHARED with the game runtime —
	# restore it on every exit path so a probe run never repoints the user's mount.
	var root := OS.get_environment("NOVA_RESOURCE_DIR").strip_edges()
	if root.is_empty():
		root = ResourceDirSettings.get_resource_dir()

	NovaWindow.set_fullscreen(get_window(), true)
	await _settle(6)

	var bms := OS.get_environment("NOVA_MISSION_BMS").strip_edges()
	if bms.is_empty():
		bms = "00TRa.bms"
	var session: Dictionary = await StandaloneProbe.boot(
		self, root, bms, ResourceDirSettings.get_expansion())
	if not String(session.get("error", "")).is_empty():
		push_error("[impact] " + String(session.error))
		get_tree().quit(1)
		return

	# Walk forward a little, then aim down ~35 deg so the burst hits near ground
	# a few meters ahead.
	_hold(KEY_W, true)
	await _settle(120)
	_hold(KEY_W, false)
	await _settle(30)
	_look(Vector2(0, 300))
	await _settle(24)

	var cam: Camera3D = session.camera
	var world: GameWorld = session.world
	if cam == null or world == null:
		push_error("[impact] no camera/world"); get_tree().quit(1); return

	var t := cam.global_transform
	var fwd := -t.basis.z
	print("[impact] camera pos=%s fwd=%s" % [str(t.origin), str(fwd)])
	for d in [5.0, 10.0, 20.0, 40.0]:
		print("[impact] aim ray @%0.0fm = %s" % [d, str(t.origin + fwd * float(d))])

	# Short burst through the real input path.
	_mouse_btn(MOUSE_BUTTON_LEFT, true)
	await _settle(20)
	_mouse_btn(MOUSE_BUTTON_LEFT, false)
	# Let the rounds fly and impact (a few hundred m/s over tens of meters = a
	# handful of ticks); keep groups alive for the report.
	await _settle(30)

	var fx = world.get_effect_world()
	if fx == null:
		push_error("[impact] no effect world"); get_tree().quit(1); return
	var report: Array = fx.get_debug_group_report()
	print("[impact] live groups = %d" % report.size())
	for group_v in report:
		var group: Dictionary = group_v
		for em_v in group.get("emitters", []):
			var em: Dictionary = em_v
			var bounds: AABB = em.get("bounds", AABB())
			print("[impact] group=%d %-28s alive=%d pos=%s bounds_center=%s valid=%s" % [
					int(group.get("group_id", 0)), String(em.get("name", "?")),
					int(em.get("alive", 0)), str(em.get("position", Vector3.ZERO)),
					str(bounds.get_center()), str(em.get("bounds_valid", false))])
	await _capture("impact_scene.png")
	print("[impact] done -> ", _out_abs)
	get_tree().quit()


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


func _mouse_btn(b: MouseButton, down: bool) -> void:
	var e := InputEventMouseButton.new(); e.button_index = b; e.pressed = down
	Input.parse_input_event(e)


func _look(total: Vector2) -> void:
	for _i in 10:
		var mm := InputEventMouseMotion.new(); mm.relative = total / 10.0
		Input.parse_input_event(mm)


func _capture(name: String) -> void:
	await RenderingServer.frame_post_draw
	var img: Image = get_viewport().get_texture().get_image()
	if img != null:
		img.save_png(_out_abs.path_join(name))
		print("[impact] wrote ", name)
