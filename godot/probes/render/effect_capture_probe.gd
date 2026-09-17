extends GameProbe

## effect_capture: one authored effect under the crosshair. On the loaded
## mission walk forward `walk_frames`, step the mouse look until the local
## player pitches `pitch_deg` down, spawn `effect` once where that aim meets
## the terrain, `height_offset` metres off the surface (aimed up, a flat
## terrain hit's surface normal), then capture
## `captures` frames every `frames_between` frames while it plays, logging
## every live emitter's position and rendered bounds per capture. Localizes
## a single effect's rendering (a clipped puff, a missing layer, a mis-sized
## rock) without fp_impact's full-auto pile-up. Needs a window (capture).

const EQUIP_SETTLE_FRAMES := 90
const LOOK_STEP_PX := 5.0
const LOOK_STEP_LIMIT := 400
const MIN_PITCH_DEG := 5.0

const SETTLE_AFTER_WALK := 30
const SETTLE_AFTER_LOOK := 24
const SETTLE_AFTER_SPAWN := 1


func run(ctx: ProbeContext) -> ProbeVerdict:
	if not await ctx.wait_for_local_player():
		return ProbeVerdict.failed("no local player (mission load stalled or cancelled)")
	var effect := String(ctx.args.get("effect", "Effect_AmHitDirt"))
	var weapon := String(ctx.args.get("weapon", ""))
	var hide_node := String(ctx.args.get("hide_node", ""))
	var fire_frames := int(ctx.args.get("fire_frames", 0))
	var pitch_deg := float(ctx.args.get("pitch_deg", 30.0))
	var height_offset := float(ctx.args.get("height_offset", 0.0))
	var walk_frames := int(ctx.args.get("walk_frames", 0))
	var frames_between := int(ctx.args.get("frames_between", 3))
	var captures := int(ctx.args.get("captures", 8))
	var out_dir := ProbeOutput.resolve(ctx, String(ctx.args.get("output_dir", "")))
	var sim := ctx.sim()
	var fx := ctx.effect_world()
	if sim == null or fx == null:
		return ProbeVerdict.failed("no loaded mission")

	ctx.defer_restore(func() -> void:
		ProbeInput.hold(KEY_W, false)
		ProbeInput.mouse_btn(MOUSE_BUTTON_LEFT, false))
	if not hide_node.is_empty():
		var hidden: Node = ctx.tree.root.find_child(hide_node, true, false)
		if hidden == null or not (hidden is CanvasItem or hidden is Node3D):
			return ProbeVerdict.failed("hide_node %s is not a visible node in the tree" % hide_node)
		hidden.set("visible", false)
		ctx.defer_restore(func() -> void: hidden.set("visible", true))
		ctx.log("hid %s" % str(hidden.get_path()))
	if not weapon.is_empty():
		var equip_world := ctx.world()
		if equip_world == null or not equip_world.set_local_player_weapon_by_name(weapon):
			return ProbeVerdict.failed("%s is not in this root's weapon.def" % weapon)
		var presenter := ctx.presenter()
		if presenter != null:
			presenter.refresh_viewmodel()
		await ctx.wait_frames(EQUIP_SETTLE_FRAMES)
		ctx.log("equipped %s" % weapon)
	if walk_frames > 0:
		ProbeInput.hold(KEY_W, true)
		await ctx.wait_frames(walk_frames)
		ProbeInput.hold(KEY_W, false)
		await ctx.wait_frames(SETTLE_AFTER_WALK)
	# The mouse path saturates on one large delta, so step until the simulation
	# reports the pitch (the Camera3D node's transform does not carry it).
	var steps := 0
	while absf(sim.get_local_player_pitch_deg()) < pitch_deg and steps < LOOK_STEP_LIMIT:
		ProbeInput.look(Vector2(0, LOOK_STEP_PX))
		await ctx.wait_frames(1)
		steps += 1
	await ctx.wait_frames(SETTLE_AFTER_LOOK)
	var pitch := maxf(absf(sim.get_local_player_pitch_deg()), MIN_PITCH_DEG)
	var cam := ctx.camera()
	if cam == null:
		return ProbeVerdict.failed("no current camera")
	var world := ctx.world()
	var terrain: TerrainData = world.get_terrain_data() if world != null else null
	if terrain == null:
		return ProbeVerdict.failed("no terrain data (the spawn needs the surface height)")
	var origin: Vector3 = sim.get_local_player_position()
	var fwd := -cam.global_transform.basis.z
	var flat := Vector3(fwd.x, 0.0, fwd.z)
	if flat.length_squared() < 0.000001:
		flat = Vector3(0.0, 0.0, -1.0)
	flat = flat.normalized()
	var eye := cam.global_transform.origin
	# Where the aim meets flat ground: the eye height over the local surface
	# divided by tan(pitch), refined once against the surface at that point.
	var run := (eye.y - terrain.get_height_world(origin)) / tan(deg_to_rad(pitch))
	var spawn := origin + flat * run
	run = (eye.y - terrain.get_height_world(spawn)) / tan(deg_to_rad(pitch))
	spawn = origin + flat * run
	spawn.y = terrain.get_height_world(spawn) + height_offset
	ctx.log("camera pos=%s yaw fwd=%s pitch=%0.1f deg (%d look steps) player=%s spawn=%s (%0.2f m ahead, %+0.2f m off the surface)" % [
			str(eye), str(flat), pitch, steps, str(origin), str(spawn), run, height_offset])

	# The frame before the spawn: a difference against it isolates what the
	# effect adds to the picture.
	var base_path := out_dir.path_join("effect_base.png")
	if await ProbeCapture.save_viewport_png(ctx.viewport(), base_path):
		ctx.artifact("effect_base", base_path, "png")

	if not effect.is_empty():
		var handle: int = fx.spawn_effect_transient(effect, spawn, Vector3.UP, 0, 0, 0, 0)
		if handle == 0:
			return ProbeVerdict.failed("%s did not spawn (unknown effect name?)" % effect)
	if fire_frames > 0:
		# Hold the trigger through the real input path; the captures below
		# run during the burst and the trigger releases after them.
		ProbeInput.mouse_btn(MOUSE_BUTTON_LEFT, true)
		await ctx.wait_frames(fire_frames)
	await ctx.wait_frames(SETTLE_AFTER_SPAWN)

	var frames: Array = []
	for index in range(captures):
		if ctx.cancelled:
			return ProbeVerdict.failed("cancelled")
		var path := out_dir.path_join("effect_%02d.png" % index)
		var captured := await ProbeCapture.save_viewport_png(ctx.viewport(), path)
		if captured:
			ctx.artifact("effect_%02d" % index, path, "png")
		var emitters: Array = []
		var names := {}
		var report: Array = fx.get_debug_group_report()
		for group_v in report:
			var group: EffectGroupReport = group_v
			for em_v in group.emitters:
				var em: EffectEmitterReport = em_v
				names[int(em.emitter_id)] = "%s/%s" % [group.name, em.name]
		_log_rendered_bounds(ctx, fx, index, names)
		if index == 0:
			_log_atlas(ctx, fx)
		for group_v in report:
			var group: EffectGroupReport = group_v
			for em_v in group.emitters:
				var em: EffectEmitterReport = em_v
				if not em.alive:
					continue
				var bounds: AABB = em.bounds
				ctx.log("capture %d group=%d %-24s pos=%s bounds=%s..%s" % [
						index, int(group.id), em.name, str(em.position),
						str(bounds.position), str(bounds.end)])
				emitters.append({
					"group_id": int(group.id), "name": em.name, "position": em.position,
					"bounds_min": bounds.position, "bounds_max": bounds.end,
				})
		frames.append({"index": index, "capture": path if captured else "", "emitters": emitters})
		await ctx.wait_frames(frames_between)
	ProbeInput.mouse_btn(MOUSE_BUTTON_LEFT, false)

	var data := {
		"effect": effect,
		"spawn": spawn,
		"camera_origin": cam.global_transform.origin,
		"camera_forward": fwd,
		"frames": frames,
	}
	return ProbeVerdict.passed("%s captured %d frame(s)" % [effect, frames.size()], data)


