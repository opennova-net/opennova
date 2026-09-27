extends GutTest

# The weapon typed records (ADR 0017): PlayerViewmodelDef carries the ADS/FSM def
# fields (flags / scope_max_mag / clipsize) the sim gates on. (PlayerWeaponView and
# PlayerWeaponEvent are native records now — the sim's own getters are the contract.)


func test_viewmodel_def_carries_fsm_fields() -> void:
	# The slice decodes the WeaponDef record field for field (the record's
	# getters are the weapon.def parse; a synthetic dict no longer exists).
	var weapon_path := DefFixture.directory().path_join("weapon.def")
	var wdb := WeaponDatabase.new()
	assert_eq(wdb.load(weapon_path), OK, "the authored weapon.def loads")
	var index := wdb.find_weapon("WPN_M4AUTO")
	assert_gte(index, 0, "the fixture carries WPN_M4AUTO")
	var weapon := wdb.get_weapon(index)
	var def := PlayerViewmodelDef.from_weapon_def(weapon)
	assert_not_null(def)
	assert_eq(def.weapon_name, "WPN_M4AUTO")
	assert_eq(def.gfx1, weapon.gfx1)
	assert_eq(def.animadm, weapon.animadm)
	assert_eq(def.flags, weapon.flags)
	assert_almost_eq(def.scope_max_mag, weapon.scope_max_mag, 0.001)
	assert_eq(def.clipsize, weapon.clipsize)
	assert_gt(def.clipsize, 0, "the M4 authors a magazine")
	assert_null(PlayerViewmodelDef.from_weapon_def(null), "no weapon decodes to null")