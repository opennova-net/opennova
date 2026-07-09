extends GutTest

# The weapon typed records (ADR 0017): PlayerWeaponView decodes the sim's FSM state
# dict (net-re §5.62) and PlayerViewmodelDef carries the new ADS/FSM def fields
# (flags / scope_max_mag / clipsize) the host gates on.


func test_weapon_view_decodes_state_dict() -> void:
	var view := PlayerWeaponView.from_state_dict({
		"active": true,
		"current": 3,
		"anim_key": "anim_wpn_fire",
		"play_serial": 7,
		"fired_serial": 5,
		"dry_serial": 1,
		"reload_serial": 2,
		"unscope_serial": 1,
		"rescope_serial": 1,
		"clip": 24,
		"reserve": 270,
		"kick": 12,
	})
	assert_not_null(view)
	assert_eq(view.current_action, 3)
	assert_eq(view.anim_key, "anim_wpn_fire")
	assert_eq(view.play_serial, 7)
	assert_eq(view.fired_serial, 5)
	assert_eq(view.dry_serial, 1)
	assert_eq(view.reload_serial, 2)
	assert_eq(view.clip, 24)
	assert_eq(view.reserve, 270)
	assert_eq(view.kick, 12)


func test_weapon_view_null_when_inactive() -> void:
	assert_null(PlayerWeaponView.from_state_dict({}))
	assert_null(PlayerWeaponView.from_state_dict({ "active": false }))


func test_viewmodel_def_carries_fsm_fields() -> void:
	var def := PlayerViewmodelDef.from_weapon_dict({
		"name": "WPN_AK47AUTO",
		"gfx1": "ak47_1st",
		"gfx1a": "armsG",
		"animadm": "ak47_1st",
		"flags": 0x102,          # auto | sighted
		"scope_max_mag": 2.0,
		"clipsize": 30,
	})
	assert_not_null(def)
	assert_eq(def.flags, 0x102)
	assert_almost_eq(def.scope_max_mag, 2.0, 0.001)
	assert_eq(def.clipsize, 30)
