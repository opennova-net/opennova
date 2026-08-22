extends SceneTree

# Ladder-climb in-game probe — NOT a GUT test (needs a retail PFF install;
# *_probe.gd files are manual, never collected). Drives the D-COL-5 climb motor
# on 00TRa's ladder-climbing tutorial section:
#
#   * sweeps the collision debug view around the spawn for a CL/type-4 volume
#     (the ladder is authored purely as a .3di BVOL — no items.def attribute),
#   * teleports the player to the ladder base and tries the 8 approach yaws
#     looking up — the witnessed entry gate needs the view within 60 deg of the
#     authored facing with the look-pitch sign agreeing with the anchor side
#     [orig: @ 0x4b32a5-0x4b32c0],
#   * HOLD FORWARD after a mouse-look up: the climb block stamps climb_up and
#     the clip's vertical root lane rises the body [orig: @ 0x4b7484-0x4b750a],
#   * release: climb_idle holds height (gravity skipped while latched
#     [orig: @ 0x4b7acd]); a natural top-out hands over to the exit leg.
#   The dismount family (side/back fans, jump-off, the grounded bottom exit)
#   is pinned deterministically by the `collision`/`infantry` ctests instead.
#
#   NW_SP_MISSION=00TRa.bms NW_RESOURCE_DIR=<retail install> "$GODOT_BIN" \
#       --headless --path godot -s res://tests/ladder_climb_probe.gd
#
# godot-pos <-> mission-pos: mission = (x, -z, y) of the godot vector.

const LOAD_TIMEOUT_WALL_SECONDS := 240.0
const TIME_SCALE := 5.0
const SWEEP_STEP := 100.0
const SWEEP_RADIUS := 400.0

var _forward := false
# Action-shot mode: NW_PROBE_SHOTS=<dir> + a NON-headless run saves PNGs at
# the latch / mid-climb / top-out beats. A dedicated probe camera frames the
# third-person body from the climber's back quarter (the presenter's own chase
# cam sits inside the tower foliage here).
var _shots_dir := ""
var _shot_vp: SubViewport = null
var _shot_cam: Camera3D = null
var _shot_dir3 := Vector3.ZERO
# The open-air side of the ladder face (godot horizontal), learned from the
# hover offset that latched — the camera shoots from open air at the rungs.
var _shot_open_dir := Vector3.ZERO


func _initialize() -> void:
	_shots_dir = OS.get_environment("NW_PROBE_SHOTS")
	call_deferred("_run")


# The presenter re-asserts its own camera every frame, so the shot camera
# renders through a dedicated SubViewport sharing the game's World3D — no
# `current` competition, and no HUD overlay in the capture.
func _frame_shot(sim, ladder_center: Vector3, extra_up := 0.0) -> void:
	if _shot_cam == null:
		_shot_vp = SubViewport.new()
		_shot_vp.size = Vector2i(1280, 720)
		_shot_vp.render_target_update_mode = SubViewport.UPDATE_ALWAYS
		root.add_child(_shot_vp)
		_shot_vp.world_3d = root.world_3d
		_shot_cam = Camera3D.new()
		_shot_cam.fov = 55.0
		_shot_vp.add_child(_shot_cam)
		if _shot_open_dir.length() > 0.01:
			# Small swing only — keep the rungs facing the lens.
			_shot_dir3 = _shot_open_dir.normalized().rotated(Vector3.UP, 0.25)
		else:
			var away: Vector3 = sim.get_local_player_position() - ladder_center
			away.y = 0.0
			_shot_dir3 = away.normalized() if away.length() > 0.01 else Vector3.RIGHT
			# Swing off the face normal so a ladder support post cannot sit
			# dead between the camera and the climber.
			_shot_dir3 = _shot_dir3.rotated(Vector3.UP, 0.6)
	var player: Vector3 = sim.get_local_player_position()
	var side := _shot_dir3.cross(Vector3.UP)
	_shot_cam.position = player + _shot_dir3 * 4.2 + side * 1.6 \
			+ Vector3.UP * (1.3 + extra_up)
	_shot_cam.look_at(player + Vector3.UP * 0.8)
	_shot_cam.make_current()


