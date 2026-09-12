extends GameProbe

## entity_pick: the debug pick -> F3 Entities selection path on the live
## process, plus the per-entity items.def attrib override the window's
## checkboxes drive. Opens the tools in Interact (the world click catcher comes
## up with them), finds an entity the engine's own pick ray confirms visible
## from where the local player stands, and lands the pick two ways: a synthetic
## left click at that entity's projected viewport-local point (the catcher's
## ray from the event's own position, the embedded-SubViewport fix) and the
## shell's crosshair pick (Shift+F6's seam, whatever the crosshair ray meets).
## Each landed pick must make DevTools.selected_entity_handle the entity the
## engine trace names. Then flips nodismember on the clicked entity through
## the Simulation seam the window's toggle drains into and game_debug's
## set_entity_item_attrib calls, reads it back off the entity card, restores
## it, and captures the workspace. Needs a window and a mission.

const SETTLE_FRAMES := 8
const CANDIDATE_LIMIT := 60
const NODISMEMBER := 0x800000

var _failures: Array[String] = []


func run(ctx: ProbeContext) -> ProbeVerdict:
	if not await ctx.wait_for_local_player():
		return ProbeVerdict.failed("no local player (mission load stalled or cancelled)")
	var main_game := ctx.game()
	var dev_tools := ctx.dev_tools()
	var sim := ctx.sim()
	var world := ctx.world()
	var camera := ctx.camera()
	var viewport := ctx.viewport()
	if main_game == null or dev_tools == null or sim == null or world == null \
			or camera == null or viewport == null:
		return ProbeVerdict.failed("the shell seams are unavailable")
	var tools_were_open := dev_tools.is_open()
	ctx.defer_restore(func() -> void:
		if is_instance_valid(dev_tools):
			dev_tools.select_entity(-1)
			dev_tools.set_open(tools_were_open))

	# The tools open in Interact: the world click catcher comes up with them.
	dev_tools.set_open(true)
	await ctx.wait_frames(SETTLE_FRAMES)
	_check(main_game.is_dev_tools_open() and not dev_tools.is_game_playing(),
			"the tools opened in Interact")
	_check(world.get_node_or_null("PickClickCatcher") != null,
			"Interact installs the world click catcher")

	# A visible, pickable target: the nearest directory row whose projected
	# point is on screen AND whose ray the engine trace resolves to that very
	# entity (occluders and off-screen rows fall out here, not in the checks).
	var target: EntityRow = null
	var screen_pos := Vector2.ZERO
	var rect := viewport.get_visible_rect()
	for row in _rows_by_distance(sim):
		var chest: Vector3 = row.get_world_position() + Vector3(0.0, 1.0, 0.0)
		if camera.is_position_behind(chest):
			continue
		var candidate_pos: Vector2 = camera.unproject_position(chest)
		if not rect.has_point(candidate_pos):
			continue
		var pick := DebugEntityPicker.pick_with_camera(
				sim, camera, candidate_pos, "probe", null)
		if pick.hit and pick.entity_handle == row.get_wire_handle():
			target = row
			screen_pos = candidate_pos
			break
	if target == null:
		return ProbeVerdict.failed("no directory row is visible and pickable from the spawn view")
	var handle := target.get_wire_handle()
	ctx.log("target: '%s' item '%s' ssn %d handle %d at %s in a %s viewport" % [
			target.get_name(), target.get_item_name(), target.get_net_id(), handle,
			str(screen_pos), str(rect.size)])

	# Leg 1: the synthetic click at the entity's projected viewport-local point.
	dev_tools.select_entity(-1)
	var press := InputEventMouseButton.new()
	press.button_index = MOUSE_BUTTON_LEFT
	press.pressed = true
	press.position = screen_pos
	press.global_position = screen_pos
	viewport.push_input(press, true)
	await ctx.wait_frames(2)
	var release := InputEventMouseButton.new()
	release.button_index = MOUSE_BUTTON_LEFT
	release.pressed = false
	release.position = screen_pos
	release.global_position = screen_pos
	viewport.push_input(release, true)
	ctx.log("after the click: selected handle %d" % dev_tools.selected_entity_handle())
	_check(dev_tools.selected_entity_handle() == handle,
			"a click at the entity's viewport-local point selects it in the Entities window")

	# Leg 2: the crosshair pick (Shift+F6's seam): whatever the centre ray
	# meets is what the window must select.
	dev_tools.select_entity(-1)
	var centre_pick := DebugEntityPicker.pick_at_crosshair(sim, camera, null)
	var centre_handle := centre_pick.entity_handle if centre_pick.hit else -1
	main_game.pick_at_crosshair()
	await ctx.wait_frames(2)
	ctx.log("crosshair ray: %s -> selected handle %d" % [
			centre_pick.name if centre_handle >= 0 else "no entity",
			dev_tools.selected_entity_handle()])
	if centre_handle >= 0:
		_check(dev_tools.selected_entity_handle() == centre_handle,
				"the crosshair pick selects exactly what the engine's centre ray meets")
	else:
		ctx.log("crosshair leg skipped: the centre ray met no entity from this view")

	# Re-select the clicked target so the capture shows its card, and let the
	# detail push land.
	dev_tools.select_entity(handle)
	await ctx.wait_frames(SETTLE_FRAMES)

	# The per-entity attrib override, through the seam the F3 toggle and the
	# MCP action share. On the authority it lands and reads back off the
	# entity card; on a joiner the same call must refuse outright (its rows
	# are replicas the wire re-writes) while picking stays allowed.
	var before: EntityCard = sim.entity_card(handle)
	if before == null:
		_check(false, "the entity card resolves for the picked handle")
	elif int(sim.session_role()) == Simulation.ROLE_JOINER:
		_check(int(sim.debug_set_entity_item_attrib(handle, 0, 0)) == ERR_UNAUTHORIZED,
				"a joiner's attrib override is refused (read-only peer)")
		ctx.log("joiner: set_entity_item_attrib refused as expected")
	else:
		var attrib := int(before.get_item_attrib())
		var attrib2 := int(before.get_item_attrib2())
		ctx.defer_restore(func() -> void:
			if is_instance_valid(sim):
				sim.debug_set_entity_item_attrib(handle, attrib, attrib2))
		var err: Error = sim.debug_set_entity_item_attrib(
				handle, attrib | NODISMEMBER, attrib2)
		_check(err == OK, "set_entity_item_attrib accepts the wire handle")
		await ctx.wait_frames(2)
		var after: EntityCard = sim.entity_card(handle)
		_check(after != null and (int(after.get_item_attrib()) & NODISMEMBER) != 0,
				"nodismember reads back set on the entity card")
		var card_json: Dictionary = after.to_json_value() if after != null else {}
		_check((int(card_json.get("item_attrib", 0)) & NODISMEMBER) != 0,
				"game_entities' inspect JSON shows the override")
		ctx.log("item_attrib 0x%08X -> 0x%08X (item '%s', health_max %d)" % [
				attrib, int(after.get_item_attrib()) if after != null else 0,
				after.get_item_name() if after != null else "",
				int(after.get_health_max()) if after != null else 0])

	await ctx.wait_frames(SETTLE_FRAMES)
	# The whole window: the workspace with the Entities window beside the game.
	await ctx.capture_png("workspace", ctx.tree.root)
	return _verdict()


