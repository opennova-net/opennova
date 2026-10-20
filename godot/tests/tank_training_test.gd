extends GutTest

# A tank course from boarding to victory, minted after the retail 07TR tank
# training's authoring: the player boards the cannon of a parked tank, which
# starts the lesson (the instructor's group is ordered aboard, 07TR's event
# 15: RedirectGroupTo <group> 125 <tank>); the instructor
# then drives the tank along the course (RedirectGroupTo + PATROLSPEED); the
# tank reaching its final firing position (GroupAtWaypoint, 07TR's event 49)
# releases the APC wave onto its own route (07TR's event 48); destroying every
# enemy group meets the objective (GroupDestroyed x n, event 92) and a delayed
# event then wins the round for blue (event 93: BlueWin, delay 8). Only
# positioning and aim use a debug seam; boarding, weapons, damage and BMS
# progression use the normal runtime. The vehicles are VehicleFixture's (the
# minted tank_turret hull with its cannon and roof .50 cal children, low-hp
# course targets); the retail witness also drove landing craft onto the beach,
# which this course does not author.
const MountLook := preload("res://tests/support/mount_look.gd")

const TANK_AT := Vector2(-10.0, -20.0)
const TANK_YAW := -90.0 # faces mission -x
const TANK_GROUP := 1
const INSTRUCTOR_GROUP := 10
const COURSE_PATH := 1
const APC_PATH := 2
# The course runs west of the parking place over the Tmap relief's open
# gentle slope; the targets wait beyond it, within the final position's view.
const COURSE := [Vector2(-50.0, -20.0), Vector2(-90.0, -10.0), Vector2(-120.0, -10.0)]
const APC_START := [Vector2(-240.0, 70.0), Vector2(-205.0, 95.0)]
const APC_ROUTE := [Vector2(-200.0, 40.0), Vector2(-180.0, 20.0)]
# Enemy groups (red): two tank platoons in place and the APC wave.
const ENEMY_TANKS := {2: [Vector2(-170.0, -30.0), Vector2(-175.0, 10.0)], 3: [Vector2(-165.0, 40.0)]}
const APC_GROUP := 4
const COURSE_SPEED := 20
const DRIVE_PROFILE := """type GROUND
default_state GROUND_FOLLOWWP
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
		if candidate < score:
			score = candidate
			nearest = card
	return nearest


func _fight_convoy(sim: Simulation) -> void:
	var serial := sim.get_local_player_weapon_state().fired_serial
	sim.drain_fire_presentation_events()
	var target: EntityCard
	for tick in 37500:
		if sim.get_round_outcome_debug().get_ended():
			break
		if tick % 62 == 0:
			target = _combat_target(sim)
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
	_advance(sim, 1250)
	assert_true(sim.has_event_fired(_events["objective"]), "every enemy group satisfies the destruction objective")
	assert_true(sim.has_event_fired(_events["victory"]), "the authored delayed victory event fires")
	assert_true(sim.get_round_outcome_debug().get_ended(), "tank training ends in victory")
	assert_eq(sim.get_round_outcome_debug().get_winner_team(), 1)


func _ground(terrain: TerrainData, at: Vector2) -> Vector3:
	return Vector3(at.x, at.y, VehicleFixture.ground_z(terrain, at.x, at.y))


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


func _build_course(terrain: TerrainData, root: ResourceRoot) -> void:
	var mission := MissionData.new()
	assert_eq(mission.open_from_resource_root(root, WorldFixture.MINIMAL_MISSION), OK)
	var tank := mission.add_entity(MissionData.KIND_ITEM, VehicleFixture.TANK,
			_ground(terrain, TANK_AT), Vector3(0, TANK_YAW, 0))
	_set_fields(mission, tank, MissionData.KIND_ITEM, {"group": TANK_GROUP, "team": 1})
	assert_true(mission.set_entity_property_string(MissionData.KIND_ITEM, tank.index, "name2", "tdrive"))
	_ids["tank"] = tank.bms_id
	# The instructor waits beside the tank's hatch for the lesson's board order.
	var instructor := mission.add_entity(MissionData.KIND_ORGANIC, VehicleFixture.RIDER,
			_ground(terrain, TANK_AT + Vector2(0.5, 0.0)), Vector3.ZERO)
	_set_fields(mission, instructor, MissionData.KIND_ORGANIC, {"group": INSTRUCTOR_GROUP, "team": 1})
	_ids["instructor"] = instructor.bms_id
	for group: int in ENEMY_TANKS:
		for at: Vector2 in ENEMY_TANKS[group]:
			var enemy := mission.add_entity(MissionData.KIND_ITEM, VehicleFixture.TARGET_TANK,
					_ground(terrain, at), Vector3(0, 90, 0))
			_set_fields(mission, enemy, MissionData.KIND_ITEM, {"group": group, "team": 2})
			_enemies.append(enemy.bms_id)
	for at: Vector2 in APC_START:
		var apc := mission.add_entity(MissionData.KIND_ITEM, VehicleFixture.TARGET_APC,
				_ground(terrain, at), Vector3(0, 90, 0))
		_set_fields(mission, apc, MissionData.KIND_ITEM, {"group": APC_GROUP, "team": 2})
		_enemies.append(apc.bms_id)
	_path(mission, terrain, COURSE_PATH, COURSE)
	_path(mission, terrain, APC_PATH, APC_ROUTE)
	# A marker removal lays every path out again from its waypoint markers, as
	# the original editor's writer does (bms_edit lay_out_waypoint_path, D-MIS-6).
	var scratch := mission.add_entity(MissionData.KIND_MARKER, 106005, Vector3.ZERO, Vector3.ZERO)
	assert_true(mission.remove_entity(MissionData.KIND_MARKER, scratch.index))

	# The lesson: the player on the tank orders the instructor aboard.
	var lesson := mission.add_event(0, 0, 0)
	# PLYRONGUN of the tank: 07TR keys its lesson on PLYRONSSN, the player
	# standing on the tank's deck (it is carried there on a landing craft); this
	# course parks the tank on the ground, so the boarding itself is the key.
	assert_true(mission.add_event_trigger(lesson, 7, 41, tank.bms_id), "PLYRONGUN of the tank")
	assert_true(mission.add_event_action(lesson, 1, 0, INSTRUCTOR_GROUP, 125, tank.bms_id),
			"the instructor's group boards the tank")
	_events["lesson"] = lesson
	# The drive, once the instructor is aboard.
	var drive := mission.add_event(0, 0, 8)
	assert_true(mission.add_event_trigger(drive, 3, 0, lesson))
	assert_true(mission.add_event_action(drive, 1, 0, TANK_GROUP, COURSE_PATH, 0), "the tank drives the course")
	assert_true(mission.add_event_action(drive, 3, 30, TANK_GROUP, COURSE_SPEED), "at its patrol speed")
	_events["drive"] = drive
	# The final firing position, then the APC wave's release.
	var final_position := mission.add_event(0, 0, 0)
	# GroupAtWaypoint's node is the stop's place on the path, from 0.
	assert_true(mission.add_event_trigger(final_position, 1, 7, TANK_GROUP, COURSE_PATH, COURSE.size() - 1))
	_events["final_position"] = final_position
	var release := mission.add_event(0, 0, 0)
	assert_true(mission.add_event_trigger(release, 3, 0, final_position))
	assert_true(mission.add_event_action(release, 1, 0, APC_GROUP, APC_PATH, 0), "the APC wave drives in")
	assert_true(mission.add_event_action(release, 3, 30, APC_GROUP, COURSE_SPEED))
	_events["release"] = release
	# The objective and the delayed victory.
	var groups := ENEMY_TANKS.keys()
	groups.append(APC_GROUP)
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


func _tank_cannon(sim: Simulation, tank: EntityCard) -> EntityCard:
	var snapshot := sim.get_present_snapshot()
	for base in range(0, snapshot.size(), sim.get_present_stride()):
		var card := sim.entity_card(int(snapshot[base + Simulation.PF_WIRE_HANDLE]))
		if card != null and card.get_item_id() == VehicleFixture.TANK_TURRET - 100000 \
				and card.get_mission_position().distance_to(tank.get_mission_position()) < 10.0:
			return card
	return null


func test_tank_course_from_boarding_to_victory() -> void:
	var root := _boot_training()
	if root == null:
		return
	var sim := _world.get_sim()
	_advance(sim)
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
	# Fire clear of the instructor after the turret settles.
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
	assert_true(sim.has_event_fired(_events["final_position"]), "the tank reaches its final firing position")
	assert_true(sim.has_event_fired(_events["release"]), "the final position releases the APC wave")
	_presenter.teardown()
	_presenter = null
	_world.unload()


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


func test_cannon_prompt_from_beside_the_tank() -> void:
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
	# From the tank's roof-gun side, three units off the cannon seat.
	var approach := target + Vector3(0, 3, 0).rotated(Vector3(0, 0, 1), deg_to_rad(TANK_YAW - 90.0))
	# Approach at the player's standing height and let normal collision settle
	# the position.
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
