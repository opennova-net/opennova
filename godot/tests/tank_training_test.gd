extends GutTest

# A landing-craft tank course from boarding to victory, minted after the retail
# 07TR tank training's authoring. The player's tank rides a landing craft
# afloat at the waterline, the player and the instructor on its deck (07TR's
# LCAC 37 and tank 33); a second craft crawls in from the sea carrying the other
# allied tank and lands beside it (LCAC 38 and tank 34: event 11 sends it in,
# event 12 slows it at its second stop). The player standing on the tank starts
# the lesson (event 15: PLYRONSSN 33, delay 2 -> RedirectGroupTo <instructor>
# 125 33); the instructor then drives the tank off the craft and along the
# course (event 16: PLYRONSSN 33 -> RedirectGroupTo + PATROLSPEED), its first
# stop releasing the other tank onto its own route (event 17); the tank's final
# firing position (event 49, GroupAtWaypoint) and the other tank's, which
# releases the APC wave (event 48), follow; destroying every enemy group meets
# the objective (GroupDestroyed x n, event 92) and a delayed event wins the round
# for blue (event 93: BlueWin, delay 8). Every vehicle has its crew aboard from
# the start (a board order, waypoint_id 125, on each crew record) but the
# player's tank, whose driver is the instructor. The APCs are PlayerControl
# vehicles, as the shipped BTR-80 is: a destroyed one respawns where it spawned,
# without limit when its record's spawn budget is 0 (07TR's are) [orig:
# AI_TickState_VehicleDead @0x467EA0 -> Entity_RespawnVehicle @0x45FF40]. The
# final APCs carry none and keep respawning behind the escarpment, out of the
# convoy's sight; the near APCs carry a budget of one, so a destroyed one stays
# down (07TR vaporizes its own 23 seconds after their deaths instead, events
# 79/80). Only
# positioning and aim use a debug seam; boarding, weapons, damage and BMS
# progression use the normal runtime. The vehicles are VehicleFixture's.
const MountLook := preload("res://tests/support/mount_look.gd")

# The course's sea level: the header's water override (half world units, as
# 07TR's water_level 24 puts its sea at 12) over the Tmap relief's west slope,
# which runs down to it at mission y 200.
const WATER_Z := 24.0
const WATER_OVERRIDE := 48
const HEADER_WATER_OVERRIDE := 0x1
# The lcac model's deck height over its keel.
const DECK_Z := 1.2
# The landing faces up the beach (mission -y), the enemy in the lowland down it
# toward the landing (+y), the enemy on the plateau west, down the escarpment.
const BOW_YAW := 180.0
const ENEMY_YAW := 0.0
const PLATEAU_YAW := -90.0
const CRAFT_AT := Vector2(-60.0, 214.0)
const SECOND_CRAFT_AT := Vector2(-100.0, 300.0)
const TANK_AT := Vector2(-60.0, 210.0)
const ALLY_AT := Vector2(-100.0, 296.0)
const PLAYER_AT := Vector2(-61.0, 219.0)
const INSTRUCTOR_AT := Vector2(-63.5, 212.5)
# The crews: each craft's pilot and the other tank's driver.
const CRAFT_CREW_AT := Vector2(-57.0, 207.0)
const SECOND_CRAFT_CREW_AT := Vector2(-97.0, 293.0)
const ALLY_CREW_AT := Vector2(-104.0, 301.0)
const SECOND_CRAFT_GROUP := 3
const TANK_GROUP := 5
const ALLY_GROUP := 6
const INSTRUCTOR_GROUP := 10
const LANDING_PATH := 2
const ALLY_PATH := 3
const COURSE_PATH := 4
# The second craft's run in: two stops at sea, the landing at the waterline
# a craft's length beside the first.
const LANDING := [Vector2(-110.0, 270.0), Vector2(-105.0, 240.0), Vector2(-100.0, 212.0)]
# 07TR's speeds: the craft in at 80 and on at 40 from its second stop, the
# player's tank at 23 and the other tank at 26, the APC wave at 20.
const LANDING_SPEEDS := [80, 40]
const COURSE_SPEED := 23
const ALLY_SPEED := 26
const APC_SPEED := 20
# Both tanks drive off the beach and up the slope inland to their firing
# positions (each route's last stop), at the foot of the Tmap relief's east
# escarpment.
const COURSE := [Vector2(-62.0, 192.0), Vector2(-50.0, 150.0), Vector2(-30.0, 100.0), Vector2(-20.0, 40.0)]
const ALLY_ROUTE := [Vector2(-102.0, 192.0), Vector2(-90.0, 150.0), Vector2(-75.0, 100.0), Vector2(-55.0, 55.0)]
# The enemy (red): two target tanks and each wave group's near APC wait in the
# lowland in sight of the firing positions; each wave group's final APC and a
# reserve APC start on the plateau beyond the escarpment, in reach but out of
# sight, and drive down it into sight when released. A destroyed final APC
# respawns up there, out of sight again (07TR's final APCs respawn behind
# terrain). The reserve drives in once the wave is down, after the final APCs
# have respawned (07TR holds back two more T80 groups the same way, events
# 56/57): it keeps the objective open past the final APCs' first deaths, as
# 07TR's late T80s do.
const ENEMY_TANKS := {18: Vector2(-90.0, -40.0), 19: Vector2(-40.0, -70.0)}
const APC_GROUPS := [20, 21]
const NEAR_APCS := [Vector2(-120.0, -10.0), Vector2(-70.0, -90.0)]
const FINAL_APCS := [Vector2(260.0, 70.0), Vector2(260.0, 20.0)]
const APC_ROUTES := [[Vector2(130.0, 60.0), Vector2(40.0, 50.0)],
		[Vector2(130.0, 15.0), Vector2(40.0, 10.0)]]
