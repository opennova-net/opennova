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
	assert_eq(fuse_sounds, 0,
			"without a kill-zone class the expiry presents no second obj-row event")


# The windup exposure the HUD charge bar reads [orig: g_FireChargeStartTick ->
# HUD_DrawPowerThrowChargeBar @0x599830]: active only while held with ammo on a
# PowerThrow weapon, with the held tick count.
