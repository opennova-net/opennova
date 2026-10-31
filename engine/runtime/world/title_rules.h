// The in-match rules a title runs (CONTEXT.md "Title": the retail game a game runs as, its rules
// and the protocol it speaks): TitleRules, one named knob per difference the DFX2 catalog records
// between dfx2.exe and Jointops.exe in the host's and the joiner's in-match code
// (docs/titles/dfx2-vs-jo-re.md §7, the §8 row each knob names), and its registry keyed by title
// code. Each title's row lives in games/<code>/<code>_rules.cpp, which games::install_rules() hands
// to install_title_rules; engine/ holds the type and the lookups alone, and several titles may run one
// TitleRules object (OP: Acid Gambit runs Joint Operations').
//
// A default-constructed TitleRules is Jointops.exe's: JO is the baseline every port here was
// witnessed on, and its tables stay where the port keeps them (builtin_score_values,
// hit_zone_damage_multiplier, playersav::default_single_player_page). A game's file sets what its
// executable does otherwise, each knob with both executables' cites.
//
// Where the other per-title rules live, and why not here:
// - DFX2-8's player caps are the Title's (base/title/title.h max_players, max_players_host_screen):
//   the NovaWorld host leg (net/novaworld/connect_or_host.h) and the service's hosted limits read
//   them beside the session's caps (inmatch title_player_caps), and net/ cannot see runtime/. They
//   are rules, not wire, so they are no Protocol's.
// - DFX2-7/8's game.cfg rows, defaults and ceilings stay in formats/gamecfg (config_var_rows, the
//   Config_SetDefaults and post-load clamps), keyed by the Title's config_table: formats/ cannot
//   see runtime/ either.
// - §7.12's zoom-out cap and §5.3's friendly-tags toasts and death-screen step are the Title's
//   (base/title/title.h map_zoom_max_q16, friendly_tag_toast_literals,
//   friendly_tags_skip_mode_1_on_death_screen): read on the local player's input alone, they are
//   the executable's own HUD and input, as its HUD variants are, and change no peer's world.
// - The wire dialect (DFX2-1, 2, 10..14, 18) is the Protocol's (net/protocol/protocol.h
//   ProtocolInMatch, by message). DFX2-1's 0x0A fifth u32 stays there; DFX2-9's checkpoint RULES
//   are here.
//
// A knob marked "joiner-side" is read on a joiner's world or its replica, not only on the
// authority: a change to one changes what a joiner does, so it bumps the game's protocol version
// (PV2) in its <code>_protocol.cpp (games/CLAUDE.md); games_test lists them per title.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <runtime/world/impact_damage.h> // hit_zone_damage_multiplier (Jointops.exe's table)
#include <runtime/world/weapon_scope_zero.h> // ScopeZeroSeed (DFX2-120)