const APC_PATHS := [20, 21]
const RESERVE_GROUP := 22
const RESERVE_AT := Vector2(300.0, 120.0)
const RESERVE_ROUTE := [Vector2(130.0, 110.0), Vector2(30.0, 90.0)]
const RESERVE_PATH := 22
const RESERVE_DELAY := 20
# The near APCs' spawn budget (the record's movetimer: a budget of n respawns
# n - 1 times, 0 without limit) [orig: Game_StartMission @0x526079..0x52608F].
const NEAR_APC_SPAWNS := 1
# The deck approach: 07TR's spot off tank 33's cannon seat (three units mission
# +y of it, its tank facing -119: 1.45 behind the seat and 2.62 to its side),
# on this tank's roof-gun side, where both guns' seats sit inside the 4-unit
# label radius.
const DECK_APPROACH := Vector3(2.62, 1.45, 0.0)
const CRAFT_PROFILE := """type GROUND
subtype BOAT
default_state GROUND_FOLLOWWP
patrol_speed 80
combat_speed 80
drive_skill 4
turn_rate 20
accel_time 45
"""
# The vehicles keep to their routes in combat (the shipped D_5ton.aip's
# FOLLOW_WP, without its FLEE).
const DRIVE_PROFILE := """type GROUND
default_state GROUND_FOLLOWWP
evade_flags FOLLOW_WP
combat_flags FOLLOW_WP
patrol_speed 30
combat_speed 30
drive_skill 4
"""

var _presenter: LocalPlayerPresenter
var _world: GameWorld
var _cannon_handle := -1
var _launch_offset := Vector3.ZERO
var _camera: Camera3D
var _dir := ""
var _ids := {}
var _events := {}
var _enemies: Array[int] = []
var _finals: Array[int] = []


func after_each() -> void:
	if is_instance_valid(_presenter):
		_presenter.teardown()
	_presenter = null
	Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)
	if not _dir.is_empty():
		TestFs.remove_dir_recursive(_dir)
	_dir = ""
	_ids.clear()
	_events.clear()
	_enemies.clear()
	_finals.clear()


func _advance(sim: Simulation, ticks := 125) -> void:
	for i in ticks:
		sim.step()
		_presenter.after_world_tick()


func _sights() -> PackedStringArray:
	var textures := PackedStringArray()
	for row: WeaponSightRow in _world.local_player_hud_weapon_def().sights:
		textures.append(row.get_texture())
	return textures


func _fire_gun(sim: Simulation) -> int:
	var serial := sim.get_local_player_weapon_state().fired_serial
	sim.drain_fire_presentation_events()
	var ammo := -1
	var alignment := 1.0
	for tick in 125:
		sim.set_local_player_weapon_input(tick < 16, tick == 0, false)
		_advance(sim, 1)
		for shot: FirePresentationEvent in sim.drain_fire_presentation_events():
			if shot.get_is_local_player():
				ammo = shot.get_ammo_index()
				if sim.get_local_player_weapon_name() == "WPN_M1TURRET":
					_launch_offset = _camera.global_basis.inverse() * (
							shot.get_origin() - sim.entity_card(_cannon_handle).get_position())
				alignment = minf(alignment, shot.get_forward().dot(-_camera.global_basis.z))
	assert_gt(sim.get_local_player_weapon_state().fired_serial, serial, "the equipped tank gun fires")
	assert_gte(ammo, 0, "firing produces a real local projectile")
	assert_gt(alignment, 0.999, "the selected gun fires along its sight")
	return ammo


func _seat_target(handle: int, bone: int) -> Vector3:
	# Independently read the rendered live userpoint, not the native USE scorer.
	var frame := _presenter.before_world_tick(Simulation.tick_dt(), false, true)
	_world.tick(_camera.global_position, _camera.global_transform, Simulation.tick_dt(), frame)
	var model := _world.get_runtime().get_entity_presenter().resolve_wire_handle(handle) as ObjectModel
	var point := model.get_object_data().get_user_point_info(bone - 1)
	# Emplacement models can carry userpoints without a render part.
	var part := model.get_render_part_nodes().get(point.subobject, model) as Node3D
	var pos := part.to_global(point.position)
	return Vector3(pos.x, -pos.z, pos.y)


