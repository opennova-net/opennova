extends GutTest

# Throwable weapon-switch + PowerThrow lifecycle against the committed JO defs,
# driven the way the game shells drive Simulation (loadout -> switch walk ->
# commit events -> def installs, including the FP model resolve's delayed
# same-weapon re-install). Pins the PR #282 field bugs:
#  - a same-name install landing during a SWITCHFROM must not destroy the
#    switch chain (it desynced weapon_def_ from equipped_adm_index — the
#    "rifle fires the last-thrown ammo" bug),
#  - a PowerThrow press during the draw-in must not leak a latched charge
#    onto a later shot,
#  - the thrown grenade flies as its TrcrID item and dies by fuse, never by
#    ground contact.

var _sim: Simulation = null
var _db: WeaponDatabase = null
var _root: ResourceRoot = null
var _reinstall_pending := ""
var _reinstall_ticks := 0


# The synthetic Tmap terrain (fixtures/terrain/tmap) staged over the minimal
# assets it names; one root per test file (TestFs.staged_tmap), removed at the end.
const TMAP_STAGE := "throwable"


func should_skip_script():
	return RetailData.def_root_skip()


func after_all() -> void:
	TestFs.release_staged_tmap(TMAP_STAGE)


func before_each() -> void:
	_reinstall_pending = ""
	_reinstall_ticks = 0
	var def_root := RetailData.def_root()
	assert_true(DirAccess.dir_exists_absolute(def_root), "the shipped def tables are staged")
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	_sim = Simulation.new()
	assert_true(_sim.load_from_mission_data(md))
	assert_true(_sim.spawn_local_player(Vector3(0, 0, 0), 0.0, 1))
	_root = ResourceRoot.new()
	assert_eq(_root.set_root_dir(def_root), OK)
	assert_eq(_sim.load_weapon_table(_root, "weapon.def"), OK)
	assert_eq(_sim.load_ammo_table(_root, "ammo.def"), OK)
	_db = WeaponDatabase.new()
	assert_eq(_db.load_from_resource_root(_root, "weapon.def"), OK)


func after_each() -> void:
	_sim = null
	_db = null
	_root = null


func _install(weapon_name: String) -> void:
	var idx: int = _db.find_weapon(weapon_name)
	assert_gt(idx, -1, "weapon %s in weapon.def" % weapon_name)
	_sim.set_local_player_weapon(_db.get_weapon(idx), {})


func _rebake(weapon_name: String) -> void:
	var idx: int = _db.find_weapon(weapon_name)
	assert_gt(idx, -1, "weapon %s in weapon.def" % weapon_name)
	_sim.rebake_local_player_weapon(_db.get_weapon(idx), {})


# Step + drain like the shells: a switch event installs the def, and the rebuilt
# viewmodel installs the SAME def again ~a frame later (the FP model resolve —
# GameWorld's local-player weapon setup). The re-install racing the FSM is the
# regression surface this file exists for.
func _step_and_pump(ticks: int) -> Array[String]:
	var switched: Array[String] = []
	for _i in ticks:
		_sim.step()
		if not _reinstall_pending.is_empty():
			_reinstall_ticks -= 1
			if _reinstall_ticks <= 0:
				_rebake(_reinstall_pending)
				_reinstall_pending = ""
		for ev in _sim.drain_local_player_weapon_events():
			var name := (ev as PlayerWeaponEvent).switch_to_weapon
			if not name.is_empty():
				switched.append(name)
				_install(name)
				_reinstall_pending = name
				_reinstall_ticks = 2
	return switched


func _visual_ids() -> Array[int]:
	var out: Array[int] = []
	for v in _sim.get_throwable_visuals():
		out.append((v as ThrowableVisualRow).item_id)
	return out


func _visual_move_effect(item_id: int) -> String:
	for value in _sim.get_throwable_visuals():
		var visual := value as ThrowableVisualRow
		if visual.item_id == item_id:
			return visual.move_effect
	return ""


func _wait_for_idle(max_ticks: int) -> bool:
	for _i in max_ticks:
		var s := _sim.get_local_player_weapon_state()
		if s.current_action == 0 and s.next_action == 0:
			return true
		_step_and_pump(1)
	return false