namespace opennova::world {

// A game type's score row: its 39 VAR values, the 0x9C bytes GameType_CreateDefaultSettings
// zeroes per row before it stores them (S2C 0x58 carries the same 39).
// [orig: GameType_CreateDefaultSettings @0x52DD00] [orig: GameType_CreateDefaultSettings
//  @0x52ae80 - the row's memset(.., 0x9C) @0x52afd0 (dfx2.exe)]
using ScoreValues = std::array<int32_t, 39>;

// Jointops.exe's built-in score table: the VAR values of a game type's row, as
// GameType_CreateDefaultSettings stores them (match_score_table.cpp, which carries the rows'
// witness; MatchRules::score_values). TitleRules::score_values' default.
// [orig: GameType_CreateDefaultSettings @0x52DD00]
ScoreValues builtin_score_values(uint32_t game_type);

// DFX2-9: what a fired event's waypoint hook marks done, on the authority (the hook returns at
// once off it [orig: the is_authority test @0x452ce0]).
enum class WaypointCompletion : uint8_t {
	// Jointops.exe's: the waypoints the event links, and the chained ones before them, in every
	// game type (WaypointTrack::on_event_fired).
	// [orig: EventTrigger_MarkLinkedSpawnPoints @0x452ce0, called @0x454cbd / @0x454d25]
	LinkedSpawnPoints,
	// dfx2.exe's rewrite: in co-op, the player route's markers whose linked event has fired, the
	// newest one's pool-3 slot becoming the checkpoint (BmsEventSystem::mark_coop_checkpoint).
	// [orig: EventTrigger_MarkCoopCheckpoint @0x4534a0 (dfx2.exe)]
	CoopCheckpoint,
};

// DFX2-25: what an IsTurret child's class update does to its gun words on a carrier.
enum class EmplacedTurretUpdate : uint8_t {
	// Jointops.exe's: the words as the slew left them.
	// [orig: Entity_UpdateTransformAndTurret @0x440ca0]
	Plain,
	// dfx2.exe's: the words turned against the turn the rebuilt attachment matrix took, so the
	// gun holds its world aim on a turning carrier (world::emplaced_counter_rotates_frame).
	// [orig: Entity_UpdateTransformAndTurret @0x440B20 (dfx2.exe) - IsTurret @0x440F87, the euler
	//  @0x440FD6, the words @0x440FDB..0x440FFE]
	CounterRotate,
};

// A title's built-in score table: game type -> the row's VAR values (builtin_score_values).
using ScoreValuesTable = ScoreValues (*)(uint32_t game_type);
// A title's person hit-zone -> bullet damage scale (hit_zone_damage_multiplier).
using HitZoneScaleTable = double (*)(int32_t hit_zone);

// DFX2-23: the rotor machines' rates (world/vehicle_part_anim.h): the three spin-up rates a part
// seeds (the full one a player-control item's, an EngineRunning spawn's and a flying aircraft's
// bone init's too; the other two a non-player-control item's 34..66 and 67..99 rolls) and the two
// machines' spin-down per tick. Jointops.exe's below; dfx2.exe's are each about twice as large.
// [orig: Entity_UpdatePartSpinAccumulator @0x4928B0 - 186413 @0x4928F3 / @0x492915, 163110 and
//  139809 @0x49290E..0x492935, the decay 186413 @0x492954; Entity_UpdateHeloRotorSpin @0x48FA70 -
//  186413 @0x48FB0C / @0x48FB2E, the decay 46603 @0x48FB67; Entity_SpawnFromBMSRecord @0x40EE80;
//  Entity_InitAircraftWeaponBones @0x4902ED]
struct PartSpinRates {
	int32_t full = 186413; // 0x2D82D
	int32_t mid = 163110; // 0x27D26
	int32_t low = 139809; // 0x22221
	int32_t ground_decay = 186413;
	int32_t helo_decay = 46603; // 0xB60B
};

struct TitleRules {
	// --- DFX2-9: the co-op checkpoint (§7.1). --------------------------------------------------
	// The authority's waypoint hook at an event's dispatch (event_runtime.cpp).
	// [orig: EventTrigger_MarkLinkedSpawnPoints @0x452ce0] [orig: EventTrigger_MarkCoopCheckpoint
	//  @0x4534a0 (dfx2.exe)]
	WaypointCompletion waypoint_completion = WaypointCompletion::LinkedSpawnPoints;
	// A co-op respawn lands on the checkpoint marker, its position and angle words over the start
	// marker's pose (spawn_select.cpp); Jointops.exe has no such arm.
	// [orig: Server_PositionPlayerForSpawn @0x50cf60] [orig: Server_PositionPlayerForSpawn
	//  @0x50a0a0 (@0x50a5b2..0x50a60d), Pool_GetEntryUnchecked(3, g_CoopCheckpointSlot)
	//  @0x50a5d9 (dfx2.exe)]
	bool coop_checkpoint_spawn = false;
	// joiner-side: the local player's waypoint advances at once when every goal the waypoint
	// asks for is won, wherever the player stands (waypoint_track.cpp); dfx2.exe has no such
	// advance. Every world's per-frame selection runs it, the joiner's too.
	// [orig: Player_UpdatePerFrame @0x4de350 (`cmp [esp+outEntry], ebx` @0x4de68e ->
	//  Spectator_CycleTarget(1) @0x4de695)] [orig: Player_UpdatePerFrame @0x4db400, the test gone
	//  after @0x4db73a (dfx2.exe)]
	bool waypoint_goals_met_advance = true;
	// joiner-side: the local player's waypoint advances by reach only past an entry marked done
	// (the co-op checkpoint's marking, waypoint_track.cpp); Jointops.exe's advances past any.
	// [orig: Player_UpdatePerFrame @0x4de350 (@0x4de68e..0x4de6d4)] [orig: @0x4db762..0x4db774,
	//  `cmp [esi+218h], bl` @0x4db76b (dfx2.exe)]
	bool waypoint_advance_needs_done = false;
	// joiner-side: the joiner's S2C 0x0F fold stores each route record's done byte on its
	// waypoint and resets its checkpoint copy to the last done record's slot
	// (replication::ClientReplicaPipeline, inmatch replica_track); Jointops.exe's skips the byte.
	// [orig: NapiNPClientMsg_0x00F @0x42e497 (the skip)] [orig: NapiNPClientMsg_0x00F @0x42fcf0
	//  (@0x42fe82, @0x42ffe7..0x42fffc, `mov [esi+218h], bl` @0x42fff0) (dfx2.exe)]
	bool route_done_records = false;

