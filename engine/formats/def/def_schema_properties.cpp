#include "def_schema.h"

namespace opennova::def {
namespace {

// What the parsers make of a line's numbers, for an editor's words: each note is the parse
// the property's encoding inverts, each unit the one that parse reads.
// The vehicle physics keys, one parser's set [orig: ItemDef_ParsePhysicsProperty @ 0x49D870:
// turn_rate / turn_rate2 atol x 192426 @ 0x49d8af / 0x49d8f1, max_slope / slip_slope x 11930464
// @ 0x49d933 / 0x49d975, player_speed / water_speed / climb_speed x 293 @ 0x49d9b7 / 0x49d9f9 /
// 0x49db4a, acceleration / deceleration / slip_speed x 4 @ 0x49da34 / 0x49da8c / 0x49db08, an
// unset deceleration twice the acceleration @ 0x49da42..0x49da4d].
const char *const kPhysics = "Physics";
const char *const kSpeed = "A whole number of km/h, stored times 293 (16.16 units a tick).";
const char *const kTurnRate = "Whole degrees a second, stored as a binary angle a tick (times 192426).";
const char *const kWholeDegrees = "Whole degrees, stored as a binary angle (times 11930464).";
const char *const kTimesFour = "A whole number, stored times 4.";
const char *const kDeceleration = "A whole number, stored times 4; left out, twice the acceleration.";
// [orig: light_transfer atoi clamped 0..100 then x 0.01f @ 0x4A1A12..0x4A1A50]
const char *const kPercent = "A whole percent, clamped to 0..100, stored as a fraction.";
// [orig: Math_ParseFixedPoint16 @ 0x6131f0; scale's atof x 65536 @ 0x49F6E0..0x49F73D]
const char *const kFixed16 = "A decimal, stored as 16.16.";
// [orig: destroy_timing seconds x 62 @ 0x49EE7E / 0x49EEA5 / 0x49EECF; max_age / arm_age
// AmmoDef_ParseSecondsToTicks @ 0x40a0f0]
const char *const kSeconds = "Seconds, stored in 62 Hz ticks.";
// [orig: ItemDef_ParseProperty @ 0x49EB00, the shots' seconds x 62 @ 0x49FC79..0x49FE5C]
const char *const kShotTiming = "A flag name, then two times in seconds, stored in 62 Hz ticks.";
// [orig: heat_values @ 0x543eb7: Math_ParseFixedPoint16 / 100 -> +0x36C, / 6200 -> +0x370]
const char *const kHeat = "A percent a shot, then a percent a second of cooling; stored as 16.16 fractions of one, "
                          "the second per 62 Hz tick.";
// [orig: WeaponDefs_ParseLineCallback @ 0x543680, pos @ 0x544614..0x5446D8: three atof values,
// three Math_ParseFixedPoint16 degrees; a line of fewer than six stores nothing]
const char *const kPose = "Three view-position values, then three rotations in degrees stored as 16.16; a line of "
                          "fewer than six is not read.";
// [orig: kz_pieslice atol signed-div-2 x 0xB60B60 @ 0x40ad51]
const char *const kHalfAngle = "The cone's whole angle in degrees; the game keeps half of it, as a binary angle.";
// [orig: AmmoDef_ParseProperty 'light_move' -> +120 16.16 radius, +124 (r << 16) | (g << 8) | b;
// 'light_impact' @ 0x40af79 -> +132 / +128 / +136, a 0 fade 10 ticks @ 0x40b005]
const char *const kLight = "A radius (a decimal, stored as 16.16), then the colour's red, green and blue.";
const char *const kLightImpact = "A radius (a decimal, stored as 16.16), the colour's red, green and blue, then the "
                                 "fade in seconds, stored in 62 Hz ticks (0: 10 ticks).";
// [orig: AmmoDef_ParseTurnRate @ 0x40a130: (192426 * 16.16 + 0x8000) >> 16]
const char *const kAmmoTurnRate = "Degrees a second (a decimal), stored as a binary angle a tick.";
// [orig: AmmoDef_ParseProperty boresight_maxang atol x 0xB60B60 @ 0x40ae3b..0x40ae52] reads
// kWholeDegrees.
// [orig: WeaponDefs_ParseLineCallback 'farpinfo' @ 0x544da3 -> +0xE8 / +0xEC; the rearm pass
// Server_UpdateEntityTargetLockAndWeaponOverlays @ 0x51190b..0x511934, a vehicle's (def type 1
// @ 0x51187E) weapon when its item def carries EWeap (+0x54 bit 0x20 @ 0x5118EF)]
const char *const kFarpInfo = "On a FARP, the rounds a vehicle's weapon gains in clip and reserve, then how many "
                              "steps of the rearm count apart; 0 in either adds none.";
// [orig: WeaponDefs_ParseLineCallback 'gfx1' @ 0x5448fc / 'gfx3' @ 0x544912, `nocheckdepth`
// compared @ 0x544F92, the model loaded with the load-pass flags 0x300000 @ 0x544FA4]
const char *const kModelOption = "A model, then `nocheckdepth` to load it with its depth check off.";
// [orig: WeaponDefs_ParseLineCallback @ 0x543FB5..0x543FD3 (red, blue: the host's loadout
// check, NapiNPServerMsg_HandlePlayerLoadout @ 0x515790); WeaponDef_ParseProperty
// @ 0x54daae..0x54db08 (also yellow, violet: the armory's lists, PlayerInfo_PopulateWeaponSlotLists
// @ 0x560430)]
const char *const kTeamFilter = "A team the weapon is offered to: red or blue; yellow (as blue) and violet (as red) "
                                "only put it in the armory's lists, the host's check refusing it.";

// Property syntax of the witnessed family parsers. The writer and inspector
// share these rows; parser-equivalence tests pin their native member mappings.
const std::vector<DefProperty> kItemProperties = {
    {"powerupdef", {"powerup_def"}},
    {"score", {"score"}},
    {"graphicenemy", {"graphic_enemy"}},
    {"textid", {"text_id"}},
	{"kz", {"kz"}},
	{"debris_scale", {"debris_scale"}},
	{"sid", {"sid"}, DefEncoding::Plain, 1.0, ""},
	{"graphic", {"graphic"}, DefEncoding::Plain, 1.0, ""},
	{"anim_def", {"anim_def"}, DefEncoding::Plain, 1.0, ""},
	{"husk", {"husk"}, DefEncoding::Plain, 1.0, ""},
	{"default_aip", {"default_aip"}, DefEncoding::Plain, 1.0, ""},
	{"ai_function", {"ai_function"}, DefEncoding::Plain, 1.0, ""},
	{"move_function", {"move_function"}, DefEncoding::Plain, 1.0, ""},
	{"render_function", {"render_function"}, DefEncoding::Plain, 1.0, ""},
	{"disk_function", {"disk_function"}, DefEncoding::Plain, 1.0, ""},
	{"input_function", {"input_function"}, DefEncoding::Plain, 1.0, ""},
	{"ammo_closeattack", {"ammo_closeattack"}, DefEncoding::Plain, 1.0, ""},
	{"ammo_marker3", {"ammo_marker3"}, DefEncoding::Plain, 1.0, ""},
	{"ammo_easyrocket", {"ammo_easyrocket"}, DefEncoding::Plain, 1.0, ""},
	{"ammo_advancedrocket", {"ammo_advancedrocket"}, DefEncoding::Plain, 1.0, ""},
	{"launchups_closeattack", {"launchups_closeattack"}, DefEncoding::Plain, 1.0, ""},
	{"launchups_rocket", {"launchups_rocket"}, DefEncoding::Plain, 1.0, ""},
	{"launchups_marker3", {"launchups_marker3"}, DefEncoding::Plain, 1.0, ""},
	{"weaplbup2", {"weapon_userpoints[6]"}, DefEncoding::Plain, 1.0, ""},
	{"weaplmup2", {"weapon_userpoints[7]"}, DefEncoding::Plain, 1.0, ""},
	{"weaplcup2", {"weapon_userpoints[8]"}, DefEncoding::Plain, 1.0, ""},
	{"weaprbup2", {"weapon_userpoints[9]"}, DefEncoding::Plain, 1.0, ""},
	{"weaprmup2", {"weapon_userpoints[10]"}, DefEncoding::Plain, 1.0, ""},
	{"weaprcup2", {"weapon_userpoints[11]"}, DefEncoding::Plain, 1.0, ""},
	{"weaplbup", {"weapon_userpoints[0]"}, DefEncoding::Plain, 1.0, ""},
	{"weaplmup", {"weapon_userpoints[1]"}, DefEncoding::Plain, 1.0, ""},
	{"weaplcup", {"weapon_userpoints[2]"}, DefEncoding::Plain, 1.0, ""},
	{"weaprbup", {"weapon_userpoints[3]"}, DefEncoding::Plain, 1.0, ""},
	{"weaprmup", {"weapon_userpoints[4]"}, DefEncoding::Plain, 1.0, ""},
	{"weaprcup", {"weapon_userpoints[5]"}, DefEncoding::Plain, 1.0, ""},
	{"primary_weapon", {"primary_weapon"}, DefEncoding::Plain, 1.0, ""},
	{"clipsize", {"clipsize"}, DefEncoding::Plain, 1.0, ""},
	{"climb_speed", {"climb_speed"}, DefEncoding::ScaledInteger, 293.0, "", {}, kPhysics, {"km/h"}, kSpeed},
	{"turnroll", {"turn_roll"}, DefEncoding::Plain, 1.0, "", {}, kPhysics, {}, ""},
	{"speedpitch", {"speed_pitch"}, DefEncoding::Plain, 1.0, "", {}, kPhysics, {}, ""},
	{"turn_rate2", {"turn_rate2"}, DefEncoding::ScaledInteger, 192426.0, "", {}, kPhysics, {"deg/s"}, kTurnRate},
	{"turn_rate", {"turn_rate"}, DefEncoding::ScaledInteger, 192426.0, "", {}, kPhysics, {"deg/s"}, kTurnRate},
	{"torque", {"torque"}, DefEncoding::Plain, 1.0, "", {}, kPhysics, {}, ""},
	{"lean_velocity", {"lean_velocity"}, DefEncoding::Plain, 1.0, "", {}, kPhysics, {}, ""},
	{"pitch_velocity", {"pitch_velocity"}, DefEncoding::Plain, 1.0, "", {}, kPhysics, {}, ""},
	{"mass", {"mass"}, DefEncoding::Plain, 1.0, "", {}, kPhysics, {}, ""},
	{"weathervane", {"weathervane"}, DefEncoding::Plain, 1.0, "", {}, kPhysics, {}, ""},
	{"minai", {"min_ai"}, DefEncoding::Plain, 1.0, "", {}, kPhysics, {}, ""},
	{"lean", {"lean"}, DefEncoding::Plain, 1.0, "", {}, kPhysics, {}, ""},
	{"pitch", {"pitch"}, DefEncoding::Plain, 1.0, "", {}, kPhysics, {}, ""},
	{"bob", {"bob"}, DefEncoding::Plain, 1.0, "", {}, kPhysics, {}, ""},
	{"flip", {"flip"}, DefEncoding::Plain, 1.0, "", {}, kPhysics, {}, ""},
	{"hand_brake", {"hand_brake"}, DefEncoding::Plain, 1.0, "", {}, kPhysics, {}, ""},
	{"tire_slip", {"tire_slip"}, DefEncoding::Plain, 1.0, "", {}, kPhysics, {}, ""},
	{"spring_comp", {"spring_comp"}, DefEncoding::Plain, 1.0, "", {}, kPhysics, {}, ""},
	{"spring", {"spring"}, DefEncoding::Plain, 1.0, "", {}, kPhysics, {}, ""},
	{"shock", {"shock"}, DefEncoding::Plain, 1.0, "", {}, kPhysics, {}, ""},
	{"top_heavy", {"top_heavy"}, DefEncoding::Plain, 1.0, "", {}, kPhysics, {}, ""},
	{"max_slope", {"max_slope"}, DefEncoding::ScaledInteger, 11930464.0, "", {}, kPhysics, {"deg"}, kWholeDegrees},
	{"slip_slope", {"slip_slope"}, DefEncoding::ScaledInteger, 11930464.0, "", {}, kPhysics, {"deg"}, kWholeDegrees},
	{"player_speed", {"player_speed"}, DefEncoding::ScaledInteger, 293.0, "", {}, kPhysics, {"km/h"}, kSpeed},
	{"water_speed", {"water_speed"}, DefEncoding::ScaledInteger, 293.0, "", {}, kPhysics, {"km/h"}, kSpeed},
	{"slip_speed", {"slip_speed"}, DefEncoding::ScaledInteger, 4.0, "", {}, kPhysics, {}, kTimesFour},
	{"physics", {"physics"}, DefEncoding::Plain, 1.0, "", {}, kPhysics, {}, ""},
	{"criticalhp", {"critical_hp"}, DefEncoding::Plain, 1.0, ""},
	{"criticaldrain", {"critical_drain"}, DefEncoding::Plain, 1.0, ""},
	{"noncriticalregen", {"non_critical_regen"}, DefEncoding::Plain, 1.0, ""},
	{"hud_image", {"hud_image"}, DefEncoding::Plain, 1.0, ""},
	{"unit_type", {"unit_type"}, DefEncoding::Plain, 1.0, ""},
	{"huskfinal", {"huskfinal"}, DefEncoding::Plain, 1.0, ""},
	{"husk_sub_parts", {"husk_sub_parts"}, DefEncoding::Plain, 1.0, ""},
	{"id", {"id"}, DefEncoding::Plain, 1.0, ""},
	{"mana", {"mana"}, DefEncoding::Plain, 1.0, ""},
	{"music", {"music_location"}, DefEncoding::Plain, 1.0, ""},
	{"hp", {"hp"}, DefEncoding::Plain, 1.0, ""},
	{"radarsig", {"radar_sig"}, DefEncoding::Plain, 1.0, ""},
	{"heatsig", {"heat_sig"}, DefEncoding::Plain, 1.0, ""},
	{"reverb", {"reverb"}, DefEncoding::Plain, 1.0, ""},
	{"sounddeath", {"sounddeath"}, DefEncoding::Plain, 1.0, ""},
	{"sound_profile", {"sound_profile"}, DefEncoding::Plain, 1.0, ""},
	{"sound_profilefemale", {"sound_profile_female"}, DefEncoding::Plain, 1.0, ""},
	{"acceleration", {"acceleration"}, DefEncoding::ScaledInteger, 4.0, "", {}, kPhysics, {}, kTimesFour},
	{"deceleration", {"deceleration"}, DefEncoding::ScaledInteger, 4.0, "", {}, kPhysics, {}, kDeceleration},
	{"type", {"type"}, DefEncoding::ItemType, 1.0, ""},
	{"attrib2", {"attrib2"}, DefEncoding::ItemAttrib2, 1.0, ""},
	{"parent", {"attrib_parent"}, DefEncoding::ItemParent, 1.0, ""},
	{"armor", {"armor_kz", "armor_impact"}, DefEncoding::Plain, 1.0, ""},
	{"damage_reduc_pp", {"damage_reduc_pp", "damage_reduc_max"}, DefEncoding::Plain, 1.0, ""},
	{"virtualdisplay", {"virtual_display", "virtual_display_userpoint"}, DefEncoding::Plain, 1.0, ""},
	{"shadow", {"shadow_texture", "shadow_width", "shadow_length", "shadow_offset_x", "shadow_offset_y"}, DefEncoding::Plain, 1.0, ""},
	{"light_transfer", {"light_transfer"}, DefEncoding::Percent, 1.0, "", {}, "", {"%"}, kPercent},
	{"scale", {"scale_q16"}, DefEncoding::ScaledReal, 65536.0, "", {}, "", {}, kFixed16},
	{"phrase_set", {"phrase_set"}, DefEncoding::Plain, 1.0, "phrase_set_valid"},
	{"destroy_timing", {"destroy_timing_ticks[0]", "destroy_timing_ticks[1]", "destroy_timing_ticks[2]"}, DefEncoding::ScaledReal, 62.0, "", {}, "", {"s"}, kSeconds},
	{"dawnshot", {"dawnshot", "shot_delay_ticks[0][0]", "shot_delay_ticks[0][1]"}, DefEncoding::ShotTiming, 62.0, "", {}, "", {"", "s", "s"}, kShotTiming},
	{"dayshot", {"dayshot", "shot_delay_ticks[1][0]", "shot_delay_ticks[1][1]"}, DefEncoding::ShotTiming, 62.0, "", {}, "", {"", "s", "s"}, kShotTiming},
	{"duskshot", {"duskshot", "shot_delay_ticks[2][0]", "shot_delay_ticks[2][1]"}, DefEncoding::ShotTiming, 62.0, "", {}, "", {"", "s", "s"}, kShotTiming},
	{"nightshot", {"nightshot", "shot_delay_ticks[3][0]", "shot_delay_ticks[3][1]"}, DefEncoding::ShotTiming, 62.0, "", {}, "", {"", "s", "s"}, kShotTiming},
	{"soundloop_1", {"soundloops[0]"}, DefEncoding::Plain, 1.0, ""},
	{"soundloop_2", {"soundloops[1]"}, DefEncoding::Plain, 1.0, ""},
	{"soundloop_3", {"soundloops[2]"}, DefEncoding::Plain, 1.0, ""},
	{"soundloop_4", {"soundloops[3]"}, DefEncoding::Plain, 1.0, ""},
	{"soundloop_5", {"soundloops[4]"}, DefEncoding::Plain, 1.0, ""},
	{"soundloop_6", {"soundloops[5]"}, DefEncoding::Plain, 1.0, ""},
	{"soundloop_7", {"soundloops[6]"}, DefEncoding::Plain, 1.0, ""},
	{"particlefx", {"particlefx.effect", "particlefx.userpoint"}, DefEncoding::ParticleSlot, 1.0, ""},
	{"particlefxs", {"particlefxs.effect", "particlefxs.userpoint", "particlefxs.secondary_effect"}, DefEncoding::ParticleSlot, 1.0, ""},
	{"particlefxw1", {"particlefxw1.effect", "particlefxw1.userpoint", "particlefxw1.secondary_effect"}, DefEncoding::ParticleSlot, 1.0, ""},
	{"particlefxw2", {"particlefxw2.effect", "particlefxw2.userpoint", "particlefxw2.secondary_effect"}, DefEncoding::ParticleSlot, 1.0, ""},
	{"particlefxw3", {"particlefxw3.effect", "particlefxw3.userpoint"}, DefEncoding::ParticleSlot, 1.0, ""},
	{"particlefxw4", {"particlefxw4.effect", "particlefxw4.userpoint"}, DefEncoding::ParticleSlot, 1.0, ""},
	{"particledeath", {"particledeath"}, DefEncoding::Plain, 1.0, ""},
	{"particleh2odeath", {"particleh2odeath"}, DefEncoding::Plain, 1.0, ""},
	{"particlefire", {"particlefire"}, DefEncoding::Plain, 1.0, ""},
	{"particleother", {"particleother"}, DefEncoding::Plain, 1.0, ""},
	{"particlefinale", {"particlefinale"}, DefEncoding::Plain, 1.0, ""},
	{"particlespawn", {"particlespawn"}, DefEncoding::Plain, 1.0, ""},
	{"attrib:", {"attrib"}, DefEncoding::ItemAttrib, 1.0, ""},
	{"deathtime", {"deathtime_ticks"}, DefEncoding::ItemDeathTime, 1.0, ""},
	{"door_type", {"door_type"}, DefEncoding::DoorType, 1.0, ""},
	{"open_rate", {"door_open_rate_q16"}, DefEncoding::DoorOpenRate, 1.0, ""},
	{"max_angle", {"door_max_angle_bam"}, DefEncoding::DoorMaxAngle, 1.0, ""},
	{"husk_swap_at_sec", {"husk_swap_at_sec"}, DefEncoding::HuskSeconds, 1.0, ""},
	{"husk_swap_at", {"husk_swap_at"}, DefEncoding::HuskSwap, 1.0, ""},
	{"door_open_sound_id", {"door_open_sound"}, DefEncoding::Plain, 1.0, ""},
	{"door_close_sound_id", {"door_close_sound"}, DefEncoding::Plain, 1.0, ""},
	{"husk_sub_part_types", {"husk_sub_part_types[0]", "husk_sub_part_types[1]", "husk_sub_part_types[2]", "husk_sub_part_types[3]", "husk_sub_part_types[4]", "husk_sub_part_types[5]", "husk_sub_part_types[6]", "husk_sub_part_types[7]", "husk_sub_part_types[8]", "husk_sub_part_types[9]", "husk_sub_part_types[10]", "husk_sub_part_types[11]", "husk_sub_part_types[12]", "husk_sub_part_types[13]", "husk_sub_part_types[14]", "husk_sub_part_types[15]"}, DefEncoding::DeathPieces, 1.0, ""},
	{"pcvehicle_spawnlist", {"vehicle_spawn_mask"}, DefEncoding::SpawnMask, 1.0, ""},
};

const std::vector<DefProperty> kWeaponProperties = {
	{"attachtextid", {"attach_text_id"}},
	{"rank", {"rank"}, DefEncoding::Plain, 1.0, ""},
	{"clipsize", {"clipsize"}, DefEncoding::Plain, 1.0, ""},
	{"startrounds", {"startrounds"}, DefEncoding::Plain, 1.0, ""},
	{"targetyawrange", {"targetyawrange"}, DefEncoding::Plain, 1.0, ""},
	{"targetpitchmax", {"targetpitchmax"}, DefEncoding::Plain, 1.0, ""},
	{"targetpitchmin", {"targetpitchmin"}, DefEncoding::Plain, 1.0, ""},
	{"statid", {"statid"}, DefEncoding::Plain, 1.0, ""},
	{"maxclips", {"maxclips"}, DefEncoding::Plain, 1.0, ""},
	{"ammobucket", {"ammobucket"}, DefEncoding::Plain, 1.0, ""},
	{"loadout_selectable", {"loadout_selectable"}, DefEncoding::Plain, 1.0, ""},
	{"loadout_subclasses", {"loadout_subclasses"}, DefEncoding::Plain, 1.0, ""},
	{"round_type", {"round_type"}, DefEncoding::Plain, 1.0, ""},
	{"loadout_menu_textid", {"loadout_menu_textid"}, DefEncoding::Plain, 1.0, ""},
	{"loadout_menu_ttdesc", {"loadout_menu_ttdesc"}, DefEncoding::Plain, 1.0, ""},
	{"loadout_menu_icon", {"loadout_menu_icon"}, DefEncoding::Plain, 1.0, ""},
	{"animadm", {"animadm"}, DefEncoding::Plain, 1.0, ""},
	{"launchuserpoint", {"launch_user_point"}, DefEncoding::Plain, 1.0, ""},
	{"commandersx", {"commanders_x"}, DefEncoding::Plain, 1.0, ""},
	{"hud_loadout_select", {"hud_loadout_select"}, DefEncoding::Plain, 1.0, ""},
	{"splash", {"splash"}, DefEncoding::Plain, 1.0, ""},
	{"hudicon", {"hudicon"}, DefEncoding::Plain, 1.0, ""},
	{"gfx1a", {"gfx1a"}, DefEncoding::Plain, 1.0, ""},
	{"gfx1b", {"gfx1b"}, DefEncoding::Plain, 1.0, ""},
	{"gfx1", {"gfx1", "gfx1_nocheckdepth"}, DefEncoding::ModelOption, 1.0, "", {"", "No depth check"}, "", {}, kModelOption},
	{"gfx3", {"gfx3", "gfx3_nocheckdepth"}, DefEncoding::ModelOption, 1.0, "", {"", "No depth check"}, "", {}, kModelOption},
	{"error_hiptheta", {"error_hip_theta_fp16"}, DefEncoding::Fixed16, 1.0, "", {}, "", {}, kFixed16},
	{"error_uptheta", {"error_up_theta_fp16"}, DefEncoding::Fixed16, 1.0, "", {}, "", {}, kFixed16},
	{"renderfov", {"renderfov"}, DefEncoding::Plain, 1.0, ""},
	{"scope_min_mag", {"scope_min_mag"}, DefEncoding::Plain, 1.0, ""},
	{"soundfireloop", {"soundfireloop"}, DefEncoding::Plain, 1.0, ""},
	{"soundtrailoff", {"soundtrailoff"}, DefEncoding::Plain, 1.0, ""},
	{"vmacrotoken", {"vmacrotoken"}, DefEncoding::Plain, 1.0, ""},
	{"soundhead", {"soundhead"}, DefEncoding::Plain, 1.0, ""},
	{"soundlockedtone", {"soundlockedtone"}, DefEncoding::Plain, 1.0, ""},
	{"emplacedstance", {"emplacedstance"}, DefEncoding::Plain, 1.0, ""},
	{"special_hold", {"special_hold"}, DefEncoding::Plain, 1.0, ""},
	{"attack_anim", {"attack_anim"}, DefEncoding::Plain, 1.0, ""},
	{"run_anim", {"run_anim"}, DefEncoding::Plain, 1.0, ""},
	{"category", {"category"}, DefEncoding::Plain, 1.0, ""},
	{"ammoclass", {"ammo_class", "ammo_class_count"}, DefEncoding::Plain, 1.0, ""},
	{"crosshair", {"crosshair", "crosshair_secondary"}, DefEncoding::Plain, 1.0, ""},
	{"hudclipgfx", {"hudclipgfx_offset[0]", "hudclipgfx_offset[1]", "hudclipgfx_texture"}, DefEncoding::Plain, 1.0, ""},
	{"hudrndgfx", {"hudrndgfx_offset[0]", "hudrndgfx_offset[1]", "hudrndgfx_layout[0]", "hudrndgfx_layout[1]", "hudrndgfx_layout[2]", "hudrndgfx_texture"}, DefEncoding::Plain, 1.0, ""},
	{"scope_max_mag", {"scope_max_mag", "scope_max_mag_arg2"}, DefEncoding::Plain, 1.0, ""},
	{"scope_max_zero", {"scope_max_zero_steps", "scope_zero_step", "scope_zero_default", "scope_zero_extra"}, DefEncoding::Plain, 1.0, ""},
	{"switchcategory", {"switchcategory"}, DefEncoding::Plain, 1.0, "has_switchcategory"},
	{"weapon_class", {"weapon_class"}, DefEncoding::WeaponClass, 1.0, ""},
	{"charfilter", {"charfilter[0]", "charfilter[1]", "charfilter[2]", "charfilter[3]", "charfilter[4]", "charfilter[5]", "charfilter[6]", "charfilter[7]"}, DefEncoding::CharacterFilter, 1.0, ""},
	{"teamfilter", {"teamfilter[0]", "teamfilter[1]", "teamfilter[2]", "teamfilter[3]"}, DefEncoding::TeamFilter, 1.0, "", {}, "", {}, kTeamFilter},
	{"classrounds", {"classrounds[0]", "classrounds[1]", "classrounds[2]", "classrounds[3]", "classrounds[4]", "classrounds[5]", "classrounds[6]"}, DefEncoding::ClassRounds, 1.0, ""},
	{"stability", {"stability_fp16[0]", "stability_fp16[1]", "stability_fp16[2]"}, DefEncoding::Fixed16, 1.0, "", {}, "", {}, kFixed16},
	{"flags", {"flags", "flags2"}, DefEncoding::WeaponFlags, 1.0, ""},
	{"error", {"error[0]", "error[1]", "error[2]", "error[3]", "error[4]", "error[5]", "error_fp16[0]", "error_fp16[1]", "error_fp16[2]", "error_fp16[3]", "error_fp16[4]", "error_fp16[5]"}, DefEncoding::FloatFixed, 1.0, ""},
	{"weaponweight", {"weaponweight", "weaponweight_fp16"}, DefEncoding::FloatFixed, 1.0, ""},
	{"clipweight", {"clipweight", "clipweight_fp16"}, DefEncoding::FloatFixed, 1.0, ""},
	{"pos", {"pos[0]", "pos[1]", "pos[2]", "pos_rotation_deg_q16[0]", "pos_rotation_deg_q16[1]", "pos_rotation_deg_q16[2]"}, DefEncoding::Pose, 1.0, "", {}, "", {"", "", "", "deg", "deg", "deg"}, kPose},
	{"tpos", {"tpos[0]", "tpos[1]", "tpos[2]", "tpos_rotation_deg_q16[0]", "tpos_rotation_deg_q16[1]", "tpos_rotation_deg_q16[2]"}, DefEncoding::Pose, 1.0, "", {}, "", {"", "", "", "deg", "deg", "deg"}, kPose},
	{"scope_paralax_distance", {"scope_paralax_distance_fp16"}, DefEncoding::ScopeParallax, 1.0, ""},
	{"heat_values", {"heat_per_shot", "heat_decay_per_tick"}, DefEncoding::Heat, 1.0, "", {"Heat per shot", "Cooling per second"}, "", {"%", "%/s"}, kHeat},
	{"heat_effect", {"heat_effect", "heat_glow_threshold"}, DefEncoding::Fixed16, 1.0, "", {"", "Glow threshold"}, "", {}, kFixed16},
	{"heat_sound", {"heat_sound"}, DefEncoding::Plain, 1.0, ""},
	{"farpinfo", {"farp_rounds", "farp_interval"}, DefEncoding::Plain, 1.0, "", {"FARP rounds", "FARP interval"}, "", {},
	 kFarpInfo},
	{"designation_time", {"designation_ticks"}, DefEncoding::ScaledInteger, 62.0, "", {}, "", {"s"}, kSeconds},
	{"sameas", {"sameas"}, DefEncoding::Plain, 1.0, ""},
};

const std::vector<DefProperty> kAmmoProperties = {
    {"dopplerdiv", {"doppler_divisor"}},
    {"kz_sound", {"kz_sound"}},
    {"secondary_effect", {"secondary_effect"}},
	{"min_damage", {"min_damage"}, DefEncoding::Plain, 1.0, ""},
	{"max_damage", {"max_damage"}, DefEncoding::Plain, 1.0, ""},
	{"penetration_impact", {"penetration_impact"}, DefEncoding::Plain, 1.0, ""},
	{"penetration_kz", {"penetration_kz"}, DefEncoding::Plain, 1.0, ""},
	{"secondary_anim", {"secondary_anim"}, DefEncoding::Plain, 1.0, ""},
	{"kz_physics", {"kz_physics"}, DefEncoding::Plain, 1.0, ""},
	{"error", {"error_fp16"}, DefEncoding::Fixed16, 1.0, "", {}, "", {}, kFixed16},
	{"drag", {"drag_fp16"}, DefEncoding::Fixed16, 1.0, "", {}, "", {}, kFixed16},
	{"bullet_radius", {"bullet_radius_fp16"}, DefEncoding::Fixed16, 1.0, "", {}, "", {}, kFixed16},
	{"spread_count", {"spread_count"}, DefEncoding::Plain, 1.0, ""},
	{"kz_damage", {"kz_damage"}, DefEncoding::Plain, 1.0, ""},
	{"kz_minradius", {"kz_minradius_fp16"}, DefEncoding::Fixed16, 1.0, "", {}, "", {}, kFixed16},
	{"kz_maxradius", {"kz_maxradius_fp16"}, DefEncoding::Fixed16, 1.0, "", {}, "", {}, kFixed16},
	{"scorch_id", {"scorch_id"}, DefEncoding::Plain, 1.0, ""},
	{"scar_type", {"scar_type"}, DefEncoding::Plain, 1.0, ""},
	{"min_stable_velocity", {"min_stable_velocity"}, DefEncoding::Plain, 1.0, ""},
	{"tumble_error", {"tumble_error_fp16"}, DefEncoding::Fixed16, 1.0, "", {}, "", {}, kFixed16},
	{"weight_in_grains", {"weight_in_grains"}, DefEncoding::Plain, 1.0, ""},
	{"tracerrate", {"tracer_rate"}, DefEncoding::Plain, 1.0, ""},
	{"frndlytrcrid", {"frndly_trcr_type_id"}, DefEncoding::Plain, 1.0, ""},
	{"foetrcrid", {"foe_trcr_type_id"}, DefEncoding::Plain, 1.0, ""},
	{"velocity", {"velocity"}, DefEncoding::Plain, 1.0, ""},
	{"heat_det_range", {"heat_det_range"}, DefEncoding::Plain, 1.0, ""},
	{"ai_launch", {"ai_launch"}, DefEncoding::Plain, 1.0, ""},
	{"ai_launcheffect", {"ai_launcheffect"}, DefEncoding::Plain, 1.0, ""},
	{"notarmmedammo", {"notarmmed_ammo"}, DefEncoding::Plain, 1.0, ""},
	{"max_age", {"max_age_ticks"}, DefEncoding::FixedSeconds, 1.0, "", {}, "", {"s"}, kSeconds},
	{"arm_age", {"arm_age_ticks"}, DefEncoding::FixedSeconds, 1.0, "", {}, "", {"s"}, kSeconds},
	{"boresight_maxang", {"boresight_maxang"}, DefEncoding::Degrees, 1.0, "", {}, "", {"deg"}, kWholeDegrees},
	{"kz_pieslice", {"kz_pieslice_bam"}, DefEncoding::HalfDegrees, 1.0, "", {}, "", {"deg"}, kHalfAngle},
	{"armor_density", {"armor_density[0]", "armor_density[1]", "armor_density[2]"}, DefEncoding::Plain, 1.0, ""},
	{"recoil", {"recoil[0]", "recoil[1]", "recoil[2]"}, DefEncoding::Plain, 1.0, ""},
	{"flag", {"flags"}, DefEncoding::AmmoFlags, 1.0, ""},
	{"kztype", {"kztype"}, DefEncoding::AmmoKillZone, 1.0, ""},
	{"mf_light", {"mf_light_value"}, DefEncoding::Plain, 1.0, "mf_light"},
	{"tracer_type", {"tracer_type_friendly", "tracer_type_enemy"}, DefEncoding::Plain, 1.0, ""},
	{"light_move", {"light_move_radius_fp16", "light_move_color"}, DefEncoding::LightMove, 1.0, "", {"Radius", "Colour"}, "", {}, kLight},
	{"light_impact", {"light_impact_radius_fp16", "light_impact_color", "light_impact_ticks"}, DefEncoding::LightImpact, 1.0, "", {"Radius", "Colour", "Fade"}, "", {"", "", "s"}, kLightImpact},
	{"turnrate_maxpit", {"turnrate_maxpit"}, DefEncoding::TurnRate, 1.0, "", {}, "", {"deg/s"}, kAmmoTurnRate},
	{"turnrate_maxyaw", {"turnrate_maxyaw"}, DefEncoding::TurnRate, 1.0, "", {}, "", {"deg/s"}, kAmmoTurnRate},
};

const std::vector<DefProperty> kActionProperties = {
	{"anim", {"anim"}, DefEncoding::Plain, 1.0, ""},
	{"soundset", {"soundset"}, DefEncoding::Plain, 1.0, ""},
	{"soundsetend", {"soundsetend"}, DefEncoding::Plain, 1.0, ""},
	{"particle", {"particle"}, DefEncoding::Plain, 1.0, ""},
	{"particleuserpoint", {"particleuserpoint"}, DefEncoding::Plain, 1.0, ""},
	{"action_value", {"action_value"}, DefEncoding::Plain, 1.0, ""},
	{"delaystart", {"delaystart"}, DefEncoding::Delay, 1.0, ""},
	{"delayend", {"delayend"}, DefEncoding::Delay, 1.0, ""},
	{"function", {"function", "function_args[0]", "function_args[1]", "function_args[2]", "function_args[3]"}, DefEncoding::Function, 1.0, ""},
	{"ctrlreg", {"ctrl_register"}, DefEncoding::Plain, 1.0, ""},
	{"ctrlreginc", {"ctrl_increment"}, DefEncoding::Plain, 1.0, ""},
	{"texttoken", {"text_token"}, DefEncoding::Plain, 1.0, ""},
	{"dupsound", {"duplicate_sound_count", "duplicate_sound_delay"}, DefEncoding::Plain, 1.0, ""},
};

const std::vector<DefProperty> kSightProperties = {
	{"sights", {"texture", "x1", "y1", "x2", "y2", "blend", "scale", "slide", "slide_frames"}, DefEncoding::Sight, 1.0, ""},
};

const std::vector<DefProperty> kAttachmentProperties = {
	{"addeweap", {"userpoint", "item_id", "down_angle", "up_angle", "right_angle", "left_angle", "angle_count", "kind"}, DefEncoding::Attachment, 1.0, ""},
};

const std::vector<DefProperty> kEffectProperties = {
	{"", {"surface_type", "hit_effect", "impact_sound", "value"}, DefEncoding::Plain, 1.0, ""},
};

const std::vector<DefProperty> kCarryProperties = {
	{"ammoclass_max_carry", {"name", "max_carry"}, DefEncoding::Plain, 1.0, ""},
};

// powerup.def's row keys [orig: PowerUpDef_ParseProperty @0x442EE0: respawn_time @0x443103, max_respawns
// @0x44312C, hp @0x443155, mana @0x44317E, weapon @0x4431A7..0x443216, allammo @0x443220].
const char *const kRespawnTime = "Seconds, taken as 62 Hz ticks when the powerup is picked up.";
const char *const kPowerupHp = "-1 raises the taker's health to its most; more than 0 adds that much.";
const char *const kPowerupMana = "-1 refills ammo class 1; any other number is added.";
const std::vector<DefProperty> kPowerupProperties = {
	{"respawn_time", {"respawn_time"}, DefEncoding::Plain, 1.0, "", {}, "", {"s"}, kRespawnTime},
	{"max_respawns", {"max_respawns"}},
	{"hp", {"hp"}, DefEncoding::Plain, 1.0, "", {}, "", {}, kPowerupHp},
	{"mana", {"mana"}, DefEncoding::Plain, 1.0, "", {}, "", {}, kPowerupMana},
	{"weapon", {"weapon", "weapon_all"}, DefEncoding::PowerupWeapon, 1.0, "", {"", "Every weapon"}},
	{"allammo", {"allammo"}, DefEncoding::Switch},
};

// `ammo <class> <count>` [orig: @0x443240..0x443277]: -1 fills the class, any other count is added.
const std::vector<DefProperty> kPowerupAmmoProperties = {
	{"ammo", {"class_name", "count"}, DefEncoding::Plain, 1.0, "", {"Ammo class", "Rounds"}},
};

// An action block's lines, the ActionDef keys the powerup handlers read [orig: ActionDef_ParseScriptLine
// @0x4023C0 -- function @0x40296E, anim @0x402873, delaystart/delay/delayend @0x40279A/@0x402B2C].
const std::vector<DefProperty> kPowerupActionProperties = {
	{"function", {"function"}},
	{"anim", {"anim"}},
	{"soundset", {"soundset"}},
	{"soundsetend", {"soundsetend"}},
	{"particle", {"particle"}},
	{"particleuserpoint", {"particleuserpoint"}},
	{"texttoken", {"texttoken"}},
	{"delaystart", {"delaystart"}, DefEncoding::Delay},
	{"delayend", {"delayend"}, DefEncoding::Delay},
	{"action_value", {"action_value"}},
};

} // namespace

const std::vector<DefProperty> &def_properties(DefRecordKind kind) {
	switch (kind) {
	case DefRecordKind::Item: return kItemProperties;
	case DefRecordKind::Weapon: return kWeaponProperties;
	case DefRecordKind::Ammo: return kAmmoProperties;
	case DefRecordKind::Action: return kActionProperties;
	case DefRecordKind::Sight: return kSightProperties;
	case DefRecordKind::Attachment: return kAttachmentProperties;
	case DefRecordKind::Effect: return kEffectProperties;
	case DefRecordKind::Carry: return kCarryProperties;
	case DefRecordKind::Powerup: return kPowerupProperties;
	case DefRecordKind::PowerupAmmo: return kPowerupAmmoProperties;
	case DefRecordKind::PowerupAction: return kPowerupActionProperties;
	}
	return kItemProperties;
}

} // namespace opennova::def