func _boot_kit(kit: Array[WeaponKitEntry]) -> void:
	assert_true(_sim.apply_local_player_loadout(kit, 0), "loadout applied")
	var boot := _step_and_pump(2)
	if boot.is_empty():
		_install(_sim.get_local_player_weapon_name())
	assert_true(_wait_for_idle(150), "spawn draw-in settles")


func _switch_to(category: int, expect_name: String) -> void:
	_sim.request_local_player_weapon_category(category)
	var sw: Array[String] = []
	for _i in 150:
		sw.append_array(_step_and_pump(1))
		if sw.has(expect_name):
			break
	assert_has(sw, expect_name, "category %d commits %s" % [category, expect_name])
	assert_true(_wait_for_idle(150), "%s draw-in settles" % expect_name)
	_step_and_pump(20)  # human pause; also flushes the delayed re-install
	assert_eq(_sim.get_local_player_weapon_name(), expect_name,
			"equipped_adm_index stamped to %s" % expect_name)


var KIT_M4_GRENADE: Array[WeaponKitEntry] = [
	WeaponKitEntry.make("WPN_M4AUTO"),
	WeaponKitEntry.make("WPN_GRENADEHE"),
]
var KIT_M4_CLAYMORE: Array[WeaponKitEntry] = [
	WeaponKitEntry.make("WPN_M4AUTO"),
	WeaponKitEntry.make("WPN_CLAYMORE"),
]
var KIT_SATCHEL: Array[WeaponKitEntry] = [
	WeaponKitEntry.make("WPN_SATCHEL_CHARGE"),
]


# The core regression: a same-name install (the FP model resolve) landing while
# the outgoing SWITCHFROM holsters must leave the switch chain intact — the
# commit still fires and equipped_adm_index moves to the new weapon.
func test_reinstall_during_holster_preserves_the_switch() -> void:
	_boot_kit(KIT_M4_GRENADE)
	assert_eq(_sim.get_local_player_weapon_name(), "WPN_M4AUTO")
	_sim.request_local_player_weapon_category(5)
	_sim.step()  # the M4 FSM enters SWITCHFROM
	assert_eq(_sim.get_local_player_weapon_state().current_action, 7,
			"the holster is playing")
	_rebake("WPN_M4AUTO")  # the racing presentation late-bind
	assert_eq(_sim.get_local_player_weapon_state().current_action, 7,
			"the re-install must not reset the live action slot")
	var sw: Array[String] = []
	for _i in 150:
		sw.append_array(_step_and_pump(1))
		if sw.has("WPN_GRENADEHE"):
			break
	assert_has(sw, "WPN_GRENADEHE", "the displaced switch still commits")
	_wait_for_idle(150)
	assert_eq(_sim.get_local_player_weapon_name(), "WPN_GRENADEHE")


# A PowerThrow press inside the queued draw-in is refused (the fireable gate),
# and no charge byte leaks onto a later shot's wire slot_byte.
func test_windup_during_draw_leaves_no_stale_charge() -> void:
	_boot_kit(KIT_M4_GRENADE)
	_switch_to(5, "WPN_GRENADEHE")
	_switch_to(3, "WPN_M4AUTO")
	# Re-mount the grenade and press DURING the draw (before idle).
	_sim.request_local_player_weapon_category(5)
	var sw: Array[String] = []
	for _i in 150:
		sw.append_array(_step_and_pump(1))
		if sw.has("WPN_GRENADEHE"):
			break
	assert_has(sw, "WPN_GRENADEHE")
	_sim.set_local_player_weapon_input(true, true, false)
	_step_and_pump(3)
	_sim.set_local_player_weapon_input(false, false, false)
	_step_and_pump(3)
	assert_eq(_sim.get_local_player_weapon_state().fired_serial, 0,
			"the mid-draw press does not fire")
	# Settle, then a real tap-throw: the wire slot_byte must be the tap's 255,
	# not a leftover from the refused windup — and the fire must happen.
	assert_true(_wait_for_idle(150), "draw settles")
	_step_and_pump(20)
	_sim.set_local_player_weapon_input(true, true, false)
	_step_and_pump(5)
	_sim.set_local_player_weapon_input(false, false, false)
	var fired := false
	for _i in 120:
		_step_and_pump(1)
		if _sim.get_local_player_weapon_state().fired_serial == 1:
			fired = true
			break
	assert_true(fired, "the settled tap throws")
	assert_eq(_sim.get_local_player_weapon_state().last_round_slot_byte,
			255, "the tap rides the full charge byte")


