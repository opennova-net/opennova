extends GutTest

# The authored course, including its landing-craft crews and instructor.
# Only positioning/aim use a debug seam; boarding, weapons, damage and BMS
# progression use the normal runtime. No objectives or deaths are injected.
const MountLook := preload("res://tests/support/mount_look.gd")
const ENEMY_VEHICLES := [416, 318, 321, 415, 317, 320, 568, 572]
const FINAL_APCS := [568, 572]
var _presenter: LocalPlayerPresenter
var _world: GameWorld
var _cannon_handle := -1
var _launch_offset := Vector3.ZERO
var _camera: Camera3D


func after_each() -> void:
	if is_instance_valid(_presenter):
		_presenter.teardown()
	_presenter = null
	Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)


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
	_presenter.after_world_tick()
	var model := _world.get_runtime().get_entity_presenter().resolve_wire_handle(handle) as ObjectModel
	var point := model.get_object_data().get_user_point_info(bone - 1)
	# Stock emplacement models can carry userpoints without a render part.
	var part := model.get_render_part_nodes().get(point.subobject, model) as Node3D
	var pos := part.to_global(point.position)
	return Vector3(pos.x, -pos.z, pos.y)


func _combat_target(sim: Simulation) -> EntityCard:
	var nearest: EntityCard
	var score := INF
	var hull := sim.entity_card_by_net_id(33)
	for ssn in ENEMY_VEHICLES:
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
		if card.get_item_id() == 1213:
			candidate += 1000.0
		# The final APCs can respawn. Weaken both before finishing them
		# together so the course can enter its victory window.
		if card.get_net_id() in FINAL_APCS:
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
	for tick in 37500:
		if sim.get_round_outcome_debug().get_ended():
			break
		if tick % 62 == 0:
			target = _combat_target(sim)
			if target == null and not repositioned and sim.has_event_fired(49) and sim.has_event_fired(48):
				var survivors: Array[EntityCard] = []
				var other_targets_alive := false
				for ssn in ENEMY_VEHICLES:
					var enemy := sim.entity_card_by_net_id(ssn)
					if enemy == null or not enemy.is_alive() or enemy.get_health() <= 0:
						continue
					if ssn in FINAL_APCS:
						survivors.append(enemy)
					else:
						other_targets_alive = true
				if not other_targets_alive and not survivors.is_empty():
					# Stock APCs can respawn behind terrain after the convoy stops.
					# Preserve the authored drive, then position the combat fixture
					# in sight of the survivors. Damage and victory remain normal.
					var position := survivors[0].get_mission_position() + Vector3(30, -40, 0)
					position.z = _world.get_terrain_data().get_height_world_bilinear(Vector3(position.x, 0, -position.y)) + 0.5
					var hull := sim.entity_card_by_net_id(33)
					assert_eq(sim.debug_set_entity_position(hull.get_ai_index(), position), OK)
					repositioned = true
					gut.p("07TR combat fixture: advance the tank to the respawned APCs after the authored route.")
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
	assert_true(sim.has_event_fired(92), "all six enemy groups satisfy the destruction objective")
	assert_true(sim.has_event_fired(93), "the authored delayed victory event fires")
	assert_true(sim.get_round_outcome_debug().get_ended(), "tank training ends in victory")
	assert_eq(sim.get_round_outcome_debug().get_winner_team(), 1)


func _boot_training() -> ResourceRoot:
	if RetailData.install().is_empty():
		pending("OPENNOVA_JO_DIR with 07TR.bms is required")
		return null
	var root := RetailData.mount_tank_training()
	assert_not_null(root, "the installed base or an expansion must serve 07TR.bms")
	if root == null:
		return null
	var expansion := root.get_expansion()
	gut.p("07TR course mount: base" if expansion.is_empty() else "07TR course mount: " + expansion)
	var mission := MissionData.new()
	var open_error := mission.open_from_resource_root(root, "07TR.bms")
	assert_eq(open_error, OK)
	if open_error != OK:
		return null
	_world = WorldFixture.make_world(self)
	_world.set_resource_root(root)
	var load_error := _world.load_mission_data(mission, "07TR.bms")
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
	return root


