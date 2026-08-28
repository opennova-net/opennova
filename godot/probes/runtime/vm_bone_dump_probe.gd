extends GameProbe

## vm_bone_dump: the first-person viewmodel bone dump. Equips `weapon` on
## the loaded mission and writes the viewmodel rig's placement inputs plus
## every bone's camera-relative transform (vm_bone_dump.log), then the
## full-clip sweep (vm_sweep.log): every frame of every anim_wpn_idle /
## anim_wpn_reload variant posed on BOTH parts (the gun carries the receiver
## bones, the arms the left hand/fingers) with each bone's global origin, so
## the rig compares numerically against the retail FP bone builders (the
## transliteration that lived in scripts/render/fp_bone_oracle.py, retired
## 2026-08-26 with the Python tooling, history at 5820432c1). The sweep is
## synchronous (play_body_clip_variant_at_time writes the Skeleton3D in the
## same call), so the presenter's per-tick playhead pin never runs between a
## pose and its dump. Needs a window (the projection feed reads the real
## viewport).

const SWEEP_KEYS: Array[String] = ["anim_wpn_idle", "anim_wpn_reload"]
const EQUIP_SETTLE_FRAMES := 30
const REBUILD_WAIT_FRAMES := 120
const POSE_SETTLE_FRAMES := 90

var _ctx: ProbeContext
var _dump_lines: PackedStringArray = []
var _sweep_lines: PackedStringArray = []
var _sweep_rows := 0


func run(ctx: ProbeContext) -> ProbeVerdict:
	_ctx = ctx
	if not await ctx.wait_for_local_player():
		return ProbeVerdict.failed("no local player (mission load stalled or cancelled)")
	var weapon := String(ctx.args.get("weapon", "WPN_M16BURST"))
	var out_dir := ProbeOutput.resolve(ctx, String(ctx.args.get("output_dir", "")))
	await ctx.wait_frames(EQUIP_SETTLE_FRAMES)
	var world := ctx.world()
	var presenter := ctx.presenter()
	if world == null or presenter == null:
		return ProbeVerdict.failed("presenter/world missing")
	if not world.set_local_player_weapon_by_name(weapon):
		return ProbeVerdict.failed("%s is not in this root's weapon.def" % weapon)
	presenter.viewmodel_rig().refresh_viewmodel()
	# Let the rebuild + a few idle ticks run.
	for _i in REBUILD_WAIT_FRAMES:
		await ctx.tree.process_frame
		presenter = ctx.presenter()
		if presenter == null or ctx.cancelled:
			return ProbeVerdict.failed("the presenter went away during the rebuild")
		var rig := presenter.viewmodel_rig()
		if rig.viewmodel() != null and rig.vm_parts().size() > 0:
			break
	await ctx.wait_frames(POSE_SETTLE_FRAMES)
	presenter = ctx.presenter()
	if presenter == null or ctx.cancelled:
		return ProbeVerdict.failed("cancelled")
	var bones := _dump(presenter, weapon)
	_sweep_dump(presenter)
	var dump_path := out_dir.path_join("vm_bone_dump.log")
	var sweep_path := out_dir.path_join("vm_sweep.log")
	if not _write_lines(dump_path, _dump_lines) or not _write_lines(sweep_path, _sweep_lines):
		return ProbeVerdict.failed("could not write the dump files under %s" % out_dir)
	ctx.artifact("vm_bone_dump", dump_path, "log")
	ctx.artifact("vm_sweep", sweep_path, "log")
	var data := {
		"weapon": weapon,
		"parts": presenter.viewmodel_rig().vm_parts().size(),
		"bones": bones,
		"dump_lines": _dump_lines.size(),
		"sweep_rows": _sweep_rows,
		"dump": dump_path,
		"sweep": sweep_path,
	}
	if bones <= 0:
		return ProbeVerdict.failed("no viewmodel skeleton to dump for %s" % weapon, data)
	return ProbeVerdict.passed("%s: %d bone(s) dumped, %d sweep row(s)" % [weapon, bones, _sweep_rows], data)


## A dump header line: into the file and the run log.
func _note(text: String) -> void:
	_dump_lines.append(text)
	_ctx.log(text)


func _fmt_t(t: Transform3D) -> String:
	var e := t.basis.get_euler()
	return "origin=(%.5f, %.5f, %.5f) euler_deg=(%.3f, %.3f, %.3f) basis=[%s | %s | %s]" % [
		t.origin.x, t.origin.y, t.origin.z,
		rad_to_deg(e.x), rad_to_deg(e.y), rad_to_deg(e.z),
		_fmt_v(t.basis.x), _fmt_v(t.basis.y), _fmt_v(t.basis.z)]


func _fmt_v(v: Vector3) -> String:
	return "%.5f %.5f %.5f" % [v.x, v.y, v.z]


