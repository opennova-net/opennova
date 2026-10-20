class_name VehicleFixture
extends RefCounted

## The GUT vehicle suites' authored catalog and staging (ADR 0043 rule 11: the
## packaged world and the real Simulation over generated data). The models are
## the minted fixtures/gut set (tests/fixtures/gut_vehicle_gen.cpp) and the
## synthetic fixtures/threedi/synth set, each staged under the graphic name its
## items.def row names; the item, weapon and ammo rows are authored here with
## only the fields those suites read, the shipped value cited where a suite
## pins it. The item ids are the shipped ones of the vehicles the retail suites
## once crewed (items.def), so a reader can follow a suite back to its witness.
##
## - stage(): a flat root (items/weapon/ammo/gametext + every model), for the
##   sim-level suites (Simulation + ResourceRoot, MissionRoot).
## - stage_world(): the same catalog over the minimal pack
##   (WorldFixture.stage_minimal_root: mnml.bms on the Tmap relief), for the
##   world-level suites (GameWorld + LocalPlayerPresenter); boot() loads it with
##   the caller's entities placed.

const GUT_MODELS := "res://../fixtures/gut"
const SYNTH_MODELS := "res://../fixtures/threedi/synth"
const MINIMUZ_SOURCE := "res://../fixtures/particle/synth_minimal_effect.ptl"

# items.def ids (the shipped vehicles' ids).
const TANK := 100164        # Drivable M1A1 Tank
const TANK_T80 := 100165    # Drivable T80 Tank
const TANK_TURRET := 100166 # Turret for M1A1
const T80_TURRET := 100167  # Turret for T80
const TANK_ROOF_GUN := 100182 # Gun for M1A1 (the roof .50 cal, OnTurret)
const G_HELO := 100171      # Flyable Apache (addeweapG chin gun)
const G_CHIN_GUN := 100183  # the Apache's G child
const BLACKHAWK := 102010   # Flyable Blackhawk
const MINIGUN := 101871     # Minigun for the Blackhawk
const LITTLE_BIRD := 101306 # Flyable LITTLE BIRD with miniguns (an EWeap carrier)
const ATTACK_TRUCK := 101295 # Drivable Attack Vehicle with 50cal
const TRUCK_GUN := 101892   # the attack vehicle's 50 cal
const BUGGY := 101291       # Drivable Dune Buggy
const BUGGY_GUN := 101419   # NON-Armored Emplaced 50cal for FAV
const MARK_V := 101299      # Drivable Mark V
const MARK_V_GUN_A := 101991
const MARK_V_GUN_B := 101988
const TRIPOD := 101881      # 50cal on 360 tripod (E50triB)
const TARGET_TANK := 100169 # a tank course's target T80 (no turret child, low hp)
const TARGET_APC := 101213  # a tank course's target APC (07TR's APC wave id)
const RIDER := 102048       # an NPC rider (person)

## graphic name -> the committed model that stands in for it.
const MODELS := {
	"tank_turret": GUT_MODELS + "/tank_turret.3di",
	"turret": GUT_MODELS + "/cannon.3di",
	"roofgun": GUT_MODELS + "/fifty.3di",
	"fifty": GUT_MODELS + "/fifty.3di",
	"helo": GUT_MODELS + "/helo.3di",
	"minigun": GUT_MODELS + "/minigun.3di",
	"carrier": SYNTH_MODELS + "/carrier.3di",
	"mount": SYNTH_MODELS + "/mount.3di",
	"gunboat": GUT_MODELS + "/gunboat.3di",
	"tripod": GUT_MODELS + "/tripod_usegun_cradle.3di",
	# The driver's virtual display (`Virtualdisplay tankdrvr camera`): like the
	# shipped tankdrvr.3di, a model that authors a `camera` userpoint.
	"tankdrvr": SYNTH_MODELS + "/mount.3di",
	"t80_drvr": SYNTH_MODELS + "/mount.3di",
}

