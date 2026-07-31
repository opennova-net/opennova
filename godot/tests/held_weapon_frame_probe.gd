extends SceneTree

# Diagnostic probe for the THIRD-PERSON held weapon's COMPLETE model frame (D-WPN-32).
#
# held_weapon_axis_probe.gd measured only the merged AABB. An AABB constrains ONE axis: it
# says a firearm is long in +/-Z, but nothing about WHICH END of Z is the muzzle (a 180 deg
# error) nor about the model's ROLL about that axis (which way is "up"). This probe closes
# that gap with landmarks that are directionally signed:
#
#   * every USERPOINT (name + position + direction) -- the gun-flash / bullet userpoint sits
#     at the muzzle, so its sign along the long axis NAMES the front;
#   * every BONE (part pivot), rel + parent -- pivots for barrel / mag / bolt locate the frame;
#   * the PER-PART mesh AABB, not just the merged one -- the magazine part's own box is
#     displaced along the model's DOWN axis, which names up;
#   * a LONG-AXIS SCAN: bin every vertex along the long axis and report the cross-section
#     extent + centre per bin. The barrel end is thin and the receiver end is fat, and the
#     receiver's mass hangs to one side of the bore -- that side is DOWN.
#
# Everything is reported in the model's own local (Godot render) space, i.e. exactly the
# frame PresentHeldWeapon.attach_transform multiplies.
#
# Run:
#   "$GODOT_BIN" --headless --path godot -s res://tests/held_weapon_frame_probe.gd
# Not collected by GUT (*_probe.gd).

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")

const TARGETS: Array[String] = ["COLT_3RD", "M4_3RD", "M16_3RD", "M60_3RD", "M9K_3rd"]
const BINS := 12


func _init() -> void:
	var root := OS.get_environment("NOVA_RESOURCE_DIR").strip_edges()
	if root.is_empty():
		root = ResourceDirSettings.get_resource_dir()
	var exp := ResourceDirSettings.get_expansion()
	print("resource root: %s   expansion: %s" % [root, exp])

	var res := NovaResourceRoot.new()
	if res.mount_runtime(root, exp) != OK:
		print("FAIL: cannot mount ", root)
		quit(1)
		return

	var db := NovaWeaponDatabase.new()
	if db.load_from_resource_root(res, "weapon.def") != OK:
		print("FAIL: weapon.def -> ", db.get_last_error())
		quit(1)
		return
	print("weapon rows: ", db.get_count())

	# gfx3 -> the weapon.def rows that use it, so the printed model is named.
	var gfx3_users := {}
	for i in range(db.get_count()):
		var w: Dictionary = db.get_weapon(i)
		var g: String = String(w.get("gfx3", "")).strip_edges()
		if g.is_empty():
			continue
		var key := g.to_lower()
		if not gfx3_users.has(key):
			gfx3_users[key] = []
		(gfx3_users[key] as Array).append(String(w.get("name", "?")))

	var placer := MissionObjectPlacer.new()
	placer.resource_root = res
	var mount := Node3D.new()
	get_root().add_child(mount)

	for target in TARGETS:
		var users: Array = gfx3_users.get(target.to_lower(), [])
		_report_weapon(placer, mount, target, users)

	_report_character(placer, mount)
	_report_composition()
	quit(0)


