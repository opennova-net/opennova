#pragma once

// THE SERVER-STATUS PAGE FEED: the authority's player-slot roster and host
// statistics, read off the host's server context and its world for the page
// (hud/hud_server_status.h). The roster is the connection list the port
// keeps as retail's player-slot table: a connection holding a player slot is
// that slot's row; the listen host's own loopback connection is the local
// slot (+5). The main loop's frame statistics reach the context through the
// session (HostRole::observe_frame_statistics); the gametext strings and the
// score-list flag are the embedder's.
// [orig: Server_DrawStatusScreen @0x50a2d0 reads g_PlayerSlots,
//  g_PlayerSlotCapacity, dword_24D211C, g_NapiNPCtx (+0x50, +0x64, +0x1194,
//  +0x11A8), g_ServerNameStr, g_GameType, the round tallies, g_TeamRecords,
//  g_RoundTimeRemaining, g_PreRoundDelayTimer]

#include <runtime/hud/hud_server_status.h>

namespace opennova::world {
class World;
}

namespace opennova::inmatch {

struct NapiNPServerCtx;

// Fill `page`'s roster, host facts and (for a host with no client of its
// own) console rows from the host context (and its world, null for a
// world-less context: no entity, stats or clock facts). Leaves the text and
// the score-list flag to the caller.
void fill_server_status_page(hud::ServerStatusPageState &page, const NapiNPServerCtx &ctx,
		const world::World *world);

} // namespace opennova::inmatch
