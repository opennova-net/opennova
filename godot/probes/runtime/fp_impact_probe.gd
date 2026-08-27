extends GameProbe

## fp_impact: the impact-position probe. On the loaded mission equip `weapon`
## when given (a spawn kit holding the knife produces no impacts), walk
## forward `walk_frames`, aim down `look_px` pixels so a burst hits the ground
## a few metres ahead, fire `burst_frames` through the real input path, then
## dump the camera aim ray against every live effect-world group (name, sim
## position, rendered bounds) to localize "impacts spawn in the wrong place"
## reports, and capture the frame. Fails when the burst left no live group.
## Needs a window (input and capture).

const AIM_RAY_DISTANCES := [5.0, 10.0, 20.0, 40.0]
const EQUIP_SETTLE_FRAMES := 90
const SETTLE_AFTER_WALK := 30
const SETTLE_AFTER_LOOK := 24
const SETTLE_AFTER_BURST := 30


func run(ctx: ProbeContext) -> ProbeVerdict:
	if not await ctx.wait_for_local_player():
		return ProbeVerdict.failed("no local player (mission load stalled or cancelled)")
	var weapon := String(ctx.args.get("weapon", ""))
	var walk_frames := int(ctx.args.get("walk_frames", 120))
	var look_px := float(ctx.args.get("look_px", 300.0))
	var burst_frames := int(ctx.args.get("burst_frames", 20))
	var out_dir := ProbeOutput.resolve(ctx, String(ctx.args.get("output_dir", "")))
	ctx.defer_restore(func() -> void:
		ProbeInput.hold(KEY_W, false)
		ProbeInput.mouse_btn(MOUSE_BUTTON_LEFT, false))
	if not weapon.is_empty():
		var world := ctx.world()
		if world == null or not world.set_local_player_weapon_by_name(weapon):
			return ProbeVerdict.failed("%s is not in this root's weapon.def" % weapon)
		var presenter := ctx.presenter()
		if presenter != null:
			presenter.refresh_viewmodel()
		await ctx.wait_frames(EQUIP_SETTLE_FRAMES)
		ctx.log("equipped %s" % weapon)

	# Walk forward a little, then aim down so the burst lands near the ground
	# a few metres ahead.
	ProbeInput.hold(KEY_W, true)
	await ctx.wait_frames(walk_frames)
	ProbeInput.hold(KEY_W, false)
	await ctx.wait_frames(SETTLE_AFTER_WALK)
	ProbeInput.look(Vector2(0, look_px))
	await ctx.wait_frames(SETTLE_AFTER_LOOK)

	var cam := ctx.camera()
	if cam == null:
		return ProbeVerdict.failed("no current camera")
	var t := cam.global_transform
	var fwd := -t.basis.z
	ctx.log("camera pos=%s fwd=%s" % [str(t.origin), str(fwd)])
	var rays: Array = []
	for d in AIM_RAY_DISTANCES:
		var point := t.origin + fwd * float(d)
		ctx.log("aim ray @%0.0fm = %s" % [d, str(point)])
		rays.append({"distance": d, "point": point})

	# A short burst through the real input path, then let the rounds fly and
	# impact (a few hundred m/s over tens of metres = a handful of ticks) while
	# the groups stay alive for the report.
	ProbeInput.mouse_btn(MOUSE_BUTTON_LEFT, true)
	await ctx.wait_frames(burst_frames)
	ProbeInput.mouse_btn(MOUSE_BUTTON_LEFT, false)
	await ctx.wait_frames(SETTLE_AFTER_BURST)
	if ctx.cancelled:
		return ProbeVerdict.failed("cancelled")

	var fx := ctx.effect_world()
	if fx == null:
		return ProbeVerdict.failed("no effect world")
	var report: Array = fx.get_debug_group_report()
	ctx.log("live groups = %d" % report.size())
	var emitters: Array = []
	for group_v in report:
		var group: Dictionary = group_v
		for em_v in group.get("emitters", []):
			var em: Dictionary = em_v
			var bounds: AABB = em.get("bounds", AABB())
			ctx.log("group=%d %-28s alive=%d pos=%s bounds_center=%s valid=%s" % [
					int(group.get("group_id", 0)), String(em.get("name", "?")),
					int(em.get("alive", 0)), str(em.get("position", Vector3.ZERO)),
					str(bounds.get_center()), str(em.get("bounds_valid", false))])
			emitters.append({
				"group_id": int(group.get("group_id", 0)),
				"name": String(em.get("name", "?")),
				"alive": int(em.get("alive", 0)),
				"position": em.get("position", Vector3.ZERO),
				"bounds_center": bounds.get_center(),
				"bounds_valid": bool(em.get("bounds_valid", false)),
			})
	var capture_path := out_dir.path_join("impact_scene.png")
	var captured := await ProbeCapture.save_viewport_png(ctx.viewport(), capture_path)
	if captured:
		ctx.artifact("impact_scene", capture_path, "png")
	var data := {
		"camera_origin": t.origin,
		"camera_forward": fwd,
		"aim_rays": rays,
		"live_groups": report.size(),
		"emitters": emitters,
		"capture": capture_path if captured else "",
	}
	if not captured:
		return ProbeVerdict.failed("the frame capture produced no image", data)
	if report.is_empty():
		return ProbeVerdict.failed(
				"the burst left no live effect group (a knife spawn kit? pass `weapon`, or aim at ground in range)", data)
	return ProbeVerdict.passed("%d live group(s), %d emitter(s) reported against the aim ray" % [
			report.size(), emitters.size()], data)
