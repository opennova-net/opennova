extends SceneTree

# End-to-end placement probe for the THIRD-PERSON held weapon (D-WPN-32).
#
# The axis probe measures how a gfx3 is AUTHORED; this one measures what the shipped
# placement code actually DOES with it. It drives the real PresentHeldWeapon.attach_transform
# against a real posed character skeleton and reports, in WORLD space, where the barrel ends
# up — as an elevation angle and a compass bearing, not as a matrix to be eyeballed.
#
# A level-standing soldier holding a rifle must come out at elevation ~= the attach pitch and
# bearing ~= the attach yaw. Any other reading localises the fault:
#   * elevation ~= 90       -> the barrel is being sent up the model's wrong axis,
#   * bearing off by ~90    -> a yaw-frame error in the basis,
#   * roll off by ~90/180   -> the model's up axis is not what the basis assumes,
#   * barrel length ~= 0.1  -> this is not a rifle at all (a knife gfx3 is +Y-long, 0.40),
#                              i.e. the weapon IDENTITY is wrong, not its orientation.
#
# Run:
#   "$GODOT_BIN" --headless --path godot -s res://tests/held_weapon_placement_probe.gd
# Not collected by GUT (*_probe.gd).

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")
const PresentHeldWeapon := preload("res://engine/world/present_held_weapon.gd")

# The cases worth reading. A level soldier is the one whose answer we already know.
const CASES := [
	{"name": "level, facing yaw=0", "angles": Vector3(0.0, 0.0, 0.0)},
	{"name": "level, facing yaw=90", "angles": Vector3(0.0, 90.0, 0.0)},
	{"name": "the measured live local case", "angles": Vector3(17.95, 13.57, 0.0)},
	{"name": "pitch only, +30 (aiming up)", "angles": Vector3(30.0, 0.0, 0.0)},
	{"name": "roll only, +30 (leaning)", "angles": Vector3(0.0, 0.0, 30.0)},
]