	// --- DFX2-15: scoring (§7.8). ----------------------------------------------------------------
	// The built-in score table a match with no score.ini row scores with and S2C 0x58 advertises
	// (MatchRules::score_values).
	// [orig: GameType_CreateDefaultSettings @0x52DD00] [orig: GameType_CreateDefaultSettings
	//  @0x52ae80 (dfx2.exe)]
	ScoreValuesTable score_values = &builtin_score_values;
	// The EXP_FANFARE pair the table seeds before score.ini (lo byte the KILLTONE threshold, hi
	// the HEADSHOTTONE; GameConfig::exp_fanfare): 0 0 in Jointops.exe.
	// [orig: GameType_CreateDefaultSettings @0x52DD00 - 0 @0x52dd30, 0 @0x52dd45 (the
	//  word_24C1170 mirror @0x52dd3e / @0x52dd49)] [orig: @0x52aeb0 (4), @0x52aec5 (10)
	//  (dfx2.exe)]
	uint16_t score_exp_fanfare = 0;
	// The slot's computed score and the end-of-round score columns are multiplied by stat field
	// 34, the round marker the round end stores 2 in for the winning team (Match::
	// score_multiplier); Jointops.exe never reads it.
	// [orig: Player_ComputeScore @0x500A80 - no read of field 34] [orig: Player_ComputeScore
	//  @0x4fdbb0 - CPlayerStats_GetFieldPlusOne(34) @0x4fdbff, the `imul` @0x4fdc17;
	//  Server_BuildEndOfRoundScoreboard @0x506060 (@0x5062f3..0x50630b) (dfx2.exe)]
	bool round_win_score_multiplier = false;
	// An enemy kill also scores VATTACHKILL (VAR 37) when the victim rides a vehicle or something
	// standing on one; Jointops.exe parses VATTACHKILL and never scores it.
	// [orig: GameEvent_ProcessScoring @0x52F550] [orig: GameEvent_ProcessScoring @0x52c750
	//  (@0x52d930..0x52d9b9); Entity_HasActiveParent @0x402bb0 (dfx2.exe)]
	bool vehicle_attach_kill_scored = false;