# The emplacement's inline-slot effect points, as every shipped EWeap row spells
# them (weaplbup/weaplmup/weaplcup and the right-hand trio).
const EWEAP_POINTS := """  weaplbup bullet
  weaplmup mflash01
  weaplcup bcasing
  weaprbup bullet
  weaprmup mflash01
  weaprcup bcasing
"""

# The turret's inline-slot points (Turret for M1A1: the bullet01 muzzle row).
const CANNON_POINTS := """  weaplbup bullet01
  weaplmup mflash01
  weaplcup bcasing
  weaprbup bullet01
  weaprmup mflash01
  weaprcup bcasing
"""

static func _eweap(name: String, id: int, graphic: String, weapon: String, attrib := "",
		points := EWEAP_POINTS) -> String:
	return """begin "%s"
  id %d
  type object
  graphic %s
  attrib: EWeap forceasset%s
  HP 10
  armor -1
  ai_function ewep
  move_function ewep
  render_function ewep
  disk_function EMPLACE
  primary_weapon %s
%send
""" % [name, id, graphic, attrib, weapon, points]


## The vehicle rows (shipped items.def shapes, trimmed to what the suites read:
## the classes, the motion block, the attachments, the virtual display), and
## the Player template row (105305, kPlayerInfantryTypeId 0x14B9 + the item id
## offset): a spawned player takes its items.def index as its +0x1C word, and a
## pool-0 source with a zero word builds no proximity slice, so USE would find
## no seat [orig: Entity_BuildProximityListsFromPools @0x4B8EF9..0x4B8F00].
static func items_text() -> String:
	# Row 0 is the null item, as it is in every shipped items.def: an entity's
	# +0x1C word is its row's index and 0 reads as "no item", so a vehicle on row
	# 0 would fail every ItemTypeIndex gate (the BMS player-mount triggers', the
	# script commands') [orig: Entity_IsLocalPlayerOnGunOfSsn @0x4f11e0 head].
	var text := """begin "Null"
  id 100000
  type marker
end

begin "Drivable M1A1 Tank"
  id 100164
  type vehicle
  sid Dm1a1
  graphic tank_turret
  Virtualdisplay tankdrvr camera
  input_function tank
  attrib: AIData neutral NoScar PlayerControl forceasset HasTurret
  ai_function cveh
  move_function ctank
  turn_rate 30
  turn_rate2 15
  acceleration 8
  deceleration 20
  player_speed 68
  slip_slope 70
  max_slope 70
  torque 7
  mass 135
  physics 1
  minai 1
  render_function tank
  unit_type 2
  hp 26000
  addeweap ewep01 100166
  addeweapC ewep02 100182
end

begin "Drivable T80 Tank"
  id 100165
  type vehicle
  sid Dt80
  graphic tank_turret
  Virtualdisplay t80_drvr camera
  input_function tank
  attrib: AIData neutral NoScar PlayerControl forceasset HasTurret
  ai_function cveh
  move_function ctank
  turn_rate 30
  turn_rate2 15
  acceleration 8
  deceleration 20
  player_speed 68
  slip_slope 70
  max_slope 70
  torque 7
  mass 135
  physics 1
  minai 1
  render_function tank
  unit_type 2
  hp 26000
  addeweap ewep01 100167
  addeweapC ewep02 100182
end

begin "Flyable Apache"
  id 100171
  type vehicle
  sid DAH64
  unit_type 3
  graphic helo
  attrib: noscar AIData PlayerControl forceasset EWeap
  ai_function chel
  disk_function HELO
  move_function chel
  turn_rate 50
  turn_rate2 35
  acceleration 25
  deceleration 60
  climb_speed 35
  player_speed 140
  slip_slope 30
  max_slope 35
  torque 3
  mass 17
  minai 1
  render_function chel
  hp 8000
  rotor_parts 1 2
  primary_weapon WPN_HELOROCKET
  weaplbup BULLET01
  weaplmup MFLASH01
  weaplcup bcasing01
  weaprbup BULLET01
  weaprmup MFLASH01
  weaprcup bcasing01
  addeweapG Ewep01 100183 70 10 100 100
end

begin "Flyable Blackhawk"
  id 102010
  sid Dblkhwk1
  type vehicle
  unit_type 3
  graphic helo
  attrib: noscar AIData PlayerControl PilotOnly forceasset
  ai_function chel
  disk_function HELO
  move_function chel
  turn_rate 50
  turn_rate2 35
  acceleration 25
  deceleration 60
  climb_speed 35
  player_speed 140
  slip_slope 30
  max_slope 35
  torque 3
  mass 17
  minai 1
  render_function chel
  hp 8000
  addeweap Ewep01 101871
  addeweap Ewep02 101871
  rotor_parts 1 2
end

begin "Flyable LITTLE BIRD with miniguns"
  id 101306
  sid DAH62
  type vehicle
  graphic helo
  disk_function HELO
  ai_function chel
  move_function chel
  turn_rate 72
  turn_rate2 60
  acceleration 20
  deceleration 40
  climb_speed 35
  player_speed 160
  slip_slope 30
  max_slope 35
  torque 3
  mass 5
  minai 1
  render_function chel
  hp 5000
  unit_type 3
  rotor_parts 1 2
  attrib: noscar AIData PlayerControl forceasset EWeap
  primary_weapon WPN_EMINIDFLT
  weaplbup BULLET01
  weaplmup MFLASH01
  weaplcup bcasing01
  weaprbup BULLET01
  weaprmup MFLASH01
  weaprcup bcasing01
end

begin "Drivable Attack Vehicle with 50cal"
  id 101295
  type vehicle
  sid Daav1
  graphic carrier
  hp 8000
  ai_function cveh
  move_function cveh
  turn_rate 50
  turn_rate2 25
  acceleration 15
  deceleration 30
  player_speed 78
  slip_slope 50
  max_slope 60
  physics 1
  mass 5
  torque 5
  minai 1
  render_function cveh
  attrib: PlayerControl
  unit_type 1
  addeweap ewep01 101892 17 48 180 180
end

begin "Drivable Dune Buggy"
  id 101291
  sid dbuggy1
  type vehicle
  graphic carrier
  hp 3000
  ai_function cveh
  render_function cveh
  move_function cveh
  attrib: AIData noscar neutral PlayerControl forceasset
  turn_rate 65
  turn_rate2 41
  acceleration 15
  deceleration 70
  player_speed 94
  slip_slope 50
  max_slope 60
  mass 3
  minai 1
  physics 1
  torque 9
  unit_type 1
  addeweap ewep01 101419 15 37 92 92
end

begin "Drivable Mark V"
  id 101299
  sid Dmrk51
  type vehicle
  unit_type 7
  graphic gunboat
  ai_function cbot
  move_function cbot
  render_function cveh
  hp 18000
  addeweap ewep01 101991 45 45 100 90
  addeweap ewep02 101988 45 45 90 90
  addeweap ewep03 101991 45 45 90 100
  addeweap ewep04 101988 45 45 90 90
end

begin "Training target T80"
  id 100169
  type vehicle
  sid Dt80
  graphic tank_turret
  attrib: AIData NoScar forceasset
  ai_function cveh
  move_function ctank
  render_function tank
  unit_type 2
  hp 3000
end

begin "Training target APC"
  id 101213
  type vehicle
  sid Dapc1
  graphic carrier
  attrib: AIData NoScar forceasset
  ai_function cveh
  move_function cveh
  render_function cveh
  turn_rate 50
  turn_rate2 25
  acceleration 15
  deceleration 30
  player_speed 60
  slip_slope 50
  max_slope 60
  physics 1
  mass 5
  torque 5
  minai 1
  unit_type 1
  hp 2000
end

begin "Player #1, Multiplayer"
  id 105305
  type person
  unit_type 9
  graphic US01
  anim_def US01
  hp 150
  ai_function plyr
  disk_function PLAYER
  move_function org2
end

begin "Rider"
  id 102048
  type person
  graphic US01
  anim_def US01
  hp 100
  ai_function org1
  move_function org1
end
"""
	text += _eweap("Turret for M1A1", TANK_TURRET, "turret", "WPN_M1TURRET", " IsTurret", CANNON_POINTS)
	text += _eweap("Turret for T80", T80_TURRET, "turret", "WPN_T80TURRET", " IsTurret", CANNON_POINTS)
	text += _eweap("Gun for M1A1", TANK_ROOF_GUN, "roofgun", "WPN_EMPLCD50", " OnTurret")
	text += _eweap("Apache chin gun", G_CHIN_GUN, "minigun", "WPN_HELOGUN")
	text += _eweap("Minigun for the Blackhawk", MINIGUN, "minigun", "WPN_EMPLCDMINI")
	text += _eweap("Attack vehicle 50cal", TRUCK_GUN, "fifty", "WPN_EMPLCD50NA")
	text += _eweap("NON-Armored Emplaced 50cal for FAV", BUGGY_GUN, "mount", "WPN_EMPLCD50NA")
	text += _eweap("Mark V minigun", MARK_V_GUN_A, "minigun", "WPN_EMPLCDMINI")
	text += _eweap("Mark V 50cal", MARK_V_GUN_B, "mount", "WPN_EMPLCD50")
	text += _eweap("50cal on 360 tripod", TRIPOD, "tripod", "WPN_EMPLCD50")
	return text


