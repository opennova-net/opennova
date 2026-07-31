extends GutTest

# Throwable weapon-switch + PowerThrow lifecycle against the committed JO defs,
# driven the way the game shells drive NovaSimulation (loadout -> switch walk ->
# commit events -> def installs, including the FP model resolve's delayed
# same-weapon re-install). Pins the PR #282 field bugs:
#  - a same-name install landing during a SWITCHFROM must not destroy the
#    switch chain (it desynced weapon_def_ from equipped_adm_index — the
#    "rifle fires the last-thrown ammo" bug),
#  - a PowerThrow press during the draw-in must not leak a latched charge
#    onto a later shot,
#  - the thrown grenade flies as its TrcrID item and dies by fuse, never by
#    ground contact.
const DEF_FIXTURES := "res://../fixtures/def"
const TERRAIN_FIXTURE := "res://../fixtures/godot/dvxi5/Dvxi5.trn"

var _sim: NovaSimulation = null
var _db: NovaWeaponDatabase = null
var _root: NovaResourceRoot = null
var _reinstall_pending := ""
var _reinstall_ticks := 0


func before_each() -> void:
	_reinstall_pending = ""
	_reinstall_ticks = 0
	var def_root := ProjectSettings.globalize_path(DEF_FIXTURES)
	assert_true(DirAccess.dir_exists_absolute(def_root), "committed def fixtures exist")
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	_sim = NovaSimulation.new()
	assert_true(_sim.load_from_mission_data(md))
	assert_true(_sim.spawn_local_player(Vector3(0, 0, 0), 0.0, 1))
	_root = NovaResourceRoot.new()
	assert_eq(_root.set_root_dir(def_root), OK)
	assert_eq(_sim.load_weapon_table(_root, "weapon.def"), OK)
	assert_eq(_sim.load_ammo_table(_root, "ammo.def"), OK)
	_db = NovaWeaponDatabase.new()
	assert_eq(_db.load_from_resource_root(_root, "weapon.def"), OK)


func after_each() -> void:
	if _sim != null:
		_sim.free()
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
			var name := String((ev as Dictionary).get("switch_to_weapon", ""))
			if not name.is_empty():
				switched.append(name)
				_install(name)
				_reinstall_pending = name
				_reinstall_ticks = 2
	return switched


func _visual_ids() -> Array[int]:
	var out: Array[int] = []
	for v in _sim.get_throwable_visuals():
		out.append(int((v as Dictionary).get("item_id", 0)))
	return out


func _visual_move_effect(item_id: int) -> String:
	for value in _sim.get_throwable_visuals():
		var visual := value as Dictionary
		if int(visual.get("item_id", 0)) == item_id:
			return String(visual.get("move_effect", ""))
	return ""


func _wait_for_idle(max_ticks: int) -> bool:
	for _i in max_ticks:
		var s: Dictionary = _sim.get_local_player_weapon_state()
		if int(s.get("current", -1)) == 0 and int(s.get("next", -1)) == 0:
			return true
		_step_and_pump(1)
	return false


func _boot_kit(kit: Array[Dictionary]) -> void:
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


const KIT_M4_GRENADE: Array[Dictionary] = [
	{"name": "WPN_M4AUTO", "ammo_primary": -1, "ammo_secondary": -1, "flags": -1},
	{"name": "WPN_GRENADEHE", "ammo_primary": -1, "ammo_secondary": -1, "flags": -1},
]
const KIT_M4_CLAYMORE: Array[Dictionary] = [
	{"name": "WPN_M4AUTO", "ammo_primary": -1, "ammo_secondary": -1, "flags": -1},
	{"name": "WPN_CLAYMORE", "ammo_primary": -1, "ammo_secondary": -1, "flags": -1},
]
const KIT_SATCHEL: Array[Dictionary] = [
	{"name": "WPN_SATCHEL_CHARGE", "ammo_primary": -1, "ammo_secondary": -1, "flags": -1},
]


