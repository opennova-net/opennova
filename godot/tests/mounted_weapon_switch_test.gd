extends GutTest

# The mounted alternate-gun switch (action 6, RMB by default): a player in the
# seat that borrows a carrier's designated-G EWeap child (`addeweapG`) toggles
# the borrowed weapon slot between the child's own gun and the carrier's own
# weapon, each slot keeping its ammunition. The request rides the host
# loopback as C2S 0x16 MOUNTED_WEAPON_SLOT_SELECT and the REAL presenter
# consumes the weapon event.
#
# Which installed vehicles author that attachment is data: the stock Escalation
# tanks author addeweap + addeweapC only, the stock attack helicopters (Apache,
# Ka-52) author addeweapG (the chin gun, borrowed by the `Usegun` gunner seat,
# against the helicopter's own rockets), and the JOTAC mod authors it on the
# tanks as well (the turret cannon against the hull's coaxial gun). The suite
# therefore discovers, over the base mount and every expansion of the install,
# which of its candidate carriers author a designated-G child, reads both
# weapon names from the installed defs, and runs the whole scenario on each;
# an install with none is a failure, never a quiet pend. The ungated host_role
# ctest covers the same queue with synthetic slot records.

const MountLook := preload("res://tests/support/mount_look.gd")

const TRAINING_MISSION := "07TR.bms"
# 07TR's authored player tank: the reference the carriers are placed against.
const AUTHORED_TANK := 100164
# The candidate carriers (items.def ids): the M1A1, the T80, the Apache and
# the Ka-52 (id 100172 in the stock items.def).
const CARRIER_CANDIDATES: Array[int] = [100164, 100165, 100171, 100172]
# Where a tank is the carrier, its designated-G child is the turret and the gun
# it borrows first is the cannon.
const KNOWN_CHILD_WEAPONS := {100164: "WPN_M1TURRET", 100165: "WPN_T80TURRET"}
# The unoccupied carrier goes 40 units north of the authored tank, on the
# training ground (terrain about z 13-16, the water level is 12). Twelve units
# east of that tank is the lake (terrain z 1.8): a helicopter placed there is
# judged submerged and drowns at 100 health per tick as soon as it is claimed.
const CARRIER_OFFSET := Vector3(0, 40, 0)

var _presenter: LocalPlayerPresenter


## One installed designated-G carrier: the mount that authors it and the two
## weapon names its seat toggles between, read from that mount's defs.
class Carrier:
	extends RefCounted
	var root: ResourceRoot
	var expansion: String
	var item_id: int
	var display_name: String
	var child_item_id: int
	var child_weapon: String  # the G child's primary_weapon, borrowed first
	var parent_weapon: String  # the carrier's own primary_weapon, the alternate

	func label() -> String:
		return "%s (%d, mount '%s')" % [display_name, item_id, expansion]


func after_each() -> void:
	if is_instance_valid(_presenter):
		_presenter.teardown()
	_presenter = null
	Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)


# The candidate carriers the install authors a designated-G child for, each
# from the first mount (base, then each expansion) that both authors it and
# serves the training mission. Empty without an install.
func _installed_g_carriers() -> Array[Carrier]:
	var found: Array[Carrier] = []
	var installed := RetailData.install()
	if installed.is_empty():
		return found
	var expansions := PackedStringArray([""])
	expansions.append_array(RetailData.expansions())
	var claimed: Array[int] = []
	for expansion: String in expansions:
		var art := ResourceRoot.new()
		assert_eq(art.mount_runtime(installed, expansion), OK, "mount '%s'" % expansion)
		if not art.has_file(TRAINING_MISSION):
			continue
		var items := ItemDatabase.new()
		assert_eq(items.load_from_resource_root(art, "items.def"), OK,
				"items.def loads from mount '%s'" % expansion)
		var authored := AuthoredItemFixture.read_rows(art)
		for item_id: int in CARRIER_CANDIDATES:
			if claimed.has(item_id):
				continue
			if not authored.has(item_id):
				continue
			for attachment: Dictionary in authored[item_id]["attachments"]:
				if attachment["kind"] != "addeweapg":
					continue
				var carrier := Carrier.new()
				carrier.root = art
				carrier.expansion = expansion
				carrier.item_id = item_id
				carrier.display_name = items.get_display_name(item_id)
				carrier.child_item_id = attachment["item_id"]
				carrier.child_weapon = authored[carrier.child_item_id]["weapon"]
				carrier.parent_weapon = authored[item_id]["weapon"]
				found.append(carrier)
				claimed.append(item_id)
				break
	return found