## One emplaced weapon row: the header the suites read and the seven actions
## every shipped emplaced row authors (idle, fire, recoil, reload, empty,
## switchto, switchfrom). `fields` carries the row's own keys; `fire_particle`
## the fire action's muzzle effect and its user point.
static func emplaced_weapon(name: String, rank: int, round_type: String, clip: int,
		start: int, fields: String, fire_particle := "", fire_delay_end := 1,
		recoil_delay_end := 7) -> String:
	var particle := ""
	if not fire_particle.is_empty():
		particle = "  particle %s\n  particleuserpoint mflash01\n" % fire_particle
	return """weapon "%s"
 category 11
 rank %d
 round_type %s
 clipsize %d
 startrounds %d
%s action "idle"
  function wpn_std_idle
  anim anim_wpn_idle
 end
 action "fire"
  delaystart 0
  delayend %d
  function wpn_std_fire
%s end
 action "recoil"
  delaystart 0
  delayend %d
  function wpn_std_recoil
 end
 action "reload"
  delayend 0
  function wpn_std_reload
 end
 action "empty"
  delayend 0
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
""" % [name, rank, round_type, clip, start, fields, fire_delay_end, particle, recoil_delay_end]


## The weapon table: the minimal pack's personal rifle (the player's kit) and
## every mounted gun the vehicles carry. The cannon rows carry the shipped
## optics: `scope_max_mag 10 2` (the maximum and the slot-init seed) and two
## sights rows, ForceScoped (weapon.def WPN_M1TURRET / WPN_T80TURRET).
static func weapon_text() -> String:
	var personal := FileAccess.get_file_as_string(RuntimeFixture.file("weapon.def"))
	var cannon_fields := """ targetpitchmax 10
 targetpitchmin 45
 targetyawrange 180
 emplacedstance 5
 launchuserpoint camera
 attachtextid attach_M1Turret
 flags sighted
 flags Emplaced
 flags ForceScoped
 flags NoCardSwitch
 scope_max_zero 10 100 200 1
 scope_max_mag 10 2
 sights cannon_irn.tga 0 0 1023 767 blend
 sights cannon_pip.tga 384 256 639 511 blend
"""
	var fifty_fields := """ targetpitchmax 15
 targetpitchmin 45
 targetyawrange 180
 launchuserpoint camera
 attachtextid attach_50cal
 flags auto
 flags Emplaced
 scope_max_zero 1 100 100 1
"""
	var gunner_fields := """ targetpitchmax 15
 targetpitchmin 45
 targetyawrange 180
 launchuserpoint camera
 attachtextid attach_gunner
 flags auto
 flags Emplaced
 flags sighted
 flags ForceScoped
 sights helo_irn.tga 0 0 1023 767 blend
 sights helo_pip.tga 480 351 543 416 blend
 scope_max_zero 1 200 200 1
"""
	var mini_fields := """ targetpitchmax 75
 targetpitchmin 12
 targetyawrange 75
 launchuserpoint camera
 attachtextid attach_minigun
 flags auto
 flags Emplaced
"""
	var pilot_mini_fields := mini_fields.replace("launchuserpoint camera", "launchuserpoint bullet")
	var rocket_fields := """ targetpitchmax 5
 targetpitchmin 5
 targetyawrange 5
 launchuserpoint bullet
 attachtextid attach_gunner
 flags auto
 flags Emplaced
 flags sighted
 flags ForceScoped
 sights helo_irn.tga 0 0 1023 767 blend
 sights helo_pip.tga 480 351 543 416 blend
"""
	return personal \
		+ emplaced_weapon("WPN_M1TURRET", 21, "M1Round", 1, 40, cannon_fields, "", 0, 30) \
		+ emplaced_weapon("WPN_T80TURRET", 22, "M1Round", 1, 40, cannon_fields.replace(
				"attach_M1Turret", "attach_T80Turret"), "", 0, 30) \
		+ emplaced_weapon("WPN_EMPLCD50", 0, "50cal", -1, 500, fifty_fields) \
		+ emplaced_weapon("WPN_EMPLCD50NA", 0, "50cal", -1, 500, fifty_fields) \
		+ emplaced_weapon("WPN_EMPLCDMINI", 1, "minirnds", -1, 500, mini_fields,
				"Effect_MiniMuz", 0, 7) \
		+ emplaced_weapon("WPN_EMINIDFLT", 16, "minirnds", 1000, 1000, pilot_mini_fields,
				"Effect_MiniMuz", 1, 1) \
		+ emplaced_weapon("WPN_HELOGUN", 25, "30mm", 1000, 1000, gunner_fields,
				"Effect_MiniMuz", 1, 7) \
		+ emplaced_weapon("WPN_HELOROCKET", 17, "ROCKET", 32, 32, rocket_fields, "", 0, 30)