	// --- DFX2-16: damage (§7.9). -----------------------------------------------------------------
	// The person hit-zone scale of a bullet's damage (impact_damage.cpp); the seat leg's 6.0 and
	// the 0x800 latch are the same in both.
	// [orig: Weapon_CalcImpactDamage @ 0x4EC920 - the zone table @ 0x4ec9bf] [orig:
	//  Weapon_CalcImpactDamage @0x4e9a50 - the zone switch @0x4e9ae2..0x4e9b17 (dfx2.exe)]
	HitZoneScaleTable hit_zone_damage_scale = &hit_zone_damage_multiplier;
	// The ammo's minimum and maximum (Def[47], Def[48]) clamp the speed-scaled base before the
	// section and class scales, where Jointops.exe clamps after them (clamp_impact_damage).
	// [orig: Weapon_CalcImpactDamage - the floor @0x4ecb77..0x4ecb81, the cap @0x4ecb83..0x4ecb91]
	// [orig: @0x4e9c4d..0x4e9c63 (dfx2.exe)]
	bool impact_damage_clamp_first = false;
	// A vehicle's occupants reduce what reaches its hull on the generic damage path
	// (destruction.cpp); dfx2.exe drops that call and keeps only the projectile path's.
	// [orig: ItemDef type 1 @ 0x4e69c7 -> Entity_ApplyOccupantDamageScale @ 0x4e69d3] [orig:
	//  @0x4e3960, the test and the call gone after @0x4e3b07 (dfx2.exe)]
	bool occupant_damage_scale = true;

	// --- DFX2-17: timers (§7.10). ----------------------------------------------------------------
	// The end-of-round linger a round end stores, in ticks: 2790 (45 s). The admin's CYCLE and
	// GOTO 620 is the same in both.
	// [orig: Server_ProcessRoundEnd @0x5164F0 (@0x5166c4)] [orig: Server_ProcessRoundEnd
	//  @0x513730 (@0x51391e) (dfx2.exe)]
	uint32_t round_end_linger_ticks = 2790;
	// The deploy's spawn protection (entity+292) when the deploy targets a vehicle (ItemDef type
	// 1) or a spawn-point item (attrib 0x40000); with any other target, or none, it is 620 in both
	// (inmatch deploy_spawn_protection_ticks).
	// [orig: Server_DeployPlayer @0x517740 - 620 @0x517937 (type 1) / @0x517952 (0x40000)] [orig:
	//  Server_DeployPlayer @0x514990 - 310 @0x514b78 / @0x514b93, 620 @0x514ba1 (dfx2.exe)]
	int32_t deploy_target_protection_ticks = 620;

	// --- DFX2-19: the medic (§4.3). --------------------------------------------------------------
	// The medic dispatcher revives and heals (server_medic.cpp); dfx2.exe's keeps the tests and
	// calls two empty functions.
	// [orig: GameEvent_HandleMedicInteraction @0x4e6790 (@0x4e67d8, @0x4e6805)] [orig: @0x4e38d0 -
	//  nullsub_114 @0x4fcff0 from @0x4e3918, nullsub_115 @0x4fd000 from @0x4e3945 (dfx2.exe)]
	bool medic_heal_and_revive = true;
	// The server tick's team-mode 1 Hz S2C 0x46 resend of the downed players
	// (Server_ResendTeamDownedStates); dfx2.exe's tick has no such block.
	// [orig: Server_TickUpdate @0x51e333..0x51e373] [orig: Server_TickUpdate @0x51a810, the
	//  periodic block's tail @0x51b2c9 (dfx2.exe)]
	bool downed_state_resend = true;

	// --- DFX2-20: the revive window. -------------------------------------------------------------
	// A normal other-player kill arms the victim slot's 120-second revive window (slot+0x170);
	// dfx2.exe stores 0 on both arms, so it sends no S2C 0x54 at a death.
	// [orig: GameEvent_PlayerDeath @0x516DD0: playerSlot+368 (@0x516f4f)] [orig:
	//  GameEvent_PlayerDeath `mov [edi+170h], ebx` @0x51419b, the 0x54 gate @0x5145cb (dfx2.exe)]
	bool revive_window = true;

	// --- DFX2-23: the vehicle part spin. ---------------------------------------------------------
	// joiner-side: every peer spins the rotors and the spinning parts of the vehicles it does not
	// drive (world/vehicle_part_anim.h). The defaults are Jointops.exe's; dfx2.exe doubles them
	// (§5.4).
	PartSpinRates part_spin;

