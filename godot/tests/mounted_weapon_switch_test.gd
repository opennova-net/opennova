extends GutTest

# The mounted alternate-gun switch (action 6, RMB by default): a player in the
# seat that borrows a carrier's designated-G EWeap child (`addeweapG`) toggles
# the borrowed weapon slot between the child's own gun and the carrier's own
# weapon, each slot keeping its ammunition. The request rides the host
# loopback as C2S 0x16 MOUNTED_WEAPON_SLOT_SELECT and the REAL presenter
# consumes the weapon event.
#
# The carrier is VehicleFixture's attack helicopter, shaped after the stock
# carriers that author the attachment (the Apache and the Ka-52: `addeweapG`
# names the chin gun, which the `Usegun` gunner seat borrows against the
# helicopter's own rockets; the retail witness ran on both): the minted helo
# with its G child the minted minigun, `WPN_HELOGUN` against `WPN_HELOROCKET`.
# The ungated host_role ctest covers the same queue with synthetic slot records.

const MountLook := preload("res://tests/support/mount_look.gd")

# The carrier sits on the training ground beside the spawn.
const CARRIER_POSITION := Vector2(9.0, 3.0)

var _presenter: LocalPlayerPresenter
var _dir := ""


func after_each() -> void:
	if is_instance_valid(_presenter):
		_presenter.teardown()
	_presenter = null
	Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)
	if not _dir.is_empty():
		TestFs.remove_dir_recursive(_dir)
	_dir = ""


func _advance(sim: Simulation, ticks: int = 90) -> void:
	for i in range(ticks):
		sim.step()
		_presenter.after_world_tick()