# The frame measured above is only half the question: what matters on screen is where the
# model's own forward/up/right END UP after the attach basis multiplies them. Compose the
# LIVE measured angle triples (body (0.00, 13.75, 0.00), attach (17.95, 13.57, 0.00)) with
# the model frame and print the resulting world directions, so "the model frame is/isn't
# the bug" is a number and not an inference.
func _report_composition() -> void:
	print("")
	print("================================================================")
	print("COMPOSITION: model frame x bms_to_godot_basis(angles)")
	print("  firearm model frame: forward(muzzle)=+Z  up(sight)=+Y  right(eject)=-X")
	print("  character  frame:    forward(face)  =+Z  up        =+Y  right(R leg)=-X")
	print("================================================================")
	var cases := {
		"body   (live)   p=0.00  y=13.75 r=0.00": Vector3(0.00, 13.75, 0.00),
		"attach (live)   p=17.95 y=13.57 r=0.00": Vector3(17.95, 13.57, 0.00),
		"attach yaw-only p=0.00  y=13.57 r=0.00": Vector3(0.00, 13.57, 0.00),
		"zero            p=0.00  y=0.00  r=0.00": Vector3(0.00, 0.00, 0.00),
		"pitch 90        p=90.0  y=0.00  r=0.00": Vector3(90.0, 0.00, 0.00),
	}
	for label in cases:
		var b: Basis = MissionObjectPlacer.bms_to_godot_basis(cases[label])
		var fwd: Vector3 = b * Vector3(0, 0, 1)      # model +Z = muzzle
		var up: Vector3 = b * Vector3(0, 1, 0)       # model +Y = sight side
		var right: Vector3 = b * Vector3(-1, 0, 0)   # model -X = ejection side
		var elev := rad_to_deg(asin(clampf(fwd.y, -1.0, 1.0)))
		print("  %s" % label)
		print("      muzzle(+Z) -> %s   elevation above horizontal = %+7.2f deg" % [_v(fwd), elev])
		print("      up    (+Y) -> %s   right(-X) -> %s" % [_v(up), _v(right)])


# ---------------------------------------------------------------------------- weapons


func _report_weapon(placer, mount: Node3D, gfx3: String, users: Array) -> void:
	print("")
	print("================================================================")
	print("MODEL gfx3=%s   weapon.def rows using it: %s" % [gfx3, str(users)])
	print("================================================================")
	var model: Node3D = placer.build_model_from_graphic(gfx3, "", mount, "", null)
	if model == null:
		print("  <no model built>")
		return
	var data = model.get_object_data() if model.has_method("get_object_data") else null
	if data == null:
		print("  <model has no NovaObjectData>")
		model.queue_free()
		return
	_dump_object_data(data, model, 0)
	model.queue_free()