	// --- DFX2-24: the BMS load's table warnings. -------------------------------------------------
	// A mission whose header counts more items, decorations, markers or organics than their pool
	// holds warns at its start's header checks and its load's (mission::warn_tables_past_pools,
	// from MissionKernel::boot and mission::promote_mission). Jointops.exe jumps over every test;
	// dfx2.exe shows "Too many <table> %i (max is %i)" in its warning box (OK carries on, Cancel
	// ends the process), which ours logs and asks the embedder's box
	// (KernelBootOptions::load_warning).
	// [orig: Mission_LoadBMSFile @0x40F4E0 - `cmp eax, 4B0h; jmp` @0x40F5B5 / @0x40F5BA and the
	//  same unconditional jumps for the other three; BMS_LoadAndValidateHeader @0x40E250 -
	//  @0x40E31A, @0x40E34D, @0x40E380, @0x40E3B3]
	// [orig: Mission_LoadBMSFile @0x40F330 - `jle` @0x40F40A, Game_ShowWarningDialog @0x40F431,
	//  @0x40F46A, @0x40F4A3, @0x40F4DC; BMS_LoadAndValidateHeader @0x40E0A0 - `jle` @0x40E16A,
	//  @0x40E19D, @0x40E1D0, @0x40E203; Game_ShowWarningDialog @0x4A5D60 - MessageBoxA(.., 0x31)
	//  @0x4A5D8B (the push 31h @0x4A5D81), Game_ShutdownAndExit on Cancel @0x4A5D96 (dfx2.exe)]
	bool bms_table_warnings = false;

	// --- DFX2-27: the BMS loadout chunk's repair. ------------------------------------------------
	// Out of a session the mission load walks its loadout chunk four strings at a time
	// (mission::stash_mission_loadout_rules). Jointops.exe first repairs every record that wrote
	// three strings (bms.h WeaponLoadoutRecord::has_flags), so each stride is one record; dfx2.exe
	// measures the chunk with strlen and walks it as the file holds it, so a three-string record
	// takes the next record's name as its fourth string and every later stride runs one string off.
	// [orig: Mission_LoadBMSFile @0x40F4E0 - AIProfile_SanitizeConfigData @0x40CFE0 from @0x40F739,
	//  the length @0x40F741, the stride @0x40F7B4..0x40F809]
	// [orig: Mission_LoadBMSFile @0x40F330 - strlen @0x40F590..0x40F599, the length @0x40F59B, the
	//  stride @0x40F610..0x40F65D (dfx2.exe)]
	bool bms_loadout_sanitized = true;

	// --- DFX2-28: the single player's mission-start difficulty. ----------------------------------
	// Out of a session Jointops.exe's mission start sets the difficulty word (SessionRules::
	// difficulty) to 0, and under the objective Co-op game type to -1 with the single-player
	// respawn attribute (0x40) and the local player's health at its doubled ceiling
	// (world::mission_start_difficulty). dfx2.exe has no such block: the word stays the one the
	// fresh start's session settings copied from the profile (+0x564), on a restart too, and an
	// objective Co-op mission takes neither the 0x40 nor the health.
	// [orig: Game_StartMission @0x524360 - @0x525CC2..0x525D12, the 0 @0x525CDD, the -1
	//  @0x525CFA, the 0x40 OR @0x525CF2]
	// [orig: Game_StartMission @0x521530 - Environment_MissionStartInit @0x522EA4, then the
	//  terrain call @0x522EA9 and Game_SpawnAllEntityGlowEffects @0x522EAE, nothing between; the
	//  word's other writers Game_ApplySessionSettingsToGlobals @0x54F158 / @0x54F1C5 and the net
	//  handlers (dfx2.exe)]
	bool mission_start_sets_difficulty = true;

