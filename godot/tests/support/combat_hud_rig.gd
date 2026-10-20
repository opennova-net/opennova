class_name CombatHudRig
extends RefCounted

## The launcher/mortar combat-HUD rig the HUD deploy test
## (tests/hud_installed_assets_test.gd) and its windowed pixel leg
## (tests/windowed/hud_installed_assets_pixels_test.gd) share: three authored
## weapon and ammo rows staged over the minimal world, their sight art minted as
## TGAs beside them, and a real GameHudPresenter drawing into a SubViewport.
## The rows are the shipped Escalation weapon.def / ammo.def rows (WPN_JAVELIN,
## WPN_STINGER, WPN_MORTAR; JAVELIN, STINGER, AMMO_60MM_MORTAR) trimmed to the
## fields the deploy reads: the flags, the sights rows and their rectangles,
## the optics, the actions and their delays, the ballistics the mortar's impact
## preview flies. Every helper that asserts takes the GutTest so the verdict
## lands in the calling test's report.

const NAMES: Array[String] = ["WPN_JAVELIN", "WPN_STINGER", "WPN_MORTAR"]

# The sight art the rows name, minted as opaque white TGAs.
const SIGHT_TEXTURES := ["jav_pip.tga", "jav_dot.tga", "stg_pip.tga", "stg_dot.tga"]

const WEAPONS := """
weapon "WPN_JAVELIN"
 category 7
 rank 7
 teamfilter red
 teamfilter blue
 charfilter engineer
 flags OnlyFireLocked
 flags OnlyFireScoped
 flags onlylockscoped
 flags Scoped
 flags ShowRange
 flags ViewLock
 loadout_selectable 1
 maxclips 1
 weapon_class accessory
 round_type JAVELIN
 clipsize 1
 startrounds 1
 targetpitchmax 22
 targetpitchmin 50
 targetyawrange 15
 launchuserpoint bullet
 special_hold 8
 ammoclass CLASS_JAVELIN 1
 sights jav_pip.tga 171 40 856 725 blend
 sights jav_dot.tga 497 369 528 400 blend
 scope_max_mag 2
 action "idle"
  delaystart 1
  delayend 1
  function wpn_std_idle
 end
 action "emptyidle"
  delaystart 1
  delayend 1
  function wpn_std_emptyidle
 end
 action "fire"
  delayend auto
  function wpn_std_fire
 end
 action "recoil"
  delaystart 1
  delayend auto
  function wpn_std_recoil
 end
 action "reload"
  delaystart 260
  function wpn_std_reload
 end
 action "empty"
  delaystart 0
  delayend auto
  function wpn_std_empty
 end
 action "switchto"
  delaystart 1
  delayend 1
  function wpn_std_switchto
 end
 action "switchfrom"
  delaystart 1
  delayend 1
  function wpn_std_switchfrom
 end
end

weapon "WPN_STINGER"
 category 7
 rank 4
 teamfilter red
 teamfilter blue
 charfilter engineer
 flags OnlyFireLocked
 flags OnlyFireScoped
 flags onlylockscoped
 flags Scoped
 loadout_selectable 1
 maxclips 2
 weapon_class accessory
 round_type STINGER
 clipsize 1
 startrounds 2
 targetpitchmax 22
 targetpitchmin 50
 targetyawrange 15
 launchuserpoint bullet
 special_hold 4
 ammoclass CLASS_STINGER 1
 sights stg_pip.tga 157 45 864 752 blend
 sights stg_dot.tga 481 383 544 414 blend
 scope_max_mag 2
 action "idle"
  delaystart 1
  delayend 1
  function wpn_std_idle
 end
 action "emptyidle"
  delaystart 1
  delayend 1
  function wpn_std_emptyidle
 end
 action "fire"
  delayend auto
  function wpn_std_fire
 end
 action "recoil"
  delaystart 1
  delayend auto
  function wpn_std_recoil
 end
 action "reload"
  delaystart 240
  function wpn_std_reload
 end
 action "empty"
  delaystart 0
  delayend auto
  function wpn_std_empty
 end
 action "switchto"
  delaystart 1
  delayend 1
  function wpn_std_switchto
 end
 action "switchfrom"
  delaystart 1
  delayend 1
  function wpn_std_switchfrom
 end
end

weapon "WPN_MORTAR"
 category 7
 rank 3
 teamfilter red
 teamfilter blue
 charfilter engineer
 loadout_selectable 1
 maxclips 4
 weapon_class accessory
 round_type AMMO_60MM_MORTAR
 clipsize 1
 startrounds 4
 error 4 4 4 4 4 4
 targetpitchmax 85
 targetpitchmin -45
 targetyawrange 15
 ammoclass CLASS_60MM_MORTAR 1
 flags AbsorbPitch
 flags 2DImpact
 flags Sighted
 flags NoMove
 flags NoCardSwitch
 flags ForceCrouch
 flags OnlyScoped
 flags OnlyFireScoped
 flags UseDesignator
 flags UseSpreadTwo
 flags ShowImpactDist
 launchuserpoint bullet
 action "idle"
  delayend auto
  function wpn_std_idle
 end
 action "emptyidle"
  delaystart auto
  function wpn_std_emptyidle
 end
 action "fire"
  delayend auto
  function wpn_std_fire
 end
 action "recoil"
  delaystart 60
  function wpn_std_recoil
 end
 action "reload"
  delaystart 120
  function wpn_std_reload
 end
 action "scopeup"
  delaystart 60
  function wpn_std_scopeup_map
 end
 action "scopedown"
  delaystart 60
  function wpn_std_scopedown_map
 end
 action "empty"
  delaystart 0
  delayend auto
  function wpn_std_empty
 end
 action "switchto"
  delaystart 1
  delayend 1
  function wpn_std_switchto
 end
 action "switchfrom"
  delaystart 1
  delayend 1
  function wpn_std_switchfrom_map
 end
end
"""