func test_g_carrier_right_click_switches_gun_and_hud() -> void:
	_dir = VehicleFixture.stage_world(self, "weapon_switch")
	var terrain := VehicleFixture.terrain(self, _dir)
	var ground := VehicleFixture.ground_z(terrain, CARRIER_POSITION.x, CARRIER_POSITION.y)
	var ids := []
	var world := VehicleFixture.boot(self, _dir, [{
		"item": VehicleFixture.G_HELO,
		"pos": Vector3(CARRIER_POSITION.x, CARRIER_POSITION.y, ground),
		"yaw": 30.0,
	}], ids)
	var carrier_bms_id: int = ids[0]
	assert_gte(carrier_bms_id, 0, "the carrier is placed")
	var root := world.get_resource_root()
	var authored := AuthoredItemFixture.read_rows(root)
	var child_item_id := -1
	for attachment: Dictionary in authored[VehicleFixture.G_HELO]["attachments"]:
		if attachment["kind"] == "addeweapg":
			child_item_id = attachment["item_id"]
	assert_eq(child_item_id, VehicleFixture.G_CHIN_GUN, "the carrier authors its designated-G child")
	var first: String = authored[child_item_id]["weapon"]
	var alternate: String = authored[VehicleFixture.G_HELO]["weapon"]
	var sim := world.get_sim()
	var camera := Camera3D.new()
	add_child_autofree(camera)
	camera.make_current()
	_presenter = LocalPlayerPresenter.new()
	add_child_autofree(_presenter)
	_presenter.setup(world, camera, null, ControlsModel.new())
	var ui := Control.new()
	ui.size = Vector2(1024, 768)
	add_child_autofree(ui)
	var hud_presenter := GameHudPresenter.new()
	add_child_autofree(hud_presenter)
	hud_presenter.setup(world, _presenter, ui)
	hud_presenter.ensure_game_hud()
	hud_presenter.set_hud_detail_level(0)
	var card := hud_presenter.get_game_hud().get_node("SightsCard") as HudSightsCard
	var hull := sim.entity_card_by_net_id(carrier_bms_id)
	assert_not_null(hull, "the placed carrier is in the world")
	if hull == null:
		return
	assert_eq(hull.get_item_id(), VehicleFixture.G_HELO - 100000)
	# Look at the control seat and press USE from beside it; the seat scan
	# admits the seat only inside the view cone. Proximity tables rebuild every
	# 17 ticks, so refresh them after the move and aim again at the carrier's
	# settled pose.
	var seat: EntityCardSeat = hull.get_seats()[0]
	var target := hull.get_mission_position() + seat.get_local().rotated(
			Vector3(0, 0, 1), deg_to_rad(-hull.get_yaw_deg()))
	MountLook.face(self, sim, target, target + Vector3(1, 0, 0))
	_advance(sim, 18)
	hull = sim.entity_card_by_net_id(carrier_bms_id)
	target = hull.get_mission_position() + seat.get_local().rotated(
			Vector3(0, 0, 1), deg_to_rad(-hull.get_yaw_deg()))
	MountLook.face(self, sim, target, target + Vector3(1, 0, 0))
	var boarded := sim.local_player_toggle_mount()
	assert_true(boarded, "board the carrier")
	if not boarded:
		return
	# Panel slot 1 is the G child's own `usegun` seat (the chin gunner): the
	# seat whose occupant borrows the child.
	var selected := sim.local_player_select_seat(1)
	assert_true(selected, "select the seat that borrows the G child")
	if not selected:
		return
	_advance(sim)
	assert_eq(sim.entity_card(sim.get_local_player_wire_handle()).get_mount_type(), 3,
			"the borrowing seat is a gunner seat")
	assert_true(sim.entity_card(sim.get_local_player_wire_handle()).is_mounted(),
			"still aboard after the seat change")
	assert_eq(first, "WPN_HELOGUN", "the G child authors its primary_weapon")
	assert_eq(alternate, "WPN_HELOROCKET", "the carrier authors its primary_weapon")
	assert_ne(first, alternate, "the two slots carry different guns")
	assert_eq(sim.get_local_player_weapon_name(), first,
			"the seat borrows the G child's own gun first")
	assert_eq(world.local_player_hud_weapon_def().weapon_name, first)
	hud_presenter.tick()
	assert_true(card.is_card_up(), "the mounted gun's forced sights are visible")
	assert_eq(card.row_count(), world.local_player_hud_weapon_def().sights.size())
	var first_clip := sim.get_local_player_weapon_state().clip
	var first_reserve := sim.get_local_player_weapon_state().reserve

	# The request must survive the host loopback, and the REAL presenter
	# consumes the weapon event.
	MountLook.right_click(self, sim, _presenter)
	_advance(sim)
	assert_eq(sim.get_local_player_weapon_name(), alternate,
			"right-click selects the carrier's own weapon")
	assert_ne(sim.get_local_player_weapon_name(), first)
	assert_eq(world.local_player_hud_weapon_def().weapon_name, alternate,
			"the HUD weapon def follows the switch")
	# The alternate is the carrier's embedded slot: give it distinct ammo.
	assert_eq(sim.debug_set_world_entity_weapon_ammo(hull.get_net_id(), 7, 19), OK)
	assert_eq(world.local_player_weapon_view().clip, 7)
	assert_eq(world.local_player_weapon_view().reserve, 19)
	hud_presenter.tick()
	assert_true(card.is_card_up(), "switching retains the mounted optical HUD")
	assert_eq(card.row_count(), world.local_player_hud_weapon_def().sights.size())
	assert_gt(hud_presenter.get_game_hud().get_draw_list_stats().glyphs, 0)

	MountLook.right_click(self, sim, _presenter)
	_advance(sim)
	assert_eq(sim.get_local_player_weapon_name(), first, "right-click returns")
	assert_eq(world.local_player_hud_weapon_def().weapon_name, first)
	assert_eq(world.local_player_weapon_view().clip, first_clip,
			"the child's slot kept its own clip")
	assert_eq(world.local_player_weapon_view().reserve, first_reserve)
	MountLook.right_click(self, sim, _presenter)
	_advance(sim)
	assert_eq(sim.get_local_player_weapon_name(), alternate)
	assert_eq(world.local_player_weapon_view().clip, 7,
			"returning preserves the alternate's ammo")
	assert_eq(world.local_player_weapon_view().reserve, 19)

	hud_presenter.teardown()
	_presenter.teardown()
	_presenter = null
	world.unload()
