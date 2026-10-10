// The def catalogs' members in a modder's words (def_words.h): a row per member of each record kind,
// in def_fields' order, each meaning cited by its witness (the RE records, the runtime's cited
// consumers); "Unknown: ..." where none is witnessed.
#include "def_words.h"

#include <cstring>
#include <iterator>

namespace opennova::editor {
namespace {

using def::DefRecordKind;

const DefWords kItemWords[] = {
	{"powerup_def", "Powerup row", "Powerup", "The powerup.def row this item binds at mission start; setting it makes the item a powerup, and an unknown name removes the item.", "[orig: PowerupEntity_InitFromDef @ 0x442D00]"},
	{"score", "Kill score", "Attributes", "The unit score booked when this item is killed; zero means killing it is never scored.", "[orig: Score_ProcessKillEvent @ 0x4FD400]"},
	{"graphic_enemy", "Enemy model", "Looks", "Unknown: the game loads this model at mission start, but no reader of the loaded model is witnessed.", "[orig: EntityDef_LoadModelsAndCallbacks @ 0x439f50]"},
	{"text_id", "Text key", "Identity", "Unknown: the parser stores the text key; no reader is witnessed.", "itemdef-re.md, Catalog authoring (the property table)"},
	{"display_name", "Name", "Identity", "Not a key: the name on the begin line; the game keeps 46 characters, and a lookup by name finds the first match, ignoring case.", "[orig: ItemDef_ParseProperty @ 0x49eb00, the begin arm's cut @ 0x49ebfb; ItemList_FindIndexByPrimaryName @ 0x49e010]"},
	{"id", "Item id", "Identity", "The type id missions place this item by; when two items share an id, the first in the file is used.", "[orig: ItemList_FindIndexByTypeId @ 0x49e100]"},
	{"sid", "Short id", "Identity", "The name hudpos.def vehicle panel blocks use to give this item its mounted HUD panel (silhouette and seat boxes).", "[orig: HUD_ParseHudposToken @ 0x59F370]"},
	{"type", "Item type", "Identity", "The item's kind (vehicle, decoration, person, marker, building, powerup, effect); loading treats persons and effects specially, and FARP rearming serves vehicles only.", "[orig: EntityDef_LoadModelsAndCallbacks @ 0x439f50]"},
	{"type_word", "Type written as", "Identity", "The word the type line writes where the game reads two alike: decoration or foliage, powerup or object. The game keeps the kind alone; retail's trees and bushes say foliage, its crates and objects object.", "[orig: ItemDef_ParseProperty @ 0x49eb00, the type chain @ 0x4a02e4..0x4a04b7]"},
	{"graphic", "Model", "Looks", "The item's model, loaded at mission start and given to each of its entities; its sitex, ctrlx and UseGun points become seats and controls.", "[orig: EntityDef_LoadModelsAndCallbacks @ 0x439f50]"},
	{"anim_def", "Animation map", "Looks", "The .adm animation map the item's model plays; a missing file falls back to default.adm.", "[orig: AnimMap_LoadAdmFile @ 0x40cc40]"},
	{"husk", "Wreck model", "Death and wreck", "The wreck model drawn and hit-tested after death; its KZ points place the death blasts, and it gives debris when no final wreck exists.", "[orig: Render_SectorEntity @ 0x5C4190]; [orig: Entity_QueueKzBlastAtUserPoints @ 0x4eabf0]"},
	{"hp", "Health", "Health and armour", "How much damage the item takes before it dies; copied to the entity's health when it spawns.", "[orig: Entity_InitFromItemDef @ 0x49e550]"},
	{"sound_profile", "Sound profile", "Sound", "The SndProf.def profile that gives the item its default sounds: footsteps, screams, engine, loops, doors, death and time-of-day sounds.", "[orig: Entity_GetProfileSlotSound @ 0x528300]"},
	{"default_aip", "Default AI profile", "Behaviour", "The .aip AI profile a ground vehicle, boat or train uses when the mission names none (helicopters ignore it); setting it also sets AIData.", "[orig: Entity_InitVehicleAIFromDef @ 0x4686C0]"},
	{"sound_profile_female", "Female sound profile", "Sound", "Used instead of the sound profile for a female character's body sounds; it follows the sound profile unless set.", "[orig: Entity_GetProfileSlotSound @ 0x528300]"},
	{"soundloops[0]", "Sound loop 1", "Sound", "Looping sound: an ambient marker's 4am to 10am loop, a ground vehicle's or boat's idle engine, a helicopter's climb rotor loop.", "[orig: Entity_UpdateEnvSoundEmitter @ 0x4A8080]; [orig: Entity_ProcessMovementSoundEffects @ 0x5294A0]; [orig: VehicleEffect_UpdateEmissions @ 0x528F20]"},
	{"soundloops[1]", "Sound loop 2", "Sound", "Looping sound: an ambient marker's 10am to 5pm loop, a ground vehicle's or boat's forward-drive loop, a helicopter's medium-speed rotor loop.", "[orig: Entity_UpdateEnvSoundEmitter @ 0x4A8080]; [orig: Entity_ProcessMovementSoundEffects @ 0x5294A0]; [orig: VehicleEffect_UpdateEmissions @ 0x528F20]"},
	{"soundloops[2]", "Sound loop 3", "Sound", "Looping sound: an ambient marker's 5pm to 9pm loop, a ground vehicle's or boat's reverse-drive loop, a helicopter's cruise rotor loop.", "[orig: Entity_UpdateEnvSoundEmitter @ 0x4A8080]; [orig: Entity_ProcessMovementSoundEffects @ 0x5294A0]; [orig: VehicleEffect_UpdateEmissions @ 0x528F20]"},
	{"soundloops[3]", "Sound loop 4", "Sound", "Looping sound: an ambient marker's 9pm to 4am loop; a tank's turn-on-the-spot loop, its volume following the turn rate.", "[orig: Entity_UpdateEnvSoundEmitter @ 0x4A8080]; [orig: Entity_ProcessMovementSoundEffects @ 0x5294A0]"},
	{"soundloops[4]", "Sound loop 5", "Sound", "Unknown: resolved with the item like the other loops; no reader is witnessed.", "[orig: ItemDef_ResolveAllResources @ 0x49E5F0]; vehicle-client-movers-re.md section 29"},
	{"soundloops[5]", "Sound loop 6", "Sound", "Unknown: resolved with the item like the other loops; no reader is witnessed.", "[orig: ItemDef_ResolveAllResources @ 0x49E5F0]; vehicle-client-movers-re.md section 29"},
	{"soundloops[6]", "Sound loop 7", "Sound", "Unknown: resolved with the item like the other loops; no reader is witnessed.", "[orig: ItemDef_ResolveAllResources @ 0x49E5F0]; vehicle-client-movers-re.md section 29"},
	{"nightshot", "Night sound", "Sound", "A one-shot sound the item replays at random intervals from 9pm to 4am; a name not found silences the profile's default.", "[orig: Entity_SpawnRegionalEffect @ 0x408290]"},
	{"dawnshot", "Dawn sound", "Sound", "A one-shot sound the item replays at random intervals from 4am to 10am; a name not found silences the profile's default.", "[orig: Entity_SpawnRegionalEffect @ 0x408290]"},
	{"duskshot", "Dusk sound", "Sound", "A one-shot sound the item replays at random intervals from 5pm to 9pm; a name not found silences the profile's default.", "[orig: Entity_SpawnRegionalEffect @ 0x408290]"},
	{"dayshot", "Day sound", "Sound", "A one-shot sound the item replays at random intervals from 10am to 5pm; a name not found silences the profile's default.", "[orig: Entity_SpawnRegionalEffect @ 0x408290]"},
	{"shot_delay_ticks[0][0]", "Dawn delay", "Sound", "Dawn: the base seconds between the item's sound attempts; the profile's timing replaces it when the profile gives the dawn sound. particletesttime also sets it.", "[orig: Entity_SpawnRegionalEffect @ 0x408290]"},
	{"shot_delay_ticks[0][1]", "Dawn random delay", "Sound", "Dawn: up to this many random extra seconds added to the wait between sound attempts.", "[orig: Entity_SpawnRegionalEffect @ 0x408290]"},
	{"shot_delay_ticks[1][0]", "Day delay", "Sound", "Day: the base seconds between the item's sound attempts; the profile's timing replaces it when the profile gives the day sound.", "[orig: Entity_SpawnRegionalEffect @ 0x408290]"},
	{"shot_delay_ticks[1][1]", "Day random delay", "Sound", "Day: up to this many random extra seconds added to the wait between sound attempts.", "[orig: Entity_SpawnRegionalEffect @ 0x408290]"},
	{"shot_delay_ticks[2][0]", "Dusk delay", "Sound", "Dusk: the base seconds between the item's sound attempts; the profile's timing replaces it when the profile gives the dusk sound.", "[orig: Entity_SpawnRegionalEffect @ 0x408290]"},
	{"shot_delay_ticks[2][1]", "Dusk random delay", "Sound", "Dusk: up to this many random extra seconds added to the wait between sound attempts.", "[orig: Entity_SpawnRegionalEffect @ 0x408290]"},
	{"shot_delay_ticks[3][0]", "Night delay", "Sound", "Night: the base seconds between the item's sound attempts; the profile's timing replaces it when the profile gives the night sound.", "[orig: Entity_SpawnRegionalEffect @ 0x408290]"},
	{"shot_delay_ticks[3][1]", "Night random delay", "Sound", "Night: up to this many random extra seconds added to the wait between sound attempts.", "[orig: Entity_SpawnRegionalEffect @ 0x408290]"},
	{"destroy_timing_ticks[0]", "Destroy delay", "Death and wreck", "Seconds after the item becomes a wreck before its staged destroy animation starts; armed again when a vehicle respawns.", "[orig: Entity_InitFromItemDef @ 0x49e550]; [orig: Entity_PublishSwapFadePhases @ 0x5C3F40]"},
	{"destroy_timing_ticks[1]", "Destroy phase length", "Death and wreck", "Seconds each of the five destroy phases driving the wreck's destroy animation lasts (0 uses 50 ticks).", "[orig: Entity_PublishSwapFadePhases @ 0x5C3F40]"},
	{"destroy_timing_ticks[2]", "Destroy phase stagger", "Death and wreck", "Seconds between the starts of successive destroy phases (0 uses 25 ticks).", "[orig: Entity_PublishSwapFadePhases @ 0x5C3F40]"},
	{"ai_function", "AI class", "Behaviour", "Picks the item's class: its death and damage handler, its network update format and, for vehicles, the helicopter or ground AI setup.", "[orig: Entity_LookupRenderCallbacks @ 0x407dc0]"},
	{"move_function", "Movement class", "Behaviour", "Picks the movement code the item runs every tick (cveh, ctank, cbot, chel, cpln, nade and others).", "[orig: EntityDef_LookupPhysicsCallback @ 0x4a9240]"},
	{"render_function", "Render class", "Looks", "Picks the model control run each frame: tank tracks and turret, helicopter rotor and gear, gun yaw and heat glow, landmine points.", "[orig: BoneCallback_LookupByTag @ 0x4e32b0]"},
	{"disk_function", "Disk class", "Behaviour", "Unknown: the parser stores the class name; how the game binds it is not traced.", "itemdef-re.md, Open follow-ups"},
	{"input_function", "Input class", "Behaviour", "Picks the item's key handler (null, troop or tank) and, while mounted, its first-person camera.", "[orig: Input_HandleActionBinding @ 0x49AD40]"},
	{"virtual_display", "Cockpit model", "Looks", "The cockpit model a tank draws instead of its hull while the local player drives it in first person.", "[orig: EntityDef_LoadModelsAndCallbacks @ 0x439f50]; novaworld-net-re.md, FP mounted refinements"},
	{"virtual_display_userpoint", "Cockpit eye point", "Looks", "The point in the cockpit model where the tank driver's first-person eye sits, before it is pulled back along the view.", "novaworld-net-re.md, FP mounted refinements"},
	{"attrib", "Attributes", "Attributes", "Flags that switch item behaviours on (Door, NoTarget, AIData, LeaveCorpse, NoShadow and others), each read by its own code.", "itemdef-re.md, Enums"},
	{"attrib2", "More attributes", "Attributes", "More flags (VehicleBay, VehicleSpawn, the shadow kinds, IsTurret, Farp, LandMine and others), each read by its own code.", "itemdef-re.md, Enums"},
	{"physics", "Physics model", "Physics", "0 runs the class's simple motor; any other value runs the full physics motor (cars, trucks, amphibians and boats check it).", "[orig: Entity_DispatchPhysics_cveh @ 0x48EFC0]"},
	{"acceleration", "Acceleration", "Physics", "Speed the vehicle gains per tick under throttle; also a boat's thrust and an aircraft's tilt-command cap.", "[orig: Entity_UpdateVehiclePhysics @ 0x48AF00]"},
	{"deceleration", "Deceleration", "Physics", "How fast speed bleeds off without throttle, in a skid, airborne (half) or crashed; left out, twice the acceleration.", "[orig: Entity_UpdateVehiclePhysics @ 0x48AF00]"},
	{"player_speed", "Top speed", "Physics", "Top speed in km/h of land vehicles and aircraft; also the AI's speed cap and the speed where steering narrows to turn rate 2.", "[orig: Entity_UpdateVehiclePhysics @ 0x48AF00]"},
	{"water_speed", "Water top speed", "Physics", "Top speed in km/h of boats and floating amphibians; also the AI's speed cap and the speed where steering narrows to turn rate 2.", "[orig: Entity_UpdateWatercraftPhysics @ 0x48D480]"},
	{"slip_speed", "Slip speed", "Physics", "Simple-motor land vehicles skid instead of gripping when their velocity strays from their heading by more than this.", "[orig: Entity_ProcessInfantryPhysics @ 0x46E100]"},
	{"max_slope", "Max slope", "Physics", "Slopes steeper than this many degrees act as walls: full push-back and the hardest collision speed loss.", "[orig: Entity_ComputeCollisionForces @ 0x462150]"},
	{"slip_slope", "Slip slope", "Physics", "Slopes gentler than this many degrees never push the vehicle back; between this and the max slope it is pushed back in steps.", "[orig: Entity_ComputeCollisionForces @ 0x462150]"},
	{"climb_speed", "Climb speed", "Physics", "Aircraft only: the fastest climb in km/h; descent is capped at twice this.", "[orig: Entity_UpdateAircraftPhysics @ 0x490310]"},
	{"turn_roll", "Roll rate", "Physics", "Aircraft: the fastest roll in degrees a second (0 no cap); simple-motor land vehicles scale their body roll in turns by it.", "[orig: Entity_UpdateAircraftPhysics @ 0x490310]; [orig: Entity_ProcessInfantryPhysics @ 0x46E100]"},
	{"speed_pitch", "Pitch rate", "Physics", "Aircraft: the fastest pitch in degrees a second (0 no cap); simple-motor land vehicles scale their nose dip under acceleration by it.", "[orig: Entity_UpdateAircraftPhysics @ 0x490310]; [orig: Entity_ProcessInfantryPhysics @ 0x46E100]"},
	{"turn_rate", "Turn rate", "Physics", "Steering in degrees a second: the fastest turn at a standstill for land vehicles and boats, narrowing with speed; an aircraft's yaw cap.", "[orig: Entity_UpdateVehiclePhysics @ 0x48AF00]; [orig: Entity_UpdateAircraftPhysics @ 0x490310]"},
	{"turn_rate2", "Turn rate at speed", "Physics", "The turn rate left at top speed (0: a quarter of the turn rate); a low value makes AI drivers slow for sharp turns.", "[orig: Entity_UpdateVehiclePhysics @ 0x48AF00]"},
	{"torque", "Collision slowdown", "Physics", "Speed a collision takes: each hit removes the speed divided by 2 to the power torque+2 (torque+1 for medium hits).", "[orig: Entity_ProcessTrackedVehiclePhysics @ 0x47C1C0]"},
	{"mass", "Mass", "Physics", "Weight in collisions: the momentum share, who shoves whom, impact damage and crushing; boats of 1 or less are light hulls.", "[orig: Entity_ComputeCollisionForces @ 0x462150]; [orig: Entity_ProcessPlatformPhysics @ 0x481870]"},
	{"weathervane", "Pedal turn rate", "Physics", "Aircraft: holding a diagonal move key while airborne yaws the aircraft at this many degrees a second.", "[orig: Entity_UpdateAircraftPhysics @ 0x490310]"},
	{"min_ai", "Minimum crew", "Physics", "An AI vehicle away from its spawn with fewer occupants than this has its health capped at its burning level.", "[orig: Entity_UpdateVehiclePhysics @ 0x48AF00]"},
	{"lean", "Lean", "Physics", "The most a motorcycle or planing boat leans into a turn, 0 to 30.", "[orig: Entity_SmoothHeadingToTarget @ 0x45B2C0]; [orig: Vehicle_UpdateTurretRotation @ 0x45AEA0]"},
	{"lean_velocity", "Lean speed", "Physics", "How quickly a motorcycle or planing boat rolls into its lean.", "[orig: Entity_SmoothHeadingToTarget @ 0x45B2C0]; [orig: Vehicle_UpdateTurretRotation @ 0x45AEA0]"},
	{"pitch", "Bow lift limit", "Physics", "Planing boats: the nose-up limit (0 to 10) the bow lifts to at speed; past it the boat can start porpoising.", "[orig: Entity_ProcessPlatformPhysics @ 0x481870]"},
	{"pitch_velocity", "Bow lift force", "Physics", "Planing boats: how hard the bow lifts at planing speed and slams down while porpoising (0 to 10).", "[orig: Entity_ProcessPlatformPhysics @ 0x481870]"},
	{"bob", "Porpoise depth", "Physics", "Planing boats: how far the bow must drop below the lift limit before a porpoise bounce ends (0 to 10).", "[orig: Entity_ProcessPlatformPhysics @ 0x481870]"},
	{"flip", "Tip threshold", "Physics", "Land vehicles: an airborne hull tilted until its uprightness drops below this percent counts as crashed; boats ignore it.", "[orig: Entity_ProcessWheeledVehiclePhysics @ 0x475DE0]; [orig: Entity_ProcessTrackedVehiclePhysics @ 0x47C1C0]"},
	{"hand_brake", "Handbrake", "Physics", "Nonzero makes the lean-right key a car's handbrake, cutting throttle and starting a skid; tanks never use it.", "[orig: Entity_UpdateVehiclePhysics @ 0x48AF00]"},
	{"tire_slip", "Skid time", "Physics", "After a handbrake skid the vehicle slides along its skid for five times this many ticks before its grip returns.", "[orig: Entity_UpdateVehiclePhysics @ 0x48AF00]"},
	{"spring", "Spring stiffness", "Physics", "Suspension stiffness of each wheel, 0 to 10; 0 turns the wheel's spring off.", "[orig: Suspension_CompressWheelQuadratic @ 0x45CFB0]"},
	{"spring_comp", "Spring travel", "Physics", "The most a wheel's suspension compresses, as a percent of one world unit (0 to 100).", "[orig: Entity_ProcessTrackedVehiclePhysics @ 0x47C1C0]"},
	{"shock", "Shock damping", "Physics", "Suspension damping, 0 to 10: higher values stop a wheel's bounce sooner after a landing.", "[orig: Suspension_OscillateWheelFast @ 0x45D110]"},
	{"top_heavy", "Top heavy", "Physics", "The game reads it but never uses it.", "vehicle-client-movers-re.md section 7.3"},
	{"critical_hp", "Burning health", "Health and armour", "Health at or below which a vehicle burns (fire, warning sound, helicopter spin); undercrewed AI vehicles are capped here.", "[orig: Entity_UpdateVehiclePhysics @ 0x48AF00]; [orig: Entity_UpdateAircraftPhysics @ 0x490310]"},
	{"critical_drain", "Burn drain", "Health and armour", "Health a burning vehicle loses every 64 ticks; an aircraft wreck also uses it as its burn-down floor.", "[orig: Entity_UpdateAircraftPhysics @ 0x490310]; [orig: Entity_ProcessAircraftContactPhysics @ 0x47EF10]"},
	{"non_critical_regen", "Health regen", "Health and armour", "Health a vehicle regains every 64 ticks while above its burning health and below its full health minus this amount.", "[orig: Entity_UpdateAircraftPhysics @ 0x490310]"},
	{"radar_sig", "Radar signature", "Attributes", "The distance within which vehicle AI spot this item in their main view cone (0 hides it there); radar weapon checks treat 0 as unlimited.", "[orig: AI_FindBestTargetB @ 0x466f60]; [orig: Entity_ValidateWeaponTarget @ 0x53a400]"},
	{"heat_sig", "Heat signature", "Attributes", "The distance within which heat seekers and vehicle AI's side view cone detect this item; 0 hides it from heat seekers.", "[orig: Entity_ValidateWeaponTarget @ 0x53a400]; [orig: AI_FindBestTargetB @ 0x466f60]"},
	{"hud_image", "HUD picture", "Looks", "The picture the HUD shows: the vehicle's silhouette for its driver or controller, and the cargo icon while it is carried.", "[orig: HUD_BuildEntityInfo @ 0x4B8440]"},
	{"unit_type", "Unit type", "Attributes", "The vehicle family (land, air, water; 3 a helicopter, 11 a bridge): sets its minimap icon, voice set, death, fire size and score tally.", "[orig: Entity_ClassifyForMinimap @ 0x50FA70]; itemdef-re.md, witnessed value tables"},
	{"particlefx.effect", "Effect", "Effects", "An effect spawned at its user point when the mission starts; on drivable items only helicopters and planes show it, while occupied.", "[orig: Game_ResolveItemMaterialsAndSpawnBoneTrails @ 0x522ee0]; [orig: Entity_UpdateHeloRotorSpin @ 0x48fa70]"},
	{"particlefx.userpoint", "Effect point", "Effects", "The model user point the effect spawns at, one per match among the first 16 points; no match uses the item's origin.", "[orig: Entity_SpawnBoneTrailEffect @ 0x43bef0]; [orig: ItemDef_GetBoneMaskByName @ 0x49ea40]"},
	{"particlefx.secondary_effect", "Effect alternate", "Effects", "Not written by any key: the game reads no third word on a particlefx line, so this stays empty.", "[orig: ItemDef_ParseProperty @ 0x4A13BF]"},
	{"particlefxs.effect", "Skid effect", "Effects", "An effect spawned at each matching point while a grounded car or bike skids near the camera.", "[orig: Entity_SpawnBoneEffectsAtMask @ 0x458750]"},
	{"particlefxs.userpoint", "Skid effect point", "Effects", "The model user points the skid effect spawns at (every match among the first 16); no match, no skid effect.", "[orig: Entity_SpawnBoneEffectsAtMask @ 0x458750]"},
	{"particlefxs.secondary_effect", "Snow skid effect", "Effects", "The skid effect used instead on snow; left out, the normal skid effect plays.", "[orig: Entity_SpawnBoneEffectsAtMask @ 0x458750]"},
	{"particlefxw1.effect", "Trail 1 effect", "Effects", "An on-land movement trail for bikes, tanks and boats (not plain wheeled vehicles), its strength following the commanded speed.", "[orig: Entity_UpdateBoneTrailEffects @ 0x4589C0]"},
	{"particlefxw1.userpoint", "Trail 1 point", "Effects", "The model user points this trail spawns at (first 16; a point an earlier trail claimed stays with it); no match, no trail.", "[orig: Entity_UpdateBoneTrailEffects @ 0x4589C0]"},
	{"particlefxw1.secondary_effect", "Trail 1 alternate", "Effects", "Unknown: the trail updater checks it when resetting the trail; no spawn of it is witnessed.", "[orig: Entity_UpdateBoneTrailEffects @ 0x4589C0]"},
	{"particlefxw2.effect", "Trail 2 effect", "Effects", "An on-land movement trail whose strength follows the current speed; the only land trail plain wheeled vehicles use.", "[orig: Entity_UpdateBoneTrailEffects @ 0x4589C0]"},
	{"particlefxw2.userpoint", "Trail 2 point", "Effects", "The model user points this trail spawns at (first 16; a point an earlier trail claimed stays with it); no match, no trail.", "[orig: Entity_UpdateBoneTrailEffects @ 0x4589C0]"},
	{"particlefxw2.secondary_effect", "Trail 2 alternate", "Effects", "Unknown: the trail updater checks it when resetting the trail; no spawn of it is witnessed.", "[orig: Entity_UpdateBoneTrailEffects @ 0x4589C0]"},
	{"particlefxw3.effect", "Wake 1 effect", "Effects", "A wake spawned while in water; the commanded speed scales its rate and height, and it stops out of water, on death or at rest.", "[orig: Entity_UpdateBoneTrailEffects @ 0x4589C0]"},
	{"particlefxw3.userpoint", "Wake 1 point", "Effects", "The model user points the wake spawns at, held at the water surface (first 16 points); no match, no wake.", "[orig: Entity_UpdateBoneTrailEffects @ 0x4589C0]; [orig: ItemDef_GetBoneMaskByName @ 0x49ea40]"},
	{"particlefxw3.secondary_effect", "Wake 1 alternate", "Effects", "Not written by any key: the game reads no third word on a particlefxw3 line, so this stays empty.", "[orig: ItemDef_ParseProperty @ 0x4a158b]"},
	{"particlefxw4.effect", "Wake 2 effect", "Effects", "A second in-water wake driven by the current speed; plain wheeled vehicles and trains never use it.", "[orig: Entity_UpdateBoneTrailEffects @ 0x4589C0]"},
	{"particlefxw4.userpoint", "Wake 2 point", "Effects", "The model user points the second wake spawns at, held at the water surface (first 16 points); no match, no wake.", "[orig: Entity_UpdateBoneTrailEffects @ 0x4589C0]; [orig: ItemDef_GetBoneMaskByName @ 0x49ea40]"},
	{"particlefxw4.secondary_effect", "Wake 2 alternate", "Effects", "Not written by any key: the game reads no third word on a particlefxw4 line, so this stays empty.", "[orig: ItemDef_ParseProperty @ 0x4a15eb]"},
	{"particledeath", "Death effect", "Effects", "Played when the item dies above water, at up to four Dead points of its wreck or its origin; a corpse plays it before removal.", "[orig: Entity_InitDeathSounds @ 0x4939b0]"},
	{"particleh2odeath", "Underwater death effect", "Effects", "Replaces the death effect at the wreck's Dead points when the item dies fully underwater.", "[orig: Entity_InitDeathSounds @ 0x4939b0]"},
	{"particlefire", "Wreck fire effect", "Effects", "Started on death at up to four Fire points of the wreck; it follows the wreck, steams underwater and restarts on resurfacing.", "[orig: Entity_InitDeathSounds @ 0x4939b0]; [orig: Entity_UpdateDeadWreckEffects @ 0x493140]"},
	{"particleother", "Wreck extra effect", "Effects", "Started on death at up to four Other points of the wreck, following the wreck.", "[orig: Entity_InitDeathSounds @ 0x4939b0]; [orig: Entity_UpdateDeadWreckEffects @ 0x493140]"},
	{"particlefinale", "Wreck landing effect", "Effects", "Played once at the wreck when a falling or sinking wreck comes to rest.", "[orig: Entity_TransitionToGroundDeath @ 0x493080]"},
	{"particlespawn", "Respawn effect", "Effects", "Played at a person's position when they respawn.", "[orig: Entity_ResetToSpawnState @ 0x4B9610]"},
	{"ammo_closeattack", "Close attack ammo", "Weapons", "AI soldier: the ammo its animation's close-attack fire event shoots; on a landmine, the ammo of its small mine points.", "[orig: Entity_InitOrganicAI @ 0x4BFCC0]; [orig: Entity_InitHardpoints @ 0x4417D0]"},
	{"ammo_marker3", "Third fire ammo", "Weapons", "AI soldier: the ammo its animation's third fire event shoots; on a landmine, the ammo of its large mine points.", "[orig: Entity_InitOrganicAI @ 0x4BFCC0]; [orig: Entity_LandmineThink @ 0x441A40]"},
	{"ammo_easyrocket", "Rocket ammo", "Weapons", "AI soldier: the ammo its secondary fire shoots from the rocket point, walking fire included; each shot costs one magazine round.", "[orig: Entity_InitOrganicAI @ 0x4BFCC0]; [orig: Entity_UpdateInfantryAI @ 0x4b9910]"},
	{"ammo_advancedrocket", "Second rocket ammo", "Weapons", "AI soldier: a second ammo fired with the rocket ammo when it differs, from the same point, at no extra magazine cost.", "[orig: Entity_InitOrganicAI @ 0x4BFCC0]; [orig: Entity_UpdateInfantryAI @ 0x4b9910]"},
	{"launchups_closeattack", "Close attack point", "Weapons", "The point on the soldier's own model close-attack shots leave from, matched ignoring case; missing, the body's position.", "[orig: Entity_InitOrganicAI @ 0x4BFCC0]; [orig: Entity_GetAttachmentWorldPosition @ 0x4b2670]"},
	{"launchups_rocket", "Rocket point", "Weapons", "The point on the soldier's model both rocket ammos fire from; also the soldier's aim anchor in combat.", "[orig: Entity_InitOrganicAI @ 0x4BFCC0]; [orig: Entity_GetAttachmentWorldPosition @ 0x4b2670]"},
	{"launchups_marker3", "Third fire point", "Weapons", "The point on the soldier's model the third fire event's shots leave from.", "[orig: Entity_InitOrganicAI @ 0x4BFCC0]; [orig: Entity_GetAttachmentWorldPosition @ 0x4b2670]"},
	{"weapon_userpoints[0]", "Left fire point", "Weapons", "Where the gun's second barrel fires from on this model (barrels follow the clip count); unset, the first barrel's points are used.", "[orig: Entity_InitBoneReferences @ 0x441470]; [orig: Entity_ResolveBoneUserpoints @ 0x545940]"},
	{"weapon_userpoints[1]", "Left flash point", "Weapons", "The second barrel's point while the fire action turns into recoil (the flash anchor); unset, the first barrel's.", "[orig: Entity_InitBoneReferences @ 0x441470]; [orig: Entity_ComputeUserpointWorldTransform @ 0x545c60]"},
	{"weapon_userpoints[2]", "Left recoil point", "Weapons", "The second barrel's point during the recoil action (the casing anchor); unset, the first barrel's.", "[orig: Entity_InitBoneReferences @ 0x441470]; [orig: Entity_ComputeUserpointWorldTransform @ 0x545c60]"},
	{"weapon_userpoints[3]", "Right fire point", "Weapons", "Where the gun's first barrel fires from on this model; only items with the EWeap attribute use these points.", "[orig: Entity_InitBoneReferences @ 0x441470]; [orig: Entity_GetWeaponSlotByte @ 0x5459c0]"},
	{"weapon_userpoints[4]", "Right flash point", "Weapons", "The first barrel's point while the fire action turns into recoil (the flash anchor).", "[orig: Entity_InitBoneReferences @ 0x441470]; [orig: Entity_ComputeUserpointWorldTransform @ 0x545c60]"},
	{"weapon_userpoints[5]", "Right recoil point", "Weapons", "The first barrel's point during the recoil action (the casing anchor).", "[orig: Entity_InitBoneReferences @ 0x441470]; [orig: Entity_ComputeUserpointWorldTransform @ 0x545c60]"},
	{"weapon_userpoints[6]", "Left fire point 2", "Weapons", "Where the fourth barrel fires from; unset, the second barrel's points. A gun without a clip always fires from it.", "[orig: Entity_InitBoneReferences @ 0x441470]; [orig: WeaponSlot_InitFromEntityDef @ 0x5466c0]"},
	{"weapon_userpoints[7]", "Left flash point 2", "Weapons", "The fourth barrel's point while the fire action turns into recoil (the flash anchor); unset, the second barrel's.", "[orig: Entity_InitBoneReferences @ 0x441470]; [orig: Entity_ComputeUserpointWorldTransform @ 0x545c60]"},
	{"weapon_userpoints[8]", "Left recoil point 2", "Weapons", "The fourth barrel's point during the recoil action (the casing anchor); unset, the second barrel's.", "[orig: Entity_InitBoneReferences @ 0x441470]; [orig: Entity_ComputeUserpointWorldTransform @ 0x545c60]"},
	{"weapon_userpoints[9]", "Right fire point 2", "Weapons", "Where the third barrel fires from; unset, the first barrel's points are used.", "[orig: Entity_InitBoneReferences @ 0x441470]; [orig: Entity_ResolveBoneUserpoints @ 0x545940]"},
	{"weapon_userpoints[10]", "Right flash point 2", "Weapons", "The third barrel's point while the fire action turns into recoil (the flash anchor); unset, the first barrel's.", "[orig: Entity_InitBoneReferences @ 0x441470]; [orig: Entity_ComputeUserpointWorldTransform @ 0x545c60]"},
	{"weapon_userpoints[11]", "Right recoil point 2", "Weapons", "The third barrel's point during the recoil action (the casing anchor); unset, the first barrel's.", "[orig: Entity_InitBoneReferences @ 0x441470]; [orig: Entity_ComputeUserpointWorldTransform @ 0x545c60]"},
	{"clipsize", "AI magazine size", "Weapons", "An AI soldier's magazine: it reloads when empty and refills to this, reset on respawn; door and death keys share its storage.", "[orig: Entity_UpdateInfantryAI @ 0x4b9910]; [orig: Entity_ResetToSpawnState @ 0x4B9610]"},
	{"deathtime_ticks", "Corpse time", "Death and wreck", "Seconds a dead person's body stays, plus one (an authored 0 gives 9 seconds); the squib, door and rotor keys share its storage.", "[orig: Entity_UpdateInfantryAI @ 0x4b9910]"},
	{"primary_weapon", "Built-in weapon", "Weapons", "The weapon.def entry this gun, vehicle or aircraft carries built in; its attach text names the seat on the HUD.", "[orig: WeaponSlot_InitFromEntityDef @ 0x5466c0]; [orig: HUD_DrawVehicleSeatAndArmoryLabels @ 0x5a351d]"},
	{"huskfinal", "Final wreck model", "Death and wreck", "A second wreck model: death debris is cut from it, and the wreck's Dead, Fire and Other effect points come from it first.", "[orig: Entity_SpawnDeathPieces @ 0x493400]; [orig: Game_ResolveItemMaterialsAndSpawnBoneTrails @ 0x522ee0]"},
	{"sounddeath", "Death sound", "Sound", "Played at the item's position when it is destroyed (the silent cleanup death skips it); left out, the sound profile's death sound.", "[orig: Entity_InitDeathSounds @ 0x4939b0]"},
	{"armor_impact", "Bullet armour", "Health and armour", "Bullets with a lower impact penetration do no damage (-1: immune to bullets); the armor line's second number, else its first.", "[orig: Projectile_ProcessDamageOnTarget @ 0x4e7fb0]"},
	{"armor_blast", "Blast armour copy", "Health and armour", "Not a key: a copy of the blast armour (the armor line's first number) that the blast damage check reads.", "[orig: Entity_ApplyWeaponDamage @ 0x4e6820]"},
	{"kz", "Death blast radius", "Death and wreck", "The blast radius used when the wreck has no KZ points (one blast at its origin) and for a falling wreck's landing blast.", "[orig: Entity_QueueKzBlastAtUserPoints @ 0x4eabf0]; [orig: Entity_UpdateFallingDeathPhysics @ 0x4941be]"},
	{"husk_swap_at", "Husk swap point", "Death and wreck", "Unknown: parsed as a percent (or as seconds after husk_swap_at_sec); no reader is witnessed.", "world-wac-ai-re.md section 24.7 (D-ITEM-2)"},
	{"husk_swap_at_sec", "Husk swap time", "Death and wreck", "Unknown: parsed as seconds; no reader is witnessed.", "world-wac-ai-re.md section 24.7 (D-ITEM-2)"},
	{"scale_q16", "Model scale", "Looks", "A size multiplier for the item's model, collision and bounds, unless the placed instance sets its own; 0 means unscaled.", "[orig: Entity_InitFromModel @ 0x40dc30]; [orig: Entity_ComputeBoundingSphere @ 0x5c69a0]"},
	{"debris_scale", "Debris scale", "Death and wreck", "A size multiplier for the flying wreck debris pieces; 0 or left out means normal size.", "[orig: Entity_SpawnDeathPieces @ 0x493400]"},
	{"husk_sub_parts", "Tower sections", "Death and wreck", "The section count of tower items that collapse section by section; other wrecks ignore it and throw one piece per wreck-model section.", "[orig: Entity_UpdateSectionDamage @ 0x4406A0]"},
	{"husk_sub_part_types[0]", "Debris piece 1", "Death and wreck", "Unknown: an 01_ entry, the slot of section 1 (the hull), which never flies off; no reader of it is witnessed.", "[orig: Entity_SpawnDeathPieces @ 0x493400]; world-wac-ai-re.md section 24.4"},
	{"husk_sub_part_types[1]", "Debris piece 2", "Death and wreck", "The debris type of wreck section 2 (an 02_ entry) when it flies off on death: its odds, speed, spin, bounces, trail and sounds.", "[orig: Entity_SpawnDeathPieces @ 0x493400]"},
	{"husk_sub_part_types[2]", "Debris piece 3", "Death and wreck", "The debris type of wreck section 3 (an 03_ entry) when it flies off on death: its odds, speed, spin, bounces, trail and sounds.", "[orig: Entity_SpawnDeathPieces @ 0x493400]"},
	{"husk_sub_part_types[3]", "Debris piece 4", "Death and wreck", "The debris type of wreck section 4 (an 04_ entry) when it flies off on death: its odds, speed, spin, bounces, trail and sounds.", "[orig: Entity_SpawnDeathPieces @ 0x493400]"},
	{"husk_sub_part_types[4]", "Debris piece 5", "Death and wreck", "The debris type of wreck section 5 (an 05_ entry) when it flies off on death: its odds, speed, spin, bounces, trail and sounds.", "[orig: Entity_SpawnDeathPieces @ 0x493400]"},
	{"husk_sub_part_types[5]", "Debris piece 6", "Death and wreck", "The debris type of wreck section 6 (an 06_ entry) when it flies off on death: its odds, speed, spin, bounces, trail and sounds.", "[orig: Entity_SpawnDeathPieces @ 0x493400]"},
	{"husk_sub_part_types[6]", "Debris piece 7", "Death and wreck", "The debris type of wreck section 7 (an 07_ entry) when it flies off on death: its odds, speed, spin, bounces, trail and sounds.", "[orig: Entity_SpawnDeathPieces @ 0x493400]"},
	{"husk_sub_part_types[7]", "Debris piece 8", "Death and wreck", "The debris type of wreck section 8 (an 08_ entry) when it flies off on death: its odds, speed, spin, bounces, trail and sounds.", "[orig: Entity_SpawnDeathPieces @ 0x493400]"},
	{"husk_sub_part_types[8]", "Debris piece 9", "Death and wreck", "The debris type of wreck section 9 (an 09_ entry) when it flies off on death: its odds, speed, spin, bounces, trail and sounds.", "[orig: Entity_SpawnDeathPieces @ 0x493400]"},
	{"husk_sub_part_types[9]", "Debris piece 10", "Death and wreck", "The debris type of wreck section 10 (a 10_ entry) when it flies off on death: its odds, speed, spin, bounces, trail and sounds.", "[orig: Entity_SpawnDeathPieces @ 0x493400]"},
	{"husk_sub_part_types[10]", "Debris piece 11", "Death and wreck", "The debris type of wreck section 11 (an 11_ entry) when it flies off on death: its odds, speed, spin, bounces, trail and sounds.", "[orig: Entity_SpawnDeathPieces @ 0x493400]"},
	{"husk_sub_part_types[11]", "Debris piece 12", "Death and wreck", "The debris type of wreck section 12 (a 12_ entry) when it flies off on death: its odds, speed, spin, bounces, trail and sounds.", "[orig: Entity_SpawnDeathPieces @ 0x493400]"},
	{"husk_sub_part_types[12]", "Debris piece 13", "Death and wreck", "The debris type of wreck section 13 (a 13_ entry) when it flies off on death: its odds, speed, spin, bounces, trail and sounds.", "[orig: Entity_SpawnDeathPieces @ 0x493400]"},
	{"husk_sub_part_types[13]", "Debris piece 14", "Death and wreck", "The debris type of wreck section 14 (a 14_ entry) when it flies off on death: its odds, speed, spin, bounces, trail and sounds.", "[orig: Entity_SpawnDeathPieces @ 0x493400]"},
	{"husk_sub_part_types[14]", "Debris piece 15", "Death and wreck", "The debris type of wreck section 15 (a 15_ entry) when it flies off on death: its odds, speed, spin, bounces, trail and sounds.", "[orig: Entity_SpawnDeathPieces @ 0x493400]"},
	{"husk_sub_part_types[15]", "Debris piece 16", "Death and wreck", "The debris type of wreck section 16 (a 16_ entry) when it flies off on death: its odds, speed, spin, bounces, trail and sounds.", "[orig: Entity_SpawnDeathPieces @ 0x493400]"},
	{"phrase_set", "Gunner pose", "Behaviour", "On a mountable gun: which seated pose its gunner plays (1 to 8 pick variants) and how the gunner's upper body follows the aim.", "[orig: Entity_BuildBoneTransformMatrices @ 0x4b1884]; [orig: Entity_InitOrganicAI @ 0x4BFCC0]"},
	{"phrase_set_valid", "Gunner pose set", "Behaviour", "Not a key: on when the item has a phrase_set line, since an authored 0 is a real setting.", "itemdef-re.md, ItemDef field map (phraseSet)"},
	{"damage_reduc_pp", "Crew damage cut", "Health and armour", "With more than one occupant aboard, a vehicle's incoming damage is cut by this fraction per occupant, up to the cap.", "[orig: Entity_ApplyOccupantDamageScale @ 0x4E5A50]"},
	{"damage_reduc_max", "Crew damage cap", "Health and armour", "The most of a vehicle's incoming damage the occupant cut can remove, as a fraction; the damage_reduc_pp line's second number.", "[orig: Entity_ApplyOccupantDamageScale @ 0x4E5A50]"},
	{"armor_kz", "Blast armour", "Health and armour", "The armor line's first number: blasts with a lower kill-zone penetration do no damage (-1: immune); with both armours -1, nothing targets the item.", "[orig: Entity_ApplyWeaponDamage @ 0x4e6820]; [orig: Entity_FindTargets @ 0x53a610]"},
	{"emplacement_attachments_count", "Mounted guns", "Weapons", "Not a key: how many addeweap rows the item has; the game spawns at most four child guns when the mission loads.", "[orig: Entity_SpawnWeaponOverlays @ 0x40F300]; itemdef-re.md, Vehicle child-emplacement attachments"},
	{"emplacement_g_slot", "Swap gun row", "Weapons", "Not a key: the row the last addeweapG line stored; that gun's rider can swap it with the vehicle's own weapon.", "[orig: Input_HandleActionBinding_0 @ 0x4E0420]; [orig: NapiNPServerMsg_HandleWeaponToggle @ 0x511A70]"},
	{"emplacement_c_slot", "Commander gun row", "Weapons", "Not a key: the row the last addeweapC line stored; a scoped commander's HUD shows where that gun's gunner aims.", "[orig: HUD_DrawScopeOverlayDetails @ 0x59E420]"},
	{"light_transfer", "Interior daylight", "Looks", "For buildings: the percent of daylight that reaches inside, lighting the interior and the people in it, and scaling indoor rain volume.", "[orig: Terrain_RenderSectorModels @ 0x5c5df2]; [orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7f7a]"},
	{"reverb", "Interior reverb", "Sound", "While the local player is inside this building, a nonzero value picks the reverb preset (reverb points still win), but presets sound the same.", "[orig: Entity_UpdateInfantryPlayerBody @ 0x4B5F9E]; lwf-dbf-sound-re.md, Reverb selection"},
	{"shadow_texture", "Shadow texture", "Looks", "The game reads it but never uses it: the shadow decal it names never draws in JO, its texture never being loaded.", "[orig: RenderSlot_DrawAuthoredBlobDecal @ 0x5d59d0]; render-lighting-re.md"},
	{"shadow_width", "Shadow width", "Looks", "The game reads it but never uses it: only the shadow decal that never draws in JO reads it.", "[orig: RenderSlot_DrawAuthoredBlobDecal @ 0x5d59d0]; render-lighting-re.md"},
	{"shadow_length", "Shadow length", "Looks", "The game reads it but never uses it: only the shadow decal that never draws in JO reads it.", "[orig: RenderSlot_DrawAuthoredBlobDecal @ 0x5d59d0]; render-lighting-re.md"},
	{"shadow_offset_x", "Shadow offset X", "Looks", "The game reads it but never uses it: only the shadow decal that never draws in JO reads it.", "[orig: RenderSlot_DrawAuthoredBlobDecal @ 0x5d59d0]; render-lighting-re.md"},
	{"shadow_offset_y", "Shadow offset Y", "Looks", "The game reads it but never uses it: only the shadow decal that never draws in JO reads it.", "[orig: RenderSlot_DrawAuthoredBlobDecal @ 0x5d59d0]; render-lighting-re.md"},
	{"vehicle_spawn_mask", "Spawnable vehicles", "Vehicle spawn", "The vehicle ids this item can spawn: spawn requests and respawn markers accept only these, and a vehicle bay's minimap icon follows them.", "[orig: NapiNPServerMsg_HandleVehicleSpawnRequest @ 0x51C4C0]; [orig: Spawn_AssignOverlaySpawnPoints @ 0x529E60]"},
	{"music_location", "Location number", "Behaviour", "The location number scripts get for anything inside this building, overriding the mission's location boxes; no music use is witnessed.", "[orig: WacCmd_SsnLoc @ 0x4F0E90]; [orig: WacCmd_Location @ 0x4ED190]"},
	{"mana", "Mana", "Health and armour", "The entity's starting mana word (also its class 1 ammo pool), set at spawn and on respawn; mana powerups top it up.", "[orig: Entity_InitFromItemDef @ 0x49e550]; [orig: Entity_ResetToSpawnState @ 0x4B9610]"},
	{"door_type", "Door type", "Doors", "No door code reads it; its one reader takes the value as a squib's random spread (it shares sqb_error's storage).", "[orig: sub_448CE0 @ 0x448d2c]; itemdef-re.md, witnessed value tables"},
	{"door_open_rate_q16", "Door swing time", "Doors", "Seconds a door section takes to swing fully open or fully closed.", "[orig: Entity_SpawnFromBMSRecord @ 0x40E9F0]; [orig: FadeEffect_UpdateAll @ 0x44E920]"},
	{"door_max_angle_bam", "Door max angle", "Doors", "Copied to each door, but the swing ignores it; only a late-joining client uses it to set an already-open door's position.", "[orig: Entity_SpawnFromBMSRecord @ 0x40E9F0]; [orig: NapiNPClientMsg_0x010 @ 0x4336B5]"},
	{"door_open_sound", "Door open sound", "Doors", "Played at a door section's pivot when it starts opening; also when a carried object such as a flag is dropped.", "[orig: Entity_ProcessSectionDamageTransition @ 0x43F370]; [orig: Entity_DropCarriedObject @ 0x439DF0]"},
	{"door_close_sound", "Door close sound", "Doors", "Played at a door section's pivot when it starts closing.", "[orig: Entity_ProcessSectionDamageTransition @ 0x43F370]"},
	{"attrib_parent", "Parent", "Attributes", "Set by the Parent attribute: the vehicle carries items sharing its reference number on its agun points and kills them when it dies.", "[orig: Entity_SetupGunnerAttachments @ 0x468100]; [orig: AI_TransitionToDeath_GroundVehicle @ 0x467B90]"},
	{"max_attack_dist", "Placed attack distance", "Behaviour", "The original mission editor's: a record placed of the item takes it as its attack distance (16 unless set). The game reads nothing of the key.", "[orig: JOTACmed.exe ItemsDef_ParseToken @ 0x431a58; MissionItem_InitFromDefinition @ 0x44dc46]; [orig: ItemDef_ParseProperty @ 0x4a1a7b]"},
	{"max_engagement_dist", "Placed engagement distance (far)", "Behaviour", "The original mission editor's: a record placed of the item takes it as its far engagement distance (320 unless set). The game reads nothing of the key.", "[orig: JOTACmed.exe ItemsDef_ParseToken @ 0x431a89; MissionItem_InitFromDefinition @ 0x44dc6b]; [orig: ItemDef_ParseProperty @ 0x4a1a94]"},
	{"min_engagement_dist", "Placed engagement distance (near)", "Behaviour", "The original mission editor's: a record placed of the item takes it as its near engagement distance (16 unless set). The game reads nothing of the key.", "[orig: JOTACmed.exe ItemsDef_ParseToken @ 0x431aba; MissionItem_InitFromDefinition @ 0x44dc58]; [orig: ItemDef_ParseProperty @ 0x4a1aad]"},
	{"fire_timer", "Placed advance timer", "Behaviour", "The original mission editor's: a record placed of the item takes it as its advance timer (10 unless set). The game reads nothing of the key.", "[orig: JOTACmed.exe ItemsDef_ParseToken @ 0x431aeb; MissionItem_InitFromDefinition @ 0x44dcb0]; [orig: ItemDef_ParseProperty @ 0x4a1ac6]"},
	{"attrib_good", "Good", "Attributes", "The original mission editor's side word: a record placed of the item takes team 1 (Evil over Good). The game matches it and keeps nothing.", "[orig: JOTACmed.exe ItemsDef_ParseToken @ 0x431682; MissionItem_InitFromDefinition @ 0x44dd51]; [orig: ItemDef_ParseProperty @ 0x4a06c3]"},
	{"attrib_evil", "Evil", "Attributes", "The original mission editor's side word: a record placed of the item takes team 2, over Good. The game matches it and keeps nothing.", "[orig: JOTACmed.exe ItemsDef_ParseToken @ 0x43169f; MissionItem_InitFromDefinition @ 0x44dd76]; [orig: ItemDef_ParseProperty @ 0x4a06db]"},
};

const DefWords kAttachmentWords[] = {
	{"userpoint", "Mount point", "", "The carrier model's user point the child gun is mounted on, its direction aiming the child; missing, the carrier's root.", "[orig: Entity_UpdateTransformAndTurret @ 0x440CA0]"},
	{"item_id", "Child item", "", "The item id of the child spawned on the carrier when the mission loads; it joins the carrier's group and is destroyed with it.", "[orig: Entity_SpawnWeaponOverlays @ 0x40F300]; [orig: EntityReference_DestroyEWeapGroup @ 0x546F30]"},
	{"down_angle", "Aim down limit", "", "How far the child gun may aim down, in degrees; with all four limits zero, the gun's weapon.def window applies instead.", "[orig: Entity_GetWeaponTurretLimits @ 0x540d70]"},
	{"up_angle", "Aim up limit", "", "How far the child gun may aim up, in degrees; with all four limits zero, the gun's weapon.def window applies instead.", "[orig: Entity_GetWeaponTurretLimits @ 0x540d70]"},
	{"right_angle", "Aim right limit", "", "How far the child gun may turn right, in degrees; with all four limits zero, the gun's weapon.def window applies instead.", "[orig: Entity_GetWeaponTurretLimits @ 0x540d70]"},
	{"left_angle", "Aim left limit", "", "How far the child gun may turn left, in degrees; with all four limits zero, the gun's weapon.def window applies instead.", "[orig: Entity_GetWeaponTurretLimits @ 0x540d70]"},
	{"angle_count", "Aim limits given", "", "Not a key: 4 when the line gives the four aim limits, 0 when it gives none; the game reads all four or none.", "[orig: ItemDef_ParseProperty @ 0x4A1BB4]; itemdef-re.md, Catalog authoring"},
	{"kind", "Line kind", "", "The row's key: addeweap; addeweapG, a gun its rider can swap with the vehicle's own weapon; addeweapC, whose aim a scoped commander's HUD shows.", "[orig: Input_HandleActionBinding_0 @ 0x4E0420]; [orig: HUD_DrawScopeOverlayDetails @ 0x59E420]"},
};

const DefWords kWeaponWords[] = {
	{"weapon_name", "Weapon name", "Identity", "The weapon's id (32 characters kept): the HUD shows its WepDes text, and a later block of the same name replaces this one.", "[orig: HUD_DrawWeaponAmmoAndName @ 0x593b7f; WeaponDefs_ParseLineCallback @ 0x5436e1]"},
	{"category", "Category", "Identity", "The weapon's row of the 12-row weapon slot table: keys 1-9 select rows 1-9 (knife, sidearm, primary...), row 11 holds a vehicle's mounted guns.", "[orig: WeaponSlotTable_LoadAllFromDefs @ 0x5415d3; Input_HandleActionBinding_0 @ 0x4e1144]"},
	{"rank", "Rank", "Identity", "The weapon's place within its category row (0-64); switching to a row tries rank 0 first, then walks up the ranks.", "[orig: Player_SwitchToWeaponByHandle @ 0x4e02f1]"},
	{"clipsize", "Clip size", "Ammo", "Rounds one magazine holds: loadout ammo comes in whole magazines of it; -1 means the weapon has no magazine (knife, medpack).", "[orig: WeaponSlot_GetTotalClips @ 0x5425F0; PlayerInfo_PopulateAmmoComboBoxes @ 0x55def0]"},
	{"startrounds", "Starting rounds", "Ammo", "Rounds the weapon is issued when the loadout asks for no count; a class's own classrounds value replaces it.", "[orig: NapiNPServerMsg_HandlePlayerLoadout @ 0x515790; WeaponSlots_SeedAmmoPoolsFromDefs @ 0x5416c4]"},
	{"targetyawrange", "Traverse range", "Handling", "How far a mounted gun turns either side of centre, in degrees, when its vehicle's addeweap line sets no arc; 0 locks the traverse.", "[orig: Entity_GetWeaponTurretLimits @ 0x540E2C..0x540E58]"},
	{"targetpitchmax", "Elevation limit", "Handling", "How far up a mounted gun aims, in degrees, when its vehicle's addeweap line sets no arc; 0 stops it at level.", "[orig: Entity_GetWeaponTurretLimits @ 0x540E2C..0x540E58]"},
	{"targetpitchmin", "Depression limit", "Handling", "How far down a mounted gun aims, in degrees below level, when its vehicle's addeweap line sets no arc; 0 stops it at level.", "[orig: Entity_GetWeaponTurretLimits @ 0x540E2C..0x540E58]"},
	{"statid", "Stat id", "Identity", "Unknown: the parser stores the number; no reader is witnessed.", "[orig: WeaponDefs_ParseLineCallback @ 0x543680]"},
	{"maxclips", "Most magazines", "Loadout", "The most magazines the loadout can take: the loadout screen offers 1 to this many, and the host caps a request at it.", "[orig: PlayerInfo_PopulateAmmoComboBoxes @ 0x55def0; NapiNPServerMsg_HandlePlayerLoadout @ 0x515790]"},
	{"ammobucket", "Shared magazine", "Ammo", "Nonzero: the loaded rounds live in a shared count of that number, so weapons with the same bucket share one magazine.", "[orig: sub_5405F0 @ 0x5405F0; WeaponSlot_ReloadAmmo @ 0x541720]"},
	{"ammo_class", "Ammo class", "Ammo", "The ammo pool the weapon reloads from: weapons naming the same class share it, and that class's ammoclass_max_carry line caps it.", "[orig: WeaponSlot_ReloadAmmo @ 0x541720; WeaponDefs_ParseLineCallback @ 0x5441CB]"},
	{"ammo_class_count", "Pool units per round", "Ammo", "Pool units one round costs: a reload takes a magazine's rounds times this, and a weapon without magazines needs this much to fire.", "[orig: WeaponSlot_ReloadAmmo @ 0x541720; WeaponSlot_CanFire @ 0x541cba..0x541d08]"},
	{"charfilter[0]", "Soldier class 1", "Filters", "A soldier class offered the weapon (medic, sniper, gunner, rifleman, engineer); only each charfilter line's first class reaches the armory and host checks.", "[orig: PlayerInfo_PopulateWeaponSlotLists @ 0x560430; NapiNPServerMsg_HandlePlayerLoadout @ 0x515790; WeaponDefs_ParseLineCallback @ 0x543F6E]"},
	{"charfilter[1]", "Soldier class 2", "Filters", "A soldier class offered the weapon (medic, sniper, gunner, rifleman, engineer); only each charfilter line's first class reaches the armory and host checks.", "[orig: PlayerInfo_PopulateWeaponSlotLists @ 0x560430; NapiNPServerMsg_HandlePlayerLoadout @ 0x515790; WeaponDefs_ParseLineCallback @ 0x543F6E]"},
	{"charfilter[2]", "Soldier class 3", "Filters", "A soldier class offered the weapon (medic, sniper, gunner, rifleman, engineer); only each charfilter line's first class reaches the armory and host checks.", "[orig: PlayerInfo_PopulateWeaponSlotLists @ 0x560430; NapiNPServerMsg_HandlePlayerLoadout @ 0x515790; WeaponDefs_ParseLineCallback @ 0x543F6E]"},
	{"charfilter[3]", "Soldier class 4", "Filters", "A soldier class offered the weapon (medic, sniper, gunner, rifleman, engineer); only each charfilter line's first class reaches the armory and host checks.", "[orig: PlayerInfo_PopulateWeaponSlotLists @ 0x560430; NapiNPServerMsg_HandlePlayerLoadout @ 0x515790; WeaponDefs_ParseLineCallback @ 0x543F6E]"},
	{"charfilter[4]", "Soldier class 5", "Filters", "A soldier class offered the weapon (medic, sniper, gunner, rifleman, engineer); only each charfilter line's first class reaches the armory and host checks.", "[orig: PlayerInfo_PopulateWeaponSlotLists @ 0x560430; NapiNPServerMsg_HandlePlayerLoadout @ 0x515790; WeaponDefs_ParseLineCallback @ 0x543F6E]"},
	{"charfilter[5]", "Soldier class 6", "Filters", "A soldier class offered the weapon (medic, sniper, gunner, rifleman, engineer); only each charfilter line's first class reaches the armory and host checks.", "[orig: PlayerInfo_PopulateWeaponSlotLists @ 0x560430; NapiNPServerMsg_HandlePlayerLoadout @ 0x515790; WeaponDefs_ParseLineCallback @ 0x543F6E]"},
	{"charfilter[6]", "Soldier class 7", "Filters", "A soldier class offered the weapon (medic, sniper, gunner, rifleman, engineer); only each charfilter line's first class reaches the armory and host checks.", "[orig: PlayerInfo_PopulateWeaponSlotLists @ 0x560430; NapiNPServerMsg_HandlePlayerLoadout @ 0x515790; WeaponDefs_ParseLineCallback @ 0x543F6E]"},
	{"charfilter[7]", "Soldier class 8", "Filters", "A soldier class offered the weapon (medic, sniper, gunner, rifleman, engineer); only each charfilter line's first class reaches the armory and host checks.", "[orig: PlayerInfo_PopulateWeaponSlotLists @ 0x560430; NapiNPServerMsg_HandlePlayerLoadout @ 0x515790; WeaponDefs_ParseLineCallback @ 0x543F6E]"},
	{"charfilter_count", "Soldier class count", "Filters", "Set by the charfilter lines: how many soldier classes they list (at most 8).", "def_weapons.cpp (derived; [orig: WeaponDefs_ParseLineCallback @ 0x543F6E])"},
	{"teamfilter[0]", "Team 1", "Filters", "A team offered the weapon (only a line's first counts): red or blue; yellow (as blue) and violet (as red) pass the armory, not the host.", "[orig: NapiNPServerMsg_HandlePlayerLoadout @ 0x515790; PlayerInfo_PopulateWeaponSlotLists @ 0x560430; WeaponDefs_ParseLineCallback @ 0x543FE3]"},
	{"teamfilter[1]", "Team 2", "Filters", "A team offered the weapon (only a line's first counts): red or blue; yellow (as blue) and violet (as red) pass the armory, not the host.", "[orig: NapiNPServerMsg_HandlePlayerLoadout @ 0x515790; PlayerInfo_PopulateWeaponSlotLists @ 0x560430; WeaponDefs_ParseLineCallback @ 0x543FE3]"},
	{"teamfilter[2]", "Team 3", "Filters", "A team offered the weapon (only a line's first counts): red or blue; yellow (as blue) and violet (as red) pass the armory, not the host.", "[orig: NapiNPServerMsg_HandlePlayerLoadout @ 0x515790; PlayerInfo_PopulateWeaponSlotLists @ 0x560430; WeaponDefs_ParseLineCallback @ 0x543FE3]"},
	{"teamfilter[3]", "Team 4", "Filters", "A team offered the weapon (only a line's first counts): red or blue; yellow (as blue) and violet (as red) pass the armory, not the host.", "[orig: NapiNPServerMsg_HandlePlayerLoadout @ 0x515790; PlayerInfo_PopulateWeaponSlotLists @ 0x560430; WeaponDefs_ParseLineCallback @ 0x543FE3]"},
	{"teamfilter_count", "Team count", "Filters", "Set by the teamfilter lines: how many teams they list (at most 4).", "def_weapons.cpp (derived; [orig: WeaponDefs_ParseLineCallback @ 0x543FE3])"},
	{"loadout_selectable", "In loadout lists", "Loadout", "Nonzero lists the weapon in the loadout screen's primary, secondary, accessory and grenade choices; 0 keeps it off them.", "[orig: PlayerInfo_PopulateWeaponSlotLists @ 0x560430; PlayerInfo_PopulateAmmoComboBoxes @ 0x55def0]"},
	{"loadout_subclasses", "Variant count", "Loadout", "How many weapon blocks right after this one are its variants; the loadout offers the first whose round differs as a second ammo choice (satchel, detonator).", "[orig: PlayerInfo_PopulateAmmoComboBoxes @ 0x55def0; Server_SendWeaponSlotListToPlayer @ 0x5027c8]"},
	{"weapon_class", "Loadout list", "Loadout", "Which loadout list shows the weapon: accessory, primary, secondary or grenade; primary and secondary weapons can be switched to with no ammo.", "[orig: PlayerInfo_PopulateWeaponSlotLists @ 0x560430; Player_SwitchToWeaponByHandle @ 0x4e0294]"},
	{"round_type", "Round", "Ammo", "The ammo.def round the weapon fires; the loadout screen's ammo rows also show this name through gametext.bin's WepDes section.", "[orig: AmmoDef_LookupByName @ 0x409870; PlayerInfo_PopulateAmmoComboBoxes @ 0x55def0]"},
	{"animadm", "First-person animations", "Animation", "The first-person animation file (.adm) the weapon's actions play; a missing file loads default.adm, and none leaves every auto delay at 0.", "[orig: Anim_InitActions @ 0x541FA0; AnimMap_LoadAdmFile @ 0x40CC40]"},
	{"soundfireloop", "Fire loop sound", "Sound", "A sound set looped while the weapon keeps firing, each shot extending it; when the shots stop, the fire tail sound plays.", "[orig: WeaponAction_ProcessFrame @ 0x540E60; ActionSlot_FinishActivePhase @ 0x53f7b0]"},
	{"soundtrailoff", "Fire tail sound", "Sound", "A sound set played once when a stream of fire ends and the fire loop runs out.", "[orig: WeaponAction_ProcessFrame @ 0x540E60]"},
	{"soundhead", "Volley start sound", "Sound", "A sound set played on the first shot of a stream of fire (no fire loop running yet).", "[orig: WeaponAction_Fire @ 0x542ccc..0x542ce9]"},
	{"vmacrotoken", "Gunner voice token", "Sound", "Added after the vehicle's prefix to name the radio and emote voice lines a gunner on this weapon says.", "[orig: VMacros_BuildShaderPassName @ 0x5BF5D0]"},
	{"soundlockedtone", "Lock tone", "Sound", "A sound set the local player hears looping while the weapon holds a target lock.", "[orig: Entity_UpdateInfantryPlayerBody @ 0x4b5229]"},
	{"launch_user_point", "Launch user point", "Models", "The user point on the third-person model where the weapon's shots and muzzle effects start; unnamed or unmatched, the carrier's own point is used.", "[orig: WeaponDef_ResolveAllReferences @ 0x5402C2..0x540316; Entity_ComputeUserpointWorldTransform @ 0x545D06]"},
	{"gfx1", "First-person model", "Models", "The first-person model: the gun drawn with the soldier's arms in the player's own view.", "[orig: Player_RenderFirstPersonViewModel @ 0x4ded60]"},
	{"gfx1a", "Arms model", "Models", "Ignored: the game recognises the line and stores nothing; first-person arms come from the soldier's character instead.", "[orig: WeaponDefs_ParseLineCallback, the plain return @ 0x545098]"},
	{"gfx1b", "Second arms model", "Models", "Ignored: the game recognises the line and stores nothing; first-person arms come from the soldier's character instead.", "[orig: WeaponDefs_ParseLineCallback, the plain return @ 0x545098]"},
	{"gfx3", "Third-person model", "Models", "The third-person model: the gun drawn in a soldier's hands, and the model whose launch user point shots start from.", "[orig: BoneCallback_org0_World @ 0x4e3940; WeaponDef_ResolveAllReferences @ 0x5402D8]"},
	{"crosshair", "Crosshair", "HUD", "The aim marker a ShowHudPip weapon draws where its barrel's line hits, replacing the ordinary crosshair.", "[orig: HUD_DrawCrosshair @ 0x592973..0x592AB8]"},
	{"crosshair_secondary", "Second crosshair", "HUD", "Unknown: the game loads it as a HUD texture; no reader of it is witnessed.", "hud-re.md, the HUD texture loaders ([orig: WeaponDefs_ParseLineCallback @ 0x5449A6])"},
	{"splash", "Impact preview radius", "HUD", "The impact preview's map circle radius, in world units, used when the weapon's aim error is zero (2DImpact weapons such as mortars).", "[orig: Player_UpdatePerFrame @ 0x4DEBBD]"},
	{"commanders_x", "Commander reticle", "HUD", "The reticle a ShowComander weapon's scoped view draws where an attached gun is aiming, joined to the screen centre by a line.", "[orig: HUD_DrawScopeOverlayDetails @ 0x59E74D..0x59E87F]"},
	{"hud_loadout_select", "Slot bar icon", "HUD", "The weapon's icon on the HUD weapon slot bar, shown for its category's first weapon; the stock hudpos.def never turns the bar on.", "[orig: HUD_DrawWeaponSlotBar @ 0x599CD0]"},
	{"hudicon", "HUD weapon icon", "HUD", "The weapon's silhouette drawn at the HUD's weapon icon position; it flashes brighter when the weapon changes.", "[orig: HUD_RenderOverlays @ 0x5A7CBE..0x5A7D64]"},
	{"hudclipgfx_texture", "Magazine picture", "HUD", "The magazine picture the HUD ammo indicator draws; if the file is missing the game skips the whole hudclipgfx line.", "[orig: HUD_DrawAmmoIndicator @ 0x599a30; WeaponDefs_ParseLineCallback @ 0x544295]"},
	{"hudclipgfx_offset[0]", "Magazine picture X", "HUD", "The magazine picture's X offset from the HUD's ammo indicator anchor, in design-screen pixels.", "[orig: HUD_DrawAmmoIndicator @ 0x599a30]"},
	{"hudclipgfx_offset[1]", "Magazine picture Y", "HUD", "The magazine picture's Y offset from the HUD's ammo indicator anchor, in design-screen pixels.", "[orig: HUD_DrawAmmoIndicator @ 0x599a30]"},
	{"hudrndgfx_texture", "Round icon", "HUD", "The icon the HUD ammo indicator draws once per loaded round; if the file is missing the game skips the whole hudrndgfx line.", "[orig: HUD_DrawAmmoIndicator @ 0x599a30; WeaponDefs_ParseLineCallback @ 0x544316]"},
	{"hudrndgfx_offset[0]", "First round icon X", "HUD", "The first round icon's X offset from the HUD's ammo indicator anchor, in design-screen pixels.", "[orig: HUD_DrawAmmoIndicator @ 0x599a30]"},
	{"hudrndgfx_offset[1]", "First round icon Y", "HUD", "The first round icon's Y offset from the HUD's ammo indicator anchor, in design-screen pixels.", "[orig: HUD_DrawAmmoIndicator @ 0x599a30]"},
	{"hudrndgfx_layout[0]", "Round icon step X", "HUD", "How far each next round icon moves across, in design-screen pixels.", "[orig: HUD_DrawAmmoIndicator @ 0x599b9c..0x599c0a]"},
	{"hudrndgfx_layout[1]", "Round icon step Y", "HUD", "How far each next round icon moves down, in design-screen pixels.", "[orig: HUD_DrawAmmoIndicator @ 0x599b9c..0x599c0a]"},
	{"hudrndgfx_layout[2]", "Rounds per icon", "HUD", "Above 1, the HUD draws one round icon per this many rounds (rounding up); at most 40 icons draw. Kept as a byte.", "[orig: HUD_DrawAmmoIndicator @ 0x599b9c..0x599c0a]"},
	{"actions_count", "Action count", "Animation", "Set by the action blocks: how many the weapon defines; any of the twelve actions it leaves out gets a generated default.", "[orig: Anim_InitActions @ 0x541fa0]"},
	{"flags", "Flags", "Handling", "The weapon's behaviour switches, one flags line each (Scoped, Sighted, Auto, Burst, Underwater, Emplaced and more); each turns on a rule the game tests.", "[orig: the flag token table @ 0x830bf0; e.g. WeaponSlot_CanFire @ 0x541ba0, WeaponAction_Fire @ 0x542b10]"},
	{"error[0]", "Hip spread prone", "Handling", "Spread, in degrees, of an un-aimed shot while prone; the crosshair opens to it then too.", "[orig: RoundData_SpawnRound @ 0x4ec0d0; HUD_DrawCrosshair @ 0x592640]"},
	{"error[1]", "Hip spread crouched", "Handling", "Spread, in degrees, of an un-aimed shot while crouched or mounted; the crosshair opens to it then too.", "[orig: RoundData_SpawnRound @ 0x4ec0d0; HUD_DrawCrosshair @ 0x592640]"},
	{"error[2]", "Hip spread standing", "Handling", "Spread, in degrees, of an un-aimed shot while standing, swimming or airborne; the crosshair opens to it then too.", "[orig: RoundData_SpawnRound @ 0x4ec0d0; HUD_DrawCrosshair @ 0x592640]"},
	{"error[3]", "Aimed spread", "Handling", "Spread, in degrees, of every aimed shot whatever the stance; the crosshair shows it when aiming prone.", "[orig: RoundData_SpawnRound @ 0x4ec0d0; HUD_DrawCrosshair @ 0x592640]"},
	{"error[4]", "Aimed crosshair crouched", "Handling", "The crosshair's spread, in degrees, when aiming crouched or mounted; fired rounds never read it.", "[orig: HUD_DrawCrosshair @ 0x592640; RoundData_SpawnRound @ 0x4ec0d0]"},
	{"error[5]", "Aimed crosshair standing", "Handling", "The crosshair's spread, in degrees, when aiming standing; fired rounds never read it.", "[orig: HUD_DrawCrosshair @ 0x592640; RoundData_SpawnRound @ 0x4ec0d0]"},
	{"pos[0]", "Hip view X", "Models", "Where the first-person gun and arms sit while not aiming: X offset from the eye, in 1/256 world units.", "[orig: Player_UpdateFirstPersonCamera @ 0x4dd380]"},
	{"pos[1]", "Hip view Y", "Models", "Where the first-person gun and arms sit while not aiming: Y offset from the eye, in 1/256 world units.", "[orig: Player_UpdateFirstPersonCamera @ 0x4dd380]"},
	{"pos[2]", "Hip view Z", "Models", "Where the first-person gun and arms sit while not aiming: Z offset from the eye, in 1/256 world units.", "[orig: Player_UpdateFirstPersonCamera @ 0x4dd380]"},
	{"pos_rotation_deg_q16[0]", "Hip view yaw", "Models", "The first-person gun and arms' yaw, in degrees, added to the view while not aiming.", "[orig: Player_UpdateFirstPersonCamera @ 0x4dd380]"},
	{"pos_rotation_deg_q16[1]", "Hip view pitch", "Models", "The first-person gun and arms' pitch, in degrees, added to the view while not aiming.", "[orig: Player_UpdateFirstPersonCamera @ 0x4dd380]"},
	{"pos_rotation_deg_q16[2]", "Hip view roll", "Models", "The first-person gun and arms' roll, in degrees, added to the view while not aiming.", "[orig: Player_UpdateFirstPersonCamera @ 0x4dd380]"},
	{"tpos[0]", "Aimed view X", "Models", "Where the first-person gun and arms sit when aiming: X offset in 1/256 world units, eased to from the hip view as the scope raises.", "[orig: Player_StepFpViewBiasInterp @ 0x4ddd20]"},
	{"tpos[1]", "Aimed view Y", "Models", "Where the first-person gun and arms sit when aiming: Y offset in 1/256 world units, eased to from the hip view as the scope raises.", "[orig: Player_StepFpViewBiasInterp @ 0x4ddd20]"},
	{"tpos[2]", "Aimed view Z", "Models", "Where the first-person gun and arms sit when aiming: Z offset in 1/256 world units, eased to from the hip view as the scope raises.", "[orig: Player_StepFpViewBiasInterp @ 0x4ddd20]"},
	{"tpos_rotation_deg_q16[0]", "Aimed view yaw", "Models", "The first-person gun and arms' yaw, in degrees, when aiming; eased to from the hip yaw as the scope raises.", "[orig: Player_StepFpViewBiasInterp @ 0x4DDD2B..0x4DDFC3]"},
	{"tpos_rotation_deg_q16[1]", "Aimed view pitch", "Models", "The first-person gun and arms' pitch, in degrees, when aiming; eased to from the hip pitch as the scope raises.", "[orig: Player_StepFpViewBiasInterp @ 0x4DDD2B..0x4DDFC3]"},
	{"tpos_rotation_deg_q16[2]", "Aimed view roll", "Models", "The first-person gun and arms' roll, in degrees, when aiming; eased to from the hip roll as the scope raises.", "[orig: Player_StepFpViewBiasInterp @ 0x4DDD2B..0x4DDFC3]"},
	{"sights_count", "Sight row count", "Scope", "Set by the sights lines: how many rows the sight card draws; any row at all hides the scope's own cross and grid.", "[orig: HUD_DrawWeaponSightOverlays @ 0x4dce00; Hud_DrawScopeCircleMask @ 0x5d17a0]"},
	{"loadout_menu_textid", "Loadout name", "Loadout", "The weapon's name in the loadout screen's lists: a key of gametext.bin's WepDes section; absent, the raw weapon name shows.", "[orig: PlayerInfo_PopulateWeaponSlotLists @ 0x560430]"},
	{"loadout_menu_ttdesc", "Loadout tooltip", "Loadout", "Unknown: the loadout reader stores it as the row's tooltip text; no reader of it is witnessed.", "avatars-re.md, the producer field map ([orig: WeaponDef_ParseProperty @ 0x54d730])"},
	{"loadout_menu_icon", "Loadout icon", "Loadout", "The picture the loadout screen's weapon icon window shows while this weapon is selected.", "[orig: PlayerInfo_UpdateWeightAndWeaponIcons @ 0x55f480]"},
	{"weapon_class_slot", "Loadout list number", "Loadout", "Set by weapon_class: the list number the loadout screen routes by (0 accessory, 1 primary, 2 secondary, 3 grenade).", "def_weapons.cpp (derived; [orig: WeaponDef_ParseProperty @ 0x54d730])"},
	{"teamfilter_mask", "Loadout team bits", "Filters", "Set by the teamfilter lines: the loadout screen's team bits (blue or yellow 2, red or violet 1) that decide which side lists it.", "def_weapons.cpp (derived; [orig: WeaponDef_ParseProperty @ 0x54daae..0x54db08])"},
	{"charfilter_mask", "Loadout class bits", "Filters", "Set by the charfilter lines: the loadout screen's class bits (medic 1, sniper 2, gunner 4, rifleman 8, engineer 16).", "def_weapons.cpp (derived; [orig: WeaponDef_ParseProperty @ 0x54d916..0x54d9be])"},
	{"weaponweight", "Weapon weight", "Handling", "The weapon's weight: added to the loadout's total weight, the kit weight that picks the run speed, and the aim sway while moving.", "[orig: PlayerInfo_CalculateLoadoutWeight @ 0x55f1f0; Entity_UpdateInfantryPlayerBody @ 0x4b40e0]"},
	{"clipweight", "Magazine weight", "Handling", "A magazine's weight: times the magazines carried in the loadout total and the run-speed kit weight; once into the aim sway while moving.", "[orig: PlayerInfo_CalculateLoadoutWeight @ 0x55f1f0; Server_RecalculateAllPlayerScores @ 0x5014E0]"},
	{"renderfov", "First-person field of view", "Models", "The horizontal field of view, in degrees, the first-person gun and arms are drawn with (default 80); the world view keeps its own.", "[orig: Player_RenderFirstPersonViewModel @ 0x4ded60, read @ 0x4dee71]"},
	{"scope_max_mag", "Most zoom", "Scope", "The scope's most zoom: the scoped view's field of view is 80 degrees over the zoom, which the zoom keys step up to this.", "[orig: Player_ToggleWeaponScope @ 0x4df401; Player_AdjustWeaponElevation @ 0x4dbe29..0x4dbe57]"},
	{"special_hold", "Hold pose", "Animation", "Third-person hold pose: 1 knife, 2 pistol (reloads with reload2), 3 grenade, 4 stinger, 5 designator, 6 P90, 7 MP7, 8 javelin; others: rifle.", "[orig: Entity_UpdateInfantryPlayerBody @ 0x4b5dba..0x4b5e35]"},
	{"attack_anim", "Fire body animation", "Animation", "What firing plays on the third-person body: 1 knife_attack, 2 grenade_attack; any other value plays nothing.", "[orig: WeaponAction_Fire @ 0x542bbc]"},
	{"flags2", "Flags, second set", "Handling", "Set by the same flags lines: the second set of switches (NoSelect, Parachute, Thermal, ViewLock, Inset, NoAutoZero, Invisible and more).", "[orig: the flag token table @ 0x830bf0; e.g. Player_SwitchToWeaponByHandle @ 0x4e02c3]"},
	{"run_anim", "Run tier bonus", "Animation", "Added to the carried-weight tier that picks the run: 0 walks, 1 jogs (run_2), 2 or more sprints (run_3).", "[orig: Entity_UpdateInfantryPlayerBody @ 0x4b72aa..0x4b731b]"},
	{"attach_text_id", "Attach label", "HUD", "The floating label drawn over this mounted gun: a key of gametext.bin's Overlays section; absent, the default use-gun label shows.", "[orig: HUD_DrawVehicleSeatAndArmoryLabels @ 0x5a3538; WeaponDefs_ParseLineCallback @ 0x544d6c]"},
	{"classrounds[0]", "Unused start rounds 1", "Ammo", "Unused: no class token stores here (the class values are 1, 2, 3, 5 and 6), so it stays 0.", "[orig: classrounds handler @ 0x543ab0; class table @ 0x830EE8]"},
	{"classrounds[1]", "Medic start rounds", "Ammo", "A medic's starting rounds for this weapon, used in place of startrounds; 0 uses startrounds.", "[orig: WeaponSlots_SeedAmmoPoolsFromDefs @ 0x5416c4]"},
	{"classrounds[2]", "Sniper start rounds", "Ammo", "A sniper's starting rounds for this weapon, used in place of startrounds; 0 uses startrounds.", "[orig: WeaponSlots_SeedAmmoPoolsFromDefs @ 0x5416c4]"},
	{"classrounds[3]", "Gunner start rounds", "Ammo", "A gunner's starting rounds for this weapon, used in place of startrounds; 0 uses startrounds.", "[orig: WeaponSlots_SeedAmmoPoolsFromDefs @ 0x5416c4]"},
	{"classrounds[4]", "Unused start rounds 5", "Ammo", "Unused: no class token stores here (the class values are 1, 2, 3, 5 and 6), so it stays 0.", "[orig: classrounds handler @ 0x543ab0; class table @ 0x830EE8]"},
	{"classrounds[5]", "Rifleman start rounds", "Ammo", "A rifleman's starting rounds for this weapon, used in place of startrounds; 0 uses startrounds.", "[orig: WeaponSlots_SeedAmmoPoolsFromDefs @ 0x5416c4]"},
	{"classrounds[6]", "Engineer start rounds", "Ammo", "An engineer's starting rounds for this weapon, used in place of startrounds; 0 uses startrounds.", "[orig: WeaponSlots_SeedAmmoPoolsFromDefs @ 0x5416c4]"},
	{"switchcategory", "Switch when empty", "Handling", "When a shot leaves the weapon empty with nothing to reload, the game switches to this category, trying its rank 0 first.", "[orig: WeaponAction_Recoil @ 0x54307c]"},
	{"has_switchcategory", "Has switch when empty", "Handling", "Set when a switchcategory line is present: only then does the emptied weapon switch away.", "def_weapons.cpp (derived; [orig: switchcategory handler @ 0x5445a8])"},
	{"heat_per_shot", "Heat per shot", "Heat", "Heat a shot adds, as a percent of the overheat point where fire clicks dry; 0 turns the weapon's heat off.", "[orig: WeaponAction_Recoil @ 0x542f8b..0x542fdc; WeaponAction_ProcessFrame @ 0x541046]"},
	{"heat_decay_per_tick", "Cooling per second", "Heat", "How fast the weapon cools, in percent of the overheat point a second (the game keeps it per 62 Hz tick).", "[orig: WeaponSlot_CalcAccumulatedHeat @ 0x53f780]"},
	{"heat_glow_threshold", "Glow threshold", "Heat", "The heat level, as a fraction of the overheat point, above which the barrel glow effect shows, brighter as heat climbs.", "[orig: WeaponAction_ProcessFrame @ 0x540fed..0x54125f]"},
	{"heat_effect", "Heat glow effect", "Heat", "The particle effect drawn at the overheated action's muzzle point while heat is above the glow threshold.", "[orig: WeaponAction_ProcessFrame @ 0x540fed..0x54125f]"},
	{"heat_sound", "Heat sound", "Heat", "Unused: the game looks the sound set up and stores it, but nothing in the game reads it back.", "novaworld-net-re.md, the weapon heat model ([orig: WeaponDefs_ParseLineCallback @ 0x543eac])"},
	{"error_fp16[0]", "Hip spread prone (exact)", "Handling", "Set by the error line: Hip spread prone as the game's exact fixed-point number, the value shots and the crosshair read.", "[orig: WeaponDefs_ParseLineCallback @ 0x543B21; Math_ParseFixedPoint16 @ 0x6131F0]"},
	{"error_fp16[1]", "Hip spread crouched (exact)", "Handling", "Set by the error line: Hip spread crouched as the game's exact fixed-point number, the value shots and the crosshair read.", "[orig: WeaponDefs_ParseLineCallback @ 0x543B21; Math_ParseFixedPoint16 @ 0x6131F0]"},
	{"error_fp16[2]", "Hip spread standing (exact)", "Handling", "Set by the error line: Hip spread standing as the game's exact fixed-point number, the value shots and the crosshair read.", "[orig: WeaponDefs_ParseLineCallback @ 0x543B21; Math_ParseFixedPoint16 @ 0x6131F0]"},
	{"error_fp16[3]", "Aimed spread (exact)", "Handling", "Set by the error line: Aimed spread as the game's exact fixed-point number, the value shots and the crosshair read.", "[orig: WeaponDefs_ParseLineCallback @ 0x543B21; Math_ParseFixedPoint16 @ 0x6131F0]"},
	{"error_fp16[4]", "Aimed crosshair crouched (exact)", "Handling", "Set by the error line: Aimed crosshair crouched as the game's exact fixed-point number, the value the crosshair reads.", "[orig: WeaponDefs_ParseLineCallback @ 0x543B21; Math_ParseFixedPoint16 @ 0x6131F0]"},
	{"error_fp16[5]", "Aimed crosshair standing (exact)", "Handling", "Set by the error line: Aimed crosshair standing as the game's exact fixed-point number, the value the crosshair reads.", "[orig: WeaponDefs_ParseLineCallback @ 0x543B21; Math_ParseFixedPoint16 @ 0x6131F0]"},
	{"error_hip_theta_fp16", "Hip vertical spread", "Handling", "The up-down spread, in degrees, of an un-aimed shot crouched or standing; 0 uses the hip spread both ways.", "[orig: RoundData_SpawnRound @ 0x4ec0d0; Weapon_CalcRandomSpreadOffset @ 0x4E4120]"},
	{"error_up_theta_fp16", "Aimed vertical spread", "Handling", "The up-down spread, in degrees, of an aimed or prone shot; 0 uses that shot's spread both ways.", "[orig: RoundData_SpawnRound @ 0x4ec0d0; Weapon_CalcRandomSpreadOffset @ 0x4E4120]"},
	{"weaponweight_fp16", "Weapon weight (exact)", "Handling", "Set by the weaponweight line: the weight as the game's exact fixed-point number, which the run-speed kit weight and moving aim sway read.", "[orig: WeaponDefs_ParseLineCallback @ 0x54410D; Entity_UpdateInfantryPlayerBody @ 0x4b40e0]"},
	{"clipweight_fp16", "Magazine weight (exact)", "Handling", "Set by the clipweight line: the weight as the game's exact fixed-point number, which the run-speed kit weight and moving aim sway read.", "[orig: WeaponDefs_ParseLineCallback @ 0x5440DB; Entity_UpdateInfantryPlayerBody @ 0x4b40e0]"},
	{"stability_fp16[0]", "Prone scope sway", "Scope", "Multiplies the scoped aim drift while prone; 1 (the default when the line is absent) keeps it, 0 removes it.", "[orig: Entity_UpdateInfantryPlayerBody @ 0x4B5966..0x4B5C97]"},
	{"stability_fp16[1]", "Crouched scope sway", "Scope", "Multiplies the scoped aim drift while crouched; 1 (the default when the line is absent) keeps it, 0 removes it.", "[orig: Entity_UpdateInfantryPlayerBody @ 0x4B5966..0x4B5C97]"},
	{"stability_fp16[2]", "Standing scope sway", "Scope", "Multiplies the scoped aim drift while standing; 1 (the default when the line is absent) keeps it, 0 removes it.", "[orig: Entity_UpdateInfantryPlayerBody @ 0x4B5966..0x4B5C97]"},
	{"scope_max_zero_steps", "Zero steps", "Scope", "The most range-zero steps the scope's zero keys reach (up to 40 ranges are worked out); 0 turns zeroing off.", "[orig: Player_AdjustWeaponZoomLevel @ 0x4dbd0c..0x4dbd3f; WeaponSlot_CalcElevationTable @ 0x545100]"},
	{"scope_zero_step", "Metres per zero step", "Scope", "Metres each zero step adds to the range the scope's elevation is zeroed for.", "[orig: WeaponSlot_CalcElevationTable @ 0x545100]"},
	{"scope_zero_default", "Default zero range", "Scope", "The range in metres the scope starts zeroed at, taken in whole zero steps.", "[orig: WeaponSlot_InitFromDef @ 0x53ee70]"},
	{"scope_zero_extra", "Least zero step", "Scope", "When nonzero, the lowest step the scope's zero adjustment allows.", "[orig: Player_AdjustWeaponZoomLevel @ 0x4dbd0c..0x4dbd3f]"},
	{"scope_paralax_distance_fp16", "Sight offset", "Scope", "The sight's offset from the barrel, in metres: the scoped view turns by its angle at the zeroed range; 0 turns it off.", "[orig: WeaponSlot_InitFromDef @ 0x53ef4f..0x53ef8b; Player_AdjustWeaponZoomLevel @ 0x4dbd91..0x4dbdd5]"},
	{"scope_max_mag_arg2", "Starting zoom", "Scope", "The zoom a scope starts at, kept between the least and the most zoom; 0 or absent starts it at the least.", "[orig: WeaponSlot_InitFromDef @ 0x53ef2d..0x53ef44]"},
	{"scope_min_mag", "Least zoom", "Scope", "The scope's least zoom (default 2): the zoom keys step no lower, and a scope starts here unless told otherwise.", "[orig: Player_AdjustWeaponElevation @ 0x4dbe29..0x4dbe57; WeaponSlot_InitFromDef @ 0x53ef2d..0x53ef44]"},
	{"emplacedstance", "Gunner stance icon", "HUD", "For a gunner on this weapon, the HUD stance icon to show, plus one; 0 shows the Emplaced icon.", "[orig: HUD_BuildEntityInfo @ 0x4B8539..0x4B8549]"},
	{"farp_rounds", "FARP rounds", "Ammo", "Rounds a vehicle's weapon regains in clip and reserve at each FARP top-up; 0 adds none.", "[orig: Server_UpdateEntityTargetLockAndWeaponOverlays @ 0x51190b..0x511934]"},
	{"farp_interval", "FARP interval", "Ammo", "How many FARP rearm steps pass per top-up (every Nth step while standing on the FARP); 0 adds none.", "[orig: Server_UpdateEntityTargetLockAndWeaponOverlays @ 0x51190b..0x511934]"},
	{"designation_ticks", "Designation time", "Handling", "How long a target this weapon designates stays designated, in seconds (kept as 62 Hz ticks).", "[orig: WeaponDefs_ParseLineCallback @ 0x544895; the DesignateTarget spawn leg @ 0x4ec264]"},
	{"sameas", "Same as weapon", "Identity", "A weapon this one stands in for: a pickup of this weapon refills that weapon's slot when the picker holds it.", "[orig: WeaponSlot_InitFromAvatarDef @ 0x542779..0x5427C8]"},
	{"gfx1_nocheckdepth", "First-person no depth check", "Models", "On (the gfx1 line's nocheckdepth option): the game loads the first-person model with its depth check turned off.", "[orig: WeaponDefs_ParseLineCallback @ 0x544F92 / 0x544FA4]"},
	{"gfx3_nocheckdepth", "Third-person no depth check", "Models", "On (the gfx3 line's nocheckdepth option): the game loads the third-person model with its depth check turned off.", "[orig: WeaponDefs_ParseLineCallback @ 0x544F92 / 0x544FA4]"},
};

const DefWords kActionWords[] = {
	{"name", "Action", "Action", "Which of the weapon's twelve actions this block defines (idle, fire, recoil, reload, switchto, scopeup, overheated...); a later block of the name replaces it.", "[orig: ActionDef_ParseScriptLine @ 0x4023C0; the action suffix table @ 0x830B90]"},
	{"anim", "Animation", "Action", "The animation slot (such as anim_wpn_fire) the action plays on the first-person gun and arms from the weapon's animadm file.", "[orig: ActionSlot_BeginActivePhase @ 0x53f830; AnimMap_FindSlotByName @ 0x40cfa0]"},
	{"function", "Handler", "Function", "The handler the action runs (wpn_std_fire, wpn_std_reload...); none, null or an unknown name runs the action's own default handler.", "[orig: Anim_InitActions @ 0x542117..0x542139; ActionFuncDef_FindByName @ 0x401040]"},
	{"delaystart", "Active ticks", "Timing", "Ticks the action runs, its clip playing, before it finishes (a reload refills, a shot's end sound plays); auto takes the clip's length.", "[orig: WeaponAction_ProcessFrame @ 0x5413ea; Anim_InitActions @ 0x5421c5]"},
	{"delayend", "End delay", "Timing", "Ticks the weapon holds after the action finishes before another may start; fire's sets the rate of fire. auto: the clip less the active ticks.", "[orig: ActionSlot_FinishActivePhase @ 0x53f7b0; Anim_InitActions @ 0x5421d8]"},
	{"soundset", "Begin sound", "Sound", "A sound set played at the weapon's owner as the action begins.", "[orig: ActionSlot_BeginActivePhase @ 0x53f873]"},
	{"soundsetend", "End sound", "Sound", "A sound set played when the action finishes (the gunshot of fire, the end of a reload); an action cut short plays nothing.", "[orig: ActionSlot_FinishActivePhase @ 0x53f7d6]"},
	{"particle", "Particle effect", "Effects", "The particle effect the action spawns at its user point: fire's on the shot (the muzzle flash), recoil's at its end (casings); reload's never shows.", "[orig: ActionSlot_ExecuteActionTick @ 0x541a70; WeaponAction_Recoil @ 0x542efa; @ 0x543150]"},
	{"particleuserpoint", "Effect user point", "Effects", "The user point, on the third-person and the first-person model, where the action's particle effect spawns.", "[orig: WeaponDef_ResolveAllReferences @ 0x540270]"},
	{"action_value", "Tank rock", "Effects", "Read from the fire block: how hard a shot rocks the tank carrying this occupied gun, pushed against the gun's aim.", "[orig: WeaponAction_Fire @ 0x542B10; ActionSlot_ExecuteAction @ 0x4020A0]"},
	{"ctrl_register", "Control register", "Effects", "A model control register (a CTRL name) the action animates on the weapon's model as it begins; no shipped weapon uses it.", "[orig: ActionSlot_BeginActivePhase @ 0x53F878; CtrlRegAnimSlot_UpdateAll @ 0x401BF0]"},
	{"ctrl_increment", "Control step", "Effects", "The step the action's control register animation moves by on each update (with ctrlreg).", "[orig: CtrlRegAnimSlot_Allocate @ 0x401CA0]"},
	{"duplicate_sound_count", "End sound copies", "Sound", "How many times the end sound plays in all: the finish schedules this many less one delayed repeats (1 counts as 0).", "[orig: ActionSlot_PlayEndSoundAndDupes @ 0x401100; ActionDef_ParseScriptLine @ 0x40260F]"},
	{"duplicate_sound_delay", "End sound repeat delay", "Sound", "The delay between the end sound's repeated plays.", "[orig: ActionSlot_PlayEndSoundAndDupes @ 0x401100]"},
	{"text_token", "Text key", "Action", "Unknown: the parser looks the key up as text and stores it; no weapon reader is witnessed (powerup actions show it as HUD text).", "[orig: ActionDef_ParseScriptLine @ 0x4028B7]"},
	{"function_args[0]", "Handler argument 1", "Function", "Unknown: the parser packs the number written after the handler's name; no handler is witnessed reading it.", "[orig: ActionDef_ParseScriptLine @ 0x40296E..0x402A91]"},
	{"function_args[1]", "Handler argument 2", "Function", "Unknown: the parser packs the number written after the handler's name; no handler is witnessed reading it.", "[orig: ActionDef_ParseScriptLine @ 0x40296E..0x402A91]"},
	{"function_args[2]", "Handler argument 3", "Function", "Unknown: the parser packs the number written after the handler's name; no handler is witnessed reading it.", "[orig: ActionDef_ParseScriptLine @ 0x40296E..0x402A91]"},
	{"function_args[3]", "Handler argument 4", "Function", "Unknown: the parser packs the number written after the handler's name; no handler is witnessed reading it.", "[orig: ActionDef_ParseScriptLine @ 0x40296E..0x402A91]"},
	{"function_args_count", "Handler argument count", "Function", "Set by the function line: how many numbers follow the handler's name (0 to 4).", "def_weapons.cpp (derived; [orig: ActionDef_ParseScriptLine @ 0x40296E..0x402A91])"},
};

const DefWords kSightWords[] = {
	{"texture", "Sight picture", "", "The picture (a TGA) one row of the weapon's sight card draws over the aimed view; any row hides the scope's own cross and grid.", "[orig: HUD_DrawWeaponSightOverlays @ 0x4dce00; WeaponDef_CreateBlendNamedMaterial @ 0x540180]"},
	{"x1", "Left", "", "The row's left edge on the 1024 by 768 HUD design screen.", "[orig: HUD_DrawWeaponSightOverlays @ 0x4dce00]"},
	{"y1", "Top", "", "The row's top edge on the 1024 by 768 HUD design screen.", "[orig: HUD_DrawWeaponSightOverlays @ 0x4dce00]"},
	{"x2", "Right", "", "The row's right edge on the 1024 by 768 HUD design screen.", "[orig: HUD_DrawWeaponSightOverlays @ 0x4dce00]"},
	{"y2", "Bottom", "", "The row's bottom edge on the 1024 by 768 HUD design screen.", "[orig: HUD_DrawWeaponSightOverlays @ 0x4dce00]"},
	{"blend", "Blend mode", "", "How the row's picture mixes with the view: blend, add or multiply (doubled: mid-grey is neutral); -at forms drop pixels at half alpha or less.", "[orig: WeaponDef_CreateBlendNamedMaterial @ 0x540180]"},
	{"scale", "Scales with dot size", "", "On (the scale token): the row's box shrinks or grows about its centre with the player's sight dot size key.", "[orig: HUD_DrawWeaponSightOverlays @ 0x4dce8d..0x4dcf2c]"},
	{"slide", "Slides with zero", "", "On (the slide token): the row moves down with the scope's zero, by its slide amount per zero step.", "[orig: HUD_DrawWeaponSightOverlays @ 0x4dcf42..0x4dd043]"},
	{"slide_frames", "Slide per step", "", "How far a sliding row moves down, in design-screen pixels, for each step of the scope's zero.", "[orig: HUD_DrawWeaponSightOverlays @ 0x4dcf42..0x4dd043]"},
};

const DefWords kCarryWords[] = {
	{"name", "Ammo class", "", "The ammo class this limit is for, as weapons name it on their ammoclass line; a later line for the class overwrites it.", "[orig: WeaponDefs_ParseLineCallback @ 0x5437FE; sub_540590 @ 0x540590]"},
	{"max_carry", "Carry limit", "", "The most this ammo class's pool holds: setting or adding reserve clamps to it (the number's absolute value); a class with no line holds 0.", "[orig: WeaponSlot_SetAmmoCount @ 0x540B50; WeaponSlot_AddAmmo @ 0x540A20]"},
};

const DefWords kAmmoWords[] = {
	{"doppler_divisor", "Doppler divisor", "Sound", "Unknown: the parser stores the number as a byte (0 to 255); no reader of it is witnessed.", "[orig: AmmoDef_ParseProperty @ 0x40A6E9]"},
	{"kz_sound", "Blast victim sound", "Sound", "The sound set played at full volume on each person the round's blast reaches.", "[orig: Projectile_ProcessExplosionQueue @ 0x4EB1DA]"},
	{"secondary_effect", "Blast victim effect", "Effects", "The particle effect attached to each person the round's blast reaches.", "[orig: Projectile_ProcessExplosionQueue @ 0x4EB1F2]"},
	{"name", "Ammo name", "Identity", "Set by the ammo header line: weapons, items and other ammo find this ammo by its name, ignoring case, the first match winning.", "[orig: AmmoDef_LookupByName @ 0x409870]"},
	{"velocity", "Muzzle velocity", "Flight", "The round's launch speed, in units a second.", "[orig: RoundData_SpawnRound @ 0x4EC0D0]"},
	{"heat_det_range", "Seeker range", "Guidance", "How far a heat seeker can lock on, in units; a target farther away is not acquired.", "[orig: Entity_FindTargets @ 0x53A610]"},
	{"boresight_maxang", "Seeker cone", "Guidance", "The heat seeker's lock-on cone in degrees: a target must lie within this angle of the aim to be acquired.", "[orig: Entity_FindTargets @ 0x53A610]"},
	{"min_damage", "Least damage", "Damage", "The least damage a direct hit does; a lower speed-based damage is raised to it.", "[orig: Weapon_CalcImpactDamage @ 0x4EC920]"},
	{"max_damage", "Most damage", "Damage", "The most damage a direct hit does; 0 means no cap.", "[orig: Weapon_CalcImpactDamage @ 0x4EC920]"},
	{"penetration_impact", "Impact penetration", "Damage", "A direct hit does no damage to a target whose impact armour class is higher than this.", "[orig: Projectile_ProcessDamageOnTarget @ 0x4E7FB0]"},
	{"penetration_kz", "Blast penetration", "Blast", "The blast does no damage to a target whose blast armour class is higher than this.", "[orig: Entity_ApplyWeaponDamage @ 0x4E6820]"},
	{"armor_density[0]", "Armour density 1", "Damage", "How much body armour slows this round on a torso hit when the shooter's ammo class is the default, 0.", "[orig: Weapon_CalcImpactDamage @ 0x4EC920]"},
	{"armor_density[1]", "Armour density 2", "Damage", "How much body armour slows this round on a torso hit when the shooter's ammo class is 1.", "[orig: Weapon_CalcImpactDamage @ 0x4EC920]"},
	{"armor_density[2]", "Armour density 3", "Damage", "How much body armour slows this round on a torso hit when the shooter's ammo class is 2.", "[orig: Weapon_CalcImpactDamage @ 0x4EC920]"},
	{"secondary_anim", "Burn reaction", "Damage", "The hit reaction a struck person plays: 1 to 4 pick the four burn animations; 0 plays none.", "[orig: Entity_ApplyCollisionForce @ 0x4AF4A0]"},
	{"kz_physics", "Hit shove", "Damage", "How a hit moves a struck person: 1 a walking push, 2 a drift, 3 the local player's white hit flash, 4 a direct push.", "[orig: Entity_ApplyCollisionForce @ 0x4AF4A0]"},
	{"recoil[0]", "Recoil 1", "Flight", "The kick each shot adds to the shooter's aim and spread while prone.", "[orig: RoundData_SpawnRound @ 0x4EC0D0]"},
	{"recoil[1]", "Recoil 2", "Flight", "The kick each shot adds to the shooter's aim and spread while crouched or mounted.", "[orig: RoundData_SpawnRound @ 0x4EC0D0]"},
	{"recoil[2]", "Recoil 3", "Flight", "The kick each shot adds to the shooter's aim and spread while standing, airborne or under water.", "[orig: RoundData_SpawnRound @ 0x4EC0D0]"},
	{"flags", "Flags", "Identity", "Named switches, one per flag line (shotgun, claymore, nogravity, noage, forcetracer and others), that change how the round spawns, flies and hits.", "[orig: RoundData_SpawnRound @ 0x4EC0D0]"},
	{"max_age_ticks", "Lifetime", "Flight", "How long the round flies before it expires, in seconds; the noage flag makes it never expire.", "[orig: Projectile_UpdatePhysics @ 0x4E9D70]"},
	{"arm_age_ticks", "Arming time", "Blast", "Seconds after firing before the round is armed; an earlier hit makes no kill zone, and on a person or object swaps in the not-armed ammo.", "[orig: Projectile_HandleEntityImpact @ 0x4E9390]"},
	{"error_fp16", "Spread", "Flight", "The round's random spread in degrees, used only when it is fired without a weapon definition; otherwise the weapon's error applies.", "[orig: RoundData_SpawnRound @ 0x4EC0D0]"},
	{"drag_fp16", "Drag", "Flight", "The round's air-drag divisor: each tick it slows by a speed-table amount divided by this, so a larger number slows it less.", "[orig: Entity_ApplyDragAndBounceForce @ 0x4E5EC0]"},
	{"bullet_radius_fp16", "Bullet radius", "Flight", "The round's radius when its path is tested for hits each tick.", "[orig: Projectile_UpdatePhysics @ 0x4E9D70]"},
	{"spread_count", "Pellet count", "Flight", "How many pellets a shotgun or claymore round fires at once, from 1 to 32.", "[orig: Weapon_SpawnProjectileBurst @ 0x4EB900]"},
	{"kztype", "Kill zone type", "Blast", "The kill zone an armed hit makes: knife, standard, medic (heals), radius blast, C4, bullets or slash; none makes no kill zone.", "[orig: Projectile_ProcessExplosionQueue @ 0x4EAD80]"},
	{"kz_damage", "Blast damage", "Blast", "The kill zone's damage: a blast's full damage before it fades with distance, or the whole damage of a knife hit.", "[orig: Entity_ApplyWeaponDamage @ 0x4E6820]"},
	{"weight_in_grains", "Bullet weight", "Damage", "The bullet's mass: a hit's damage is its speed times this divided by 875, and heavier rounds lose less speed to body armour.", "[orig: Weapon_CalcImpactDamage @ 0x4EC920]"},
	{"min_stable_velocity", "Stable speed", "Flight", "Below this speed, in units a second, the round becomes unstable: it is damped each tick and tumbles off line.", "[orig: Entity_ApplyDragAndBounceForce @ 0x4E5EC0]"},
	{"tumble_error_fp16", "Tumble error", "Flight", "The random deflection a round takes when it slows below its stable speed.", "[orig: Entity_ApplyDragAndBounceForce @ 0x4E5EC0]"},
	{"tracer_rate", "Tracer rate", "Tracer", "One round in this many from a weapon is a tracer; 0 makes none unless the forcetracer flag is set.", "[orig: RoundData_SpawnRound @ 0x4EC184]"},
	{"notarmmed_ammo", "Not-armed ammo", "Blast", "The ammo that takes the round's place when it strikes something before it is armed, such as a dud grenade.", "[orig: Projectile_HandleEntityImpact @ 0x4E9390]"},
	{"ai_launch", "AI fire sound", "Sound", "The firing sound when a computer-controlled shooter fires this round, heard after a delay that grows with distance.", "[orig: Sound_PlayWithDistanceAttenuation @ 0x528E40]"},
	{"ai_launcheffect", "AI muzzle effect", "Effects", "The muzzle effect spawned at the gun each time a computer-controlled shooter fires this round.", "[orig: WeaponSlot_FireAndSpawnEffects @ 0x53F440]"},
	{"mf_light", "Muzzle light", "Light", "Set by an MF_Light line being present: each shot of this round then lights a brief muzzle-flash glow at the gun.", "[orig: Entity_UpdateMuzzleGlowEffect @ 0x56C960]"},
	{"mf_light_value", "Muzzle light value", "Light", "Unknown: the parser stores the MF_Light line's number; no reader of it is witnessed, and the muzzle glow does not use it.", "world-wac-ai-re.md, section 18.3"},
	{"tracer_type_friendly", "Friendly tracer style", "Tracer", "The streak drawn behind a tracer seen by its own side: stdred, stdgreen, rocket, at4, grenade, rapidred, rapidgreen, sniperred, snipergreen, df1red or df1green.", "[orig: RoundData_SpawnRound @ 0x4EC740]"},
	{"tracer_type_enemy", "Enemy tracer style", "Tracer", "The streak drawn behind a tracer seen by the other side; a tracer_type line naming one style sets both.", "[orig: RoundData_SpawnRound @ 0x4EC740]"},
	{"frndly_trcr_type_id", "Friendly tracer item", "Tracer", "The items.def id of the item a round becomes for its own side: its model shows on tracer shots, its behaviour always applies.", "[orig: RoundData_SpawnRound @ 0x4EC79B]"},
	{"foe_trcr_type_id", "Enemy tracer item", "Tracer", "The items.def id of the item a round becomes when seen by the other side; its model shows only on tracer shots.", "[orig: RoundData_SpawnRound @ 0x4EC79B]"},
	{"light_move_radius_fp16", "Flight light radius", "Light", "Radius of the light that follows the round in flight, from launch until the round dies.", "[orig: RoundData_SpawnRound @ 0x4EC8A9]"},
	{"light_move_color", "Flight light colour", "Light", "Colour, as red, green and blue, of the light that follows the round in flight.", "[orig: RoundData_SpawnRound @ 0x4EC8A9]"},
	{"turnrate_maxpit", "Max pitch turn rate", "Guidance", "How fast a guided missile can turn up or down, in degrees a second; 0 uses the game's built-in rate.", "[orig: Entity_UpdateTurretAim @ 0x445CC0]"},
	{"turnrate_maxyaw", "Max yaw turn rate", "Guidance", "How fast a guided missile can turn left or right, in degrees a second; 0 uses the game's built-in rate.", "[orig: Entity_UpdateTurretAim @ 0x445CC0]"},
	{"effects_table_count", "Impact rows", "Effects", "Derived, not a key of its own: the number of rows in the ammo's effects_table block.", "[orig: AmmoDef_InitEffectsTable @ 0x409F20]"},
	{"kz_minradius_fp16", "Full-damage radius", "Blast", "Within this distance a blast does full damage, fading linearly to none at the blast radius; also a claymore's trigger range.", "[orig: Entity_ApplyWeaponDamage @ 0x4E6820]"},
	{"kz_maxradius_fp16", "Blast radius", "Blast", "How far the kill zone reaches: the blast radius, a knife's reach, and a medic kit's healing range.", "[orig: Projectile_ProcessExplosionQueue @ 0x4EAD80]"},
	{"kz_pieslice_bam", "Blast cone angle", "Blast", "Nonzero narrows the blast to a cone of this angle around its direction; pellets spread within it and claymores trigger inside it.", "[orig: Projectile_ProcessExplosionQueue @ 0x4EAD80]"},
	{"light_impact_radius_fp16", "Impact light radius", "Light", "Radius of the flash of light at the impact point, shown when the impact effect plays.", "[orig: AmmoDef_ProcessImpactEffect @ 0x40A2B3]"},
	{"light_impact_color", "Impact light colour", "Light", "Colour, as red, green and blue, of the flash of light at the impact point.", "[orig: AmmoDef_ProcessImpactEffect @ 0x40A2B3]"},
	{"light_impact_ticks", "Impact light fade", "Light", "How long the impact flash takes to fade, in seconds; 0 or no value fades it in 10 ticks.", "[orig: AmmoDef_ProcessImpactEffect @ 0x40A2B3]"},
	{"scorch_id", "Ground scorch", "Effects", "The permanent scorch left in the ground where the round hits terrain: 1 small, 2 or 7 large, 8 a burn; others leave none.", "[orig: Projectile_HandleTerrainImpact @ 0x4E9314]"},
	{"scar_type", "Impact scar", "Effects", "The mark left where the round hits: 0 none, 1 a glass decal or else a ring scar, 2 the glass decal only.", "[orig: AmmoDef_ProcessImpactEffect @ 0x40A24E]"},
};

const DefWords kEffectWords[] = {
	{"surface_type", "Surface", "", "The surface tag (dirt, metal, glass, flesh, water and so on) whose hits play this row; the move and zip tags dress the round's flight.", "[orig: AmmoDef_ProcessImpactEffect @ 0x40A170]"},
	{"hit_effect", "Hit effect", "", "The particle effect spawned where the round hits this surface; none spawns nothing.", "[orig: AmmoDef_ProcessImpactEffect @ 0x40A170]"},
	{"impact_sound", "Hit sound", "", "The sound set played where the round hits this surface; none plays nothing.", "[orig: AmmoDef_ProcessImpactEffect @ 0x40A170]"},
	{"value", "Count", "", "Ignored: the game reads this fourth number and then zeroes it, so it changes nothing.", "[orig: AmmoDef_ParseProperty @ 0x40A587]"},
};

const DefWords kPowerupWords[] = {
	{"name", "Powerup name", "Powerup", "Set by the powerup header line: an item's powerupdef names it; only 16 characters are kept and the first matching row wins.", "[orig: PowerUpDef_FindByName @ 0x442660]"},
	{"respawn_time", "Respawn time", "Powerup", "Seconds a taken powerup stays hidden before it reappears; 0 removes it for good on pickup.", "[orig: PowerupAction_Pickup @ 0x4428A0]"},
	{"max_respawns", "Pickup limit", "Powerup", "How many times it can be taken in all: 1 makes it one-shot; 0 or absent lets it respawn forever.", "[orig: PowerupEntity_InitFromDef @ 0x442D00]"},
	{"hp", "Health", "Gives", "Health given: -1 fills the taker to full, a positive number adds that much even past full; a taker already full can't take it.", "[orig: PowerupAction_Pickup @ 0x4428A0]"},
	{"mana", "Class 1 ammo", "Gives", "Adds this many rounds to the taker's ammo class 1, up to its carry limit; -1 fills it.", "[orig: PowerupAction_Pickup @ 0x4428A0]"},
	{"weapon", "Weapon", "Gives", "The weapon handed over, loaded to its starting rounds even if already held; a taker already carrying it full can't take it.", "[orig: PowerupAction_Pickup @ 0x4428A0]"},
	{"weapon_all", "Every weapon", "Gives", "Set by writing weapon all: the pickup skips that value, so no weapon is handed over.", "[orig: PowerupAction_Pickup @ 0x4428A0]"},
	{"allammo", "Refill all ammo", "Gives", "Refills every weapon the taker carries to its starting rounds; a taker whose kit is already full, or empty, can't take it.", "[orig: PowerupAction_Pickup @ 0x4428A0]"},
	{"ammo_count", "Ammo lines", "Gives", "Derived, not a key of its own: how many ammo lines (an ammo class and a count) the row has.", "powerup-re.md, The powerup.def row (576 B)"},
};

const DefWords kPowerupAmmoWords[] = {
	{"class_name", "Ammo class", "", "The weapon.def ammo class this line fills; only class ids 11 and up are ever granted, and an unknown class is dropped with an error.", "[orig: PowerupAction_Pickup @ 0x4428A0]"},
	{"count", "Rounds", "", "Rounds added to that class, up to its carry limit; -1 fills it and 0 gives none.", "[orig: PowerupAction_Pickup @ 0x4428A0]"},
};

const DefWords kPowerupActionWords[] = {
	{"function", "Handler", "Action", "The handler the block runs (powerup_pickup or powerup_respawn, the defaults when the block is absent); none, null or an unknown name does nothing.", "[orig: PowerUpDef_RegisterNewEntry @ 0x442C00]"},
	{"anim", "Animation", "Action", "Unknown: the shared action parser stores it; neither powerup handler is witnessed reading it.", "[orig: ActionDef_ParseScriptLine @ 0x402873]"},
	{"soundset", "Sound", "Sound", "The sound set played when the block runs: at the taker on pickup, at the powerup when it respawns.", "[orig: ActionSlot_PlaySound @ 0x4010C0]"},
	{"soundsetend", "End sound", "Sound", "Unknown: the shared action parser stores it; neither powerup handler is witnessed reading it.", "[orig: ActionDef_ParseScriptLine @ 0x4023C0]"},
	{"particle", "Particle effect", "Effects", "The particle effect spawned when the block runs: at the taker on pickup, at the powerup when it respawns.", "[orig: ActionSlot_SpawnParticleAtEntity @ 0x442380]"},
	{"particleuserpoint", "Particle userpoint", "Effects", "Unknown: the parser stores it; the witnessed handlers spawn the particle at the entity's position, never at a userpoint.", "[orig: ActionSlot_SpawnParticleAtEntity @ 0x442380]"},
	{"texttoken", "HUD text", "Action", "On pickup, the string-table token whose text the local player's HUD shows.", "[orig: ActionSlot_OnTextTokenCallback @ 0x4011B0]"},
	{"delaystart", "Start delay", "Timing", "Unknown: the shared action parser stores it (auto reads as -1); neither powerup handler is witnessed reading it.", "[orig: ActionDef_ParseScriptLine @ 0x40279A]"},
	{"delayend", "End delay", "Timing", "Unknown: the shared action parser stores it (delay is another spelling); neither powerup handler is witnessed reading it.", "[orig: ActionDef_ParseScriptLine @ 0x402B2C]"},
	{"action_value", "Action value", "Action", "Unknown: the shared action parser stores it; neither powerup handler is witnessed reading it.", "[orig: ActionDef_ParseScriptLine @ 0x4023C0]"},
};

// Each kind's sections in the order the Inspector shows them: who the record is first, then how it
// looks, what it is, how it behaves and what it does.
const char *const kItemSections[] = {"Identity", "Looks", "Attributes", "Health and armour", "Behaviour", "Physics",
                                     "Weapons", "Sound", "Effects", "Death and wreck", "Doors", "Powerup",
                                     "Vehicle spawn", nullptr};
const char *const kWeaponSections[] = {"Identity", "Loadout", "Ammo", "Handling", "Models", "HUD", "Scope", "Heat",
                                       "Sound", "Animation", "Filters", nullptr};
const char *const kActionSections[] = {"Action", "Function", "Timing", "Sound", "Effects", nullptr};
const char *const kAmmoSections[] = {"Identity", "Flight", "Damage", "Blast", "Guidance", "Tracer", "Light", "Sound",
                                     "Effects", nullptr};
const char *const kPowerupSections[] = {"Powerup", "Gives", nullptr};
const char *const kPowerupActionSections[] = {"Action", "Timing", "Sound", "Effects", nullptr};
const char *const kNoSections[] = {"", nullptr};

struct KindWords {
	DefRecordKind kind;
	const DefWords *rows;
	size_t count;
	const char *const *sections;
};
#define ROWS(table) table, std::size(table)
const KindWords kKinds[] = {
	{DefRecordKind::Item, ROWS(kItemWords), kItemSections},
	{DefRecordKind::Weapon, ROWS(kWeaponWords), kWeaponSections},
	{DefRecordKind::Ammo, ROWS(kAmmoWords), kAmmoSections},
	{DefRecordKind::Action, ROWS(kActionWords), kActionSections},
	{DefRecordKind::Sight, ROWS(kSightWords), kNoSections},
	{DefRecordKind::Attachment, ROWS(kAttachmentWords), kNoSections},
	{DefRecordKind::Effect, ROWS(kEffectWords), kNoSections},
	{DefRecordKind::Carry, ROWS(kCarryWords), kNoSections},
	{DefRecordKind::Powerup, ROWS(kPowerupWords), kPowerupSections},
	{DefRecordKind::PowerupAmmo, ROWS(kPowerupAmmoWords), kNoSections},
	{DefRecordKind::PowerupAction, ROWS(kPowerupActionWords), kPowerupActionSections},
};
#undef ROWS
static_assert(std::size(kKinds) == def::kDefRecordKindCount, "every record kind has its words");

const KindWords *kind_words(DefRecordKind kind) {
	for (const KindWords &row : kKinds)
		if (row.kind == kind) return &row;
	return nullptr;
}

// The shadow bits (render-lighting-re.md "Static sector/model sun shadows", terrain-re.md): the
// terrain's static pass admits a building placement unless either NoShadow, an item placement only
// with StaticShadow too; the dynamic render slot is a person's or a DynamicShadow item's, which
// NoShadow never reads. Both draw only while Shadow Quality is above 0 (PolyTrn_RenderTile's gate
// @ 0x60DC1A, Entity_InitFromModel's @ 0x40E1BC).
struct ChoiceWordsRow {
	DefRecordKind kind;
	DefChoiceWords words;
};
const ChoiceWordsRow kChoiceWords[] = {
	{DefRecordKind::Item, {"attrib", "noshadow", "No shadow",
	        "The game draws no sun shadow of this item on the terrain, wherever a mission places it: the "
	        "terrain's static shadow pass skips it, as it skips a placement whose own NoShadow is set. A "
	        "person's or a DynamicShadow item's moving shadow still draws, and the item's own lighting is "
	        "unchanged. With Shadow Quality at 0 the game draws no shadow at all.",
	        "[orig: Terrain_CollectAndRenderTileModels @ 0x60D43E]; render-lighting-re.md"}},
	{DefRecordKind::Item, {"attrib2", "staticshadow", "Static shadow",
	        "Placed among a mission's items, it casts the terrain's static sun shadow, which a placement "
	        "among the buildings casts without it; either NoShadow still turns it off.",
	        "[orig: Terrain_CollectAndRenderTileModels @ 0x60D44C]; render-lighting-re.md"}},
	{DefRecordKind::Item, {"attrib2", "dynamicshadow", "Dynamic shadow",
	        "It casts the moving shadow every person casts, its silhouette drawn on the terrain under it "
	        "each frame; NoShadow does not turn this one off.",
	        "[orig: Entity_InitFromModel @ 0x40E1DD]; render-lighting-re.md"}},
};

} // namespace

const DefChoiceWords *def_choice_words_of(DefRecordKind kind, const std::string &id, const std::string &token) {
	for (const ChoiceWordsRow &row : kChoiceWords)
		if (row.kind == kind && id == row.words.id && token == row.words.token) return &row.words;
	return nullptr;
}

const DefWords *def_words(DefRecordKind kind, size_t &count) {
	const KindWords *row = kind_words(kind);
	count = row ? row->count : 0;
	return row ? row->rows : nullptr;
}

const DefWords *def_words_of(DefRecordKind kind, const std::string &id) {
	const KindWords *row = kind_words(kind);
	if (!row) return nullptr;
	for (size_t i = 0; i < row->count; ++i)
		if (id == row->rows[i].id) return &row->rows[i];
	return nullptr;
}

const char *const *def_sections(DefRecordKind kind) {
	const KindWords *row = kind_words(kind);
	return row ? row->sections : kNoSections;
}

size_t def_section_rank(DefRecordKind kind, const char *section) {
	const char *const *sections = def_sections(kind);
	for (size_t i = 0; sections[i]; ++i)
		if (std::strcmp(sections[i], section) == 0) return i;
	return SIZE_MAX;
}

} // namespace opennova::editor