func _combat_target(sim: Simulation) -> EntityCard:
	var nearest: EntityCard
	var score := INF
	var hull := sim.entity_card_by_net_id(_ids["tank"])
	for ssn in _enemies:
		var card := sim.entity_card_by_net_id(ssn)
		if card == null or not card.has_ai() or not card.is_alive() or card.get_health() <= 0:
			continue
		var candidate := card.get_mission_position().distance_to(hull.get_mission_position())
		if candidate > 500.0:
			continue
		var aim := card.get_mission_position() + Vector3(0, 0, 0.5)
		var hit := _world.get_terrain_data().raycast_terrain(
				_camera.global_position, Vector3(aim.x, aim.z, -aim.y))
		if hit.is_finite():
			continue
		# Clear the tank groups before the later APC waves.
		if card.get_item_id() == VehicleFixture.TARGET_APC - 100000:
			candidate += 1000.0
		# The final APCs can respawn. Weaken both before finishing them
		# together so the course can enter its victory window.
		if card.get_net_id() in _finals:
			candidate += 10000.0
			if card.get_health() <= 6000:
				candidate += 1000.0
		if candidate < score:
			score = candidate
			nearest = card
	return nearest


func _fight_convoy(sim: Simulation) -> void:
	var serial := sim.get_local_player_weapon_state().fired_serial
	sim.drain_fire_presentation_events()
	var target: EntityCard
	var repositioned := false
	# A final APC seen destroyed and later alive again has respawned.
	var fallen := {}
	var respawned := false
	for tick in 37500:
		if sim.get_round_outcome_debug().get_ended():
			break
		if tick % 62 == 0:
			for ssn in _finals:
				var apc := sim.entity_card_by_net_id(ssn)
				if apc == null:
					continue
				if not apc.is_alive() or apc.get_health() <= 0:
					fallen[ssn] = true
				elif fallen.has(ssn):
					respawned = true
			target = _combat_target(sim)
			if target == null and not repositioned and sim.has_event_fired(_events["final_position"]) \
					and sim.has_event_fired(_events["release"]):
				var survivors: Array[EntityCard] = []
				var other_targets_alive := false
				for ssn in _enemies:
					var enemy := sim.entity_card_by_net_id(ssn)
					if enemy == null or not enemy.is_alive() or enemy.get_health() <= 0:
						continue
					if ssn in _finals:
						survivors.append(enemy)
					else:
						other_targets_alive = true
				if not other_targets_alive and not survivors.is_empty():
					# The final APCs respawn where they spawned, out of the
					# stopped convoy's reach. Preserve the authored drive, then
					# position the combat fixture in sight of the survivors.
					# Damage and victory remain normal.
					var position := survivors[0].get_mission_position() + Vector3(30, -40, 0)
					position.z = _world.get_terrain_data().get_height_world_bilinear(Vector3(position.x, 0, -position.y)) + 0.5
					var hull := sim.entity_card_by_net_id(_ids["tank"])
					assert_eq(sim.debug_set_entity_position(hull.get_ai_index(), position), OK)
					repositioned = true
					gut.p("Combat fixture: advance the tank to the respawned APCs after the authored route.")
		elif target != null:
			target = sim.entity_card_by_net_id(target.get_net_id())
		var aligned := false
		if target != null and target.is_alive() and target.get_health() > 0:
			var aim := target.get_mission_position() + Vector3(0, 0, 0.5)
			# Use the observed native muzzle, and correct the view incrementally:
			# mounted pitch is relative to the hull, which can be on a slope.
			var launch := sim.entity_card(_cannon_handle).get_position() + _camera.global_basis * _launch_offset
			var direction := (Vector3(aim.x, aim.z, -aim.y) - launch).normalized()
			var forward := -_camera.global_basis.z
			var yaw_error := wrapf(rad_to_deg(atan2(direction.x, -direction.z) - atan2(forward.x, -forward.z)), -180.0, 180.0)
			var pitch_error := rad_to_deg(asin(direction.y) - asin(forward.y))
			sim.debug_teleport_local_player(MountLook.local_player_mission_position(sim),
					sim.get_local_player_yaw_deg() + yaw_error * 0.5,
					sim.get_local_player_pitch_deg() + pitch_error * 0.5)
			aligned = direction.dot(forward) > 0.99999
		# Keep tracking moving targets and press again when the cannon reloads.
		sim.set_local_player_weapon_input(aligned and tick % 12 < 6,
				aligned and tick % 12 == 0, false)
		_advance(sim, 1)
		for shot: FirePresentationEvent in sim.drain_fire_presentation_events():
			if shot.get_is_local_player():
				_launch_offset = _camera.global_basis.inverse() * (
						shot.get_origin() - sim.entity_card(_cannon_handle).get_position())
	sim.set_local_player_weapon_input(false, false, false)
	assert_gt(sim.get_local_player_weapon_state().fired_serial, serial, "combat fires real cannon rounds")
	assert_true(respawned, "a destroyed final APC respawns")
	assert_true(repositioned, "the final APCs respawn out of the stopped convoy's reach")
	_advance(sim, 1250)
	assert_true(sim.has_event_fired(_events["objective"]), "every enemy group satisfies the destruction objective")
	assert_true(sim.has_event_fired(_events["victory"]), "the authored delayed victory event fires")
	assert_true(sim.get_round_outcome_debug().get_ended(), "tank training ends in victory")
	assert_eq(sim.get_round_outcome_debug().get_winner_team(), 1)