func _init() -> void:
	var root := OS.get_environment("NOVA_RESOURCE_DIR").strip_edges()
	if root.is_empty():
		root = ResourceDirSettings.get_resource_dir()
	var res := NovaResourceRoot.new()
	if res.mount_runtime(root, ResourceDirSettings.get_expansion()) != OK:
		print("FAIL: cannot mount ", root)
		quit(1)
		return
	print("resource root: %s" % root)

	var placer := MissionObjectPlacer.new()
	placer.resource_root = res
	var mount := Node3D.new()
	get_root().add_child(mount)

	# A real posed character, because attach_transform reads bone 16 off a live skeleton.
	var body: Node3D = placer.build_player_animated_model(0x14B9, mount, null)
	if body == null:
		print("FAIL: the player character model did not build")
		quit(1)
		return
	var skel := PresentHeldWeapon.find_skeleton(body)
	if skel == null:
		print("FAIL: no Skeleton3D under the character model")
		quit(1)
		return
	print("skeleton bones: %d   bone %d name: %s" % [
			skel.get_bone_count(), PresentHeldWeapon.BONE_INDEX,
			skel.get_bone_name(PresentHeldWeapon.BONE_INDEX)
					if skel.get_bone_count() > PresentHeldWeapon.BONE_INDEX else "<absent>"])
	# The nudge is authored in MODEL space and carried through bone 16's model->world
	# rotation. Whether that reduces to the bone's posed basis depends on the bone's REST
	# basis being identity, so measure it rather than assume it.
	var bi := PresentHeldWeapon.BONE_INDEX
	var rest_g := skel.get_bone_global_rest(bi)
	var pose_g := skel.get_bone_global_pose(bi)
	print("bone %d rest origin   = %s" % [bi, _v(rest_g.origin)])
	print("bone %d rest basis    = %s | %s | %s   identity=%s" % [
			bi, _v(rest_g.basis.x), _v(rest_g.basis.y), _v(rest_g.basis.z),
			str(rest_g.basis.is_equal_approx(Basis.IDENTITY))])
	print("bone %d posed basis   = %s | %s | %s" % [
			bi, _v(pose_g.basis.x), _v(pose_g.basis.y), _v(pose_g.basis.z)])
	print("skeleton global basis identity=%s" % str(
			skel.global_transform.basis.is_equal_approx(Basis.IDENTITY)))
	var origins: PackedVector3Array = PackedVector3Array()
	var od: Variant = placer.object_data_for(String(placer.graphic_for(
			placer.resolve_player_visual_item_id(0x14B9))))
	if od != null and od.has_method("get_bone_origins"):
		origins = od.get_bone_origins()
	print("model bone origin[%d] = %s   (rest origin above must match)" % [
			bi, _v(origins[bi]) if origins.size() > bi else Vector3.INF])

	# The model frame this probe reasons against, measured rather than assumed.
	var weapon: Node3D = placer.build_model_from_graphic("M4_3RD", "", mount, "", null)
	if weapon == null:
		print("FAIL: M4_3RD did not build")
		quit(1)
		return
	var box: Variant = _combined_aabb(weapon, weapon.global_transform.affine_inverse())
	if box == null:
		print("FAIL: M4_3RD has no meshes")
		quit(1)
		return
	var b: AABB = box
	print("M4_3RD model-space AABB size=%s centre=%s" % [_v(b.size), _v(b.get_center())])
	# The barrel axis in MODEL space: the AABB's dominant extent, signed away from the origin
	# (the origin sits at the grip, so the muzzle is the far end).
	var barrel_model := _dominant_axis(b)
	print("model barrel axis (dominant AABB extent, signed away from origin): %s" % _v(barrel_model))

	print("")
	print("%-32s %8s %8s   %-22s %8s %9s" % [
			"case", "pitch", "yaw", "world barrel dir", "elev", "bearing"])
	for c in CASES:
		var angles: Vector3 = c["angles"]
		var attach: Variant = PresentHeldWeapon.attach_transform(body, angles)
		if attach == null:
			print("%-32s  <attach_transform returned null>" % c["name"])
			continue
		var xf: Transform3D = attach
		var dir: Vector3 = (xf.basis * barrel_model).normalized()
		# Godot Y is up; bearing measured from -Z (the basis' zero-yaw forward) toward +X.
		var elev := rad_to_deg(asin(clampf(dir.y, -1.0, 1.0)))
		var bearing := rad_to_deg(atan2(dir.x, -dir.z))
		print("%-32s %8.2f %8.2f   %-22s %8.2f %9.2f" % [
				c["name"], angles.x, angles.y, _v(dir), elev, bearing])

	print("")
	print("EXPECTED for a faithful port: elev tracks the attach PITCH and bearing tracks the")
	print("attach YAW. The level cases must read elev ~= 0.")
	quit(0)


# The AABB's longest extent as a signed unit axis, pointing away from the model origin.
func _dominant_axis(b: AABB) -> Vector3:
	var s := b.size
	var c := b.get_center()
	if s.x >= s.y and s.x >= s.z:
		return Vector3(signf(c.x) if c.x != 0.0 else 1.0, 0.0, 0.0)
	if s.y >= s.z:
		return Vector3(0.0, signf(c.y) if c.y != 0.0 else 1.0, 0.0)
	return Vector3(0.0, 0.0, signf(c.z) if c.z != 0.0 else 1.0)


func _combined_aabb(node: Node, to_local: Transform3D) -> Variant:
	var out: Variant = null
	if node is MeshInstance3D:
		var mi := node as MeshInstance3D
		if mi.mesh != null:
			out = ((to_local * mi.global_transform) * mi.mesh.get_aabb()) as AABB
	for child in node.get_children():
		var sub: Variant = _combined_aabb(child, to_local)
		if sub == null:
			continue
		out = sub if out == null else (out as AABB).merge(sub as AABB)
	return out


func _v(v: Vector3) -> String:
	return "(%+.3f, %+.3f, %+.3f)" % [v.x, v.y, v.z]
