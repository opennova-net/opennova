extends GutTest

const WORLD_TEST_ROOT := "game_world_test"
const ArmoryPresenter := preload("res://game/world/armory_presenter.gd")


func after_each() -> void:
	for staged_dir in _staged_dirs:
		TestFs.remove_dir_recursive(staged_dir)
	_staged_dirs.clear()
	TestFs.remove_dir_recursive(OS.get_cache_dir().path_join(WORLD_TEST_ROOT))


# (The Node runtime/sim doubles that used to live here — TransportRuntimeStub,
# ProfilingRuntimeStub, FxRuntimeStub, ItemPoseRuntimeStub, the anchor/blink/
# occlusion/joiner stubs — are gone: GameWorld._runtime is typed MissionRoot
# and MissionRoot._sim is typed Simulation, so every runtime-consuming
# test now boots the REAL stack through the public load path. The presentation
# doubles followed (ADR 0043 rule 11): the viewmodel/prewarm placer stubs and
# their GameWorld harnesses, the FxWorldStub/ImpactAudioStub recording sinks,
# the ItemFxDirectorProbe/ItemFxGameWorldHarness pair and the warm-pass stubs
# are replaced by the packaged world booted over staged roots (WorldFixture)
# and read through EffectWorld.get_debug_group_report,
# MissionAudio.recent_fired_soundsets and the world's typed local-player seams.)

# The WorldFixture roots this file staged (removed in after_each).
var _staged_dirs: Array[String] = []


# Register a staged WorldFixture root for after_each cleanup.
func _staged(root_dir: String) -> String:
	_staged_dirs.append(root_dir)
	return root_dir


# The committed minimal pack (the default root of every bare mission load).
static var _fx_data_cache: Dictionary = {}

const FX_MOUNT_GRAPHIC := "FxMount"  # mount.3di: MFlash01 + live PANM -> individual node
const FX_GUN_GRAPHIC := "FxGun"  # gun.3di: MFlash01, inert -> static population
const FX_SHED_GRAPHIC := "FxShed"  # shed.3di: no MFlash01, inert -> static population
# A long-lived fixture effect (60 s emit) for the persistent item attaches and
# a short one for the origin-fallback rows, so each row identifies itself by
# name in the effect world's report.
const FX_PERSISTENT_EFFECT := "Buildup"
const FX_FALLBACK_EFFECT := "synth_dust"


const IMPACT_EFFECT := "synth_dirt_hit"
const IMPACT_SOUND := "imp_bullet_dirt"
const SOUND_ONLY_SOUND := "imp_gren_dirt"
const IMPACT_AMMO_DEF := """
ammo AT_NULL
	velocity            0
	max_age             0
	drag                1
	min_damage          0
	max_damage          0
end

ammo AM_556MM
	velocity            3000
	max_age             300
	drag                1
	weight_in_grains    62
	min_damage          25
	max_damage          40
	penetration_impact  100
	light_move          6.0 128 120 80
	light_impact        10.0 255 192 96 0.2
	effects_table
		dirt          synth_dirt_hit    imp_bullet_dirt   15
		grass         synth_dirt_hit    imp_bullet_dirt   15
		snow          synth_dirt_hit    imp_bullet_dirt   15
		cement        synth_dirt_hit    imp_bullet_dirt   15
		sand          synth_dirt_hit    imp_bullet_dirt   15
		packeddirt    synth_dirt_hit    imp_bullet_dirt   15
		stone         synth_dirt_hit    imp_bullet_dirt   15
		mud           synth_dirt_hit    imp_bullet_dirt   15
		water         synth_dirt_hit    imp_bullet_dirt   15
		uwaterdeep    synth_dirt_hit    imp_bullet_dirt   15
		uwatershallow synth_dirt_hit    imp_bullet_dirt   15
		uwatersurface synth_dirt_hit    imp_bullet_dirt   15
	end
end

ammo AM_SOUNDONLY
	velocity            3000
	max_age             300
	drag                1
	min_damage          1
	max_damage          1
	effects_table
		dirt          none    imp_gren_dirt   15
		grass         none    imp_gren_dirt   15
		snow          none    imp_gren_dirt   15
		cement        none    imp_gren_dirt   15
		sand          none    imp_gren_dirt   15
		packeddirt    none    imp_gren_dirt   15
		stone         none    imp_gren_dirt   15
		mud           none    imp_gren_dirt   15
		water         none    imp_gren_dirt   15
		uwaterdeep    none    imp_gren_dirt   15
		uwatershallow none    imp_gren_dirt   15
		uwatersurface none    imp_gren_dirt   15
	end
end
"""

