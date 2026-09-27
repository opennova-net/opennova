class_name ThrowableFixture
extends RefCounted

## Authored action delays and ammo rows for the binding's switch/rebake
## regression scenarios. Motor behavior is also covered without files in the
## native throwables and weapon_fsm tests.
static var _directory := ""


static func directory() -> String:
	if not _directory.is_empty():
		return _directory
	_directory = ProjectSettings.globalize_path("res://../.godot-test-fixtures/throwable")
	assert(DirAccess.make_dir_recursive_absolute(_directory) == OK)
	var weapon := "ammoclass_max_carry CLASS_556MM 1000\n"
	weapon += DefFixture.weapon_row("WPN_M4AUTO", 3, 0, 30, "auto").replace(
			" loadout_subclasses 1\n", "")
	var grenade := DefFixture.weapon_row("WPN_GRENADEHE", 5, 0, 1, "powerthrow", "grenadehe")
	# A refused press must span multiple draw ticks; an accepted tap must wait
	# through the 80-tick fire preparation before its round appears.
	grenade = grenade.replace('ACTION "SWITCHTO"\n DELAYSTART 1\n DELAYEND 1',
			'ACTION "SWITCHTO"\n DELAYSTART 8\n DELAYEND 8')
	grenade = grenade.replace('ACTION "FIRE"\n DELAYSTART 0',
			'ACTION "FIRE"\n DELAYSTART 80')
	weapon += grenade
	weapon += DefFixture.weapon_row("WPN_CLAYMORE", 7, 0, 1, "", "claymore")
	var satchel := DefFixture.weapon_row("WPN_SATCHEL_CHARGE", 7, 1, 3, "", "satchel")
	satchel = satchel.replace(" clipsize 3", " loadout_subclasses 1\n clipsize 3")
	weapon += satchel
	weapon += DefFixture.weapon_row("WPN_SATCHEL_DETONATOR", 8, 0, 1, "", "AMMO_DETONATOR")
	WorldFixture.write_file(_directory.path_join("weapon.def"), weapon)
	WorldFixture.write_file(_directory.path_join("ammo.def"), DefFixture.ammo_text() + AMMO)
	assert(DirAccess.copy_absolute(ProjectSettings.globalize_path(ItemDbFixture.FIXTURE_ITEMS),
			_directory.path_join("items.def")) == OK)
	return _directory


const AMMO := """ammo grenadehe
 velocity 30
 max_age 4
 drag 1
 flag useownmove
 flag forcetracer
 FrndlyTrcrID 1883
 FoeTrcrID 1883
 effects_table
 dirt Effect_FragGrndDirt IMP_GREN_DIRT 0
 end
end
ammo claymore
 velocity 2
 max_age 1
 drag 1
 flag useownmove
 flag noage
 flag forcetracer
 FrndlyTrcrID 1895
 FoeTrcrID 1895
end
ammo satchel
 velocity 6
 max_age 1
 drag 1
 flag useownmove
 flag noage
 flag forcetracer
 FrndlyTrcrID 1891
end
ammo AMMO_DETONATOR
 flag detonatesatchels
end
"""