## The placement inputs, the camera, the viewmodel in camera space and every
## bone of the gun part (the arms repeat the shared rig). Returns the bone
## count dumped.
func _dump(presenter: LocalPlayerPresenter, weapon: String) -> int:
	var rig := presenter.viewmodel_rig()
	var cam: Camera3D = rig.camera()
	var vm: Node3D = rig.viewmodel()
	_note("weapon=%s POS_UNITS=%s TPOS_UNITS=%s ROT_BIAS=%s ROT=%s RENDERFOV=%s" % [
			weapon, str(rig.PLAYER_VIEWMODEL_POS_UNITS), str(rig.PLAYER_VIEWMODEL_TPOS_UNITS),
			str(rig.PLAYER_VIEWMODEL_ROT_BIAS_DEF), str(rig.PLAYER_VIEWMODEL_ROT),
			str(rig.PLAYER_VIEWMODEL_RENDERFOV_H_DEG)])
	var world := presenter.world()
	var def: PlayerViewmodelDef = world.local_player_viewmodel_def() if world != null else null
	if def != null:
		_note("def: pos=%s tpos=%s rot=%s fov=%s" % [
				str(def.pos_units), str(def.tpos_units), str(def.rot_bias_deg), str(def.renderfov_h_deg)])
	if cam != null:
		_note("camera: %s fov=%s keep_aspect=%s near=%s viewport=%s" % [
				_fmt_t(cam.global_transform), str(cam.fov), str(cam.keep_aspect), str(cam.near),
				str(cam.get_viewport().get_visible_rect().size)])
	# The gun draws inside the beauty pass through the renderfov projection
	# feed (x = focal ratio vs the beauty projection, y = near, z = far).
	_note("vm projection feed=%s window=%s" % [
			str(rig.projection_feed()), str(_ctx.viewport().get_visible_rect().size)])
	if vm == null or cam == null:
		_note("no viewmodel/camera")
		return 0
	_note("viewmodel global: " + _fmt_t(vm.global_transform))
	var rel: Transform3D = cam.global_transform.affine_inverse() * vm.global_transform
	_note("viewmodel in camera space: " + _fmt_t(rel))
	var bones := 0
	for part in rig.vm_parts():
		if not is_instance_valid(part):
			continue
		_dump_lines.append("part %s visible=%s transform(rel vm)=%s" % [
				part.name, str(part.visible), _fmt_t(vm.global_transform.affine_inverse() * part.global_transform)])
		var skel: Skeleton3D = part.get_skeleton()
		if skel == null:
			_dump_lines.append("  no Skeleton3D")
			continue
		_dump_lines.append("  skeleton transform (rel vm)=%s bones=%d" % [
				_fmt_t(vm.global_transform.affine_inverse() * skel.global_transform), skel.get_bone_count()])
		var sa: SkeletalAnim = part.get_skeletal_anim()
		var idle0: Array = []
		var reset0: Array = []
		if sa != null:
			idle0 = sa.eval_pose("anim_wpn_idle", 0.0)
			reset0 = sa.eval_pose("anim_reset", 0.0)
		for i in skel.get_bone_count():
			var g := skel.get_bone_global_pose(i)
			var r := skel.get_bone_rest(i)
			_dump_lines.append("  bone %2d %-18s parent=%3d live: %s" % [
					i, skel.get_bone_name(i), skel.get_bone_parent(i), _fmt_t(g)])
			_dump_lines.append("             rest(local): %s" % _fmt_t(r))
			if i < idle0.size():
				_dump_lines.append("             idle0: %s" % _fmt_t(idle0[i]))
			if i < reset0.size():
				_dump_lines.append("             reset0: %s" % _fmt_t(reset0[i]))
		bones = skel.get_bone_count()
		_ctx.log("part %s: %d bone(s) dumped" % [part.name, bones])
		break  # the gun part carries the shared rig; the arms repeat it
	return bones


## Clip sweep: pose every frame of every variant of SWEEP_KEYS on BOTH parts
## and record each bone's global origin.
func _sweep_dump(presenter: LocalPlayerPresenter) -> void:
	var rig := presenter.viewmodel_rig()
	for part in rig.vm_parts():
		if not is_instance_valid(part):
			continue
		var skel: Skeleton3D = part.get_skeleton()
		var sa: SkeletalAnim = part.get_skeletal_anim()
		if skel == null or sa == null:
			_ctx.log("sweep part=%s: no skeleton/skeletal" % part.name)
			_sweep_lines.append("part=%s no skeleton/skeletal" % part.name)
			continue
		for key in SWEEP_KEYS:
			if not sa.has_clip(key):
				_ctx.log("sweep part=%s key=%s MISSING" % [part.name, key])
				_sweep_lines.append("part=%s key=%s MISSING" % [part.name, key])
				continue
			for variant in sa.get_clip_variant_count(key):
				var frames: int = sa.get_clip_frame_count(key, variant)
				var fps: float = sa.get_clip_fps(key, variant)
				if frames <= 0 or fps <= 0.0:
					continue
				var header := "part=%s key=%s variant=%d frames=%d fps=%.3f loops=%s bones=%d" % [
						part.name, key, variant, frames, fps,
						str(sa.is_clip_looping(key, variant)), skel.get_bone_count()]
				_ctx.log("sweep " + header)
				_sweep_lines.append(header)
				for f in frames:
					part.play_body_clip_variant_at_time(key, variant, float(f) / fps)
					for i in skel.get_bone_count():
						var g := skel.get_bone_global_pose(i)
						_sweep_lines.append("part=%s key=%s variant=%d frame=%d bone=%d name=%s origin=(%.5f, %.5f, %.5f)" % [
								part.name, key, variant, f, i, skel.get_bone_name(i),
								g.origin.x, g.origin.y, g.origin.z])
						_sweep_rows += 1


func _write_lines(path: String, lines: PackedStringArray) -> bool:
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		return false
	for line in lines:
		file.store_line(line)
	file.close()
	return true
