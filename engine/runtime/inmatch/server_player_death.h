#pragma once

#include <runtime/inmatch/napi_np_server_ctx.h> // NapiNPServerCtx

namespace opennova::world {
class World;
struct Entity;
struct RoundDeath;
}

// The host's player-death transaction and the death queue that feeds it (split
// off server_tick.cpp): the one retail transaction, the queue's drains at the
// tick's three places, and the player-body edges the frame tail raises.
namespace opennova::inmatch {

// The frame tail after the host's S2C flush: retail's entity update follows
// Server_TickUpdate's send pump and precedes the next receive pump, so a
// player body's death edge on a Health store the tick made (the flag reset's
// -1, RoundDeath::frame_tail_edge), tested ahead of this frame's entity pass,
// raises its transaction here once that pass ran; its sends lead the next
// frame. host_session_pump calls it after its flush.
void Server_RouteFrameTailEdges(NapiNPServerCtx &ctx);

// One transport-free RoundDeath through the one retail player-death
// transaction: 0x13 remote fan, victim 0x61 seed, victim 0x52 camera, optional
// 0x1E active-player feed, conditional 0x54 Medic state, scoring, and respawn
// holds. AI victims stop after the 0x13/scoring leg. That transaction is the
// organic body's alone: its callers are the player body, the infantry AI, the
// console kill and the C2S 0x4D team change. Vehicles and items keep only the
// score ledger and the SP tally here; their class death paths own Flags and
// the S2C 0x26 kill record. The tick drains the queued deaths through it; the
// team change runs it inline, as retail calls it from the handler.
// [orig: Entity_CheckAndProcessDeath @0x51B550, called only from
// Entity_UpdateInfantryPlayerBody @0x4B4CEA, Entity_UpdateInfantryAI @0x4B9D4D
// and the console kill @0x4D29EC -> GameEvent_PlayerDeath @0x516DD0, which
// NapiNPServerMsg_0x04D_ChangeTeam also calls directly @0x519059/@0x5190C7;
// the only 0x13 sends are GameEvent_PlayerDeath @0x516E8E and
// Entity_CheckAndProcessDeath @0x51B58F]
void Server_ProcessPlayerDeath(NapiNPServerCtx &ctx, world::World &world,
		const world::RoundDeath &death);

// How a drain treats the receive pump's player-body edges (the C2S 0x41's
// queued deaths, RoundDeath::player_edge).
enum class EdgeDrain {
	Route,         // re-test the edge, route the record if it still runs
	HoldAtHead,    // re-test the edge, drop the record if it no longer runs, hold it otherwise
	AfterConverts, // route a held record unless its body is hidden or dead by now
};

// Drain the queued deaths in order, each through Server_ProcessPlayerDeath,
// treating a player-body edge's record as `edges` says (the definition carries
// the witness).
void route_round_deaths(NapiNPServerCtx &ctx, world::World &world,
		EdgeDrain edges = EdgeDrain::Route);

// The flag reset's Health = -1 on a carrier's body; a body still alive queues
// the edge's death for the host's frame tail (RoundDeath::frame_tail_edge).
void kill_player_body_at_frame_tail(world::World &world, world::Entity *player);

// The frame tail's player-body edges as this frame's entity update meets them,
// tested ahead of the pass: a record whose edge is shut is dropped.
void arm_frame_tail_edges(world::World &world);

} // namespace opennova::inmatch