func _shot(name: String) -> void:
	if _shots_dir.is_empty():
		return
	# Freeze the sim clock for the capture — the settle frames below still
	# render, but the body cannot walk out of the framed shot at probe
	# timescale. Two full rendered frames so the subviewport target holds a
	# settled image.
	Engine.time_scale = 0.0
	await RenderingServer.frame_post_draw
	await RenderingServer.frame_post_draw
	var src: Viewport = _shot_vp if _shot_vp != null else root
	var img := src.get_texture().get_image()
	Engine.time_scale = TIME_SCALE
	if img == null:
		return
	var path := _shots_dir.path_join("ladder_%s.png" % name)
	img.save_png(path)
	print("PROBE shot saved: %s" % path)


func _mission_wait(seconds: float) -> void:
	var t0 := Time.get_ticks_msec()
	while float(Time.get_ticks_msec() - t0) * TIME_SCALE < seconds * 1000.0:
		await process_frame


func _fail(msg: String) -> void:
	print("PROBE FAIL: %s" % msg)
	quit(1)


# All CL volumes among the collision instances currently in range (godot-space
# volume center + base height from the transformed corners). "lean" is the
# horizontal distance between the top-face and bottom-face corner centroids —
# ~0 for a true vertical rung ladder, large for a climbable staircase/gangway.
func _cl_volumes(sim) -> Array:
	var found: Array = []
	var cd: Dictionary = sim.get_collision_debug()
	for inst in cd.get("instances", []):
		for vd in inst.get("volumes", []):
			if int(vd.get("type", -1)) != Simulation.BVOL_LADDER_CL:
				continue
			var corners: PackedVector3Array = vd.get("corners", PackedVector3Array())
			if corners.size() != 8:
				continue
			var center := Vector3.ZERO
			var base := INF
			var top := -INF
			for c in corners:
				center += c
				base = minf(base, c.y)
				top = maxf(top, c.y)
			center /= 8.0
			var mid := (base + top) * 0.5
			var lo := Vector3.ZERO
			var hi := Vector3.ZERO
			var nlo := 0
			var nhi := 0
			for c in corners:
				if c.y <= mid:
					lo += c
					nlo += 1
				else:
					hi += c
					nhi += 1
			var lean := INF
			var top_xy := Vector3(center.x, 0.0, center.z)
			if nlo > 0 and nhi > 0:
				lo /= float(nlo)
				hi /= float(nhi)
				lean = Vector2(hi.x - lo.x, hi.z - lo.z).length()
				top_xy = Vector3(hi.x, 0.0, hi.z)
			found.append({
				"center": center,
				"base": base,
				"top": top,
				"lean": lean,
				"top_xy": top_xy,
				"corners": corners,
				"entity_handle": int(inst.get("entity_handle", -1)),
				"inst_pos": inst.get("pos", Vector3.INF),
			})
	return found


