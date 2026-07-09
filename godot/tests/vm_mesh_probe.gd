extends SceneTree

# Headless ground truth for the FP viewmodel mesh question: per-surface raw vertex AABBs
# (the authored coordinate frame), skinned-vs-rigid, and the skeleton's rest-world joint
# spread, for the FP gun (ak47_1st), the FP arms (armsG), and a body (US01). Answers:
# where IS the raw geometry (assembled model-space vs part-local vs view-space), and how
# does it compare to the skeleton rest?
#
# Use: godot --headless --path godot -s res://tests/vm_mesh_probe.gd -- <resource-dir>

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")


func _initialize() -> void:
	call_deferred("_run")


func _run() -> void:
	var args := OS.get_cmdline_user_args()
	var dir: String = args[0] if not args.is_empty() else ResourceDirSettings.get_resource_dir()
	var root := NovaResourceRoot.new()
	if root.set_root_dir(dir) != OK:
		push_error("[vm] set_root_dir failed")
		quit(1)
		return

	# AKM_1st (REVVY-only): the count-mismatch rig -- 46 .bad bones vs 45 model parts. The
	# model table drives the rig (45 rows); the .bad's 46th channel is never sampled
	# [orig: the FK is bounded by modelDef+52 @0x40c400]. Previously this rig silently fell
	# back to BadBone.position (healthy on AKM, broken on a third of the JO corpus).
	for spec in [["ak47_1st", "ak47_1st", 39], ["armsG", "ak47_1st", 39], ["US01", "", 20], ["AKM_1st", "AKM_1st", 45]]:
		_dump(root, spec[0], spec[1], spec[2])
	quit(0)


func _dump(root: NovaResourceRoot, graphic: String, adm: String, bone_count: int) -> void:
	var data := NovaObjectData.new()
	if data.open_from_resource_root(root, graphic + ".3di") != OK:
		print("[vm] %s: NOT FOUND" % graphic)
		return
	var subs: Array = data.build_lod_submeshes(0, true, bone_count)
	print("[vm] === %s: %d submeshes (skeletal build, %d bones) ===" % [graphic, subs.size(), bone_count])
	var total := AABB()
	var first := true
	var skinned_n := 0
	for entry in subs:
		var sub: Dictionary = entry
		var mesh := sub.get("mesh") as ArrayMesh
		if mesh == null or mesh.get_surface_count() == 0:
			continue
		var aabb := mesh.get_aabb()
		if bool(sub.get("is_skinned", false)):
			skinned_n += 1
		if first:
			total = aabb
			first = false
		else:
			total = total.merge(aabb)
	print("[vm] skinned_surfaces=%d/%d  raw-vertex TOTAL AABB pos=%s size=%s" % [
		skinned_n, subs.size(), str(total.position), str(total.size)])
	# a few per-part rows for shape
	var shown := 0
	for entry in subs:
		var sub: Dictionary = entry
		var mesh := sub.get("mesh") as ArrayMesh
		if mesh == null or shown >= 5:
			continue
		print("      part=%2d skinned=%s aabb pos=%s size=%s" % [
			int(sub.get("part_index", -1)), str(bool(sub.get("is_skinned", false))),
			str(mesh.get_aabb().position), str(mesh.get_aabb().size)])
		shown += 1

	# Skeleton rest-world spread (accumulated bind chain from the .adm, with the model bone
	# table). NOTE probe artifact: this passes each model's OWN table; in-game the armsG rig
	# uses the GUN model's table (build_model_from_graphic resolves the .adm's model).
	if adm.is_empty():
		return
	# Model-table rig: count/hierarchy from the model rows, rest positions reconstructed
	# from the model pivots + the reset .bad's bind rotations (NovaSkeletalAnim).
	var sk := NovaSkeletalAnim.new()
	if not sk.load_from_resource_root(root, adm + ".adm", data.get_bone_origins(0), data.get_bone_parents(0)):
		print("[vm] adm load failed: ", sk.get_last_error())
		return
	var bones: Array = sk.get_skeleton_bones()
	var parents: Array[int] = []
	for i in range(bones.size()):
		parents.append(int((bones[i] as Dictionary).get("parent_index", -1)))
	_dump_pose(sk, "rest(bind)", _locals_from_bones(bones), parents)
	# Every clip at 0 / mid / near-end: which keys actually MOVE the rig (the on-screen
	# holds) vs stay at the bind (rest/lowered states).
	for key in sk.get_clip_keys():
		var length: float = sk.get_clip_length(key)
		for phase in [0.0, 0.5, 0.95]:
			var locals: Array[Transform3D] = []
			for t in sk.eval_pose(key, length * phase):
				locals.append(t)
			_dump_pose(sk, "%s@%.2f" % [key, phase], locals, parents)


func _locals_from_bones(bones: Array) -> Array[Transform3D]:
	var out: Array[Transform3D] = []
	for b in bones:
		out.append((b as Dictionary).get("rest", Transform3D()))
	return out


func _dump_pose(sk: NovaSkeletalAnim, label: String, locals: Array[Transform3D], parents: Array[int]) -> void:
	var world: Array[Transform3D] = []
	var lo := Vector3(INF, INF, INF)
	var hi := Vector3(-INF, -INF, -INF)
	var worst_rot := 0.0
	for i in range(locals.size()):
		var p := parents[i]
		var w: Transform3D = locals[i] if p < 0 else world[p] * locals[i]
		world.append(w)
		lo = lo.min(w.origin)
		hi = hi.max(w.origin)
		var ang := rad_to_deg(w.basis.get_rotation_quaternion().get_angle())
		if ang > 180.0:
			ang = 360.0 - ang
		worst_rot = max(worst_rot, ang)
	print("[vm] %-14s joints min=%s max=%s worst|rot|=%.1f deg" % [label, str(lo), str(hi), worst_rot])
	for i in [0, 1, 3, 5, 37, 38]:
		if i < world.size():
			var e := world[i].basis.get_euler()
			print("      j%-2d org=%s eul=(%.0f,%.0f,%.0f)" % [i, str(world[i].origin),
					rad_to_deg(e.x), rad_to_deg(e.y), rad_to_deg(e.z)])
	# Gun-orientation discriminator: the rest gun mesh (flipped frame) lies barrel -> -X with
	# the mag/belly -> -Z (from the observed sense-1/3 renders). Print where the idle bone
	# basis sends those axes: the correct convention has barrel ~horizontal and belly ~ -Y.
	if world.size() > 37:
		var barrel := world[37].basis * Vector3(-1, 0, 0)
		var belly := world[37].basis * Vector3(0, 0, -1)
		print("      j37 barrel->%s belly->%s" % [str(barrel), str(belly)])