	// --- DFX2-25: the emplaced gun's counter-rotation. -------------------------------------------
	// joiner-side: the World's gun channel and the joiner's replica gun channel
	// (inmatch client_replica_emplaced.cpp) both run it.
	// [orig: Entity_UpdateTransformAndTurret @0x440ca0] [orig: Entity_UpdateTransformAndTurret
	//  @0x440B20 (dfx2.exe)]
	EmplacedTurretUpdate emplaced_turret_update = EmplacedTurretUpdate::Plain;

	// --- DFX2-26: the emplaced gun's traverse. ---------------------------------------------------
	// joiner-side: 0, Jointops.exe's per-axis traverse at the IsTurret rate (0x92CF34) on yaw and
	// pitch alike; otherwise dfx2.exe's planar one, the step's length on the yaw / pitch plane held
	// to the rate, at this slower rate while the local player mans a gun whose carrier counts one
	// occupied seat at most (world::emplaced_turret_planar_rate). The World's channel and the
	// joiner's replica channel both read it.
	// [orig: Entity_UpdateChildAttachment @0x4409a0] [orig: Entity_UpdateChildAttachment
	//  @0x4407A0 (dfx2.exe) - the rate 0x92CF34 @0x440937, 0x3AB948 @0x440941 under the gate
	//  @0x440875..0x4408A7]
	int32_t emplaced_solo_planar_slew = 0;

	// --- DFX2-31: a person's extra radius. -------------------------------------------------------
	// joiner-side: a round's bone walk narrows the round's extra radius for a player (Flags 0x100)
	// that rides something (+0x16C) or holds still (MoveOrder bit 3 clear): past 0x1000 it keeps
	// half the rest (world::person_extra_radius). The joiner's own traces walk its wire proxies'
	// bones with it too.
	// [orig: Physics_RaycastAgainstBoneSections @0x4e4670] [orig:
	//  Physics_RaycastAgainstBoneSections @0x4e1770 (@0x4e17af..0x4e17e0) (dfx2.exe)]
	bool narrow_still_player_radius = false;

	// --- DFX2-32: the AI vehicle write-off (the authority's AI). ---------------------------------
	// A live body within this reach (Q16, past both radii) keeps a stuck hull alive.
	// [orig: AI_CheckVehicleStuckState @0x465290 (@0x46531c)] [orig: @0x46532c (0x40000) in the
	//  twin @0x4652a0 (dfx2.exe)]
	int32_t ai_vehicle_near_reach = 0xC0000;
	// The stuck count past which the write-off starts; the 3720 kill is the same in both.
	// [orig: AI_CheckVehicleStuckState @0x465290 (`cmp [esi+148h], 0D52h` @0x46539c)] [orig:
	//  `cmp [esi+148h], 744h` @0x4653ac in the twin @0x4652a0 (dfx2.exe)]
	int32_t ai_vehicle_write_off_count = 3410;

	// --- DFX2-33: the profile-less spawn kit. ----------------------------------------------------
	// joiner-side: the static page the executable compiles in, its names each "<name> -1 -1 -1",
	// null-terminated; null for Jointops.exe's single-player page
	// (playersav::default_single_player_page). world::weapon_kit_default reads it: the single
	// player's kit and the equipped default every body a joiner spawns takes.
	// [orig: Buffer_CopyUntilDoubleNull(restrictionData, 0x833BF8, 2048) @ 0x5246be / @ 0x5519e4]
	// [orig: the page at 0x832368 @0x52188f / @0x54ec2a (dfx2.exe)]
	const char *const *spawn_kit = nullptr;

	// --- DFX2-37: a lone gunner holds fire. ------------------------------------------------------
	// joiner-side: the fire row's release leg, and its press leg from a seat other than the
	// Driver's and the Controller's, request nothing while the local player rides (+0x16C) an
	// IsTurret gun whose +0x28 carrier counts one occupied seat at most
	// (emplaced_carrier_occupied_seats), the gunner's own included
	// (world::local_solo_turret_holds_fire). Its own input legs, so every peer's local player
	// alike.
	// [orig: Input_HandleActionBinding_0 @0x4E0420 (no such test)] [orig: the twin @0x4DD4C0
	//  (dfx2.exe) - the release leg's @0x4DD8FE..0x4DD92C, the press leg's @0x4DDABB..0x4DDADB,
	//  each `cmp eax, ebx; jle` with ebx 1 @0x4DD4E6]
	bool emplaced_solo_holds_fire = false;