func _advance(sim: Simulation, ticks: int = 90) -> void:
	for i in range(ticks):
		sim.step()
		_presenter.after_world_tick()


# Action 6 is the configurable scope row (RMB by default). Headless Godot
# cannot capture the mouse, and PlayerInputRouter::sample_weapon_input samples
# the weapon actions only while it is captured, so a headless run calls
# Simulation.request_local_player_scope_toggle() itself: the router's one-line
# forward, everything after it (the loopback request, the host handler, the
# presenter's event) being the same. A windowed run drives the actual default
# RMB binding through the input router.
func _right_click(sim: Simulation) -> void:
	if DisplayServer.get_name() == "headless":
		assert_true(sim.request_local_player_scope_toggle())
		return
	Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)
	assert_eq(Input.get_mouse_mode(), Input.MOUSE_MODE_CAPTURED)
	for pressed: bool in [true, false]:
		var event := InputEventMouseButton.new()
		event.button_index = MOUSE_BUTTON_RIGHT
		event.pressed = pressed
		Input.parse_input_event(event)
		Input.flush_buffered_events()
		_presenter.before_world_tick(Simulation.tick_dt(), false, true)


# The whole scenario on one carrier: board it, take the seat that borrows the
# G child, then switch back and forth. False when a fatal step failed (the
# asserts name it).
func _check_carrier(carrier: Carrier) -> bool:
	var who := carrier.label()
	var art := carrier.root
	var world := WorldFixture.make_world(self)
	world.set_resource_root(art)
	var mission := MissionData.new()
	assert_eq(mission.open_from_resource_root(art, TRAINING_MISSION), OK, who)
	var carrier_bms_id := -1
	for record: EntityRef in mission.get_all_entity_refs():
		if record.item_id != AUTHORED_TANK:
			continue
		# The original T80 targets have enemy crews and the authored M1A1 is
		# the mission's; add an unoccupied carrier on the training ground
		# through the mission API, retaining its installed rig/defs.
		var added := mission.add_entity(MissionData.KIND_ITEM, carrier.item_id,
				record.position + CARRIER_OFFSET, mission.get_entity_rotation(record.kind, record.index))
		assert_not_null(added, "%s: placed beside the authored tank" % who)
		if added == null:
			return false
		carrier_bms_id = added.bms_id
		break
	assert_gte(carrier_bms_id, 0, "%s: the training mission places its M1A1" % who)
	if carrier_bms_id < 0:
		return false
	assert_eq(world.load_mission_data(mission, TRAINING_MISSION), OK, who)
	world.set_process(false)
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
	assert_eq(hull.get_item_id(), carrier.item_id - 100000, who)
	# Look at the control seat and press USE from beside it; the seat scan
	# admits the seat only inside the view cone. Proximity tables rebuild every
	# 17 ticks, so refresh them after the move and aim again at the carrier's
	# settled pose.
	var seat: EntityCardSeat = hull.get_seats()[0]
	var target := hull.get_mission_position() + seat.get_local().rotated(
			Vector3(0, 0, 1), deg_to_rad(-hull.get_yaw_deg()))
	MountLook.face(sim, target, target + Vector3(1, 0, 0))
	_advance(sim, 18)
	hull = sim.entity_card_by_net_id(carrier_bms_id)
	target = hull.get_mission_position() + seat.get_local().rotated(
			Vector3(0, 0, 1), deg_to_rad(-hull.get_yaw_deg()))
	MountLook.face(sim, target, target + Vector3(1, 0, 0))
	var boarded := sim.local_player_toggle_mount()
	assert_true(boarded, "%s: board the carrier" % who)
	if not boarded:
		return false
	# Panel slot 1 is the G child's own `usegun` seat (the turret gunner, the
	# helicopter's chin gunner): the seat whose occupant borrows the child.
	var selected := sim.local_player_select_seat(1)
	assert_true(selected, "%s: select the seat that borrows the G child" % who)
	if not selected:
		return false
	_advance(sim)
	assert_eq(sim.entity_card(sim.get_local_player_wire_handle()).get_mount_type(), 3,
			"%s: the borrowing seat is a gunner seat" % who)
	assert_true(sim.entity_card(sim.get_local_player_wire_handle()).is_mounted(),
			"%s: still aboard after the seat change" % who)
	var first := carrier.child_weapon
	var alternate := carrier.parent_weapon
	assert_ne(first, "", "%s: the installed G child authors a primary_weapon" % who)
	assert_ne(alternate, "", "%s: the installed carrier authors a primary_weapon" % who)
	assert_ne(first, alternate, "%s: the two slots carry different guns" % who)
	if KNOWN_CHILD_WEAPONS.has(carrier.item_id):
		assert_eq(first, KNOWN_CHILD_WEAPONS[carrier.item_id],
				"%s: the tank's turret borrows the cannon first" % who)
	assert_eq(sim.get_local_player_weapon_name(), first,
			"%s: the seat borrows the G child's own gun first" % who)
	assert_eq(world.local_player_hud_weapon_def().weapon_name, first, who)
	hud_presenter.tick()
	assert_true(card.is_card_up(), "%s: the mounted gun's forced sights are visible" % who)
	assert_eq(card.row_count(), world.local_player_hud_weapon_def().sights.size(), who)
	var first_clip := sim.get_local_player_weapon_state().clip
	var first_reserve := sim.get_local_player_weapon_state().reserve

	# The request must survive the host loopback, and the REAL presenter
	# consumes the weapon event.
	_right_click(sim)
	_advance(sim)
	assert_eq(sim.get_local_player_weapon_name(), alternate,
			"%s: right-click selects the carrier's own weapon" % who)
	assert_ne(sim.get_local_player_weapon_name(), first, who)
	assert_eq(world.local_player_hud_weapon_def().weapon_name, alternate,
			"%s: the HUD weapon def follows the switch" % who)
	# The alternate is the carrier's embedded slot: give it distinct ammo.
	assert_eq(sim.debug_set_world_entity_weapon_ammo(hull.get_net_id(), 7, 19), OK, who)
	assert_eq(world.local_player_weapon_view().clip, 7, who)
	assert_eq(world.local_player_weapon_view().reserve, 19, who)
	hud_presenter.tick()
	assert_true(card.is_card_up(), "%s: switching retains the mounted optical HUD" % who)
	assert_eq(card.row_count(), world.local_player_hud_weapon_def().sights.size(), who)
	assert_gt(hud_presenter.get_game_hud().get_draw_list_stats().glyphs, 0, who)

	_right_click(sim)
	_advance(sim)
	assert_eq(sim.get_local_player_weapon_name(), first, "%s: right-click returns" % who)
	assert_eq(world.local_player_hud_weapon_def().weapon_name, first, who)
	assert_eq(world.local_player_weapon_view().clip, first_clip,
			"%s: the child's slot kept its own clip" % who)
	assert_eq(world.local_player_weapon_view().reserve, first_reserve, who)
	_right_click(sim)
	_advance(sim)
	assert_eq(sim.get_local_player_weapon_name(), alternate, who)
	assert_eq(world.local_player_weapon_view().clip, 7,
			"%s: returning preserves the alternate's ammo" % who)
	assert_eq(world.local_player_weapon_view().reserve, 19, who)

	hud_presenter.teardown()
	_presenter.teardown()
	_presenter = null
	world.unload()
	return true


func test_installed_g_carriers_right_click_switches_gun_and_hud() -> void:
	if RetailData.install().is_empty():
		pending("OPENNOVA_JO_DIR is required for installed designated-G weapon switching (%s)"
				% TRAINING_MISSION)
		return
	var carriers := _installed_g_carriers()
	if carriers.is_empty():
		fail_test(("no candidate carrier %s authors a designated-G (addeweapG) child in any "
				+ "mount of %s serving %s (the base mount and the expansions [%s]); the "
				+ "alternate-gun switch has nothing to run on") % [CARRIER_CANDIDATES,
				RetailData.install(), TRAINING_MISSION, ", ".join(RetailData.expansions())])
		return
	var ran := 0
	for carrier: Carrier in carriers:
		gut.p("designated-G carrier: %s, child %d borrows '%s' against '%s'" % [
				carrier.label(), carrier.child_item_id, carrier.child_weapon,
				carrier.parent_weapon])
		if _check_carrier(carrier):
			ran += 1
	assert_gt(ran, 0, "at least one installed carrier ran the whole switch scenario")