func _run() -> void:
	if OS.get_environment("NW_SP_MISSION").is_empty():
		push_error("ladder_climb_probe: set NW_SP_MISSION=00TRa.bms")
		quit(1)
		return
	var res_dir := OS.get_environment("NW_RESOURCE_DIR")
	if not res_dir.is_empty():
		var settings := load("res://game/resource_index/resource_dir_settings.gd")
		settings.set_resource_dir(res_dir)
		settings.set_expansion("")
	Engine.time_scale = TIME_SCALE
	var packed := load("res://game/main_game.tscn") as PackedScene
	if packed == null:
		_fail("failed to load main_game.tscn")
		return
	var game := packed.instantiate()
	root.add_child(game)
	var world = game.get_node_or_null("World")
	var presenter = game.get_node_or_null("LocalPlayerPresenter")
	if world == null or presenter == null:
		_fail("main_game lacks World/LocalPlayerPresenter children")
		return

	var wall_start := Time.get_ticks_msec()
	while not (world.get_sim() != null and world.get_sim().has_local_player()):
		await process_frame
		if float(Time.get_ticks_msec() - wall_start) / 1000.0 > LOAD_TIMEOUT_WALL_SECONDS:
			_fail("player never spawned (mission load stalled?)")
			return
	var sim = world.get_sim()
	presenter.set_input_source(func() -> Dictionary:
		return {"forward": _forward})
	await _mission_wait(1.0)

	# --- Sweep for CL volumes: the debug view is player-anchored (150u), so
	# hop a teleport grid around the spawn and ACCUMULATE every CL in range.
	# 00TRa's tutorial area marks both the stilt-village staircases and the
	# market rung ladders climbable — rank by lean and climb a true vertical
	# ladder, not a gangway.
	var spawn: Vector3 = sim.get_local_player_position() # godot space
	var by_key: Dictionary = {}
	var gx := spawn.x
	var gz := spawn.z
	var step := SWEEP_STEP
	var probe_points: Array = [Vector2(gx, gz)]
	var r := step
	while r <= SWEEP_RADIUS:
		var n := int(ceil(TAU * r / step))
		for i in n:
			var a := TAU * float(i) / float(n)
			probe_points.append(Vector2(gx + r * cos(a), gz + r * sin(a)))
		r += step
	for p in probe_points:
		sim.debug_teleport_local_player(Vector3(p.x, -p.y, 60.0), 0.0, 0.0)
		await _mission_wait(0.12)
		for v in _cl_volumes(sim):
			var c: Vector3 = v["center"]
			var key := "%d_%d_%d" % [roundi(c.x * 4.0), roundi(c.y * 4.0), roundi(c.z * 4.0)]
			if not by_key.has(key):
				by_key[key] = v
	var ladders: Array = by_key.values()
	if ladders.is_empty():
		_fail("no CL/type-4 volume within %.0fu of the spawn" % SWEEP_RADIUS)
		return
	# Vertical first (lean, then taller); report the whole candidate field.
	ladders.sort_custom(func(a, b):
		if absf(float(a["lean"]) - float(b["lean"])) > 0.25:
			return float(a["lean"]) < float(b["lean"])
		return float(a["top"]) - float(a["base"]) > float(b["top"]) - float(b["base"]))
	for v in ladders:
		print("PROBE CL candidate: center %s h %.1f lean %.2f owner=%d" %
				[str(v["center"]), float(v["top"]) - float(v["base"]),
				float(v["lean"]), int(v["entity_handle"])])
	var ladder: Dictionary = ladders[0]
	var center: Vector3 = ladder["center"]
	var base: float = ladder["base"]
	var top: float = ladder["top"]
	# The prism footprint — the re-latch reach question needs its depth.
	var pc: PackedVector3Array = ladder.get("corners", PackedVector3Array())
	if pc.size() == 8:
		var pxmin := INF
		var pxmax := -INF
		var pzmin := INF
		var pzmax := -INF
		for c in pc:
			pxmin = minf(pxmin, c.x)
			pxmax = maxf(pxmax, c.x)
			pzmin = minf(pzmin, c.z)
			pzmax = maxf(pzmax, c.z)
		print("PROBE PRISM footprint: x[%.3f..%.3f] (%.2fu) z[%.3f..%.3f] (%.2fu)" %
				[pxmin, pxmax, pxmax - pxmin, pzmin, pzmax, pzmax - pzmin])
	print("PROBE ladder CL chosen at godot %s (base %.1f top %.1f lean %.2f), %d in field; owner handle=%d inst_pos=%s" %
			[str(center), base, top, float(ladder["lean"]), ladders.size(),
			int(ladder["entity_handle"]), str(ladder["inst_pos"])])

	# --- NW_PROBE_BOTTOM: full-span diagnostic — enter at the BASE like a
	# player walks up to a ladder (from-below arm: facing within 60° + pitch
	# UP), then climb the whole span with a per-beat log. Reproduces the
	# "pushed off halfway up" report; prints where and in what state the
	# latch drops.
	if not OS.get_environment("NW_PROBE_BOTTOM").is_empty():
		var bface: Vector3 = ladder["top_xy"]
		var blatched := false
		for k in 8:
			var a := TAU * float(k) / 8.0
			var hx := bface.x + 0.45 * cos(a)
			var hy := bface.z + 0.45 * sin(a)
			for yaw_deg in [0.0, 45.0, 90.0, 135.0, 180.0, 225.0, 270.0, 315.0]:
				sim.debug_teleport_local_player(Vector3(hx, -hy, base + 0.6), yaw_deg, 30.0)
				await _mission_wait(0.6)
				if String(sim.get_local_player_anim_key()).contains("climb"):
					blatched = true
					var blp: Vector3 = sim.get_local_player_position()
					print("PROBE BOTTOM LATCH: offset k=%d yaw %.0f -> %s at %s" %
							[k, yaw_deg, sim.get_local_player_anim_key(), str(blp)])
					break
			if blatched:
				break
		if not blatched:
			_fail("bottom entry never latched (from-below arm)")
			return
		# Static geometry census: every volume whose footprint spans the
		# latched column, tops between base-0.5 and base+3 — the candidate
		# "ground" surfaces a mid-span probe could hit.
		var col: Vector3 = sim.get_local_player_position()
		var cd0: Dictionary = sim.get_collision_debug()
		for inst in cd0.get("instances", []):
			for vd in inst.get("volumes", []):
				var cs0: PackedVector3Array = vd.get("corners", PackedVector3Array())
				if cs0.size() != 8:
					continue
				var vt0 := -INF
				var vb0 := INF
				var xmin0 := INF
				var xmax0 := -INF
				var zmin0 := INF
				var zmax0 := -INF
				for c in cs0:
					vt0 = maxf(vt0, c.y)
					vb0 = minf(vb0, c.y)
					xmin0 = minf(xmin0, c.x)
					xmax0 = maxf(xmax0, c.x)
					zmin0 = minf(zmin0, c.z)
					zmax0 = maxf(zmax0, c.z)
				if vt0 > base - 0.5 and vt0 < base + 3.0 \
						and col.x > xmin0 - 0.2 and col.x < xmax0 + 0.2 \
						and col.z > zmin0 - 0.2 and col.z < zmax0 + 0.2:
					print("PROBE COLUMN VOLUME: type %d z[%.2f..%.2f] x[%.2f..%.2f] zz[%.2f..%.2f] owner=%d" %
							[int(vd.get("type", -1)), vb0, vt0, xmin0, xmax0,
							zmin0, zmax0, int(inst.get("entity_handle", -1))])
		for i in 8:
			sim.add_local_player_look(0.0, -600.0)
			await process_frame
		_forward = true
		var bz_max: float = sim.get_local_player_position().y
		var drop_seen := false
		for i in 60:
			await _mission_wait(0.12)
			var lp2: Vector3 = sim.get_local_player_position()
			var akey: String = sim.get_local_player_anim_key()
			var drift := Vector2(lp2.x - bface.x, lp2.z - bface.z).length()
			var pd: Dictionary = sim.get_collision_debug().get("player", {})
			print("PROBE TICKLOG: z %.2f (max %.2f) anim %s drift %.2f in_air=%s clearance %.2f cb %.2f ph %d" %
					[lp2.y, bz_max, akey, drift,
					str(sim.get_local_player_body_debug().get("in_air", "?")),
					float(pd.get("foot_clearance", NAN)),
					float(pd.get("capsule_bottom", NAN)),
					sim.get_local_player_anim_phase_ticks()])
			bz_max = maxf(bz_max, lp2.y)
			if not akey.contains("climb") or lp2.y < bz_max - 0.4:
				drop_seen = true
				print("PROBE DROP: at z %.2f (reached %.2f of top %.1f) anim %s drift %.2f" %
						[lp2.y, bz_max, top, akey, drift])
				# What solid sits under the body? Every volume whose top face is
				# within 1.5u below the body and whose footprint spans it.
				var cd2: Dictionary = sim.get_collision_debug()
				for inst in cd2.get("instances", []):
					for vd in inst.get("volumes", []):
						var cs: PackedVector3Array = vd.get("corners", PackedVector3Array())
						if cs.size() != 8:
							continue
						var vt := -INF
						var vb := INF
						var xmin := INF
						var xmax := -INF
						var zmin := INF
						var zmax := -INF
						for c in cs:
							vt = maxf(vt, c.y)
							vb = minf(vb, c.y)
							xmin = minf(xmin, c.x)
							xmax = maxf(xmax, c.x)
							zmin = minf(zmin, c.z)
							zmax = maxf(zmax, c.z)
						if vt < lp2.y + 0.3 and vt > lp2.y - 1.5 \
								and lp2.x > xmin - 0.4 and lp2.x < xmax + 0.4 \
								and lp2.z > zmin - 0.4 and lp2.z < zmax + 0.4:
							print("PROBE UNDERFOOT: type %d top %.2f span x[%.1f..%.1f] z[%.1f..%.1f] owner=%d" %
									[int(vd.get("type", -1)), vt, xmin, xmax,
									zmin, zmax, int(inst.get("entity_handle", -1))])
				break
			if lp2.y >= top - 0.2:
				break
		_forward = false
		if drop_seen:
			print("PROBE BOTTOM RESULT: DROPPED before the top")
		else:
			print("PROBE BOTTOM RESULT: full span climbed %.2f -> %.2f" %
					[base + 0.6, bz_max])
		quit(0)
		return

	# --- Mount: drop in just ABOVE the anchor (anchorZ = top − 1.0), LOOKING
	# DOWN — the from-above entry arm has no facing requirement (heightDiff < 0
	# short-circuits the 60° gate; the pitch sign must agree). A slanted CL
	# prism only crosses the corner-average column near its top, so the top
	# band is also where contact is guaranteed. The latch detector is the
	# witnessed gravity skip: an idle latched body holds Z exactly — nothing
	# else holds a body mid-air.
	# Hover points: the prism column itself, then a ring of 0.45u offsets — a
	# thin rung-ladder prism embeds against its wall, and the wall's solid push
	# can slide the capsule off the column before fresh entry sees the contact;
	# the open-air side of the ring stays in front of the rungs. A real latch
	# stamps the climb clip family (anim_climb_*) — a Z-hold alone can be a
	# deck stand and is not proof.
	var face: Vector3 = ladder["top_xy"]
	var hovers: Array = [Vector2(face.x, face.z)]
	for k in 8:
		var a := TAU * float(k) / 8.0
		hovers.append(Vector2(face.x + 0.45 * cos(a), face.z + 0.45 * sin(a)))
	var latched := false
	var hold_z := 0.0
	for h in hovers:
		sim.debug_teleport_local_player(Vector3(h.x, -h.y, top - 0.3), 0.0, -30.0)
		await _mission_wait(0.8)
		var z0: float = sim.get_local_player_position().y
		await _mission_wait(0.5)
		var z1: float = sim.get_local_player_position().y
		var key: String = sim.get_local_player_anim_key()
		if absf(z1 - z0) < 0.05 and z1 > base and key.contains("climb"):
			latched = true
			hold_z = z1
			_shot_open_dir = Vector3(h.x - face.x, 0.0, h.y - face.z)
			var lp: Vector3 = sim.get_local_player_position()
			print("PROBE LATCHED: hover %s holds z %.2f as %s (volume %.1f..%.1f); body %s, %.2fu off the top-face column" %
					[str(h), z1, key, base, top, str(lp),
					Vector2(lp.x - face.x, lp.z - face.z).length()])
			break
		print("PROBE hover %s no latch (z %.2f -> %.2f, anim %s)" % [str(h), z0, z1, key])
	if not latched:
		_fail("no entry latched — the CL entry gate or the gravity skip is dead")
		return
	var shot1_z := hold_z
	if not _shots_dir.is_empty():
		presenter.set_debug_third_person(true)
		await _mission_wait(0.4)
		# The from-above latch lands one rung under the top — climb DOWN to
		# mid-ladder for the latch beat so the body hangs clear on the rungs
		# (the held height IS the mechanic: climb_idle, gravity off).
		for i in 8:
			sim.add_local_player_look(0.0, 600.0)
			await process_frame
		_forward = true
		await _mission_wait(0.8)
		_forward = false
		await _mission_wait(0.3)
		shot1_z = sim.get_local_player_position().y
		_frame_shot(sim, center, 0.6)
		await _shot("1_latched")

	# --- Climb: mouse-look UP (the forward fan picks climb_up by the look-pitch
	# sign), hold forward, and the climb clip's vertical lane must rise the body.
	for i in 8:
		sim.add_local_player_look(0.0, -600.0)
		await process_frame
	_forward = true
	var max_z := hold_z
	var mid_shot_taken := false
	var top_shot_taken := false
	for i in 30:
		await _mission_wait(0.1)
		max_z = maxf(max_z, sim.get_local_player_position().y)
		if not mid_shot_taken and max_z > shot1_z + 1.0:
			mid_shot_taken = true
			if _shot_cam != null:
				# Freeze mid-climb (climb_idle holds the height exactly) so the
				# body cannot outrun the framing at probe frame rates.
				_forward = false
				await _mission_wait(0.3)
				_frame_shot(sim, center)
				await _shot("2_climbing")
				_forward = true
		if mid_shot_taken and not top_shot_taken and \
				sim.get_local_player_position().y > top - 0.6:
			# Cresting — catch the body at the lip BEFORE the exit leg carries
			# it over (a freestanding wall has no floor beyond).
			top_shot_taken = true
			if _shot_cam != null:
				_forward = false
				await _mission_wait(0.25)
				_frame_shot(sim, center, 0.4)
				await _shot("3_top")
				_forward = true
	_forward = false
	if max_z <= hold_z + 0.4:
		# Diagnostic burst before failing: what did the motor actually stamp?
		_forward = true
		for i in 6:
			await _mission_wait(0.2)
			var lp: Vector3 = sim.get_local_player_position()
			print("PROBE STALL: anim=%s pos %s pitch %.1f yaw %.1f in_air=%s" %
					[sim.get_local_player_anim_key(), str(lp),
					sim.get_local_player_pitch_deg(), sim.get_local_player_yaw_deg(),
					str(sim.get_local_player_body_debug().get("in_air", "?"))])
		_forward = false
		_fail("forward + look-up did not climb (held %.2f, max %.2f)" % [hold_z, max_z])
		return
	print("PROBE CLIMB OK: %.2f -> %.2f (climb_up root motion)" % [hold_z, max_z])
	if _shot_cam != null and not top_shot_taken:
		_frame_shot(sim, center)
		await _shot("3_top")

	# --- Hold again: release the stick — climb_idle keeps the height (gravity
	# stays off while latched). A natural top-out above the volume falls instead,
	# so only assert the hold when still inside the span.
	await _mission_wait(0.5)
	var settle: float = sim.get_local_player_position().y
	if settle < top - 0.5:
		await _mission_wait(0.6)
		var settle2: float = sim.get_local_player_position().y
		if absf(settle2 - settle) > 0.3:
			_fail("height not held after the climb (%.2f -> %.2f)" % [settle, settle2])
			return
		print("PROBE HOLD OK at z %.2f (climb_idle, no gravity)" % settle2)
	else:
		print("PROBE topped out above the volume (z %.2f) — exit leg took over" % settle)
	print("PROBE PASS: CL entry + gravity-off hold + climb_up on 00TRa")
	quit(0)