static func _ammo(name: String, velocity: int, max_age: int, min_damage: int, max_damage: int) -> String:
	return """ammo %s
 velocity %d
 max_age %d
 drag 1
 min_damage %d
 max_damage %d
 penetration_impact 100
end
""" % [name, velocity, max_age, min_damage, max_damage]


## The pack's ammo plus one row per round type the mounted guns fire.
static func ammo_text() -> String:
	return FileAccess.get_file_as_string(RuntimeFixture.file("ammo.def")) \
		+ _ammo("50cal", 3000, 5, 80, 120) \
		+ _ammo("minirnds", 3000, 5, 40, 60) \
		+ _ammo("M1Round", 1200, 2, 3000, 4000) \
		+ _ammo("30mm", 2500, 6, 200, 300) \
		+ _ammo("ROCKET", 600, 10, 1000, 1500)


## The attach labels the gunner seats advertise: gametext.bin's `Overlays`
## section keys the weapons' attachtextid names (hud::attach_label_text). The
## two strings are the shipped ones (gametext.bin Overlays attach_50cal and
## attach_M1Turret), the labels the tank course's prompt pins.
const LABEL_50CAL := ".50 Cal"
const LABEL_CANNON := "120 MM Cannon"

static func gametext() -> RtxtStringFile:
	var text := RtxtStringFile.new()
	text.add_section("Overlays")
	text.add_entry("attach_50cal", LABEL_50CAL, 0, Vector2i())
	text.add_entry("attach_M1Turret", LABEL_CANNON, 0, Vector2i())
	text.add_entry("attach_T80Turret", LABEL_CANNON, 0, Vector2i())
	text.add_entry("attach_minigun", "Minigun", 0, Vector2i())
	text.add_entry("attach_gunner", "Gunner", 0, Vector2i())
	text.add_entry("STROVER_CONTROL", "Control", 0, Vector2i())
	text.add_entry("STROVER_SIT", "Sit", 0, Vector2i())
	text.add_entry("STROVER_USEGUN", "Use gun", 0, Vector2i())
	return text


