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
//     on the live HostRole: the swap or the latch, the PostMission pass, the
//     slot reset (each kept connection loses its per-mission state and its
//     entity, keeps its identity, its keys, its sequencing and its team), the
//     half toggle and the advance. NextMission: the embedder boots the next
//     map through the one host boot (inmatch/host_boot.h) with
//     HostBootRequest::next_mission; RotationEnded: the embedder closes the
//     session (HostRole::close: StopServer's goodbye).
//   the boot's bring-up (HostRole::bring_up with HostBringup::next_mission)
//     continues the kept session on the new World (host_session.h
//     continue_host_session).
//   after the kernel boot's PreMission pass and the round counters' reset,
//     the round init over the kept slots
//     (HostRole::init_round_after_premission -> init_all_player_entities_for_round).
//   the boot's phase B ends with the load-end S2C 0x7B to every slot.
// The host pumps its socket at the load's witnessed points (the 0x25s after
// the teardown, the rebuilt streams, the load end), so a joiner reloading in
// place is answered from its kept slot.

#include <base/io/crt_rand.h>
#include <runtime/mission/mission_catalog.h>

#include <cstdint>
#include <vector>

namespace opennova::gamecfg {
struct GameCfg;
}

namespace opennova::inmatch {

class HostRole;
struct HostScreenState;
struct NapiNPServerCtx;

// What the map change carries on the HostRole between its steps.
struct HostMapChange {
	// The teardown ran on the current kernel: its entities are gone and its
	// PostMission pass ran, so the role's close skips both.
	bool torn_down = false;
	// The next bring-up continues the session instead of starting one.
	bool pending = false;
	// The process CRT stream: retail seeds it once per session (at the slot
	// table's allocation), so the next mission's World continues it.
	// [orig: Server_AllocatePlayerSlotTable @0x51C1AA, srand's only session
	//  caller]
	io::CrtRand crt_rand;
};

enum class MapChangeStep : uint8_t {
	NextMission,   // the rotation names the next map; boot it inside the session
	RotationEnded, // no next mission: the session ends (StopServer) and the menu
};

// The authority's round end into the next mission: the teardown's arms and
// the router (see the head of this file). The rotation is the role's
// (HostRole::rotation()); with none the session ends. For a next mission,
// a host that keeps the game.cfg block (`cfg_block`, opennova-serve's) has
// its session settings re-applied from it into `session`, the settings the
// embedder builds the next boot's config from, so an edit to the block
// between the maps reaches the next one; a host with no block (the game's,
// which seeds its session from the host screen) passes null and keeps its
// own. [orig: Game_StartMission -> Game_ApplySessionSettingsToGlobals
// @0x524662, the apply host_config.h's host_session_settings ports]
MapChangeStep begin_host_map_change(HostRole &role,
		const std::vector<mission_catalog::Row> &catalog, gamecfg::GameCfg *cfg_block = nullptr,
		HostScreenState *session = nullptr);

// The round init over the kept slots on the next mission's World, which a
// map change runs after the PreMission pass: auto-balance when it is due, the
// previous-mode word, the objective and non-team coercion (team 2 -> 1), and
// a new entity for every active slot.
// [orig: Server_InitAllPlayerEntitiesForRound @0x516AA0 (the call @0x525BAF)]
void init_all_player_entities_for_round(HostRole &role);

// The load end's S2C 0x7B to every net player with a slot, each its own
// summary. [orig: Server_BroadcastPlayerInfoToAll @0x50B310, the call
// @0x526267 behind the is_authority test @0x52625F]
void Server_BroadcastPlayerInfoToAll(NapiNPServerCtx &ctx);

} // namespace opennova::inmatch
