#include "dfx2.h"

#include <base/gameprofile/game_type.h>
#include <runtime/world/title_rules.h>

#include <array>
#include <cstdint>

namespace opennova::games {

namespace {

namespace gt = opennova::game_type;

// dfx2.exe's rows: the same FIELD lists, other VAR values -- ENEMYKILL (3) 2 or 4, SUICIDE (4) -5,
// ALIVE (35) 2, VATTACHKILL (37) 6, and in every row with a capture or an objective
// ALIVEQUANTUM (36) 60 -- and its own Advance and Secure / Conquer and Control split, each row as
// the function stores it.
// [orig: GameType_CreateDefaultSettings @0x52ae80 (dfx2.exe)]
world::ScoreValues dfx2_score_values(uint32_t game_type) {
	world::ScoreValues values{};
	// What the capture and objective rows hold beside their own words.
	auto objective_combat = [&] {
		values[3] = 4;
		values[4] = -5;
		values[6] = 1;
		values[7] = 2;
		values[15] = 12;
		values[16] = 10;
		values[17] = 2;
		values[18] = 2;
		values[35] = 2;
		values[36] = 60;
		values[37] = 6;
	};

	if (game_type == gt::kDeathmatch) {
		values[3] = 2;
		values[4] = -5;
		values[16] = 10;
		values[17] = 2;
		values[18] = 2;
		values[35] = 2;
		values[37] = 6;
	} else if (game_type == gt::kTeamDeathmatch || gt::is_waypoint_family(game_type)) {
		values[3] = 2;
		values[4] = -5;
		values[6] = 1;
		values[15] = 12;
		values[16] = 10;
		values[17] = 2;
		values[18] = 2;
		values[35] = 2;
		values[37] = 6;
	} else if (game_type == gt::kKingOfTheHill) {
		values[3] = 4;
		values[4] = -5;
		values[12] = 10;
		values[16] = 10;
		values[17] = 2;
		values[18] = 2;
		values[21] = 2;
		values[22] = 3;
		values[33] = 2;
		values[35] = 2;
		values[36] = 60;
		values[37] = 6;
	} else if (game_type == gt::kTeamKingOfTheHill) {
		objective_combat();
		values[12] = 10;
		values[21] = 2;
		values[22] = 3;
		values[33] = 2;
	} else if (game_type == gt::kSearchAndDestroy || game_type == gt::kAttackDefend) {
		objective_combat();
		values[13] = 50;
		if (game_type == gt::kSearchAndDestroy) {
			values[24] = 2;
			values[26] = 3;
		} else {
			values[23] = 2;
			values[24] = 3;
		}
	} else if (game_type == gt::kCaptureTheFlag) {
		objective_combat();
		values[9] = 10;
		values[10] = 20;
		values[11] = 2;
		values[20] = 5;
	} else if (game_type == gt::kFlagBall) {
		objective_combat();
		values[10] = 40;
		values[11] = 2;
		values[20] = 5;
	} else if (game_type == gt::kAdvanceAndSecure) {
		objective_combat();
		values[12] = 5;
		values[24] = 2;
		values[26] = 2;
		values[31] = 1;
		values[34] = 15;
	} else if (game_type == gt::kConquerAndControl) {
		objective_combat();
		values[12] = 10;
		values[24] = 2;
		values[26] = 3;
		values[31] = 2;
		values[34] = 15;
	}
	return values;
}

// dfx2.exe's table: every section 1.0 but the head's 13 and 14 (3.0, with the 0x800 latch as
// Jointops.exe's); its seat leg is Jointops.exe's 6.0 (docs/titles/dfx2-vs-jo-re.md DFX2-16).
// [orig: Weapon_CalcImpactDamage @0x4e9a50 - the zone switch @0x4e9ae2..0x4e9b17, the seat leg
//  @0x4e9aab..0x4e9acf (dfx2.exe)]
double dfx2_hit_zone_damage_multiplier(int32_t hit_zone) {
	return world::hit_zone_is_critical(hit_zone) ? 3.0 : 1.0;
}

// dfx2.exe's rules: Jointops.exe's code, edited at the sites each knob names
// (docs/titles/dfx2-vs-jo-re.md §7, the §8 rows; runtime/world/title_rules.h carries Jointops.exe's
// side of each).
world::TitleRules make() {
	world::TitleRules r;
	// DFX2-9: the co-op checkpoint. The rewritten waypoint hook [orig:
	// EventTrigger_MarkCoopCheckpoint @0x4534a0 (dfx2.exe)]; the respawn on it [orig:
	// Server_PositionPlayerForSpawn @0x50a0a0 (@0x50a5b2..0x50a60d) (dfx2.exe)]; no goals-met
	// advance [orig: Player_UpdatePerFrame @0x4db400, the test gone after @0x4db73a (dfx2.exe)];
	// the reach advance past a done entry alone [orig: `cmp [esi+218h], bl` @0x4db76b (dfx2.exe)];
	// the 0x0F fold's done bytes and checkpoint reset [orig: NapiNPClientMsg_0x00F @0x42fcf0
	// (@0x42fe82, @0x42ffe7..0x42fffc) (dfx2.exe)].
	r.waypoint_completion = world::WaypointCompletion::CoopCheckpoint;
	r.coop_checkpoint_spawn = true;
	r.waypoint_goals_met_advance = false;
	r.waypoint_advance_needs_done = true;
	r.route_done_records = true;
	// DFX2-15: scoring. The table and the fanfare pair [orig: GameType_CreateDefaultSettings
	// @0x52ae80, 4 @0x52aeb0, 10 @0x52aec5 (dfx2.exe)]; the round-win multiplier [orig:
	// Player_ComputeScore @0x4fdbb0 (@0x4fdbff, @0x4fdc17) (dfx2.exe)]; VATTACHKILL [orig:
	// GameEvent_ProcessScoring @0x52c750 (@0x52d930..0x52d9b9) (dfx2.exe)].
	r.score_values = &dfx2_score_values;
	r.score_exp_fanfare = static_cast<uint16_t>(4u | (10u << 8));
	r.round_win_score_multiplier = true;
	r.vehicle_attach_kill_scored = true;
	// DFX2-16: damage. The zone table above; the clamp before the scales [orig:
	// @0x4e9c4d..0x4e9c63 (dfx2.exe)]; no occupant scale on the generic path [orig: @0x4e3960,
	// the test and the call gone after @0x4e3b07 (dfx2.exe)].
	r.hit_zone_damage_scale = &dfx2_hit_zone_damage_multiplier;
	r.impact_damage_clamp_first = true;
	r.occupant_damage_scale = false;
	// DFX2-17: the 25 s linger [orig: Server_ProcessRoundEnd @0x513730 (@0x51391e) (dfx2.exe)];
	// the seat or spawn-point deploy's 310 [orig: Server_DeployPlayer @0x514990 (@0x514b78,
	// @0x514b93) (dfx2.exe)].
	r.round_end_linger_ticks = 1550;
	r.deploy_target_protection_ticks = 310;
	// DFX2-19: no heal or revive [orig: @0x4e38d0 - nullsub_114 @0x4fcff0 from @0x4e3918,
	// nullsub_115 @0x4fd000 from @0x4e3945 (dfx2.exe)], no downed resend [orig: Server_TickUpdate
	// @0x51a810, the periodic block's tail @0x51b2c9 (dfx2.exe)].
	r.medic_heal_and_revive = false;
	r.downed_state_resend = false;
	// DFX2-20: no revive window [orig: GameEvent_PlayerDeath `mov [edi+170h], ebx` @0x51419b
	// (dfx2.exe)].
	r.revive_window = false;
	// DFX2-23: the part spin, about twice Jointops.exe's [orig: Entity_UpdatePartSpinAccumulator
	// @0x4928C0 - 372827 @0x492903 / @0x492925, 326222 and 279619 @0x49291E..0x492945, the decay
	// 372827 @0x492964; Entity_UpdateHeloRotorSpin @0x48FA80 - 372827 @0x48FB1C / @0x48FB3E, the
	// decay 93206 @0x48FB77; Entity_SpawnFromBMSRecord @0x40ECD0; the aircraft bone init
	// @0x4902FD (dfx2.exe)].
	r.part_spin = world::PartSpinRates{ 372827, 326222, 279619, 372827, 93206 };
	// DFX2-24: the load's table warnings [orig: Mission_LoadBMSFile @0x40F330 - `jle` @0x40F40A,
	// @0x40F443, @0x40F47C, @0x40F4B5 (dfx2.exe)].
	r.bms_table_warnings = true;
	// DFX2-27: the loadout chunk walked as the file holds it [orig: Mission_LoadBMSFile @0x40F330 -
	// strlen @0x40F599 where Jointops.exe calls AIProfile_SanitizeConfigData (dfx2.exe)].
	r.bms_loadout_sanitized = false;
	// DFX2-28: no mission-start difficulty [orig: Game_StartMission @0x521530 - the block gone
	// between @0x522EA9 and @0x522EAE (dfx2.exe)].
	r.mission_start_sets_difficulty = false;
	// DFX2-25: the counter-rotation [orig: Entity_UpdateTransformAndTurret @0x440B20 (dfx2.exe) -
	// IsTurret @0x440F87, Matrix_Multiply3x4_FixedPoint @0x440FC4, the words
	// @0x440FDB..0x440FFE].
	r.emplaced_turret_update = world::EmplacedTurretUpdate::CounterRotate;
	// DFX2-26: the planar traverse and its solo rate [orig: Entity_UpdateChildAttachment
	// @0x4407A0 (dfx2.exe) - the rate 0x92CF34 @0x440937, 0x3AB948 @0x440941 under the gate
	// @0x440875..0x4408A7].
	r.emplaced_solo_planar_slew = 0x3AB948;
	// DFX2-31: the narrowed radius [orig: Physics_RaycastAgainstBoneSections @0x4e1770
	// (@0x4e17af..0x4e17e0) (dfx2.exe)].
	r.narrow_still_player_radius = true;
	// DFX2-32: 4 u and 1860 counts [orig: @0x46532c (0x40000), @0x4653ac (0x744) in the twin of
	// AI_CheckVehicleStuckState @0x4652a0 (dfx2.exe)].
	r.ai_vehicle_near_reach = 0x40000;
	r.ai_vehicle_write_off_count = 1860;
	// DFX2-33: the profile-less spawn page dfx2.exe compiles in, the single-player page a fresh
	// profile takes too (dfx2_title.cpp's kSinglePlayerKit holds the names)
	// [orig: Buffer_CopyUntilDoubleNull(restrictionData, 0x832368, 2048) @0x52188f / @0x54ec2a
	//  (dfx2.exe)].
	r.spawn_kit = dfx2_title().profile_single_player_kit;
	// DFX2-37: the lone gunner's fire [orig: the twin of Input_HandleActionBinding_0 @0x4DD4C0
	// (dfx2.exe) - @0x4DD8FE..0x4DD92C, @0x4DDABB..0x4DDADB].
	r.emplaced_solo_holds_fire = true;
	// DFX2-111: the canopy opens on the jump bit [orig: the twin of Entity_UpdateInfantryPlayerBody
	// @0x4B4140 - @0x4B7BCD (dfx2.exe)].
	r.parachute_opens_on_jump = true;
	// DFX2-112: the airborne glide [orig: the twin of Entity_UpdateInfantryPlayerBody @0x4B4140 -
	// @0x4B7933..0x4B7A13 (dfx2.exe)].
	r.airborne_steer_glide = true;
	// DFX2-113: the impact speed's order [orig: Entity_ProcessTrackedVehiclePhysics @0x47C1C0 -
	// @0x47CCC7..0x47CCD1 (dfx2.exe)].
	r.tracked_impact_speed_xz_first = true;
	// Section 7.12: the zoom-out cap, 0x80000 for the radar and the big map alike
	// [orig: Input_HandleActionBinding @0x49ad50 - case 361, `cmp eax, 80000h` @0x49bf27 /
	// @0x49bf57 (dfx2.exe)].
	r.map_zoom_max_q16 = 0x80000;
	// Section 5.3: the friendly-tags toasts, modes 1 and 2 the literal lines, and the death screen's
	// step past mode 1 [orig: Input_HandleActionBinding @0x49ad50 - case 30, @0x49b5ee / @0x49b60c,
	// the step @0x49b59c..0x49b5b6 (dfx2.exe)].
	r.friendly_tag_toast_literals = {nullptr, "Friendly Tags: On", "Friendly Tags: Smart", nullptr};
	r.friendly_tags_skip_mode_1_on_death_screen = true;
	// DFX2-42: the voice waves' gain 240 [orig: WacCmd_SsnWave @0x4F49B0 (`imul 0F0h` @0x4F4A40);
	//  @0x4F4B20 and @0x4EA745 in the twins of WacCmd_SsnRadio and Wac_PlayScriptedVoiceWave
	//  (dfx2.exe)].
	r.script_voice_volume = 240;
	// DFX2-46: the trail lanes follow the ground, the alternate effect on snow
	//  [orig: Entity_UpdateBoneTrailEffects @0x4589A0 - @0x458AAA..0x458AB3 (dfx2.exe)].
	r.trail_surface_alternate = true;
	return r;
}

} // namespace

const world::TitleRules &dfx2_rules() {
	static const world::TitleRules rules = make();
	return rules;
}

} // namespace opennova::games
