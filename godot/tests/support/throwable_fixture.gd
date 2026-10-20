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
	var made := DirAccess.make_dir_recursive_absolute(_directory)
	assert(made == OK, "the throwable fixture directory is creatable")
	var weapon :="ammoclass_max_carry CLASS_556MM 1000\n"
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
	var copied := DirAccess.copy_absolute(ProjectSettings.globalize_path(ItemDbFixture.FIXTURE_ITEMS),
			_directory.path_join("items.def"))
	assert(copied == OK, "the committed items table copies beside the authored defs")
	return _directory


## The production smoke grenade's tables: the shipped weapon.def WPN_GRENADESM
## and ammo.def grenadesm rows, cut to the fields the throw, flight, arm event
## and fuse read (category 6, the PowerThrow grenade class, the 65-tick FIRE
## preparation; TrcrID 1875, max_age 30, arm_age 5, velocity 30, drag 1, the
## move/obj effects_table rows), beside an M4AUTO row a spawn stamps by default.
## Staged once per run through DefFixture.stage_catalog.
static var _smoke_directory := ""


static func smoke_directory() -> String:
	if not _smoke_directory.is_empty():
		return _smoke_directory
	var weapon := "ammoclass_max_carry CLASS_556MM 1000\n"
	weapon += "ammoclass_max_carry CLASS_GRENADESM 1000\n"
	weapon += DefFixture.weapon_row("WPN_M4AUTO", 3, 0, 30, "auto").replace(
			" loadout_subclasses 1\n", "")
	weapon += SMOKE_WEAPON
	_smoke_directory = DefFixture.stage_catalog("smoke", weapon,
			DefFixture.ammo_text() + SMOKE_AMMO)
	return _smoke_directory


# [shipped weapon.def `weapon "WPN_GRENADESM"`: category 6, rank 0, clipsize 1,
#  maxclips 3, startrounds 3, round_type "GRENADESM", ammoclass CLASS_GRENADESM 1,
#  weapon_class grenade, flags NoClipsNoDraw / PowerThrow, FIRE delaystart 65,
#  the 1/1 switch actions]
const SMOKE_WEAPON := """weapon "WPN_GRENADESM"
 category 6
 rank 0
 teamfilter red
 teamfilter blue
 charfilter rifleman
 charfilter engineer
 loadout_selectable 1
 loadout_subclasses 0
 maxclips 3
 weapon_class grenade
 flags NoClipsNoDraw
 flags PowerThrow
 round_type GRENADESM
 clipsize 1
 startrounds 3
 ammoclass CLASS_GRENADESM 1
 launchuserpoint bullet
 ACTION "IDLE"
 DELAYEND auto
 ANIM anim_wpn_idle
 FUNCTION WPN_STD_IDLE
 END
 ACTION "EMPTYIDLE"
 DELAYSTART auto
 ANIM anim_wpn_empty_idle
 FUNCTION WPN_STD_EMPTYIDLE
 END
 ACTION "FIRE"
 DELAYSTART 65
 DELAYEND auto
 ANIM anim_wpn_fire
 FUNCTION WPN_STD_FIRE
 END
 ACTION "RECOIL"
 DELAYEND auto
 ANIM anim_wpn_recoil
 FUNCTION WPN_STD_RECOIL
 END
 ACTION "RELOAD"
 DELAYEND auto
 ANIM anim_wpn_reload
 FUNCTION WPN_STD_RELOAD
 END
 ACTION "EMPTY"
 DELAYSTART 0
 DELAYEND auto
 FUNCTION WPN_STD_EMPTY
 END
 ACTION "SWITCHTO"
 DELAYSTART 1
 DELAYEND 1
 ANIM anim_wpn_switchto
 FUNCTION WPN_STD_SWITCHTO
 END
 ACTION "SWITCHFROM"
 DELAYSTART 1
 DELAYEND 1
 ANIM anim_wpn_switchfrom
 FUNCTION WPN_STD_SWITCHFROM
 END
 ACTION "SWITCHRANK"
 DELAYSTART 1
 DELAYEND 1
 ANIM anim_wpn_switchrank
 FUNCTION WPN_STD_SWITCHRANK
 END
end
"""

# [shipped ammo.def `ammo grenadesm`: frndlyTrcrID 1875, max_age 30, arm_age 5,
#  velocity 30, min_stable_velocity 0, error 1, tumble_error 0, drag 1, the
#  useownmove/IgnorFoilage/forcetracer/priority/ClipWaterFX flags, no kztype;
#  effects_table move Effect_SmokeToss (no sound), obj EXPLO_SMOK_GREN 80 (no
#  particle)]. max_age 30 is base JO's 1860-tick fuse; arm_age 5 the 310-tick
#  arm boundary.
const SMOKE_AMMO := """ammo grenadesm
 frndlyTrcrID 1875
 max_age 30
 arm_age 5
 velocity 30
 min_stable_velocity 0
 error 1
 tumble_error 0
 drag 1
 min_damage 0
 max_damage 0
 penetration_impact 0
 penetration_kz 0
 flag useownmove
 flag IgnorFoilage
 flag forcetracer
 flag priority
 flag ClipWaterFX
 effects_table
 move Effect_SmokeToss none 0
 obj none EXPLO_SMOK_GREN 80
 end
end
"""


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