## The renderer's per-emitter draw bounds (the quads as compiled, per draw
## scope): a quad count and extent that disagree with the simulation bounds
## localize a renderer-side fault.
func _log_rendered_bounds(ctx: ProbeContext, fx: EffectWorld, index: int, names: Dictionary) -> void:
	var renderer: ParticleRenderer = null
	for child in fx.get_children():
		if child is ParticleRenderer:
			renderer = child
			break
	if renderer == null:
		ctx.log("capture %d: no ParticleRenderer child under the effect world" % index)
		return
	var render_viewport := renderer.get_viewport()
	var render_camera: Camera3D = render_viewport.get_camera_3d() if render_viewport != null else null
	var shell_camera := ctx.camera()
	ctx.log("capture %d renderer viewport=%s camera=%s basis=%s origin=%s | shell camera=%s basis=%s" % [
			index,
			str(render_viewport.get_path()) if render_viewport != null else "<none>",
			str(render_camera.get_path()) if render_camera != null else "<none>",
			str(render_camera.get_camera_transform().basis) if render_camera != null else "-",
			str(render_camera.get_camera_transform().origin) if render_camera != null else "-",
			str(shell_camera.get_path()) if shell_camera != null else "<none>",
			str(shell_camera.get_camera_transform().basis) if shell_camera != null else "-"])
	for entry_v in renderer.get_debug_emitter_bounds():
		var entry: Dictionary = entry_v
		var bounds: AABB = entry.get("bounds", AABB())
		if int(entry.get("quad_count", 0)) == 0:
			continue
		ctx.log("capture %d rendered emitter=%s %s scope=%s quads=%d bounds=%s size=%s" % [
				index, str(entry.get("emitter_id")),
				str(names.get(int(entry.get("emitter_id", 0)), "?")),
				str(entry.get("draw_scope")), int(entry.get("quad_count", 0)),
				str(bounds.position), str(bounds.size)])
		# Each quad's corners in the camera frame (x right, y up, depth forward
		# metres) and its screen extent, to name the quad behind a screen artifact.
		var cam := ctx.camera()
		if cam == null:
			continue
		var xf := cam.get_camera_transform()
		var proj := cam.get_camera_projection()
		var size := ctx.viewport().get_visible_rect().size
		for quad_v in entry.get("quads", []):
			var corners: PackedVector3Array = quad_v
			var parts := PackedStringArray()
			for corner in corners:
				var local := xf.affine_inverse() * corner
				var clip := proj * Vector4(local.x, local.y, local.z, 1.0)
				var screen := Vector2(NAN, NAN)
				if clip.w > 0.0001:
					screen = Vector2((clip.x / clip.w * 0.5 + 0.5) * size.x,
							(0.5 - clip.y / clip.w * 0.5) * size.y)
				parts.append("cam=(%0.2f,%0.2f,%0.2f) px=(%0.0f,%0.0f)" % [
						local.x, local.y, -local.z, screen.x, screen.y])
			ctx.log("    quad %s" % " | ".join(parts))