func test_grenade_throw_then_m4_fires_bullets() -> void:
	_boot_kit(KIT_M4_GRENADE)
	assert_eq(_sim.get_local_player_weapon_name(), "WPN_M4AUTO", "spawn equip = M4")
	_switch_to(5, "WPN_GRENADEHE")

	# PowerThrow: tap (hold 5 ticks) = full charge; the fire row's delaystart 80
	# is the throw windup anim, so the round spawns ~80 ticks after release.
	_sim.set_local_player_weapon_input(true, true, false)
	_step_and_pump(5)
	_sim.set_local_player_weapon_input(false, false, false)
	var first_seen := -1
	for t in 200:
		_step_and_pump(1)
		if first_seen < 0 and _visual_ids().has(1883):
			first_seen = t
	assert_between(first_seen, 60, 100,
			"the thrown grenade flies with the TrcrID 1883 model after the windup")
	assert_true(_visual_ids().has(1883),
			"the production-thrown grenade survives through the pre-fuse flight window")

	_switch_to(3, "WPN_M4AUTO")

	# Fire the M4: no throwable-model round may appear (the flying grenade's own
	# 1883 entry persists until its fuse).
	var before := _visual_ids().count(1883)
	_sim.set_local_player_weapon_input(true, true, false)
	_step_and_pump(3)
	_sim.set_local_player_weapon_input(false, false, false)
	_step_and_pump(20)
	assert_eq(_visual_ids().count(1883), before,
			"an M4 shot must not spawn grenade-model rounds")


func test_grenade_ground_bounces_are_sound_only_until_the_fuse() -> void:
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load_from_resource_root(_root, "items.def"), OK)
	_sim.resolve_item_traits(item_db)

	var terrain := TerrainData.new()
	terrain.set_trn_path(TestFs.staged_tmap(TMAP_STAGE))
	assert_eq(terrain.load(), OK, "the committed Tmap terrain loads")
	assert_true(terrain.is_loaded())
	_sim.set_terrain_height_field(terrain)
	var ground := terrain.get_height_world_bilinear(Vector3.ZERO)
	assert_false(is_nan(ground), "the terrain covers the test origin")

	var slot := _sim.debug_spawn_round(
			Vector3(0, ground + 2.0, 0), Vector3.RIGHT, "grenadehe")
	assert_gte(slot, 0, "a production grenade round spawns over real terrain")
	var bounces: Array = []
	for _tick in 160:
		_sim.step()
		bounces.append_array(_sim.drain_round_impacts())

	assert_eq(bounces.size(), 5,
			"retail presents only the first five grenade contacts")
	for value in bounces:
		var bounce: RoundImpactRow = value
		assert_eq(bounce.effect, "",
				"a terrain bounce never submits Effect_FragGrndDirt")
		assert_eq(bounce.sound, "IMP_GREN_DIRT",
				"the retained bounce leg is the authored ground-impact sound")
	assert_true(_visual_ids().has(1883),
			"the grenade remains in flight until its fuse after bouncing")


