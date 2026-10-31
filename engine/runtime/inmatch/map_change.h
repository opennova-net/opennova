#pragma once

// The host's round-end map change (D-NET-331, net-re §5.70): one engine
// implementation the game's listen host and Serve Only host and
// opennova-serve all run.
//
// Retail runs it as a scene path. The round end's linger stores mission exit 3
// or 4 (server_tick.cpp); the main frame pushes the Post Menu, so the Game
// Loop's shutdown, Game_TeardownMission, runs first: on an in-session
// authority the Attack-and-Defend side swap (or the SETNEXT latch), the
// entities and the PostMission pass, then the slot reset with its S2C 0x25
// per slot. The Post Menu's router then replays the same entry for a
// launch-option map's second half or advances the rotation; a next mission
// goes PreMenu state 7 -> Game Loop -> Game_StartMission(0), inside the same
// session; a miss ends the session (StopServer's goodbye) and the menu.
// [orig: Game_ProcessMainFrame @0x526867; Game_TeardownMission @0x522350;
//  PostMenu_RouteMissionExit @0x5685C2..0x56864F; PreMenu_Init @0x5693E0;
//  GameLoop_Init @0x526370 -> Game_StartMission @0x524360]
//
// The port's steps, in that order:
//   begin_host_map_change -- the teardown's authority arms and the router,
//     on the live HostRole: the pools' destroy (each carrier's flag drop and
//     its 0x2F), the swap or the latch, the teardown's pump over the destroyed
//     rows, the PostMission pass, the slot reset (each kept connection loses its per-mission state and its
//     entity, keeps its identity, its keys, its sequencing and its team), the
//     half toggle and the advance. NextMission: the embedder boots the next
//     map through the one host boot (inmatch/host_boot.h) with
//     HostBootRequest::next_mission; RotationEnded: the embedder closes the
//     session (HostRole::close: StopServer's goodbye).
//   the load's head, ahead of its terrain (HostRole::begin_next_mission_load:
//     the game's world load runs it through host_boot.h's
//     begin_next_host_mission_load, a headless host's phase A itself): the
//     file-transfer session and the report clocks, the mission text, the kept
//     session continued with its streams rebuilt (host_session.h
//     continue_host_session), the per-slot reset, the chat rings' clear, then
//     the window's first two pumps.
//   the load's stage pumps (HostRole::pump_load_stage): after the terrain,
//     after the mission's document, after the avatars and the environment
//     clock, ahead of the objects.
//   the boot's bring-up (HostRole::bring_up with HostBringup::next_mission):
//     the new World configured for the kept session
//     (configure_continued_session_world), the own client's view rebuilt,
//     then the window's remaining pumps (5 with an own client, 4 dedicated), but
//     the render arm's (3 / 2) when the game's device stages run them
//     (HostRole::render_arm_pumps).
//   after the kernel's PreMission pass and the round counters' reset,
//     the round init over the kept slots, as every mission start's
//     (HostRole::run_round_init -> init_all_player_entities_for_round);
//     with the game's device stages these run in phase B, past the render arm's last pump.
//   the boot's phase B, or end_host_mission_load after it on the game,
//     ends with the load-end S2C 0x7B to every slot.
// The embedder loads the next map on the session's mount: retail opens its archives once per
// process and again only on a mods or expansion switch, never at a map change, and its teardown
// destroys the mission's device objects with the archives still open (D-NET-1057; the game:
// SessionDrive::load_next_host_mission, GameWorld::unload's decoded-texture drop).
// [orig: PFF_OpenAllArchives @0x4A4310 - its callers Game_InitSubsystems @0x4A6F44,
//  Game_ReloadExpansionAndMods @0x552798, Expansion_ReloadAllAssets @0x5683B5 (behind its
//  changed-name test @0x56837A), Expansion_SwitchTo @0x56896F; Game_TeardownMission @0x522350 -
//  CGfxDevice_DestroyAllObjects @0x52274A, whose texture-cache free is D3DTextureCache_FreeAll
//  @0x6781C6]
// The host pumps its socket at the witnessed points: the teardown's pump ahead
// of the slot reset, then the load's with its spawn gate held (D-NET-116): its
// first, then its window spread through the load as retail's is, so the 0x25s
// leave at the first pump whose send clock is open (before the terrain when
// the clock allows) and a joiner reloading in place is answered from its kept
// slot at a later pump inside the load; its replies leave ahead of the load
// end's 0x7B, which leaves at the first tick.
// [orig: Game_StartMission @0x524360 - Server_PumpNetworkTransport @0x5245E8,
//  @0x524A63, @0x524F76, @0x524F8C, @0x525256, @0x5253EA, @0x52589D, @0x5258E5,
//  @0x5259EC (an own client's), @0x525A5C, @0x525AC5]

#include <runtime/mission/mission_catalog.h>
#include <runtime/world/weather_state.h> // world::TodClock

#include <cstdint>
#include <optional>
#include <vector>

namespace opennova::gamecfg {
struct GameCfg;
}

