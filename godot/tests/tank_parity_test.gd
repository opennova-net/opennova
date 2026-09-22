extends GutTest

## Installed M1A1 and T80: driver, turret and roof gun through the real seat
## requests; the mouse-wheel event router and authored HUD/weapon definitions.
const MountLook := preload("res://tests/support/mount_look.gd")
var _presenter: LocalPlayerPresenter
var _controls: ControlsModel


func after_each() -> void:
	if is_instance_valid(_presenter):
		_presenter.teardown()
	_presenter = null
	Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)


func _advance(sim: Simulation, count := 90) -> void:
	for i in count:
		sim.step()
		_presenter.after_world_tick()


func _wheel(sim: Simulation, direction: MouseButton) -> void:
	var event := InputEventMouseButton.new()
	event.button_index = direction
	event.pressed = true
	assert_true(_presenter.handle_input(event, true), "configured wheel event reaches gameplay")
	_advance(sim, 1)


func _zoom(sim: Simulation, def: WeaponDef) -> void:
	if def.scope_max_mag <= def.scope_min_mag:
		return # Fixed optics do not use the variable-magnification branch.
	var weapon := sim.get_local_player_weapon_name()
	var previous := _presenter.presented_view().scope_magnification
	# The gun's own slot starts at the slot-init seed: the second scope_max_mag
	# value clamped into [scope_min_mag, scope_max_mag], never the lazy maximum.
	# An emplacement slot has no owner entity, so no sniper lock applies.
	assert_eq(previous, clampi(def.scope_initial_mag, def.scope_min_mag, int(def.scope_max_mag)),
			"the optic starts at its authored seed")
	for i in 32:
		_wheel(sim, MOUSE_BUTTON_WHEEL_UP)
		var zoom := _presenter.presented_view().scope_magnification
		assert_gte(zoom, previous, "wheel away increases optical magnification")
		previous = zoom
	assert_eq(previous, int(def.scope_max_mag), "the authored maximum clamps repeated events")
	for i in 32:
		_wheel(sim, MOUSE_BUTTON_WHEEL_DOWN)
		var zoom := _presenter.presented_view().scope_magnification
		assert_lte(zoom, previous, "wheel toward decreases optical magnification")
		previous = zoom
	assert_eq(previous, def.scope_min_mag, "the authored minimum clamps repeated events")
	assert_eq(sim.get_local_player_weapon_name(), weapon, "zoom keeps the occupied gun equipped")
	# Remapping swaps the physical wheel while preserving each action's sign.
	var original := _controls.save_blob()
	var remapped := original.duplicate(true)
	remapped["cycleweaponP"][4] = ControlsModel.mouse_mask_from_godot_button(MOUSE_BUTTON_WHEEL_DOWN)
	remapped["cycleweaponN"][4] = ControlsModel.mouse_mask_from_godot_button(MOUSE_BUTTON_WHEEL_UP)
	_controls.load_blob(remapped)
	_wheel(sim, MOUSE_BUTTON_WHEEL_DOWN)
	assert_eq(_presenter.presented_view().scope_magnification, mini(def.scope_min_mag + 2, int(def.scope_max_mag)))
	_controls.load_blob(original)


func test_installed_tanks_seats_optics_and_hud() -> void:
	if RetailData.install().is_empty():
		pending("OPENNOVA_JO_DIR is required")
		return
	var root := ResourceRoot.new()
	if "revx02" in RetailData.expansions():
		assert_eq(root.mount_runtime(RetailData.install(), "revx02"), OK)
	else:
		root = RetailData.mount_install_with("07TR.bms")
	assert_not_null(root)
	if root == null:
		return
	var weapons := WeaponDatabase.new()
	assert_eq(weapons.load_from_resource_root(root, "weapon.def"), OK)
	gut.p("tank parity mount: " + root.get_expansion())
	for item_id in [100164, 100165]:
		var mission := MissionData.new()
		assert_eq(mission.open_from_resource_root(root, "07TR.bms"), OK)
		var placed_id := -1
		for record: EntityRef in mission.get_all_entity_refs():
			if record.item_id == 100164:
				var added := mission.add_entity(MissionData.KIND_ITEM, item_id,
						record.position + Vector3(0, 40, 0), mission.get_entity_rotation(record.kind, record.index))
				placed_id = added.bms_id
				break
		assert_gte(placed_id, 0)
		var world := WorldFixture.make_world(self)
		world.set_resource_root(root)
		assert_eq(world.load_mission_data(mission, "07TR.bms"), OK)
		world.set_process(false)
		var camera := Camera3D.new()
		add_child_autofree(camera)
		camera.make_current()
		_presenter = LocalPlayerPresenter.new()
		add_child_autofree(_presenter)
		_controls = ControlsModel.new()
		_presenter.setup(world, camera, null, _controls)
		var sim := world.get_sim()
		var hull := sim.entity_card_by_net_id(placed_id)
		var seat: EntityCardSeat = hull.get_seats()[0]
		for attempt in 2:
			hull = sim.entity_card_by_net_id(placed_id)
			var target := hull.get_mission_position() + seat.get_local().rotated(
					Vector3(0, 0, 1), deg_to_rad(-hull.get_yaw_deg()))
			MountLook.face(sim, target, target + Vector3(1, 0, 0))
			if attempt == 0:
				_advance(sim, 18)
		var boarded := sim.local_player_toggle_mount()
		assert_true(boarded, "board the unoccupied tank")
		if not boarded:
			return
		_advance(sim)
		for panel_seat in [0, 1, 2]:
			if panel_seat > 0:
				assert_true(sim.local_player_select_seat(panel_seat), "select tank panel seat %d" % panel_seat)
			_advance(sim)
			assert_true(sim.entity_card(sim.get_local_player_wire_handle()).is_mounted())
			var view := _presenter.presented_view()
			assert_true(view.camera_pose_valid)
			assert_almost_eq(camera.global_position, view.camera_eye, Vector3.ONE * 0.00001)
			assert_eq(_presenter.presented_view(), view, "snapshot inspection does not recompose camera state")
			var panel := sim.get_vehicle_panel_view()
			assert_true(panel.shown)
			assert_eq(panel.item_id, item_id, "all seats root their HUD panel at the hull")
			var name := sim.get_local_player_weapon_name()
			gut.p("tank %d seat %d: %s" % [item_id, panel_seat, name])
			if panel_seat > 0:
				assert_ne(name, "")
				assert_eq(world.local_player_hud_weapon_def().weapon_name, name)
				var def := weapons.get_weapon(weapons.find_weapon(name))
				assert_not_null(def)
				if def != null:
					_zoom(sim, def)
		assert_true(sim.local_player_toggle_mount(), "dismount the roof gun")
		_advance(sim)
		assert_false(sim.entity_card(sim.get_local_player_wire_handle()).is_mounted())
		assert_false(sim.get_vehicle_panel_view().shown)
		_presenter.teardown()
		_presenter = null
		world.unload()
