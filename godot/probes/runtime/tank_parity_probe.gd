extends GameProbe

## Observation reads the presenter's published snapshot, never
## Simulation.get_local_player_view(), whose compose advances shake filters.
var _ctx: ProbeContext


func run(ctx: ProbeContext) -> ProbeVerdict:
	_ctx = ctx
	if not await ctx.wait_for_local_player():
		return ProbeVerdict.failed("no local player")
	if _hull() == null:
		return ProbeVerdict.failed("requested vehicle is absent")
	var mouse_mode := Input.get_mouse_mode()
	ctx.defer_restore(func() -> void:
		for key in [KEY_W, KEY_S, KEY_A, KEY_D]:
			ProbeInput.hold(key, false)
		ProbeInput.mouse_btn(MOUSE_BUTTON_LEFT, false)
		Input.set_mouse_mode(mouse_mode))
	Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)
	if int(ctx.args.mount_handle) >= 0:
		if not await _board(int(ctx.args.mount_handle)):
			return ProbeVerdict.failed("assisted approach could not board the requested seat")
	if int(ctx.args.seat) >= 0:
		if not ctx.sim().local_player_select_seat(int(ctx.args.seat)):
			return ProbeVerdict.failed("requested panel seat is unavailable")
		await ctx.wait_ms(500)
	var scenario := String(ctx.args.scenario)
	if scenario != "observe" and DisplayServer.get_name() == "headless":
		return ProbeVerdict.failed("device exercises require a window")
	var duration := int(ctx.args.sample_ms)
	var path := ctx.artifact_dir.path_join("tank-frames.jsonl")
	var output := FileAccess.open(path, FileAccess.WRITE)
	if output == null:
		return ProbeVerdict.failed("could not open trace artifact")
	var start := Time.get_ticks_msec()
	var count := 0
	var stage := -1
	var last_tick := -1
	var ticks := 0
	var capture := bool(ctx.args.capture) and DisplayServer.get_name() != "headless"
	if capture:
		await ctx.capture_png("tank-before")
	while not ctx.cancelled and Time.get_ticks_msec() - start < duration:
		await ctx.tree.process_frame
		if ctx.sim() == null or ctx.presenter() == null or ctx.camera() == null:
			output.close()
			return ProbeVerdict.failed("mission disappeared")
		var next_stage := mini(7, int(8.0 * (Time.get_ticks_msec() - start) / duration))
		if next_stage != stage:
			stage = next_stage
			_exercise(scenario, stage)
		var row := _sample()
		row["elapsed_ms"] = Time.get_ticks_msec() - start
		row["stage"] = stage
		output.store_line(JSON.stringify(row))
		count += 1
		if row.tick != last_tick:
			ticks += 1
			last_tick = row.tick
		if count % 120 == 0:
			ctx.progress({"frames": count, "observed_ticks": ticks, "stage": stage})
	output.close()
	ctx.artifact("tank frames", path, "jsonl")
	if capture and not ctx.cancelled:
		await ctx.capture_png("tank-after")
	var result := {"frames": count, "observed_ticks": ticks,
			"scenario": scenario, "vehicle_net_id": ctx.args.vehicle_net_id,
			"vehicle_handle": ctx.args.vehicle_handle, "joiner": ctx.sim().is_joiner(),
			"assisted_approach": int(ctx.args.mount_handle) >= 0,
			"seat": ctx.args.seat, "runtime_version": Engine.get_version_info().string,
			"resource_dir": ctx.resource_root().get_root_dir(),
			"expansion": ctx.resource_root().get_expansion()}
	if ctx.cancelled or ticks < 2:
		return ProbeVerdict.failed("cancelled or simulation did not advance", result)
	return ProbeVerdict.passed("tank observation captured; compare trace and HUD with retail", result)


func _exercise(scenario: String, stage: int) -> void:
	if scenario == "zoom":
		ProbeInput.mouse_btn(MOUSE_BUTTON_WHEEL_UP if stage < 4 else MOUSE_BUTTON_WHEEL_DOWN, true)
	elif scenario == "fire":
		ProbeInput.mouse_btn(MOUSE_BUTTON_LEFT, stage % 2 == 0)
	elif scenario == "drive":
		for key in [KEY_W, KEY_S, KEY_A, KEY_D]:
			ProbeInput.hold(key, false)
		if stage < 2:
			ProbeInput.hold(KEY_W, true)
		elif stage < 4:
			ProbeInput.hold(KEY_S, true)
		elif stage < 6:
			ProbeInput.hold(KEY_A, true)
		else:
			ProbeInput.hold(KEY_D, true)
	Input.flush_buffered_events()