namespace opennova::world {
class World; // the class-key must match world.h (MSVC mangles it)
}

namespace opennova::inmatch {

class HostRole;
struct HostRotation;
struct NapiNPConnection;
struct NapiNPServerCtx;

// What the map change carries on the HostRole between its steps.
struct HostMapChange {
	// The teardown ran on the current kernel: its entities are gone and its
	// PostMission pass ran, so the role's close skips both.
	bool torn_down = false;
	// The mission clock an in-session authority's teardown keeps for the next mission, whose
	// start runs on it when the session runs on time-of-day continuity (GameConfig::
	// time_of_day_continuity; host_boot.h kept_mission_clock). Set at every map change's
	// teardown, whatever the continuity word; empty from each host start (HostRole::reset_session).
	// [orig: Game_TeardownMission @0x522350 - the latch dword_24D5A0C = 1 @0x52252A, the clock
	//  @0x522539, its advance @0x522543; UI_HandleHostSessionStart @0x556D00 - the latch
	//  cleared @0x556DD1] [orig: Game_TeardownMission @0x51F520 - @0x51F6FA / @0x51F709 /
	//  @0x51F713; UI_HandleHostSessionStart @0x554EB0 - @0x554F8F (dfx2.exe)]
	std::optional<world::TodClock> kept_clock;
};

enum class MapChangeStep : uint8_t {
	NextMission,   // the rotation names the next map; boot it inside the session
	RotationEnded, // no next mission: the session ends (StopServer) and the menu
};

// The authority's round end into the next mission: the teardown's arms and
// the router (see the head of this file). The rotation is the role's
// (HostRole::rotation()); with none the session ends. For a next mission the
// embedder stages the next boot's config from its kept session and the cfg
// block as it stands (host_config.h stage_next_mission), so an edit to the
// block between the maps reaches the next one, every host alike.
MapChangeStep begin_host_map_change(HostRole &role,
		const std::vector<mission_catalog::Row> &catalog);

// The round init over the session's active slots on the mission's World, which every
// authority mission start runs after the PreMission pass, a fresh session's (its own slot)
// and a map change's (the kept slots) alike: auto-balance when it is due, the previous-mode
// word, the objective and non-team coercion (team 2 -> 1), and a new hidden entity for every
// active slot, a remote one with the slot's class (D-NET-354, D-NET-1233).
// [orig: Server_InitAllPlayerEntitiesForRound @0x516AA0 (the call @0x525BAF)]
void init_all_player_entities_for_round(NapiNPServerCtx &ctx);

// The teardown's Attack-and-Defend arm (the map change runs it on an
// in-session authority with the launch option set): every live slot takes a
// nonzero restore byte as its team, then swaps 1 <-> 2, with the side-to-team
// map; a SETNEXT latch at a first half's end skips the swap and sets the toggle.
// [orig: Game_TeardownMission @0x522435..0x522520]
void swap_sides_or_latch(NapiNPServerCtx &ctx, HostRotation &rotation);

// Each mission start's per-slot reset on an in-session authority, its
// spectator leg (D-NET-591): a spectator-latched slot whose restore byte
// (`spectator_restore_team`) is nonzero leaves the latch and, when its team is
// 0, takes the byte as its team; every slot's byte then clears. The host's
// map-change bring-up runs it; a fresh session's table has no latch to clear.
// [orig: Server_ResetAllPlayerSlotFrameState @0x4FC320, called by
//  Game_StartMission @0x5248BC behind `cmp is_in_session` @0x5248AC and
//  `cmp is_authority` @0x5248B4]
void Server_ResetAllPlayerSlotFrameState(NapiNPServerCtx &ctx);

// The teardown's pool destroy drops each carrier's flag (MissionKernel::destroy_mission_pools);
// on an in-session authority each drop's S2C 0x2F is queued right there, to every in-match slot,
// ahead of the teardown's pump (D-NET-822). Single player (out of a session) sends none.
// [orig: Entity_Destroy @0x43E810 -> Entity_DropCarriedObject @0x43E8B8; Entity_DropCarriedObject
//  @0x439DF0 - `cmp is_authority` @0x439F23 (jz @0x439F29), `cmp is_in_session` @0x439F2B (jz
//  @0x439F31), Server_SendDestructibleDeathPacket called @0x439F34 (mask 0x80 @0x50D922)]
void route_teardown_drops(NapiNPServerCtx &ctx, world::World &world);

// A kept slot's per-mission state back to a fresh mission's, around what the
// map change keeps (its identity, team and kit). [orig:
// Server_DisconnectAndResetAllPlayerSlots @0x516160, the active slot's stores
// @0x5163B2..0x516417]
void reset_slot_for_next_mission(NapiNPConnection &conn);

// The load end's S2C 0x7B to every net player with a slot, each its own
// summary. [orig: Server_BroadcastPlayerInfoToAll @0x50B310, the call
// @0x526267 behind the is_authority test @0x52625F]
void Server_BroadcastPlayerInfoToAll(NapiNPServerCtx &ctx);

} // namespace opennova::inmatch