func test_claymore_throw_then_m4_fires_bullets() -> void:
	_boot_kit(KIT_M4_CLAYMORE)
	_switch_to(7, "WPN_CLAYMORE")

	# Plain fire (no PowerThrow flag on the claymore).
	_sim.set_local_player_weapon_input(true, true, false)
	_step_and_pump(3)
	_sim.set_local_player_weapon_input(false, false, false)
	var first_seen := -1
	for t in 200:
		_step_and_pump(1)
		if first_seen < 0 and _visual_ids().has(1895):
			first_seen = t
	assert_gt(first_seen, -1, "the thrown claymore flies with the TrcrID 1895 model")

	_switch_to(3, "WPN_M4AUTO")

	var before := _visual_ids().count(1895)
	_sim.set_local_player_weapon_input(true, true, false)
	_step_and_pump(3)
	_sim.set_local_player_weapon_input(false, false, false)
	_step_and_pump(20)
	assert_eq(_visual_ids().count(1895), before,
			"an M4 shot must not spawn claymore-model rounds")


func test_satchel_loadout_can_switch_to_detonator() -> void:
	_boot_kit(KIT_SATCHEL)
	assert_eq(_sim.get_local_player_weapon_name(), "WPN_SATCHEL_CHARGE",
			"the selectable loadout row equips the satchel charge")

	var item_db := ItemDatabase.new()
	assert_eq(item_db.load_from_resource_root(_root, "items.def"), OK)
	_sim.resolve_item_traits(item_db)
	var terrain := TerrainData.new()
	terrain.set_trn_path(TestFs.staged_tmap(TMAP_STAGE))
	assert_eq(terrain.load(), OK)
	_sim.set_terrain_height_field(terrain)
	var ground := terrain.get_height_world_bilinear(Vector3.ZERO)
	assert_false(is_nan(ground))
	_sim.debug_set_entity_position(0, Vector3(0, ground + 1.8, 0))

	_sim.set_local_player_weapon_input(true, true, false)
	_step_and_pump(5)
	_sim.set_local_player_weapon_input(false, false, false)
	_step_and_pump(200)
	assert_gt(_sim.get_local_player_weapon_state().fired_serial, 0,
			"the satchel fire action completed")
	assert_true(_visual_ids().has(1891),
			"the terrain-backed satchel persists as the placed device")

	_switch_to(8, "WPN_SATCHEL_DETONATOR")


# The production smoke ammo reuses the flashbang's nade/nade TrcrID item.  Its
# five-second ARM boundary fires the obj row once (the witnessed smoke-pour
# start) but the projectile remains alive and harmless until its fuse expires.
# The same obj row presents again at actual expiry.
#
# The fuse length here is whatever the MOUNTED ammo.def authors.  This test loads
# the reference fixture set's def/ammo.def (RetailData.def_root()), a byte-exact
# copy of BASE JO: max_age 30 ->
# 1860 ticks.  The revx02 expansion re-authors that row to max_age 40 (and
# velocity 20), so a JO+revx02 mount resolves a 2480-tick fuse instead — a
# property of the mount order, not of the physics under test.  Pin the file this
# test actually reads rather than editing the retail extract to match the
# expansion (docs/adr/0003-no-raw-passthrough-create-from-scratch.md).
# [orig: Entity_UpdateGrenadePhysics @0x444908/@0x444976;
# world-wac-ai-re.md section 27.2/27.4]
func test_production_smoke_grenade_survives_arm_event_until_fuse() -> void:
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load_from_resource_root(_root, "items.def"), OK)
	assert_eq(item_db.get_graphic(101875), "Flsh_3rd",
			"the loose row mirrors the production smoke grenade model")
	assert_eq(item_db.get_ai_function(101875).to_lower(), "nade")
	assert_eq(item_db.get_move_function(101875).to_lower(), "nade")
	_sim.resolve_item_traits(item_db)
	var health_before := _sim.get_local_player_health()
	var slot := _sim.debug_spawn_round(
			Vector3(100, 100, 0), Vector3(1, 1, 0), "grenadesm")
	assert_gt(slot, -1, "the real ammo.def grenadesm row spawned")
	assert_eq(_visual_move_effect(1875), "Effect_SmokeToss",
			"the production effects_table move row is live from spawn")

	var arm_events: Array[RoundImpactRow] = []
	var move_effect_survived_arm := true
	for _tick in 312:
		_sim.step()
		move_effect_survived_arm = move_effect_survived_arm \
				and _visual_move_effect(1875) == "Effect_SmokeToss"
		for value in _sim.drain_round_impacts():
			var event := value as RoundImpactRow
			if event.sound == "EXPLO_SMOK_GREN":
				arm_events.append(event)

	assert_eq(arm_events.size(), 1, "the five-second arm boundary emits one presentation event")
	if arm_events.size() == 1:
		assert_eq(arm_events[0].effect, "",
				"the authored smoke obj row has no particle leg")
		assert_eq(arm_events[0].sound, "EXPLO_SMOK_GREN",
				"the arm boundary presents the authored smoke-pour sound")
	assert_true(_visual_ids().has(1875),
			"the smoke grenade remains alive after its five-second arm event")
	assert_true(move_effect_survived_arm,
			"the continuously attached smoke move effect survives arm_age")
	assert_eq(_visual_move_effect(1875), "Effect_SmokeToss",
			"arm_age does not retire the round-bound smoke trail")
	assert_eq(_sim.get_local_player_health(), health_before,
			"the arm event has no authoritative detonation consequence")

	var fuse_tick := -1
	var fuse_sounds := 0
	for tick in 2300:
		_sim.step()
		for value in _sim.drain_round_impacts():
			var event := value as RoundImpactRow
			if event.sound == "EXPLO_SMOK_GREN":
				fuse_sounds += 1
		if fuse_tick < 0 and not _visual_ids().has(1875):
			fuse_tick = tick + 312
			break
	assert_between(fuse_tick, 1850, 1870,
			"the smoke grenade expires on base JO's 30-second (1860-tick) fuse")
	assert_eq(fuse_sounds, 1, "the obj-row fuse sound is presented only at expiry")