func _ground(terrain: TerrainData, at: Vector2) -> Vector3:
	return Vector3(at.x, at.y, VehicleFixture.ground_z(terrain, at.x, at.y))


func _deck(at: Vector2) -> Vector3:
	return Vector3(at.x, at.y, WATER_Z + DECK_Z)


# Waypoint markers (type 6005) as path `path`'s stops, in order.
func _path(mission: MissionData, terrain: TerrainData, path: int, stops: Array) -> void:
	for i in stops.size():
		var marker := mission.add_entity(MissionData.KIND_MARKER, 106005, _ground(terrain, stops[i]), Vector3.ZERO)
		assert_true(mission.set_entity_property_int(MissionData.KIND_MARKER, marker.index, "waypoint_id", path))
		assert_true(mission.set_entity_property_int(MissionData.KIND_MARKER, marker.index, "wp_number", i + 1))


func _set_fields(mission: MissionData, entity: EntityRef, kind: int, fields: Dictionary) -> void:
	for field: String in fields:
		assert_true(mission.set_entity_property_int(kind, entity.index, field, int(fields[field])),
				"%s on %d" % [field, entity.bms_id])


func _vehicle(mission: MissionData, item: int, at: Vector3, fields: Dictionary, profile: String,
		yaw := BOW_YAW) -> EntityRef:
	var vehicle := mission.add_entity(MissionData.KIND_ITEM, item, at, Vector3(0, yaw, 0))
	_set_fields(mission, vehicle, MissionData.KIND_ITEM, fields)
	assert_true(mission.set_entity_property_string(MissionData.KIND_ITEM, vehicle.index, "name2", profile))
	return vehicle


# A crew member with a board order on `vehicle` (waypoint_id 125, "Goto SSN and
# board"), as 07TR's crew records carry.
func _crew(mission: MissionData, at: Vector3, team: int, vehicle: EntityRef) -> void:
	var crew := mission.add_entity(MissionData.KIND_ORGANIC, VehicleFixture.RIDER, at, Vector3.ZERO)
	_set_fields(mission, crew, MissionData.KIND_ORGANIC,
			{"team": team, "waypoint_id": 125, "wp_number": vehicle.bms_id})