const AMMO := """
ammo JAVELIN
 max_age 30
 arm_age .5
 velocity 100
 drag .75
 weight_in_grains 65000
 min_damage 23000
 max_damage 23000
 penetration_impact 100
 bullet_radius 0.05
 flag LAWR
 flag NoGravity
end

ammo STINGER
 max_age 10
 arm_age .5
 velocity 85
 drag 1
 weight_in_grains 16000
 min_damage 3000
 max_damage 3000
 penetration_impact 20
 bullet_radius 0.05
 flag LAWR
 flag NoGravity
end

ammo AMMO_60MM_MORTAR
 max_age 200
 arm_age .14
 velocity 163
 drag 0.05
 weight_in_grains 15000
 min_damage 0
 max_damage 0
 penetration_impact 60
 kz_minradius 6.0
 kz_maxradius 12.0
 kz_damage 205
 bullet_radius 0.03
 flag useownmove
end
"""

var art: ResourceRoot
var weapons: WeaponDatabase
var stage := ""
var world: GameWorld
var sim: Simulation
var viewport: SubViewport
var camera: Camera3D
var background: ColorRect
var ui: Control
var presenter: GameHudPresenter
var hud: HudOverlay
var card: HudSightsCard


## The rig: the rows staged over the minimal world with the vehicle HUD
## layout, the world booted and settled for 90 ticks, the presenter with its
## real overlay configured from the staged hudpos.def. release() removes the
## stage.
static func boot(test: GutTest) -> CombatHudRig:
	var rig := CombatHudRig.new()
	rig.stage = HudFixture.stage_root(true)
	var staged_weapons := FileAccess.get_file_as_string(rig.stage.path_join("weapon.def"))
	var staged_ammo := FileAccess.get_file_as_string(rig.stage.path_join("ammo.def"))
	WorldFixture.write_file(rig.stage.path_join("weapon.def"), staged_weapons + WEAPONS)
	WorldFixture.write_file(rig.stage.path_join("ammo.def"), staged_ammo + AMMO)
	var layout_file := FileAccess.open(rig.stage.path_join("hudpos.def"), FileAccess.WRITE)
	layout_file.store_string(VehicleFixture.hudpos_text())
	layout_file.close()
	for texture: String in SIGHT_TEXTURES:
		TestFs.write_bytes(test, rig.stage.path_join(texture),
				TestFs.tga_bytes(Vector2i(16, 16), Color.WHITE))
	rig.art = ResourceRoot.new()
	test.assert_eq(rig.art.set_root_dir(rig.stage), OK)
	rig.weapons = WeaponDatabase.new()
	test.assert_eq(rig.weapons.load_from_resource_root(rig.art, "weapon.def"), OK)
	for name: String in NAMES:
		test.assert_gte(rig.weapons.find_weapon(name), 0, name + " has an authored definition")
	var layout := HudPos.new()
	test.assert_eq(layout.load_from_resource_root(rig.art, "hudpos.def"), OK)
	rig.world = WorldFixture.boot_minimal(test, rig.stage)
	rig.world.set_process(false)
	rig.sim = rig.world.get_sim()
	for _i in range(90):
		rig.sim.step()
	rig.viewport = SubViewport.new()
	rig.viewport.size = Vector2i(1024, 768)
	rig.viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	test.add_child_autofree(rig.viewport)
	rig.camera = Camera3D.new()
	rig.viewport.add_child(rig.camera)
	rig.camera.keep_aspect = Camera3D.KEEP_WIDTH
	rig.camera.make_current()
	rig.background = ColorRect.new()
	rig.background.size = rig.viewport.size
	rig.background.color = Color(0.12, 0.18, 0.22)
	rig.viewport.add_child(rig.background)
	rig.ui = Control.new()
	rig.ui.size = rig.viewport.size
	rig.viewport.add_child(rig.ui)
	rig.presenter = GameHudPresenter.new()
	test.add_child_autofree(rig.presenter)
	rig.presenter.setup(rig.world, null, rig.ui)
	rig.presenter.ensure_game_hud()
	rig.presenter.set_hud_detail_level(0)
	rig.hud = rig.presenter.get_game_hud()
	rig.hud.configure(layout, rig.art)
	rig.card = rig.hud.get_node("SightsCard") as HudSightsCard
	return rig