# The windup exposure the HUD charge bar reads [orig: g_fireChargeStartTick ->
# HUD_DrawPowerThrowChargeBar @0x599830]: active only while held with ammo on a
# PowerThrow weapon, with the held tick count.
func test_windup_state_feeds_the_charge_bar() -> void:
	_boot_kit(KIT_M4_GRENADE)
	_switch_to(5, "WPN_GRENADEHE")
	assert_false(_sim.get_local_player_weapon_state().windup_active,
			"no windup before the press")
	_sim.set_local_player_weapon_input(true, true, false)
	_step_and_pump(40)
	var s := _sim.get_local_player_weapon_state()
	assert_true(s.windup_active, "held press winds up")
	assert_between(s.windup_held_ticks, 35, 45, "held ticks exposed")
	_sim.set_local_player_weapon_input(false, false, false)
	_step_and_pump(2)
	assert_false(_sim.get_local_player_weapon_state().windup_active,
			"release ends the windup")


func test_binocular_toggle_refuses_during_powerthrow_windup() -> void:
	_boot_kit(KIT_M4_GRENADE)
	_switch_to(5, "WPN_GRENADEHE")
	_sim.set_local_player_weapon_input(true, true, false)
	_step_and_pump(40)
	var wound := _sim.get_local_player_weapon_state()
	assert_true(wound.windup_active, "the grenade is charging")
	var fired_before := wound.fired_serial
	var rounds_before := wound.round_ring_count

	assert_false(_sim.request_local_player_binoculars_toggle(),
			"retail refuses binoculars during a live fire charge")
	assert_false(_sim.get_local_player_view().binoculars_requested)
	_sim.set_local_player_weapon_input(true, false, false)
	_step_and_pump(2)
	wound = _sim.get_local_player_weapon_state()
	assert_true(wound.windup_active,
			"the refusal preserves the still-held windup")
	assert_eq(wound.fired_serial, fired_before,
			"the optics request cannot release the grenade")

	_sim.set_local_player_weapon_input(false, false, false)
	var fired := false
	for _tick in 120:
		_step_and_pump(1)
		if _sim.get_local_player_weapon_state().fired_serial == fired_before + 1:
			fired = true
			break
	assert_true(fired, "the later real release fires exactly once")
	var released := _sim.get_local_player_weapon_state()
	assert_eq(released.round_ring_count, rounds_before + 1)
	assert_gt(released.last_round_slot_byte, 0,
			"the released round carries the accumulated PowerThrow charge")