func _build_course(terrain: TerrainData, root: ResourceRoot) -> void:
	var mission := MissionData.new()
	assert_eq(mission.open_from_resource_root(root, WorldFixture.MINIMAL_MISSION), OK)
	assert_true(mission.set_header_int("water_override", WATER_OVERRIDE))
	assert_true(mission.set_header_flag(HEADER_WATER_OVERRIDE, true))
	# The player starts on the first craft's deck.
	for marker: EntityRef in mission.get_entity_refs(MissionData.KIND_MARKER):
		if marker.item_id == 106001:
			assert_true(mission.set_entity_transform(MissionData.KIND_MARKER, marker.index,
					_deck(PLAYER_AT), Vector3(0, BOW_YAW, 0)))
	# The craft ride afloat, the sea floor under both.
	for at: Vector2 in [CRAFT_AT, SECOND_CRAFT_AT]:
		assert_lt(_ground(terrain, at).z, WATER_Z, "the landing craft is authored over the sea")
	var craft := _vehicle(mission, VehicleFixture.LCAC, Vector3(CRAFT_AT.x, CRAFT_AT.y, WATER_Z),
			{"team": 1}, "lcac")
	var second_craft := _vehicle(mission, VehicleFixture.LCAC,
			Vector3(SECOND_CRAFT_AT.x, SECOND_CRAFT_AT.y, WATER_Z),
			{"team": 1, "group": SECOND_CRAFT_GROUP}, "lcac")
	var tank := _vehicle(mission, VehicleFixture.TANK, _deck(TANK_AT), {"team": 1, "group": TANK_GROUP}, "tdrive")
	var ally := _vehicle(mission, VehicleFixture.TANK, _deck(ALLY_AT), {"team": 1, "group": ALLY_GROUP}, "tdrive")
	_ids["craft"] = craft.bms_id
	_ids["second_craft"] = second_craft.bms_id
	_ids["tank"] = tank.bms_id
	_ids["ally"] = ally.bms_id
	_crew(mission, _deck(CRAFT_CREW_AT), 1, craft)
	_crew(mission, _deck(SECOND_CRAFT_CREW_AT), 1, second_craft)
	_crew(mission, _deck(ALLY_CREW_AT), 1, ally)
	# The instructor waits on the deck beside the tank for the lesson's board order.
	var instructor := mission.add_entity(MissionData.KIND_ORGANIC, VehicleFixture.RIDER,
			_deck(INSTRUCTOR_AT), Vector3.ZERO)
	_set_fields(mission, instructor, MissionData.KIND_ORGANIC, {"group": INSTRUCTOR_GROUP, "team": 1})
	_ids["instructor"] = instructor.bms_id
	for group: int in ENEMY_TANKS:
		var enemy := _vehicle(mission, VehicleFixture.TARGET_TANK, _ground(terrain, ENEMY_TANKS[group]),
				{"group": group, "team": 2}, "tdrive", ENEMY_YAW)
		_enemies.append(enemy.bms_id)
	# The near APCs wait uncrewed; the final APCs and the reserve have their
	# drivers aboard.
	var movers: Array[EntityRef] = []
	for i in APC_GROUPS.size():
		var near := _vehicle(mission, VehicleFixture.TARGET_APC, _ground(terrain, NEAR_APCS[i]),
				{"group": APC_GROUPS[i], "team": 2, "spawn_count": NEAR_APC_SPAWNS}, "tdrive", ENEMY_YAW)
		_enemies.append(near.bms_id)
		var final_apc := _vehicle(mission, VehicleFixture.TARGET_APC, _ground(terrain, FINAL_APCS[i]),
				{"group": APC_GROUPS[i], "team": 2}, "tdrive", PLATEAU_YAW)
		_crew(mission, _ground(terrain, FINAL_APCS[i] + Vector2(0.0, 4.0)), 2, final_apc)
		_enemies.append(final_apc.bms_id)
		_finals.append(final_apc.bms_id)
		movers.append(final_apc)
	var reserve := _vehicle(mission, VehicleFixture.TARGET_APC, _ground(terrain, RESERVE_AT),
			{"group": RESERVE_GROUP, "team": 2, "spawn_count": NEAR_APC_SPAWNS}, "tdrive", PLATEAU_YAW)
	_crew(mission, _ground(terrain, RESERVE_AT + Vector2(0.0, 4.0)), 2, reserve)
	_enemies.append(reserve.bms_id)
	movers.append(reserve)
	_path(mission, terrain, LANDING_PATH, LANDING)
	_path(mission, terrain, COURSE_PATH, COURSE)
	_path(mission, terrain, ALLY_PATH, ALLY_ROUTE)
	for i in APC_PATHS.size():
		_path(mission, terrain, APC_PATHS[i], APC_ROUTES[i])
	_path(mission, terrain, RESERVE_PATH, RESERVE_ROUTE)
	# A marker removal lays every path out again from its waypoint markers, as
	# the original editor's writer does (bms_edit lay_out_waypoint_path, D-MIS-6).
	var scratch := mission.add_entity(MissionData.KIND_MARKER, 106005, Vector3.ZERO, Vector3.ZERO)
	assert_true(mission.remove_entity(MissionData.KIND_MARKER, scratch.index))

	# The second craft's run in, from the start (07TR's event 11), slowing at
	# its second stop (event 12; GroupAtWaypoint's node is the stop's place on
	# the path, from 0) and halting at its landing. 07TR's paths are one-shot
	# (the DoesNotLoop flag, which MissionData does not edit); a group halts
	# at a path's end on a zero patrol speed instead, 07TR's own halt for its
	# convoy (event 27: ChangeGroupAI PATROLSPEED 0).
	var launch := mission.add_event(0, 0, 0)
	assert_true(mission.add_event_action(launch, 1, 0, SECOND_CRAFT_GROUP, LANDING_PATH, 0), "the second craft runs in")
	assert_true(mission.add_event_action(launch, 3, 30, SECOND_CRAFT_GROUP, LANDING_SPEEDS[0]), "at its patrol speed")
	var slow := mission.add_event(0, 0, 0)
	assert_true(mission.add_event_trigger(slow, 1, 7, SECOND_CRAFT_GROUP, LANDING_PATH, 1))
	assert_true(mission.add_event_action(slow, 3, 30, SECOND_CRAFT_GROUP, LANDING_SPEEDS[1]))
	var landed := mission.add_event(0, 0, 0)
	assert_true(mission.add_event_trigger(landed, 1, 7, SECOND_CRAFT_GROUP, LANDING_PATH, LANDING.size() - 1))
	assert_true(mission.add_event_action(landed, 3, 30, SECOND_CRAFT_GROUP, 0), "the second craft lands")
	# The lesson: the player standing on the tank orders the instructor aboard.
	var lesson := mission.add_event(0, 0, 2)
	assert_true(mission.add_event_trigger(lesson, 7, 39, tank.bms_id), "PLYRONSSN of the tank")
	assert_true(mission.add_event_action(lesson, 1, 0, INSTRUCTOR_GROUP, 125, tank.bms_id),
			"the instructor's group boards the tank")
	_events["lesson"] = lesson
	# The drive, once the instructor is aboard.
	var drive := mission.add_event(0, 0, 8)
	assert_true(mission.add_event_trigger(drive, 3, 0, lesson))
	assert_true(mission.add_event_trigger(drive, 7, 39, tank.bms_id))
	assert_true(mission.add_event_action(drive, 1, 0, TANK_GROUP, COURSE_PATH, 0), "the tank drives the course")
	assert_true(mission.add_event_action(drive, 3, 30, TANK_GROUP, COURSE_SPEED), "at its patrol speed")
	_events["drive"] = drive
	# The tank's first stop releases the other tank onto its route.
	var ally_drive := mission.add_event(0, 0, 0)
	assert_true(mission.add_event_trigger(ally_drive, 1, 7, TANK_GROUP, COURSE_PATH, 0))
	assert_true(mission.add_event_action(ally_drive, 1, 0, ALLY_GROUP, ALLY_PATH, 0), "the other tank drives in")
	assert_true(mission.add_event_action(ally_drive, 3, 30, ALLY_GROUP, ALLY_SPEED))
	# The tank's final firing position, then the other tank's, which releases
	# the APC wave.
	var final_position := mission.add_event(0, 0, 0)
	assert_true(mission.add_event_trigger(final_position, 1, 7, TANK_GROUP, COURSE_PATH, COURSE.size() - 1))
	assert_true(mission.add_event_action(final_position, 3, 30, TANK_GROUP, 0), "the tank halts")
	_events["final_position"] = final_position
	var release := mission.add_event(0, 0, 0)
	assert_true(mission.add_event_trigger(release, 1, 7, ALLY_GROUP, ALLY_PATH, ALLY_ROUTE.size() - 1))
	assert_true(mission.add_event_action(release, 3, 30, ALLY_GROUP, 0), "the other tank halts")
	for i in APC_GROUPS.size():
		assert_true(mission.add_event_action(release, 1, 0, APC_GROUPS[i], APC_PATHS[i], 0), "the APC wave drives in")
		assert_true(mission.add_event_action(release, 3, 30, APC_GROUPS[i], APC_SPEED))
	_events["release"] = release
	# Each APC halts at its route's end (SingleAtWaypoint -> ChangeSingleAI
	# PATROLSPEED 0), as a one-shot path leaves it.
	var routes := APC_ROUTES.duplicate()
	routes.append(RESERVE_ROUTE)
	var paths := APC_PATHS.duplicate()
	paths.append(RESERVE_PATH)
	for i in movers.size():
		var halt := mission.add_event(0, 0, 0)
		assert_true(mission.add_event_trigger(halt, 2, 7, movers[i].bms_id, paths[i], routes[i].size() - 1))
		assert_true(mission.add_event_action(halt, 21, 30, movers[i].bms_id, 0))
	# The reserve drives in once the wave is down.
	var reserve_drive := mission.add_event(0, 0, RESERVE_DELAY)
	for group: int in APC_GROUPS:
		assert_true(mission.add_event_trigger(reserve_drive, 1, 4, group))
	assert_true(mission.add_event_action(reserve_drive, 1, 0, RESERVE_GROUP, RESERVE_PATH, 0), "the reserve drives in")
	assert_true(mission.add_event_action(reserve_drive, 3, 30, RESERVE_GROUP, APC_SPEED))
	# The objective and the delayed victory.
	var groups := ENEMY_TANKS.keys()
	groups.append_array(APC_GROUPS)
	groups.append(RESERVE_GROUP)
	var objective := mission.add_event(0, 0, 0)
	for group: int in groups:
		assert_true(mission.add_event_trigger(objective, 1, 4, group), "GroupDestroyed %d" % group)
	assert_true(mission.add_event_action(objective, 14, 0, 3))
	_events["objective"] = objective
	var victory := mission.add_event(0, 0, 8)
	for group: int in groups:
		assert_true(mission.add_event_trigger(victory, 1, 4, group))
	assert_true(mission.add_event_trigger(victory, 3, 0, objective))
	assert_true(mission.add_event_action(victory, 8), "BlueWin")
	_events["victory"] = victory
	assert_eq(mission.save_as(_dir.path_join("course.bms")), OK)