## The terrain leg of the light pool as the last light frame published it:
## every patch holding light rows, with its collect volume, and the patch
## under the spawn point whether lit or not.
func _log_terrain_light_rows(ctx: ProbeContext, index: int, spawn: Vector3) -> void:
	var world := ctx.world()
	var terrain: Terrain = world.get_terrain_node() if world != null else null
	if terrain == null:
		ctx.log("capture %d: no terrain node" % index)
		return
	var rows: Array = terrain.get_debug_light_rows()
	var lit := 0
	for patch_v in rows:
		var patch: Dictionary = patch_v
		var aabb: AABB = patch.get("aabb", AABB())
		var count := int(patch.get("count", 0))
		var under_spawn := spawn.x >= aabb.position.x and spawn.x <= aabb.end.x \
				and spawn.z >= aabb.position.z and spawn.z <= aabb.end.z
		if count == 0 and not under_spawn:
			continue
		if count > 0:
			lit += 1
		var lights: Array = patch.get("lights", [])
		var summary := PackedStringArray()
		for light_v in lights:
			var light: Dictionary = light_v
			summary.append("%s r=%0.2f" % [str(light.get("position")),
					0.5 / maxf(float(light.get("inv_scale", 0.0)), 0.000001)])
		ctx.log("capture %d terrain patch %d%s aabb=%s..%s rows=%d [%s]" % [
				index, int(patch.get("index", -1)), " (under spawn)" if under_spawn else "",
				str(aabb.position), str(aabb.end), count, "; ".join(summary)])
	ctx.log("capture %d terrain patches=%d lit=%d" % [index, rows.size(), lit])


## The renderer's texture resolution: every authored graphic the atlas could
## not resolve, and every atlas entry with its page and pixel rectangle.
func _log_atlas(ctx: ProbeContext, fx: EffectWorld) -> void:
	var unresolved: PackedStringArray = fx.get_unresolved_texture_names()
	ctx.log("atlas unresolved textures (%d): %s" % [unresolved.size(), ", ".join(unresolved)])
	var report: Dictionary = fx.get_debug_draw_list_report()
	var entries: Array = report.get("atlas_entries", [])
	ctx.log("atlas entries=%d pages=%s" % [entries.size(), str(report.get("atlas_page_count"))])
	for entry_v in entries:
		var entry: Dictionary = entry_v
		ctx.log("atlas entry %s type=%s page=%s rect=%s inset=(%s,%s) %s" % [
				str(entry.get("name")), str(entry.get("type")), str(entry.get("page")),
				str(entry.get("pixel_rect")), str(entry.get("inset_u")), str(entry.get("inset_v")),
				"resolved" if bool(entry.get("resolved", true)) else "UNRESOLVED"])