func _dump_object_data(data, model: Node3D, lod: int) -> void:
	var summary: Dictionary = data.get_summary()
	print("  summary: ", summary)
	var lod_info: Dictionary = data.get_render_lod_info(lod)
	print("  lod%d: %s" % [lod, str(lod_info)])
	print("  is_skinned(%d) = %s" % [lod, str(data.is_skinned(lod))])
	print("  ground_anchor  = %s" % _v(data.get_ground_anchor(lod)))

	# --- bones (part pivots) -------------------------------------------------
	# get_bone_origins returns the RAW ENGINE-NATIVE rel_position (no -x flip); the
	# godot-space equivalent negates x, which is what the mesh vertices carry.
	var origins: PackedVector3Array = data.get_bone_origins(lod)
	var parents: PackedInt32Array = data.get_bone_parents(lod)
	print("  BONES/PARTS: count=%d   (rel = engine-native; rel_godot = (-x,y,z))" % origins.size())
	for i in range(origins.size()):
		var p: int = parents[i] if i < parents.size() else -1
		var o: Vector3 = origins[i]
		print("    bone[%2d] parent=%3d  rel=%s  rel_godot=%s" % [
				i, p, _v(o), _v(Vector3(-o.x, o.y, o.z))])

	# --- userpoints ----------------------------------------------------------
	var upc := int(data.get_user_point_count())
	print("  USERPOINTS: count=%d   (position/rotation already godot-space (-x,y,z))" % upc)
	for i in range(upc):
		var info: Dictionary = data.get_user_point_info(i)
		print("    up[%2d] name='%s'  pos=%s  rot=%s  subobject=%d  type=%d" % [
				i, String(info.get("name", "")), _v(info.get("position", Vector3.ZERO)),
				_v(info.get("rotation", Vector3.ZERO)),
				int(info.get("subobject", -1)), int(info.get("point_type", -1))])

	# --- per-part AABB from the SURFACES (godot model space) -----------------
	var surfaces: Array = data.get_lod_surfaces(lod)
	var per_part := {}
	var all_verts := PackedVector3Array()
	for s in surfaces:
		var surf: Dictionary = s
		var pi := int(surf.get("part_index", 0))
		var verts: PackedVector3Array = surf.get("vertices", PackedVector3Array())
		if verts.is_empty():
			continue
		var box := AABB(verts[0], Vector3.ZERO)
		for v in verts:
			box = box.expand(v)
			all_verts.append(v)
		if per_part.has(pi):
			var merged: Dictionary = per_part[pi]
			merged["aabb"] = (merged["aabb"] as AABB).merge(box)
			merged["verts"] = int(merged["verts"]) + verts.size()
			merged["prims"] = int(merged["prims"]) + 1
			per_part[pi] = merged
		else:
			per_part[pi] = {"aabb": box, "verts": verts.size(), "prims": 1}
	var part_keys: Array = per_part.keys()
	part_keys.sort()
	print("  PER-PART MESH AABB (godot model space):")
	for pi in part_keys:
		var e: Dictionary = per_part[pi]
		var b: AABB = e["aabb"]
		print("    part[%2d] prims=%2d verts=%5d  min=%s  max=%s  size=%s  centre=%s" % [
				pi, int(e["prims"]), int(e["verts"]), _v(b.position), _v(b.end),
				_v(b.size), _v(b.get_center())])

	# --- per-MeshInstance3D node AABB in the BUILT scene ---------------------
	print("  BUILT SCENE MeshInstance3D AABBs (model-local):")
	if model != null:
		_dump_mesh_nodes(model, model.global_transform.affine_inverse(), "")
		# A non-identity node transform anywhere inside the built tree would mean the
		# RENDERED frame differs from the raw model space measured above.
		print("  BUILT SCENE Node3D LOCAL transforms (identity => rendered frame == model space):")
		_dump_node_transforms(model, "")

	# --- merged + centroid ---------------------------------------------------
	if all_verts.is_empty():
		print("  <no vertices>")
		return
	var merged_box := AABB(all_verts[0], Vector3.ZERO)
	var centroid := Vector3.ZERO
	for v in all_verts:
		merged_box = merged_box.expand(v)
		centroid += v
	centroid /= float(all_verts.size())
	print("  MERGED: verts=%d  min=%s  max=%s  size=%s  centre=%s  centroid=%s" % [
			all_verts.size(), _v(merged_box.position), _v(merged_box.end),
			_v(merged_box.size), _v(merged_box.get_center()), _v(centroid)])
	var s: Vector3 = merged_box.size
	var long_axis := 0
	if s.y >= s.x and s.y >= s.z:
		long_axis = 1
	elif s.z >= s.x and s.z >= s.y:
		long_axis = 2
	print("  LONG AXIS = %s  (origin sits at frac (%.3f, %.3f, %.3f) of the AABB, 0=min 1=max)" % [
			"XYZ"[long_axis],
			_frac(0.0, merged_box.position.x, merged_box.end.x),
			_frac(0.0, merged_box.position.y, merged_box.end.y),
			_frac(0.0, merged_box.position.z, merged_box.end.z)])

	_scan_axis(all_verts, merged_box, long_axis)


func _dump_node_transforms(node: Node, path: String) -> void:
	var n3 := node as Node3D
	if n3 != null:
		var t: Transform3D = n3.transform
		var ident: bool = t.is_equal_approx(Transform3D.IDENTITY)
		print("    %-46s %s  origin=%s X=%s Y=%s Z=%s" % [
				path + "/" + String(node.name), "IDENTITY" if ident else "NON-IDENT",
				_v(t.origin), _v(t.basis.x), _v(t.basis.y), _v(t.basis.z)])
	for child in node.get_children():
		_dump_node_transforms(child, path + "/" + String(node.name))


func _dump_mesh_nodes(node: Node, to_local: Transform3D, path: String) -> void:
	if node is MeshInstance3D:
		var mi := node as MeshInstance3D
		if mi.mesh != null:
			var b: AABB = (to_local * mi.global_transform) * mi.mesh.get_aabb()
			print("    %-40s min=%s  max=%s  size=%s" % [
					path + "/" + String(node.name), _v(b.position), _v(b.end), _v(b.size)])
	for child in node.get_children():
		_dump_mesh_nodes(child, to_local, path + "/" + String(node.name))


