extends GutTest

# The mounted alternate-gun switch (action 6, RMB by default): a player in the
# seat that borrows a carrier's designated-G EWeap child (`addeweapG`) toggles
# the borrowed weapon slot between the child's own gun and the carrier's own
# weapon, each slot keeping its ammunition. The request rides the host
# loopback as C2S 0x16 MOUNTED_WEAPON_SLOT_SELECT and the REAL presenter
# consumes the weapon event.
#
# Which vehicles author that attachment is data: in the shipped corpus only the
# Escalation attack helicopters do, the Apache and the Ka-52 (`addeweapG` names
# the chin gun, which the `usegun` gunner seat borrows against the helicopter's
# own rockets). Their two shapes differ in what the switch's slots carry: the
# Apache fires its rockets from four pods (weapl*/weapr* and the weapl*2 /
# weapr*2 pair) and its chin gun has one barrel; the Ka-52 fires from two pods
# and its chin gun has two (left bullet01, right bullet02), its gunner seated
# in the cockpit well behind the gun. The suite therefore discovers, over the
# authored catalog, which of its candidate carriers author a designated-G
# child, reads both weapon names from the authored defs, and runs the whole
# scenario on each; a catalog with none is a failure, never a quiet pend. The
# ungated host_role ctest covers the same queue with synthetic slot records.

const MountLook := preload("res://tests/support/mount_look.gd")

# The candidate carriers (items.def ids): the M1A1, the T80, the Apache and
# the Ka-52.
const CARRIER_CANDIDATES: Array[int] = [VehicleFixture.TANK, VehicleFixture.TANK_T80,
		VehicleFixture.G_HELO, VehicleFixture.G_HELO_KA52]
# Each carrier sits on the training ground beside the spawn.
const CARRIER_POSITION := Vector2(9.0, 3.0)

var _presenter: LocalPlayerPresenter
var _dir := ""


## One designated-G carrier: its id and the two weapon names its seat toggles
## between, read from the authored defs.
class Carrier:
	extends RefCounted
	var item_id: int
	var display_name: String
	var child_item_id: int
	var child_weapon: String  # the G child's primary_weapon, borrowed first
	var parent_weapon: String  # the carrier's own primary_weapon, the alternate

	func label() -> String:
		return "%s (%d)" % [display_name, item_id]


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


# The candidates the staged catalog authors a designated-G child for.
func _g_carriers(root: ResourceRoot) -> Array[Carrier]:
	var found: Array[Carrier] = []
	var items := ItemDatabase.new()
	assert_eq(items.load_from_resource_root(root, "items.def"), OK)
	var authored := AuthoredItemFixture.read_rows(root)
	for item_id: int in CARRIER_CANDIDATES:
		if not authored.has(item_id):
			continue
		for attachment: Dictionary in authored[item_id]["attachments"]:
			if attachment["kind"] != "addeweapg":
				continue
			var carrier := Carrier.new()
			carrier.item_id = item_id
			carrier.display_name = items.get_display_name(item_id)
			carrier.child_item_id = attachment["item_id"]
			carrier.child_weapon = authored[carrier.child_item_id]["weapon"]
			carrier.parent_weapon = authored[item_id]["weapon"]
			found.append(carrier)
			break
	return found


func test_g_carriers_right_click_switch_gun_and_hud() -> void:
	_dir = VehicleFixture.stage_world(self, "weapon_switch")
	var carriers := _g_carriers(VehicleFixture.root_over(self, _dir))
	assert_eq(carriers.size(), 2, "the catalog authors the two designated-G carriers")
	if carriers.is_empty():
		fail_test("no candidate carrier %s authors a designated-G (addeweapG) child" % [
				CARRIER_CANDIDATES])
		return
	var ran := 0
	for carrier: Carrier in carriers:
		gut.p("designated-G carrier: %s, child %d borrows '%s' against '%s'" % [
				carrier.label(), carrier.child_item_id, carrier.child_weapon,
				carrier.parent_weapon])
		if await _check_carrier(carrier):
			ran += 1
	assert_eq(ran, carriers.size(), "every carrier ran the whole switch scenario")


# The whole scenario on one carrier: board it, take the seat that borrows the
# G child, then switch back and forth. False when a fatal step failed (the
# asserts name it).
func _check_carrier(carrier: Carrier) -> bool:
	var who := carrier.label()
	var terrain := VehicleFixture.terrain(self, _dir)
	var ground := VehicleFixture.ground_z(terrain, CARRIER_POSITION.x, CARRIER_POSITION.y)
	var ids := []
	var world := VehicleFixture.boot(self, _dir, [{
		"item": carrier.item_id,
		"pos": Vector3(CARRIER_POSITION.x, CARRIER_POSITION.y, ground),
		"yaw": 30.0,
	}], ids)
	var carrier_bms_id: int = ids[0]
	assert_gte(carrier_bms_id, 0, "%s: the carrier is placed" % who)
	if carrier_bms_id < 0:
		return false
	var first := carrier.child_weapon
	var alternate := carrier.parent_weapon
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
	assert_not_null(hull, "%s: the placed carrier is in the world" % who)
	if hull == null:
		return false
	assert_eq(hull.get_item_id(), carrier.item_id - 100000)
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
	assert_true(boarded, "%s: board the carrier" % who)
	if not boarded:
		return false
	# Panel slot 1 is the G child's own `usegun` seat (the chin gunner): the
	# seat whose occupant borrows the child.
	var selected := sim.local_player_select_seat(1)
	assert_true(selected, "%s: select the seat that borrows the G child" % who)
	if not selected:
		return false
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
			"%s: right-click selects the carrier's own weapon" % who)
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
	return true