## The muzzle effect the minigun rows name: the synthetic minimal effect
## re-identified as Effect_MiniMuz (the shipped fire action's particle).
static func write_effects(dir: String) -> void:
	var source := FileAccess.get_file_as_string(ProjectSettings.globalize_path(MINIMUZ_SOURCE))
	assert(source.contains("id = Buildup;"), "the synthetic effect authors its id")
	WorldFixture.write_file(dir.path_join("minimuz.ptl"),
			source.replace("id = Buildup;", "id = Effect_MiniMuz;"))


## The helicopter AI profile a helicopter row that names none falls back to
## [orig: Entity_InitHelicopterAIFromDef @0x4683C0, the "helo1.aip" arm
## @0x4684c9]: a type HELO profile (the brain's rotor twin runs only for type
## HELO, the `profile+0x10 == 1` gate @0x48fa98), its flight block at the
## shipped HELO1.AIP's values.
const HELO1_AIP := """type HELO
default_state HELO_PRETTY
patrol_speed 50
patrol_altitude 30
patrol_climb 15
combat_speed 60
combat_altitude 60
combat_climb 25
turn_rate 30
accel_time 3
"""


## The HUD layout over the catalog: the synthetic one-page font as both HUD
## fonts, every declutter row shown (HudFixture), one VEHICLE_HUD panel per
## vehicle sid, and the instrument rows the mounted HUD reads, at the shipped
## hudpos.def's values (the gear text and the silhouette fade it rides, the
## pilot's AGL ladder, the spin map).
static func hudpos_text() -> String:
	var rows := PackedStringArray([
		"fonthud1_hi Gunpl22b.fnt",
		"fonthud1_lo Gunpl22b.fnt",
		"HUDCHATTEXT 142 , 711",
		"HUDGeartext 1 , 578",
		"alphafade 30 50 3",
		"HUDAGLRADIUS 12",
		"HUDAGLTLRX 120,124",
		"HUDAGLYLEN 443,80",
		"HUDSPINMAPX1 810",
		"HUDSPINMAPX2 1020",
		"HUDSPINMAPY1 552",
		"HUDSPINMAPY2 762",
		"ShowImpactDistPos 506 420",
	])
	for panel: Array in [
		["Dm1a1", "driver 33,135", "emplace 2,35,180,52,180"],
		["Dt80", "driver 39,142", "emplace 2,39,167,54,183"],
		["Dblkhwk1", "driver 47,83", "emplace 2,64,102,47,102"],
		["DAH64", "driver 47,83", "emplace 1,64,102"],
		["DAH62", "driver 47,83", ""],
		["Daav1", "driver 16,196", "emplace 1,38,196"],
		["dbuggy1", "driver 16,196", "emplace 1,38,196"],
	]:
		rows.append("VEHICLE_HUD")
		rows.append("  sid " + String(panel[0]))
		rows.append("  interface panel_" + String(panel[0]).to_lower() + ".tga")
		rows.append("  " + String(panel[1]))
		if not String(panel[2]).is_empty():
			rows.append("  " + String(panel[2]))
		rows.append("VEHICLE_END")
	return "\r\n".join(rows) + "\r\n" + HudFixture.declutter_text()