# Bin every vertex along `axis`; per bin report the cross-section min/max/extent on the
# other two axes plus the bin's cross-section CENTRE. The thin end of a firearm is the
# muzzle; the fat end is the receiver, and the receiver's mass hangs to the DOWN side of
# the bore line established by the barrel bins.
func _scan_axis(verts: PackedVector3Array, box: AABB, axis: int) -> void:
	var lo: float = box.position[axis]
	var hi: float = box.end[axis]
	var span: float = hi - lo
	if span <= 0.0:
		return
	var a := (axis + 1) % 3
	var b := (axis + 2) % 3
	var counts := PackedInt32Array()
	counts.resize(BINS)
	var amin := PackedFloat32Array()
	var amax := PackedFloat32Array()
	var bmin := PackedFloat32Array()
	var bmax := PackedFloat32Array()
	amin.resize(BINS)
	amax.resize(BINS)
	bmin.resize(BINS)
	bmax.resize(BINS)
	for i in range(BINS):
		amin[i] = 1e30
		amax[i] = -1e30
		bmin[i] = 1e30
		bmax[i] = -1e30
	for v in verts:
		var t: float = (v[axis] - lo) / span
		var idx: int = clampi(int(t * float(BINS)), 0, BINS - 1)
		counts[idx] += 1
		amin[idx] = minf(amin[idx], v[a])
		amax[idx] = maxf(amax[idx], v[a])
		bmin[idx] = minf(bmin[idx], v[b])
		bmax[idx] = maxf(bmax[idx], v[b])
	print("  LONG-AXIS SCAN along %s (%d bins, %s = cross-section axes):" % [
			"XYZ"[axis], BINS, "XYZ"[a] + "/" + "XYZ"[b]])
	print("    bin  %s_lo    %s_hi   nverts  %smin    %smax    %sext   %scen   |  %smin    %smax    %sext   %scen" % [
			"XYZ"[axis], "XYZ"[axis],
			"XYZ"[a], "XYZ"[a], "XYZ"[a], "XYZ"[a],
			"XYZ"[b], "XYZ"[b], "XYZ"[b], "XYZ"[b]])
	for i in range(BINS):
		var zl: float = lo + span * float(i) / float(BINS)
		var zh: float = lo + span * float(i + 1) / float(BINS)
		if counts[i] == 0:
			print("    %2d  %+7.3f %+7.3f  %6d   <empty>" % [i, zl, zh, 0])
			continue
		print("    %2d  %+7.3f %+7.3f  %6d  %+7.3f %+7.3f %7.3f %+7.3f | %+7.3f %+7.3f %7.3f %+7.3f" % [
				i, zl, zh, counts[i],
				amin[i], amax[i], amax[i] - amin[i], 0.5 * (amin[i] + amax[i]),
				bmin[i], bmax[i], bmax[i] - bmin[i], 0.5 * (bmin[i] + bmax[i])])


# ---------------------------------------------------------------------------- character


func _report_character(placer, mount: Node3D) -> void:
	print("")
	print("================================================================")
	print("CHARACTER runtime type 0x14B9 (build_player_animated_model)")
	print("================================================================")
	var body: Node3D = placer.build_player_animated_model(0x14B9, mount, null)
	if body == null:
		print("  <player character model did not build>")
		return
	var data = body.get_object_data() if body.has_method("get_object_data") else null
	if data != null:
		_dump_object_data(data, body, 0)
	else:
		print("  <no NovaObjectData>")

	var skel: Skeleton3D = body.get_skeleton() if body.has_method("get_skeleton") else null
	if skel == null:
		print("  <no Skeleton3D>")
		body.queue_free()
		return
	print("  SKELETON: bone_count=%d" % skel.get_bone_count())
	for i in range(skel.get_bone_count()):
		var rest: Transform3D = skel.get_bone_rest(i)
		var grest: Transform3D = skel.get_bone_global_rest(i)
		print("    bone[%2d] '%s' parent=%d" % [i, skel.get_bone_name(i), skel.get_bone_parent(i)])
		print("             rest.origin=%s  rest.X=%s rest.Y=%s rest.Z=%s" % [
				_v(rest.origin), _v(rest.basis.x), _v(rest.basis.y), _v(rest.basis.z)])
		print("             grest.origin=%s grest.X=%s grest.Y=%s grest.Z=%s" % [
				_v(grest.origin), _v(grest.basis.x), _v(grest.basis.y), _v(grest.basis.z)])
	body.queue_free()


# ---------------------------------------------------------------------------- helpers


func _frac(value: float, lo: float, hi: float) -> float:
	if hi - lo <= 0.0:
		return 0.0
	return (value - lo) / (hi - lo)


func _v(v: Vector3) -> String:
	return "(%+8.4f, %+8.4f, %+8.4f)" % [v.x, v.y, v.z]