# A render-frame delta that always banks at least one 62.5 Hz logic tick
# (TICK_DT itself can round to zero ticks in the native accumulator).
const ONE_TICK_DELTA := 0.02


# Stage the minimal fixture over the synthetic Tmap terrain plus the staged
# impact ammo, the .ptl catalog its effects intern from, and a sound bank
# carrying the two impact sets. The minimal mnml map is flat, so rounds would
# never ground on it; Tmap carries the relief the round flight can actually hit.
const BUILDING_ITEM_DEF := "\r\nbegin \"Guard Tower\"\r\n  id 102001\r\n  type building\r\n  graphic GuardTwr1\r\n  sid guardtwr1\r\n  anim_def GuardTwr1\r\n  husk GuardTwr1X\r\n  hp 5000\r\nend\r\n"


func test_armory_can_reuse_game_world_weapon_database_on_first_open() -> void:
	Strings.clear()
	# The retail weapon.mnu, weapon.def and string tables come from the
	# reference fixture set (docs/asset-gated-tests.md).
	var staged := {
		"mnu/jo_weapon.mnu": "weapon.mnu",
		"def/weapon.def": "weapon.def",
		"rtxt/menutxt.bin": "menutxt.BIN",
		"rtxt/gametext.bin": "gametext.bin",
	}
	for rel in staged:
		if RetailData.fixture(rel).is_empty():
			pending(RetailData.fixture_pending_text(rel))
			return
	var root_dir := _staged(WorldFixture.stage_minimal_root("first_armory_open"))
	for rel in staged:
		var target := root_dir.path_join(staged[rel])
		if FileAccess.file_exists(target):
			assert_eq(DirAccess.remove_absolute(target), OK)
		assert_eq(DirAccess.copy_absolute(RetailData.fixture(rel), target), OK)

	var world := WorldFixture.make_world(self)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(root_dir), OK)
	world.set_resource_root(root)
	var loadout := PlayerSpawnLoadout.new()
	loadout.primary = "WPN_M4AUTO"
	loadout.accessory = "WPN_SATCHEL_CHARGE"
	loadout.player_class = 8
	world.set_local_player_spawn_loadout(loadout)
	assert_eq(world.load_mission("mnml.bms"), OK)

	var weapons: WeaponDatabase = world.get_weapon_database()
	assert_not_null(weapons, "first armory open lazily resolves weapon.def")
	if weapons == null:
		return
	assert_true(weapons.is_loaded())
	assert_gte(weapons.find_weapon("WPN_M4AUTO"), 0)
	assert_gte(weapons.find_weapon("WPN_SATCHEL_CHARGE"), 0)
	var sim := world.get_sim()
	var expected_names := ["WPN_M4AUTO", "WPN_SATCHEL_CHARGE"]
	var before_names: Array[String] = []
	for value in sim.get_local_player_loadout():
		before_names.append((value as WeaponKitEntry).name)
	assert_eq(before_names, expected_names,
		"the production world promoted the PLAYER_INFO-style canonical profile")

	var overlay := Control.new()
	add_child_autofree(overlay)
	overlay.size = Vector2(800, 600)
	var presenter := ArmoryPresenter.new()
	add_child_autofree(presenter)
	presenter.setup(world.armory_view(), null, overlay)
	assert_true(presenter.open(), "the production world catalog reaches first armory open")
	assert_not_null(overlay.get_node_or_null("ArmoryMenu"))
	var driver: MenuDriver = presenter.get_menu_driver()
	assert_not_null(driver)
	assert_gt(driver.selected_row(driver.widget_id("PRIMARY")), 0,
		"the current primary is not NONE on first visit")
	assert_gt(driver.selected_row(driver.widget_id("ACCESSORY")), 0,
		"the current satchel is not NONE on first visit")

	driver.widget_activated.emit(driver.widget_id("ACCEPT"), "ACCEPT")
	var after_names: Array[String] = []
	for value in sim.get_local_player_loadout():
		after_names.append((value as WeaponKitEntry).name)
	assert_eq(after_names, expected_names,
		"accepting the untouched first-open rows preserves the exact canonical kit")
	world.unload()
	Strings.clear()