static func _write_catalog(dir: String, items_prefix: String, items_suffix: String) -> void:
	var hud := FileAccess.open(dir.path_join("hudpos.def"), FileAccess.WRITE)
	assert(hud != null)
	hud.store_string(hudpos_text())
	hud.close()
	# The HUD fonts (the layout's two plus the label slots the overlay loads by
	# name, Impac22b the gear label's), each the synthetic one-page font.
	for font_name in ["Gunpl22b", "Arial12b", "Arial14b", "Arial14n", "Arial16b", "Arial16n",
			"Impac22b", "Impac38b"]:
		var font := FileAccess.open(dir.path_join(font_name + ".fnt"), FileAccess.WRITE)
		assert(font != null)
		font.store_buffer(FileAccess.get_file_as_bytes(HudFixture.FONT_FIXTURE))
		font.close()
	WorldFixture.write_file(dir.path_join("items.def"), items_prefix + items_text() + items_suffix)
	WorldFixture.write_file(dir.path_join("helo1.aip"), TestFs.crlf(HELO1_AIP))
	WorldFixture.write_file(dir.path_join("weapon.def"), weapon_text())
	WorldFixture.write_file(dir.path_join("ammo.def"), ammo_text())
	var game := FileAccess.open(dir.path_join("gametext.bin"), FileAccess.WRITE)
	assert(game != null)
	game.store_buffer(gametext().to_byte_array())
	game.close()
	for graphic: String in MODELS:
		var err := DirAccess.copy_absolute(ProjectSettings.globalize_path(MODELS[graphic]),
				dir.path_join(graphic + ".3di"))
		assert(err == OK, "model fixture copies: %s" % graphic)
	write_effects(dir)