## Presented directory rows with a def, nearest first (mission-space distance
## from the local player), capped so a dense mission stays cheap.
func _rows_by_distance(sim: Simulation) -> Array[EntityRow]:
	var player: Vector3 = sim.get_local_player_position()
	var rows: Array[EntityRow] = []
	for row_v in sim.entity_directory():
		var row := row_v as EntityRow
		if row == null or row.get_item_name().is_empty() or not row.is_alive():
			continue
		if row.get_net_id() <= 0 and row.get_ai_index() >= 0:
			continue  # the local player (net_id 0 with a brain)
		rows.append(row)
	rows.sort_custom(func(a: EntityRow, b: EntityRow) -> bool:
		return a.get_world_position().distance_squared_to(player) \
				< b.get_world_position().distance_squared_to(player))
	if rows.size() > CANDIDATE_LIMIT:
		rows.resize(CANDIDATE_LIMIT)
	return rows


func _check(condition: bool, message: String) -> void:
	if not condition:
		_failures.append(message)


func _verdict() -> ProbeVerdict:
	var data := {"failures": _failures.duplicate()}
	if _failures.is_empty():
		return ProbeVerdict.passed("the pick selects its Entities row and the attrib override lands", data)
	return ProbeVerdict.failed("%d entity-pick check(s) failed" % _failures.size(), data)
