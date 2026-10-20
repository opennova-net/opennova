#pragma once

#include <runtime/inmatch/napi_np_server_ctx.h> // NapiNPServerCtx

namespace opennova::world {
class World;
struct Entity;
struct RoundDeath;
}

// The host's player-death transaction and the death queue beside it (split
// off server_tick.cpp): the one retail transaction, which a player body's death
// edge raises inside the entity update (ServerEntityUpdateTransactions,
// napi_np_server_ctx.h), and the queue's drains at the tick's three places.
namespace opennova::inmatch {

// Empty: no death waits for the frame tail any more, since the player body's
// edge raises its transaction inside the entity update. host_session_pump
// still calls it after its flush.
void Server_RouteFrameTailEdges(NapiNPServerCtx &ctx);

// One transport-free RoundDeath through the one retail player-death
// transaction: 0x13 remote fan, victim 0x61 seed, victim 0x52 camera, optional
// 0x1E active-player feed, conditional 0x54 Medic state, scoring, and respawn
// holds. AI victims stop after the 0x13/scoring leg. That transaction is the
// organic body's alone: its callers are the player body, the infantry AI, the
// console kill and the C2S 0x4D team change. Vehicles and items keep only the
// score ledger and the SP tally here; their class death paths own Flags and
// the S2C 0x26 kill record. A player body's edge runs it inside the entity
// update, its sends staged for the next tick's head; the tick drains the other
// queued deaths through it; the team change runs it inline, as retail calls it
// from the handler.
// [orig: Entity_CheckAndProcessDeath @0x51B550, called only from
// Entity_UpdateInfantryPlayerBody @0x4B4CEA, Entity_UpdateInfantryAI @0x4B9D4D
// and the console kill @0x4D29EC -> GameEvent_PlayerDeath @0x516DD0, which
// NapiNPServerMsg_0x04D_ChangeTeam also calls directly @0x519059/@0x5190C7;
// the only 0x13 sends are GameEvent_PlayerDeath @0x516E8E and
// Entity_CheckAndProcessDeath @0x51B58F]
void Server_ProcessPlayerDeath(NapiNPServerCtx &ctx, world::World &world,
		const world::RoundDeath &death);

// Which drain: the tick's head (HoldAtHead) also releases the previous entity
// update's staged sends at their records' places and arms this tick's entity
// update with the session's transactions; the two later drains (Route, and
// AfterConverts after the receive pump's spectator converts) drain alone, as
// nothing is held for a player body's edge any more.
enum class EdgeDrain {
	Route,
	HoldAtHead,
	AfterConverts,
};

// Drain the queued deaths in order (the definition carries the witness).
void route_round_deaths(NapiNPServerCtx &ctx, world::World &world,
		EdgeDrain edges = EdgeDrain::Route);

// The flag reset's Health = -1 on a carrier's body, whose death edge in this
// frame's entity update raises the transaction.
void kill_player_body_at_frame_tail(world::World &world, world::Entity *player);

// Empty: no death waits for the frame tail. Server_TickUpdate still calls it
// ahead of its entity pass.
void arm_frame_tail_edges(world::World &world);

} // namespace opennova::inmatch