	// --- DFX2-111: the parachute's open. -------------------------------------------------------
	// joiner-side: every peer's player body pass runs the canopy tail (world::parachute_tick).
	// The authority opens a falling body's canopy (vz <= -0x3800) on its carried-parachute bit
	// (+0x2C & 0x10), which every peer's open canopy then clears, under Jointops.exe; on its
	// MoveOrder jump bit (+0x12C & 0x20) under dfx2.exe, which carries and clears nothing, so a
	// jump opens it, with no parachute held, as often as it falls.
	// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the test @0x4B7AE5..0x4B7AFA, the clear
	//  @0x4B7BC9] [orig: Entity_UpdateInfantryPlayerBody's twin @0x4B4140 - `test byte ptr
	//  [esi+12Ch], 20h` @0x4B7BCD, the open @0x4B7BD6..0x4B7BD9, no clear in the open canopy's
	//  arm @0x4B7C57..0x4B7CA4 (dfx2.exe)]
	bool parachute_opens_on_jump = false;

	// --- DFX2-112: the airborne steer's glide. -------------------------------------------------
	// joiner-side: every peer's player body pass steers an airborne body (the authority's
	// AiSystem body tick and a joiner's replica rows alike). While the moving bit is held both
	// push the slide pair 64 along the look and pad angle A; dfx2.exe also adds ftol(cos A * Q)
	// and ftol(sin A * Q), Q = (-vz) >> 7, and turns savedLivePitch (+0x90) and Roll (+0x18) by
	// ftol(cos / sin of the pad angle * 2^22).
	// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - @0x4B78B7..0x4B793D] [orig: the twin
	//  @0x4B4140 - Q @0x4B794B..0x4B7956, the pair @0x4B7962..0x4B79BA, +0x90 @0x4B79D3, Roll
	//  @0x4B79E2, the canopy's double @0x4B79E5..0x4B7A13; dbl_7C8A28, dbl_7C8A38, flt_7C8A54
	//  (-64), flt_7C8A50 (4194304) (dfx2.exe)]
	bool airborne_steer_glide = false;

	// --- DFX2-113: the tracked hull's impact speed. --------------------------------------------
	// joiner-side: every peer's tracked ground solve gates its scrape sound and momentum share on
	// it. Its |velocity| at a severity-3 contact sums the squares as Jointops.exe's x87 order,
	// vx^2 + (vy^2 + vz^2), or dfx2.exe's, (vx^2 + vz^2) + vy^2 (world::tracked_impact_speed).
	// [orig: Entity_ProcessTrackedVehiclePhysics @0x47C1C0 - @0x47CCAD..0x47CCD5] [orig:
	//  Entity_ProcessTrackedVehiclePhysics @0x47C1C0 - @0x47CCAD..0x47CCD3 (dfx2.exe)]
	bool tracked_impact_speed_xz_first = false;

	// --- DFX2-42: the scripted voice's gain. -----------------------------------------------------
	// joiner-side: the gain byte a script's voice wave (wave, SSNwave, SSNradio) starts at, which the
	// channel plays as (gain * the dialogue volume + 128) >> 8 through the distance curve
	// (world::ScriptVoiceChannel); a joiner's world plays its host's SSNwave remote command with it.
	// [orig: Wac_PlayScriptedVoiceWave @0x4ED688, WacCmd_SsnWave @0x4F7989, WacCmd_SsnRadio @0x4F7A63 -
	//  `imul 0D2h`]
	// [orig: WacCmd_SsnWave @0x4F49B0 (imul @0x4F4A40); WacCmd_SsnRadio @0x4F4A90 (imul @0x4F4B20);
	//  Wac_PlayScriptedVoiceWave @0x4EA6F0 (imul @0x4EA745) - `imul 0F0h` (dfx2.exe)]
	int32_t script_voice_volume = 210;