func _boot_training() -> ResourceRoot:
	_dir = VehicleFixture.stage_world(self, "tank_training")
	VehicleFixture.stage_infantry_clips(_dir)
	assert_eq(DirAccess.copy_absolute(ProjectSettings.globalize_path(
			VehicleFixture.SYNTH_MODELS + "/person.3di"), _dir.path_join("US01.3di")), OK)
	WorldFixture.write_file(_dir.path_join("tdrive.aip"), TestFs.crlf(DRIVE_PROFILE))
	WorldFixture.write_file(_dir.path_join("lcac.aip"), TestFs.crlf(CRAFT_PROFILE))
	var terrain := VehicleFixture.terrain(self, _dir)
	var root := VehicleFixture.root_over(self, _dir)
	_build_course(terrain, root)
	_world = WorldFixture.make_world(self)
	var load_error := WorldFixture.load_mission(_world, _dir, "course.bms")
	assert_eq(load_error, OK)
	if load_error != OK:
		return null
	_world.set_process(false)
	_camera = Camera3D.new()
	add_child_autofree(_camera)
	_camera.make_current()
	_presenter = LocalPlayerPresenter.new()
	add_child_autofree(_presenter)
	_presenter.setup(_world, _camera, null, ControlsModel.new())
	return _world.get_resource_root()