const VIEWMODEL_WEAPON_ROW := """
weapon "%s"
	category 1
	rank     0
	statid   %d
	clipsize    30
	maxclips    7
	startrounds 210
	ammoclass   CLASS_556MM 1
	round_type  AM_556MM
	flags       %s
%s	pos		10.0		0.0		-201.0			0.0		0.0		1.0
	TPOS	-28.046		21.531		-187.857		0.0		0.0		0.0

	ACTION	"IDLE"
	DELAYEND	auto
	ANIM		ANIM_WPN_IDLE
	FUNCTION	WPN_STD_IDLE
	END

	ACTION	"EMPTYIDLE"
	DELAYEND	auto
	ANIM		ANIM_WPN_IDLE
	FUNCTION	WPN_STD_IDLE
	END

	ACTION	"FIRE"
	DELAYEND	5
	ANIM		ANIM_WPN_FIRE
	FUNCTION	WPN_STD_FIRE
	END

	ACTION	"RECOIL"
	DELAYEND	0
	ANIM		ANIM_WPN_RECOIL
	FUNCTION	WPN_STD_RECOIL
	END

	ACTION	"RELOAD"
	DELAYSTART	196
	DELAYEND	auto
	ANIM		ANIM_WPN_RELOAD
	FUNCTION	WPN_STD_RELOAD
	END

	ACTION	"EMPTY"
	DELAYSTART	0
	DELAYEND	auto
	ANIM		ANIM_WPN_EMPTY
	FUNCTION	WPN_STD_EMPTY
	END

	ACTION	"SWITCHTO"
	DELAYSTART	1
	DELAYEND	1
	ANIM		ANIM_WPN_SWITCHRANK
	FUNCTION	WPN_STD_SWITCHTO
	END

	ACTION	"SWITCHFROM"
	DELAYSTART	1
	DELAYEND	1
	ANIM		ANIM_WPN_SWITCHFROM
	FUNCTION	WPN_STD_SWITCHFROM
	END

	ACTION	"SWITCHRANK"
	DELAYSTART	1
	DELAYEND	1
	ANIM		ANIM_WPN_SWITCHRANK
	FUNCTION	WPN_STD_SWITCHRANK
	END
end
"""
const VIEWMODEL_GUN_GRAPHIC := "TestGun"  # gun.3di under the WPN_TEST gfx1 name
# person.3di under the arms name Avatars.def carries (with its extension, as
# the registry names its parts).
const VIEWMODEL_ARMS_GRAPHIC := "TestArms.3di"
const VIEWMODEL_ARMS_CAMO := Vector3i(17, 34, 51)
# The player's character registry: retail's ONLY first-person arms source is
# the selected combo's arms part, so one good-side combo binds the synthetic
# person head/body + the TestArms arms with an authored raw camo triplet.
const VIEWMODEL_AVATARS_DEF := """define head STAGED_HEAD
{
	graphic person.3di
	camo 0 0 0
	voice 1
	sex m
}
define body STAGED_BODY
{
	graphic person.3di
	camo 0 0 0
}
define arms STAGED_ARMS
{
	graphic TestArms.3di
	camo 17 34 51
}
nationality 0 STAGED_NAT
{
	alignment good
	division 0 STAGED_DIV
	{
		combo 1 STAGED_HEAD STAGED_BODY STAGED_ARMS
	}
}
"""


# Stage the minimal pack plus the two viewmodel weapon rows and the synthetic
# gun/arms/person models; `with_character` adds the Avatars.def combo the local
# player's arms resolve through (without it no character resolves: gun alone).
