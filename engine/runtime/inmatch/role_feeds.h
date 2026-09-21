#pragma once

// THE ROLE FEEDS (ADR 0040 ladder E8): the client-side presentation facts that
// read a running role's state — the end-of-round session state, overlay input
// and stat rows, the friendly-tags gather, and the DEATH screen's status.
// Each reads the ONE replica view every role folds (the listen host's own
// loopback included) plus the authority's session context where retail reads
// its server globals; the embedder only marshals the result into its records.
// They live in runtime/inmatch because runtime/world is net-agnostic and may
// not read ClientState.

#include <runtime/hud/end_round_overlay.h>
#include <runtime/hud/game_text_lookup.h>
#include <runtime/inmatch/stat_screen_feed.h>
#include <runtime/world/deploy_screen_feed.h>
#include <runtime/world/friendly_tags.h>
#include <runtime/world/spawn_select.h>

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::mission {
class MissionKernel;
}

namespace opennova::inmatch {

class ClientRuntime;
struct NapiNPServerCtx;

// What a feed reads of the running role.
struct RoleView {
	// Non-const: the friendly-tags world walk stamps its per-entity scratch.
	mission::MissionKernel *kernel = nullptr;
	// The role's replica runtime: the HostClient's on the authority, the
	// joiner's own; null on the bare local role and a dedicated host.
	const ClientRuntime *runtime = nullptr;
	// The authority's session context; null on a joiner and the bare local role.
	const NapiNPServerCtx *host = nullptr;
	bool joiner = false;
	// The host option word the embedder staged (GameConfig::mp_attributes) —
	// the session rules word of every role that is not a joiner.
	uint32_t staged_mp_attributes = 0;
};

// The client-local death-screen latch: the 0x0A flags1 bit-0 edges every
// role's view folds (the listen host's own loopback included)
// [orig: g_death_screen_active, NapiNPClientMsg_0x00A @0x42ff88..0x43002b].
bool local_death_screen_active(const RoleView &view);

// The one role-agnostic read of the local player's dead bit: the joiner's
// replica, the authority's entity flags (the kernel).
bool local_player_dead(const RoleView &view);

// [orig: the S2C 0x1D landing NapiNPClientMsg_0x01D @0x430840 —
//  g_spawn_success_gate, g_endround_winner_team, g_scoreTeamScore0/1,
//  g_endround_draw_flag, dword_A81B2C = GetTickCount; the 0x56 board
//  completion.] Role-agnostic: every role's view folds both lanes.
EndRoundSessionState end_round_session_state(const RoleView &view);

// The overlay ladder's input off the folded 0x1D header and the round clock.
hud::EndRoundOverlayInput end_round_overlay_input(const RoleView &view);

// The stat.mnu RESULTLIST rows the tab filter admits (0 all, 1 team 2, 2 team
// 1): the roster joined to the frozen board, the local row resolved from the
// header's board index [orig: populate_stat_results_list @0x562240 — row slot
// store @0x562576, board join @0x5624F3, selection compare @0x56272E].
std::vector<StatScreenRow> end_round_rows(const RoleView &view, int tab);

// The friendly-tags gather (D-HUD-20): the world walk with the role's pass
// facts, plus a joiner's roster walk. False when there is no local player
// (nothing gathered). The witnessed pass is cited at world/friendly_tags.
bool collect_friendly_tags(const RoleView &view, std::vector<world::FriendlyTagSource> &out);

// The DEATH screen's STATIC facts for THIS client [orig: the client globals
// UI_UpdateDeathScreenContent @0x5536a0 reads — dword_A85B5C / A85B60 / A85B68
// from the 0x0A sub-block 0, word_A85BC0 + entity+538/548 from the 0x6E fold].
// `zones` is the mission's spawn-zone list, `medic_key_label` the MedicReq
// binding's display string the embedder resolves.
world::DeployScreenStatus deploy_screen_status(const RoleView &view,
		const world::SpawnZoneRegistry &zones, const std::string &medic_key_label,
		const hud::GameTextLookup &gametext);

} // namespace opennova::inmatch