func test_tank_course_from_boarding_to_victory() -> void:
	var root := _boot_training()
	if root == null:
		return
	var sim := _world.get_sim()
	for key: String in ["craft", "second_craft"]:
		assert_gt(sim.entity_card_by_net_id(_ids[key]).get_mission_position().z, WATER_Z - 0.5,
				"landing craft starts afloat before its crew boards")
	_advance(sim)
	_await_second_landing(sim)
	var tank := sim.entity_card_by_net_id(_ids["tank"])
	assert_not_null(tank)
	if tank == null:
		return
	var cannon := _tank_cannon(sim, tank)
	assert_not_null(cannon)
	if cannon == null:
		return
	var seat: EntityCardSeat = cannon.get_seats()[0]
	var target := _seat_target(cannon.get_wire_handle(), seat.get_bone_index())
	_cannon_handle = cannon.get_wire_handle()
	MountLook.face(self, sim, target, target + Vector3(1.5, 0, 0))
	_advance(sim, 18) # refresh USE's proximity list after positioning
	target = _seat_target(cannon.get_wire_handle(), seat.get_bone_index())
	MountLook.face(self, sim, target, target + Vector3(1.5, 0, 0))
	var boarded := sim.local_player_toggle_mount()
	assert_true(boarded, "USE boards the live cannon seat on the tank")
	if not boarded:
		return
	_advance(sim)
	assert_eq(sim.get_local_player_weapon_name(), "WPN_M1TURRET")
	for tick in 1250:
		if sim.has_event_fired(_events["lesson"]):
			break
		_advance(sim, 1)
	assert_true(sim.has_event_fired(_events["lesson"]), "boarding starts the instructor within 20 seconds")
	_advance(sim)
	# Fire clear of the instructor and the landing craft after the turret settles.
	MountLook.face(self, sim, sim.entity_card_by_net_id(_ids["tank"]).get_mission_position() + Vector3(0, -150, 45))
	_advance(sim)
	_fire_gun(sim)
	# Only an authored G attachment switches slots; this tank, like the stock
	# M1A1, authors addeweap + addeweapC (mounted_weapon_switch covers G).
	var definitions := AuthoredItemFixture.read_rows(root)
	for attachment: Dictionary in definitions[VehicleFixture.TANK]["attachments"]:
		assert_ne(attachment["kind"], "addeweapg", "the tank authors no G attachment")
	if get_fail_count() > 0:
		return
	var start := sim.entity_card_by_net_id(_ids["tank"]).get_mission_position()
	_advance(sim, 3750)
	var instructor := sim.entity_card_by_net_id(_ids["instructor"])
	assert_true(instructor.is_mounted(), "the instructor boarded the tank")
	assert_gt(sim.entity_card_by_net_id(_ids["tank"]).get_mission_position().distance_to(start), 15.0,
			"the instructor drives the tank along the course")
	_fight_convoy(sim)
	assert_true(sim.has_event_fired(_events["final_position"]), "the convoy reaches its final firing position")
	assert_true(sim.has_event_fired(_events["release"]), "the other allied tank releases the APC wave")
	_presenter.teardown()
	_presenter = null
	_world.unload()


# Board once the second landing craft has put its tank on the beach. Boarding
# at once starts the instructor's chain early enough that the other tank is
# released while its craft still crawls in over deep water (07TR's LCAC 38 and
# tank 34); a player who boards after the landing gets the deterministic course.
func _await_second_landing(sim: Simulation) -> void:
	var craft := sim.entity_card_by_net_id(_ids["second_craft"])
	assert_not_null(craft)
	if craft == null:
		return
	var start := craft.get_mission_position()
	var last := start
	var still := 0
	for step in 80:
		_advance(sim, 62)
		var now := sim.entity_card_by_net_id(_ids["second_craft"]).get_mission_position()
		still = still + 1 if now.distance_to(last) < 0.05 else 0
		last = now
		if still >= 2 and now.distance_to(start) > 10.0:
			assert_lt(sim.entity_card_by_net_id(_ids["ally"]).get_mission_position().distance_to(now), 8.0,
					"the second craft lands carrying its tank")
			return
	fail_test("the second landing craft lands within 80 seconds")