## Select `name`, let its draw settle, raise its sights, aim the camera down
## the sim's view and hand the card the weapon's authored sights: the deploy
## the HUD tests walk per weapon.
func deploy(test: GutTest, name: String) -> void:
	test.assert_true(world.set_local_player_weapon_by_name(name))
	for _i in range(90):
		sim.step()
	presenter.tick()
	test.assert_false(card.is_card_up(), name + " starts lowered")
	test.assert_true(sim.request_local_player_scope_toggle())
	for _i in range(100):
		sim.step()
	var view := sim.get_local_player_view()
	camera.global_position = view.camera_eye
	camera.look_at(view.camera_eye + Simulation.presentation_forward(
			view.camera_yaw_deg, view.camera_pitch_deg), Vector3.UP)
	camera.fov = view.fov_h_deg
	presenter.tick()
	test.assert_true(world.local_player_weapon_view().aimed_shot_available,
			name + " finishes raising")
	var weapon := weapons.get_weapon(weapons.find_weapon(name))
	card.set_weapon_sights(weapon.get_sights(), art)


## Lower the raised sights again and settle the presenter.
func lower(test: GutTest, name: String) -> void:
	test.assert_true(sim.request_local_player_scope_toggle())
	for _i in range(100):
		sim.step()
	presenter.tick()
	test.assert_false(card.is_card_up(), name + " hides its card when lowered")


## Resize the viewport, its background and the UI layer together.
func resize(size: Vector2i) -> void:
	viewport.size = size
	ui.size = size
	background.size = size


## Remove the staged rows (the world and the nodes are autofreed by the test).
func release() -> void:
	if art != null:
		art.clear()
	if not stage.is_empty():
		TestFs.remove_dir_recursive(stage)
		stage = ""