func _hull() -> EntityCard:
	# Replicated rows need not have authored mission SSNs. Use their actual
	# packed handle from game_entities when observing a joiner.
	var handle := int(_ctx.args.vehicle_handle)
	return _ctx.sim().entity_card(handle) if handle >= 0 else \
			_ctx.sim().entity_card_by_net_id(int(_ctx.args.vehicle_net_id))


func _sample() -> Dictionary:
	var sim := _ctx.sim()
	var camera := _ctx.camera()
	var view := _ctx.presenter().presented_view()
	var hull := _hull()
	var local := sim.entity_card(sim.get_local_player_wire_handle())
	var weapon := sim.get_local_player_weapon_state()
	# Attached guns have no mission net id. The equipped mount handle remains
	# valid across numbered seat changes and cannon/alternate-gun switches.
	var gun := sim.entity_card(weapon.usegun_mount_handle) \
			if weapon.borrowed_usegun_slot and weapon.usegun_mount_handle >= 0 else null
	if gun == null and local != null and local.is_mounted():
		gun = sim.entity_card_by_net_id(local.get_mount_target_net_id())
	var panel := sim.get_vehicle_panel_view()
	return {"frame": Engine.get_process_frames(), "tick": sim.get_logic_tick(),
			"camera_eye": _vec(camera.global_position),
			"camera_forward": _vec(-camera.global_basis.z),
			"camera_up": _vec(camera.global_basis.y),
			"view_angles": [view.camera_yaw_deg, view.camera_pitch_deg, view.camera_roll_deg] if view != null else [],
			"fov_h": view.fov_h_deg if view != null else 0.0,
			"magnification": view.scope_magnification if view != null else 0,
			"scope_card": view.scope_card_active if view != null else false,
			"hull": hull.to_json_value() if hull != null else {},
			"mount": gun.to_json_value() if gun != null else {},
			"weapon": sim.get_local_player_weapon_name(),
			"clip": weapon.clip, "reserve": weapon.reserve,
			"fired_serial": weapon.fired_serial, "heat": weapon.heat,
			"recoil_pitch_bam": weapon.recoil_pitch_bam,
			"panel_shown": panel.shown, "panel_item": panel.item_id,
			"posed_userpoints": _userpoints(gun.get_wire_handle()) if gun != null else {}}


func _userpoints(handle: int) -> Dictionary:
	var result := {}
	var model := _ctx.runtime().get_entity_presenter().resolve_wire_handle(handle) as ObjectModel
	if model == null or model.get_object_data() == null:
		return result
	var data := model.get_object_data()
	for i in data.get_user_point_count():
		var point := data.get_user_point_info(i)
		var part := model.get_render_part_nodes().get(point.subobject, model) as Node3D
		result[point.name] = {"position": _vec(part.to_global(point.position)),
				"direction": _vec(part.global_basis * point.rotation)}
	return result


func _board(handle: int) -> bool:
	# Explicitly assisted setup, recorded in the verdict. No teleport occurs
	# during observation, and boarding itself uses the ordinary USE request.
	for attempt in 2:
		var card := _ctx.sim().entity_card(handle)
		if card == null or card.get_seats().is_empty():
			_ctx.log("mount %d has no inspectable seat" % handle)
			return false
		var seat: EntityCardSeat = card.get_seats()[0]
		var model := _ctx.runtime().get_entity_presenter().resolve_wire_handle(handle) as ObjectModel
		if model == null:
			_ctx.log("mount %d has no presented model" % handle)
			return false
		var point := model.get_object_data().get_user_point_info(seat.get_bone_index() - 1)
		var part := model.get_render_part_nodes().get(point.subobject, model) as Node3D
		var target := part.to_global(point.position)
		var pos := Vector3(target.x + 1.5, -target.z, target.y)
		var eye_offset := _ctx.sim().get_local_player_eye_offset()
		var delta := Vector3(target.x, -target.z, target.y + 0.1875) - pos \
				- Vector3(eye_offset.x, -eye_offset.z, eye_offset.y)
		_ctx.sim().debug_teleport_local_player(pos, rad_to_deg(atan2(delta.x, delta.y)),
				rad_to_deg(atan2(delta.z, Vector2(delta.x, delta.y).length())))
		if attempt == 0:
			await _ctx.wait_ms(350)
	var boarded := _ctx.sim().local_player_toggle_mount()
	await _ctx.wait_ms(500)
	var view := _ctx.presenter().presented_view()
	return boarded and view != null and view.mounted


func _vec(v: Vector3) -> Array:
	return [v.x, v.y, v.z]