func test_07tr_tank_course_from_boarding_to_victory() -> void:
	var root := _boot_training()
	if root == null:
		return
	var sim := _world.get_sim()
	for ssn in [37, 38, 202, 203]:
		assert_gt(sim.entity_card_by_net_id(ssn).get_mission_position().z, 11.5,
				"landing craft starts afloat before its crew boards")
	_advance(sim)
	_await_second_landing(sim)
	var tank := sim.entity_card_by_net_id(33)
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
	assert_true(boarded, "USE boards the live cannon seat on the authored tank")
	if not boarded:
		return
	_advance(sim)
	assert_eq(sim.get_local_player_weapon_name(), "WPN_M1TURRET")
	for tick in 1250:
		if sim.has_event_fired(15):
			break
		_advance(sim, 1)
	assert_true(sim.has_event_fired(15), "boarding starts the instructor within 20 seconds")
	_advance(sim)
	var cannon_sights := _sights()
	# Fire clear of the instructor and landing craft after the turret settles.
	MountLook.face(self, sim, sim.entity_card_by_net_id(33).get_mission_position() + Vector3(0, -150, 45))
	_advance(sim)
	var cannon_ammo := _fire_gun(sim)
	# Stock mounts and mods differ: only an authored G attachment switches
	# slots. The existing mounted_weapon_switch suite also tests helicopters.
	var has_alternate := false
	var definitions := AuthoredItemFixture.read_rows(root)
	for attachment: Dictionary in definitions[100164]["attachments"]:
		has_alternate = has_alternate or attachment["kind"] == "addeweapg"
	if has_alternate:
		MountLook.right_click(self, sim, _presenter)
		_advance(sim)
		assert_eq(sim.get_local_player_weapon_name(), definitions[100164]["weapon"])
		assert_eq(_world.local_player_hud_weapon_def().weapon_name, definitions[100164]["weapon"],
				"right-click selects the alternate gun HUD definition")
		assert_false(_sights().is_empty(), "the alternate gun has its authored sight")
		assert_ne(_fire_gun(sim), cannon_ammo, "the alternate gun fires its own ammunition")
		MountLook.right_click(self, sim, _presenter)
		_advance(sim)
		assert_eq(sim.get_local_player_weapon_name(), "WPN_M1TURRET")
		assert_eq(_sights(), cannon_sights, "switching back restores the cannon sight")
	else:
		gut.p("Installed M1A1 has no G attachment; validating its cannon and course.")
	if get_fail_count() > 0:
		return
	var start := sim.entity_card_by_net_id(33).get_mission_position()
	_advance(sim, 3750)
	assert_gt(sim.entity_card_by_net_id(33).get_mission_position().distance_to(start), 15.0,
			"the instructor drives the tank along the course")
	_fight_convoy(sim)
	assert_true(sim.has_event_fired(49), "the convoy reaches its final firing position")
	assert_true(sim.has_event_fired(48), "the other allied tank releases the APC wave")
	_presenter.teardown()
	_presenter = null
	_world.unload()


# Board once the second landing craft has put its tanks on the beach. Boarding
# at once starts the instructor's chain early enough that event 17 releases
# tank 34 while LCAC 38 still crawls in over deep water; whether that tank
# lands or nose-dives off the moving bow then depends on sub-second timing
# under the retail boarder hold and avoid brake (docs/world/tank-parity-re.md).
# A player who boards after the landing gets the deterministic course.
func _await_second_landing(sim: Simulation) -> void:
	var lcac := sim.entity_card_by_net_id(38)
	assert_not_null(lcac)
	if lcac == null:
		return
	var start := lcac.get_mission_position()
	var last := start
	var still := 0
	for step in 80:
		_advance(sim, 62)
		var now := sim.entity_card_by_net_id(38).get_mission_position()
		still = still + 1 if now.distance_to(last) < 0.05 else 0
		last = now
		if still >= 2 and now.distance_to(start) > 10.0:
			return
	fail_test("LCAC 38 lands within 80 seconds")


func _tank_cannon(sim: Simulation, tank: EntityCard) -> EntityCard:
	var snapshot := sim.get_present_snapshot()
	for base in range(0, snapshot.size(), sim.get_present_stride()):
		var card := sim.entity_card(int(snapshot[base + Simulation.PF_WIRE_HANDLE]))
		if card != null and card.get_item_id() == 166 and card.get_mission_position().distance_to(tank.get_mission_position()) < 10.0:
			return card
	return null


func _visible_attach_texts(hud: HudOverlay, gametext: RtxtStringFile) -> Array[String]:
	_presenter.after_world_tick()
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


func test_07tr_cannon_prompt_from_landing_craft_deck() -> void:
	var root := _boot_training()
	if root == null:
		return
	var sim := _world.get_sim()
	_advance(sim)
	var cannon := _tank_cannon(sim, sim.entity_card_by_net_id(33))
	assert_not_null(cannon)
	if cannon == null:
		return
	var seat: EntityCardSeat = cannon.get_seats()[0]
	assert_false(seat.is_occupied(), "the player's training cannon is free")
	var target := _seat_target(cannon.get_wire_handle(), seat.get_bone_index())
	var approach := target + Vector3(0, 3, 0)
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
	# Stock and mod models place the cannon seat at different heights. Start
	# just above its view-frustum edge, keeping the higher roof gun in view.
	MountLook.face(self, sim, target)
	var half_fov := rad_to_deg(atan(1.0 / _presenter.view_projection().y.y))
	var upper_pitch := sim.get_local_player_pitch_deg() + half_fov + 5.0
	assert_eq(sim.debug_teleport_local_player(here, yaw, upper_pitch), OK)
	_advance(sim, 1)
	var texts := _visible_attach_texts(hud, gametext)
	gut.p("Upper-turret view attachment labels: " + str(texts))
	assert_has(texts, ".50 Cal", "the upper gun remains in view")
	assert_does_not_have(texts, "120 MM Cannon", "the lower cannon anchor is below this view")
	# Re-aim at the cannon from the SAME standing position. This catches an
	# empty/missing HUD label as well as a label/USE selection disagreement.
	MountLook.face(self, sim, target)
	_advance(sim, 1)
	texts = _visible_attach_texts(hud, gametext)
	gut.p("Cannon view attachment labels: " + str(texts))
	assert_has(texts, "120 MM Cannon")
	assert_gte(hud.get_attach_label_selected(), 0)
	assert_eq(hud.get_attach_label_text(hud.get_attach_label_selected()), "120 MM Cannon",
			"looking at the visible cannon prompt selects its seat")
	_use(sim)
	_advance(sim)
	assert_eq(sim.get_local_player_weapon_name(), "WPN_M1TURRET",
			"USE boards the cannon advertised by the HUD")
	for tick in 1250:
		if sim.has_event_fired(15):
			break
		_advance(sim, 1)
	assert_true(sim.has_event_fired(15), "boarding the visible cannon prompt starts the lesson")
