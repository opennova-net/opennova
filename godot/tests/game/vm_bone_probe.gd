extends Node

# First-person viewmodel bone probe — NOT a GUT test (needs a retail PFF install;
# *_probe.gd files are manual, never collected). A windowed probe: it boots the
# standalone game, equips NOVA_VM_WEAPON (default WPN_M16BURST), and dumps the
# viewmodel rig's placement inputs plus every bone's camera-relative transform so
# they can be compared numerically against the retail FP bone-builder
# transliteration in scripts/render/fp_bone_oracle.py (--probe-log). Run by hand:
#   NOVA_RESOURCE_DIR=<game dir> NOVA_MISSION_BMS=00TRa.bms NOVA_EXPANSION=revx02
#   NOVA_MISSION_PATH=<loose .bms path when the mount lacks it>
#   Godot --path godot --resolution 2000x1200 res://tests/game/vm_bone_probe.tscn

const ResourceDirSettings := preload("res://game/resource_index/resource_dir_settings.gd")
const StandaloneProbe := preload("res://tests/standalone_game_probe.gd")


func _ready() -> void:
	var root := OS.get_environment("NOVA_RESOURCE_DIR").strip_edges()
	if root.is_empty():
		root = ResourceDirSettings.get_resource_dir()
	var expansion := OS.get_environment("NOVA_EXPANSION").strip_edges()
	if expansion.is_empty():
		expansion = ResourceDirSettings.get_expansion()
	var bms := OS.get_environment("NOVA_MISSION_BMS").strip_edges()
	if bms.is_empty():
		bms = "00TRa.bms"
	var weapon := OS.get_environment("NOVA_VM_WEAPON").strip_edges()
	if weapon.is_empty():
		weapon = "WPN_M16BURST"
	await _settle(6)
	var saved := OS.get_environment("NOVA_MISSION_PATH").strip_edges()
	var session: Dictionary = await StandaloneProbe.boot(self, root, bms, expansion, saved)
	if not String(session.get("error", "")).is_empty():
		push_error("[vmbone] " + String(session.error))
		get_tree().quit(1)
		return
	var world: Node = session.get("world")
	var presenter := _find_by_method(get_tree().root, "set_debug_force_viewmodel")
	if presenter == null or world == null:
		push_error("[vmbone] presenter/world missing")
		get_tree().quit(1)
		return
	await _settle(30)
	if world.has_method("set_local_player_weapon_by_name"):
		world.call("set_local_player_weapon_by_name", weapon)
		presenter.viewmodel_rig().refresh_viewmodel()
	# Let the rebuild + a few idle ticks run.
	for _i in 120:
		await get_tree().process_frame
		if presenter.viewmodel_rig().viewmodel() != null and presenter.viewmodel_rig().vm_parts().size() > 0:
			break
	await _settle(90)
	_dump(presenter)
	await _settle(2)
	get_tree().quit()


func _fmt_t(t: Transform3D) -> String:
	var e := t.basis.get_euler()
	return "origin=(%.5f, %.5f, %.5f) euler_deg=(%.3f, %.3f, %.3f) basis=[%s | %s | %s]" % [
		t.origin.x, t.origin.y, t.origin.z,
		rad_to_deg(e.x), rad_to_deg(e.y), rad_to_deg(e.z),
		_fmt_v(t.basis.x), _fmt_v(t.basis.y), _fmt_v(t.basis.z)]


func _fmt_v(v: Vector3) -> String:
	return "%.5f %.5f %.5f" % [v.x, v.y, v.z]


func _dump(presenter: Node) -> void:
	var rig = presenter.viewmodel_rig()
	var cam: Camera3D = rig.get("_camera")
	var vm: Node3D = rig.viewmodel()
	print("[vmbone] POS_UNITS=", rig.PLAYER_VIEWMODEL_POS_UNITS, " TPOS_UNITS=", rig.PLAYER_VIEWMODEL_TPOS_UNITS,
			" ROT_BIAS=", rig.PLAYER_VIEWMODEL_ROT_BIAS_DEF, " ROT=", rig.PLAYER_VIEWMODEL_ROT,
			" RENDERFOV=", rig.PLAYER_VIEWMODEL_RENDERFOV_H_DEG)
	var def = presenter.get("_world").local_player_viewmodel_def() if presenter.get("_world") != null else null
	if def != null:
		print("[vmbone] def: pos=", def.pos_units, " tpos=", def.tpos_units, " rot=", def.rot_bias_deg, " fov=", def.renderfov_h_deg)
	if cam != null:
		print("[vmbone] camera: ", _fmt_t(cam.global_transform), " fov=", cam.fov,
				" keep_aspect=", cam.keep_aspect, " near=", cam.near,
				" viewport=", cam.get_viewport().get_visible_rect().size)
	var pvp: SubViewport = rig.get("_vm_viewport")
	var pcam: Camera3D = rig.get("_vm_camera")
	if pvp != null and pcam != null:
		var container := pvp.get_parent() as SubViewportContainer
		print("[vmbone] vm pass: viewport size=", pvp.size, " cam fov=", pcam.fov,
				" keep_aspect=", pcam.keep_aspect, " near=", pcam.near, " far=", pcam.far,
				" container rect=", container.get_rect() if container != null else Rect2(),
				" container stretch=", container.stretch if container != null else false,
				" stretch_shrink=", container.stretch_shrink if container != null else -1,
				" window=", get_window().size,
				" pass cam (rel gameplay cam)=", _fmt_t(cam.global_transform.affine_inverse() * pcam.global_transform))
	if vm == null or cam == null:
		print("[vmbone] no viewmodel/camera")
		return
	print("[vmbone] viewmodel global: ", _fmt_t(vm.global_transform))
	var rel: Transform3D = cam.global_transform.affine_inverse() * vm.global_transform
	print("[vmbone] viewmodel in camera space: ", _fmt_t(rel))
	for part in rig.vm_parts():
		if not is_instance_valid(part):
			continue
		print("[vmbone] part ", part.name, " visible=", part.visible, " transform(rel vm)=", _fmt_t(vm.global_transform.affine_inverse() * part.global_transform))
		var skel: Skeleton3D = part.get_skeleton()
		if skel == null:
			print("[vmbone]   no Skeleton3D")
			continue
		print("[vmbone]   skeleton transform (rel vm)=", _fmt_t(vm.global_transform.affine_inverse() * skel.global_transform), " bones=", skel.get_bone_count())
		var sa = part.get("_skeletal")
		var idle0: Array = []
		var reset0: Array = []
		if sa != null:
			idle0 = sa.eval_pose("anim_wpn_idle", 0.0)
			reset0 = sa.eval_pose("anim_reset", 0.0)
		for i in skel.get_bone_count():
			var g := skel.get_bone_global_pose(i)
			var r := skel.get_bone_rest(i)
			var line := "[vmbone]   bone %2d %-18s parent=%3d live: %s" % [i, skel.get_bone_name(i), skel.get_bone_parent(i), _fmt_t(g)]
			line += "\n[vmbone]              rest(local): %s" % _fmt_t(r)
			if i < idle0.size():
				line += "\n[vmbone]              idle0: %s" % _fmt_t(idle0[i])
			if i < reset0.size():
				line += "\n[vmbone]              reset0: %s" % _fmt_t(reset0[i])
			print(line)
		var aabb: AABB = part.get_aabb() if part.has_method("get_aabb") else AABB()
		print("[vmbone]   aabb(rel part)=", aabb)
		break  # the gun part carries the shared rig; the arms repeat it


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