func _tank_cannon(sim: Simulation, tank: EntityCard) -> EntityCard:
	var snapshot := sim.get_present_snapshot()
	for base in range(0, snapshot.size(), sim.get_present_stride()):
		var card := sim.entity_card(int(snapshot[base + Simulation.PF_WIRE_HANDLE]))
		if card != null and card.get_item_id() == VehicleFixture.TANK_TURRET - 100000 \
				and card.get_mission_position().distance_to(tank.get_mission_position()) < 10.0:
			return card
	return null


# The labels over the frame the last _advance presented (its after_world_tick
# is that tick's one camera compose).
func _visible_attach_texts(hud: HudOverlay, gametext: RtxtStringFile) -> Array[String]:
	hud.set_attach_labels(_camera.global_transform, _presenter.screen_projection(), gametext, _world.get_sim())
	var texts: Array[String] = []
	for i in hud.get_attach_label_count():
		if hud.get_viewport().get_visible_rect().has_point(hud.get_attach_label_position(i)):
			texts.append(hud.get_attach_label_text(i))
	return texts


func _use(sim: Simulation) -> void:
	if DisplayServer.get_name() == "headless":
		assert_true(sim.local_player_toggle_mount())
		return
	Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)
	assert_eq(Input.get_mouse_mode(), Input.MOUSE_MODE_CAPTURED)
	for pressed: bool in [true, false]:
		var event := InputEventKey.new()
		event.keycode = KEY_SHIFT
		event.physical_keycode = KEY_SHIFT
		event.pressed = pressed
		Input.parse_input_event(event)
		Input.flush_buffered_events()
		_presenter.before_world_tick(Simulation.tick_dt(), false, true)


func test_cannon_prompt_from_the_landing_craft_deck() -> void:
	var root := _boot_training()
	if root == null:
		return
	var sim := _world.get_sim()
	_advance(sim)
	var cannon := _tank_cannon(sim, sim.entity_card_by_net_id(_ids["tank"]))
	assert_not_null(cannon)
	if cannon == null:
		return
	var seat: EntityCardSeat = cannon.get_seats()[0]
	assert_false(seat.is_occupied(), "the player's training cannon is free")
	var target := _seat_target(cannon.get_wire_handle(), seat.get_bone_index())
	var approach := target + DECK_APPROACH
	# Approach at the player's walkable craft-deck height and let normal
	# collision settle the position.
	approach.z = MountLook.local_player_mission_position(sim).z
	MountLook.face(self, sim, target, approach)
	_advance(sim, 32)
	target = _seat_target(cannon.get_wire_handle(), seat.get_bone_index())
	var gametext := RtxtStringFile.new()
	assert_eq(gametext.load_from_byte_array(root.read_file("gametext.bin")), OK)
	var hud := HudOverlay.new()
	add_child_autofree(hud)
	var here := MountLook.local_player_mission_position(sim)
	var yaw := rad_to_deg(atan2(target.x - here.x, target.y - here.y))
	# Start just above the cannon seat's view-frustum edge, keeping the higher
	# roof gun in view.
	MountLook.face(self, sim, target)
	var half_fov := rad_to_deg(atan(1.0 / _presenter.view_projection().y.y))
	var upper_pitch := sim.get_local_player_pitch_deg() + half_fov + 5.0
	assert_eq(sim.debug_teleport_local_player(here, yaw, upper_pitch), OK)
	_advance(sim, 1)
	var texts := _visible_attach_texts(hud, gametext)
	gut.p("Upper-turret view attachment labels: " + str(texts))
	assert_has(texts, VehicleFixture.LABEL_50CAL, "the upper gun remains in view")
	assert_does_not_have(texts, VehicleFixture.LABEL_CANNON, "the lower cannon anchor is below this view")
	# Re-aim at the cannon from the SAME standing position. This catches an
	# empty/missing HUD label as well as a label/USE selection disagreement.
	MountLook.face(self, sim, target)
	_advance(sim, 1)
	texts = _visible_attach_texts(hud, gametext)
	gut.p("Cannon view attachment labels: " + str(texts))
	assert_has(texts, VehicleFixture.LABEL_CANNON)
	assert_gte(hud.get_attach_label_selected(), 0)
	assert_eq(hud.get_attach_label_text(hud.get_attach_label_selected()), VehicleFixture.LABEL_CANNON,
			"looking at the visible cannon prompt selects its seat")
	_use(sim)
	_advance(sim)
	assert_eq(sim.get_local_player_weapon_name(), "WPN_M1TURRET",
			"USE boards the cannon advertised by the HUD")
	for tick in 1250:
		if sim.has_event_fired(_events["lesson"]):
			break
		_advance(sim, 1)
	assert_true(sim.has_event_fired(_events["lesson"]), "boarding the visible cannon prompt starts the lesson")