# The core regression: a same-name install (the FP model resolve) landing while
# the outgoing SWITCHFROM holsters must leave the switch chain intact — the
# commit still fires and equipped_adm_index moves to the new weapon.
func test_reinstall_during_holster_preserves_the_switch() -> void:
	_boot_kit(KIT_M4_GRENADE)
	assert_eq(_sim.get_local_player_weapon_name(), "WPN_M4AUTO")
	_sim.request_local_player_weapon_category(5)
	_sim.step()  # the M4 FSM enters SWITCHFROM
	assert_eq(int(_sim.get_local_player_weapon_state().get("current", -1)), 7,
			"the holster is playing")
	_rebake("WPN_M4AUTO")  # the racing presentation late-bind
	assert_eq(int(_sim.get_local_player_weapon_state().get("current", -1)), 7,
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
	assert_eq(int(_sim.get_local_player_weapon_state().get("fired_serial", -1)), 0,
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
		if int(_sim.get_local_player_weapon_state().get("fired_serial", 0)) == 1:
			fired = true
			break
	assert_true(fired, "the settled tap throws")
	assert_eq(int(_sim.get_local_player_weapon_state().get("last_round_slot_byte", -1)),
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
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load_from_resource_root(_root, "items.def"), OK)
	_sim.resolve_item_traits(item_db)

	var terrain := NovaTerrainData.new()
	terrain.set_trn_path(ProjectSettings.globalize_path(TERRAIN_FIXTURE))
	assert_eq(terrain.load(), OK, "the committed Dvxi5 terrain loads")
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
		var bounce: Dictionary = value
		assert_eq(String(bounce.get("effect", "")), "",
				"a terrain bounce never submits Effect_FragGrndDirt")
		assert_eq(String(bounce.get("sound", "")), "IMP_GREN_DIRT",
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

	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load_from_resource_root(_root, "items.def"), OK)
	_sim.resolve_item_traits(item_db)
	var terrain := NovaTerrainData.new()
	terrain.set_trn_path(ProjectSettings.globalize_path(TERRAIN_FIXTURE))
	assert_eq(terrain.load(), OK)
	_sim.set_terrain_height_field(terrain)
	var ground := terrain.get_height_world_bilinear(Vector3.ZERO)
	assert_false(is_nan(ground))
	_sim.debug_set_entity_position(0, Vector3(0, ground + 1.8, 0))

	_sim.set_local_player_weapon_input(true, true, false)
	_step_and_pump(5)
	_sim.set_local_player_weapon_input(false, false, false)
	_step_and_pump(200)
	assert_gt(int(_sim.get_local_player_weapon_state().get("fired_serial", 0)), 0,
			"the satchel fire action completed")
	assert_true(_visual_ids().has(1891),
			"the terrain-backed satchel persists as the placed device")

	_switch_to(8, "WPN_SATCHEL_DETONATOR")


func test_grenade_round_survives_its_flight_until_the_fuse() -> void:
	var slot := _sim.debug_spawn_round(Vector3(0, 10, 0), Vector3(1, 1, 0), "grenadehe")
	assert_gt(slot, -1, "grenadehe spawned")
	var expired_tick := -1
	for t in 300:
		_sim.step()
		for ev in _sim.get_round_debug().get("events", []):
			var d := ev as Dictionary
			if int(d.get("kind", -1)) == 4 and expired_tick < 0:  # kExpired
				expired_tick = t
	assert_between(expired_tick, 240, 260, "the fuse, not an impact, ends the round")


# The production smoke ammo reuses the flashbang's nade/nade TrcrID item.  Its
# five-second ARM boundary fires the obj row once (the witnessed smoke-pour
# start) but the projectile remains alive and harmless until its fuse expires.
# The same obj row presents again at actual expiry.
#
# The fuse length here is whatever the MOUNTED ammo.def authors.  This test loads
# fixtures/def/ammo.def, which is a byte-exact copy of BASE JO: max_age 30 ->
# 1860 ticks.  The revx02 expansion re-authors that row to max_age 40 (and
# velocity 20), so a JO+revx02 mount resolves a 2480-tick fuse instead — a
# property of the mount order, not of the physics under test.  Pin the file this
# test actually reads rather than editing the retail extract to match the
# expansion (docs/adr/0003-no-raw-passthrough-create-from-scratch.md).
# [orig: Entity_UpdateGrenadePhysics @0x444908/@0x444976;
# world-wac-ai-re.md section 27.2/27.4]
func test_production_smoke_grenade_survives_arm_event_until_fuse() -> void:
	var item_db := NovaItemDatabase.new()
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

	var arm_events: Array[Dictionary] = []
	var move_effect_survived_arm := true
	for _tick in 312:
		_sim.step()
		move_effect_survived_arm = move_effect_survived_arm \
				and _visual_move_effect(1875) == "Effect_SmokeToss"
		for value in _sim.drain_round_impacts():
			var event := value as Dictionary
			if String(event.get("sound", "")) == "EXPLO_SMOK_GREN":
				arm_events.append(event)

	assert_eq(arm_events.size(), 1, "the five-second arm boundary emits one presentation event")
	if arm_events.size() == 1:
		assert_eq(String(arm_events[0].get("effect", "")), "",
				"the authored smoke obj row has no particle leg")
		assert_eq(String(arm_events[0].get("sound", "")), "EXPLO_SMOK_GREN",
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
			var event := value as Dictionary
			if String(event.get("sound", "")) == "EXPLO_SMOK_GREN":
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
	assert_false(bool(_sim.get_local_player_weapon_state().get("windup_active", true)),
			"no windup before the press")
	_sim.set_local_player_weapon_input(true, true, false)
	_step_and_pump(40)
	var s: Dictionary = _sim.get_local_player_weapon_state()
	assert_true(bool(s.get("windup_active", false)), "held press winds up")
	assert_between(int(s.get("windup_held_ticks", 0)), 35, 45, "held ticks exposed")
	_sim.set_local_player_weapon_input(false, false, false)
	_step_and_pump(2)
	assert_false(bool(_sim.get_local_player_weapon_state().get("windup_active", true)),
			"release ends the windup")


func test_binocular_toggle_refuses_during_powerthrow_windup() -> void:
	_boot_kit(KIT_M4_GRENADE)
	_switch_to(5, "WPN_GRENADEHE")
	_sim.set_local_player_weapon_input(true, true, false)
	_step_and_pump(40)
	var wound: Dictionary = _sim.get_local_player_weapon_state()
	assert_true(bool(wound.get("windup_active", false)), "the grenade is charging")
	var fired_before := int(wound.get("fired_serial", 0))
	var rounds_before := int(wound.get("round_ring_count", 0))

	assert_false(_sim.request_local_player_binoculars_toggle(),
			"retail refuses binoculars during a live fire charge")
	assert_false(bool(_sim.get_local_player_view().get("binoculars_requested", true)))
	_sim.set_local_player_weapon_input(true, false, false)
	_step_and_pump(2)
	wound = _sim.get_local_player_weapon_state()
	assert_true(bool(wound.get("windup_active", false)),
			"the refusal preserves the still-held windup")
	assert_eq(int(wound.get("fired_serial", -1)), fired_before,
			"the optics request cannot release the grenade")

	_sim.set_local_player_weapon_input(false, false, false)
	var fired := false
	for _tick in 120:
		_step_and_pump(1)
		if int(_sim.get_local_player_weapon_state().get("fired_serial", 0)) == fired_before + 1:
			fired = true
			break
	assert_true(fired, "the later real release fires exactly once")
	var released: Dictionary = _sim.get_local_player_weapon_state()
	assert_eq(int(released.get("round_ring_count", -1)), rounds_before + 1)
	assert_gt(int(released.get("last_round_slot_byte", 0)), 0,
			"the released round carries the accumulated PowerThrow charge")
