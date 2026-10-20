#pragma once

#include <runtime/inmatch/napi_np_server_ctx.h> // NapiNPServerCtx

#include <cstdint>
#include <vector>

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

// The transaction as a direct caller runs it on a slot's body (the C2S 0x4D
// team change, the admin console's kill, the admin KillPlayer): the killer the
// body's +0x178 as it stands, no tally, no Flags store (a caller that latches
// the dead bit does it after; KillPlayer does not, so the body's edge raises
// the transaction again), and the 0x13 word the body's live +0x2C0.
// [orig: NapiNPServerMsg_0x04D_ChangeTeam @0x519059 (Flags |= 2 @0x519061);
//  CAdminServer_HandlePlayer @0x404254 (Flags @0x40425E);
//  CNapiGameSession_HandleServerCommand @0x4D29EC -> Entity_CheckAndProcessDeath
//  @0x51B55F; GameEvent_PlayerDeath @0x516DD0 has no store to +0x24]
void Server_RunPlayerDeath(NapiNPServerCtx &ctx, world::World &world,
		world::EntityHandle victim);

// Which drain: the tick's head (HoldAtHead) also releases the previous entity
// update's staged sends at their records' places; the later drain (Route)
// drains alone, as nothing is held for a player body's edge any more.
// Server_TickUpdate arms the session's transactions around its entity pass.
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

// One send of a transaction: to the connection's transport, or, while the
// entity update runs the transaction (an EntityUpdateStaging is open), into its
// staged batch.
void send_entity_update_message(NapiNPServerCtx &ctx, NapiNPConnection &c, uint8_t tag,
		std::vector<uint8_t> body);

// The scope of one transaction the entity update runs inline: its sends land in
// one batch, which the scope's end queues behind a death-queue record naming it
// (RoundDeath::staged_sends, the victim's), at that place among the update's
// deaths; the next tick's head releases it there. Retail's update queues the
// sends after the frame's send pump, so they lead the next frame.
// [orig: Game_ProcessMainFrame -- the send pump @0x51E487 inside the
//  Server_TickUpdate call @0x5266B6, then Entity_UpdateAllEntities @0x52674B]
class EntityUpdateStaging {
public:
	EntityUpdateStaging(NapiNPServerCtx &ctx, world::World &world, world::EntityHandle victim);
	~EntityUpdateStaging();
	EntityUpdateStaging(const EntityUpdateStaging &) = delete;
	EntityUpdateStaging &operator=(const EntityUpdateStaging &) = delete;

private:
	NapiNPServerCtx &ctx_;
	world::World &world_;
	world::EntityHandle victim_;
	std::vector<StagedEntityUpdateSend> sends_;
};

} // namespace opennova::inmatch