## A flat root carrying the catalog (and the minimal pack's person rows, which
## the riders and the local player read). Callers remove it with
## TestFs.remove_dir_recursive.
static func stage(test: GutTest, label: String) -> String:
	var dir := TestFs.cache_dir(test, "vehicle_" + label)
	var pack_items := FileAccess.get_file_as_string(RuntimeFixture.file("items.def"))
	_write_catalog(dir, "", "\n" + pack_items)
	return dir


## The catalog over the minimal pack (mnml.bms on the Tmap relief). The rows
## lead items.def (a type id resolves to its first row).
static func stage_world(test: GutTest, label: String) -> String:
	var dir := WorldFixture.stage_minimal_root("vehicle_" + label, true)
	var pack_items := FileAccess.get_file_as_string(dir.path_join("items.def"))
	_write_catalog(dir, "", "\n" + pack_items)
	return dir


## The infantry clips the riders board and ride with: the synthetic person
## table (fixtures/anim/US01.adm: reset, idle and the emplaced rows over
## idle.bad) as the default infantry map E_STAND.adm and as the riders' own.
static func stage_infantry_clips(dir: String) -> void:
	for pair: Array in [["US01.adm", "E_STAND.adm"], ["US01.adm", "US01.adm"],
			["idle.bad", "idle.bad"]]:
		var err := DirAccess.copy_absolute(
				ProjectSettings.globalize_path("res://../fixtures/anim/" + String(pair[0])),
				dir.path_join(String(pair[1])))
		assert(err == OK, "infantry clip copies: %s" % pair[1])


## A ResourceRoot over a staged directory.
static func root_over(test: GutTest, dir: String) -> ResourceRoot:
	var root := ResourceRoot.new()
	test.assert_eq(root.set_root_dir(dir), OK)
	return root


## The staged root's terrain (mnml.trn over the Tmap relief the minimal pack's
## mnml.cpt carries), loaded on its own so placements can sit on the ground
## before the world loads.
static func terrain(test: GutTest, dir: String) -> TerrainData:
	var data := TerrainData.new()
	data.set_trn_path(dir.path_join("mnml.trn"))
	test.assert_eq(data.load(), OK, "the staged terrain loads")
	return data


## The ground height (mission z) at mission (x, y).
static func ground_z(terrain_data: TerrainData, x: float, y: float) -> float:
	return terrain_data.get_height_world_bilinear(Vector3(x, 0, -y))


## The packaged world over a stage_world() root with mnml.bms loaded and
## `placements` added first: rows of {item, pos (mission), yaw, kind}. Returns
## the world; `out_ids` receives each placement's bms id in order.
static func boot(test: GutTest, dir: String, placements: Array,
		out_ids: Array = []) -> GameWorld:
	var world := WorldFixture.make_world(test)
	var err := WorldFixture.load_mission(world, dir, WorldFixture.MINIMAL_MISSION,
			func(mission: MissionData) -> void:
				for row: Dictionary in placements:
					var added := mission.add_entity(int(row.get("kind", MissionData.KIND_ITEM)),
							int(row["item"]), row["pos"],
							Vector3(0, float(row.get("yaw", 0.0)), 0))
					out_ids.append(added.bms_id if added != null else -1))
	test.assert_eq(err, OK, "the vehicle world loads from %s" % dir)
	world.set_process(false)
	return world