	// --- DFX2-46: a vehicle trail's alternate effect on snow. ------------------------------------
	// joiner-side: a trail lane with an alternate effect (particlefxw1 / particlefxw2's third token)
	// runs in the surface type under the vehicle, spawns its points with the alternate on snow (3)
	// and restarts them when the type changes (world vehicle_update_trail_lane); Jointops.exe's lane
	// runs in state 1 and spawns the effect alone. A joiner's own vehicle mover samples the trails.
	// [orig: Entity_UpdateBoneTrailEffects @0x4589C0 - the state 1 @0x458A2D / @0x458A5A, the test
	//  @0x458ACE, the store @0x458DA8 / @0x458DC2]
	// [orig: Entity_UpdateBoneTrailEffects @0x4589A0 - Terrain_GetSurfaceTypeAtPosition @0x458AAA,
	//  `cmp ax, 3` @0x458AAE taking +0x300 @0x458AB3, the test @0x458AD2, the store
	//  @0x458DB9 / @0x458DD2 (dfx2.exe)]
	bool trail_surface_alternate = false;

	// --- DFX2-120: the weapon slot's sight zero. -------------------------------------------------
	// joiner-side: every slot init (the loadout fills, a pickup's grant, an emplacement's own slot)
	// seeds the zero words by it (world::weapon_scope_zero_slot_seed), and a ForceScoped mount
	// stores the rangefinder word by it (world::weapon_scope_mount_rangefinder). A joiner fills its
	// own slots, and the local shooter's zero step rides its fire (the round's subtype), so a
	// joiner's rounds climb as its title zeroes them.
	// [orig: WeaponSlot_InitFromDef @0x53EE70; Player_MountWeaponSlot @0x4DFA40 (@0x4DFB3B..
	//  0x4DFB44)] [orig: WeaponSlot_InitFromDef @0x53C0C0 (@0x53C1E8..0x53C1F9);
	//  Player_MountWeaponSlot @0x4DCAE0 (@0x4DCBDB) (dfx2.exe)]
	ScopeZeroSeed scope_zero_seed = ScopeZeroSeed::DefaultStep;
};

// One registry row: a title's code and the rules it runs.
struct TitleRulesRow {
	const char *title_code = nullptr;
	const TitleRules *rules = nullptr;
};

// Fills the registry with `rows` (each TitleRules outliving the program); a second call replaces
// the first. The lookup below aborts on an EMPTY registry, naming the invariant: a program that
// reaches it calls games::install_rules() at its start (games/games.h, opennova_games_rules).
void install_title_rules(const TitleRulesRow *rows, size_t count);

// The rules title `title_code` runs, compared case-insensitively: the default title's
// (base/title/title.h default_title) for an empty or unknown code. (A session's protocol has no
// such default: protocol_in_match aborts for a session with none.)
const TitleRules &title_rules(std::string_view title_code);

// The rules a holder (SessionRules, MatchRules, WaypointCycleContext, inmatch::GameConfig) starts
// with, before the embedder or the session sets its title's: null in every program, so a read
// before that aborts (require_title_rules) and no title's rules are ever run by default. The test
// suite's registration point alone installs one (tests/common/install_games.cpp: Joint Operations',
// which every fixture ran before titles had rules), so a fixture that builds a World with no
// session keeps its expectations.
void install_unset_title_rules(const TitleRules *rules);
const TitleRules *unset_title_rules();

// The rules a holder runs: the pointer the embedder or the session set from title_rules. A read
// while it is unset aborts, naming `holder`.
[[noreturn]] void title_rules_unset(const char *holder);
inline const TitleRules &require_title_rules(const TitleRules *rules, const char *holder) {
	if (rules == nullptr) title_rules_unset(holder);
	return *rules;
}

} // namespace opennova::world
