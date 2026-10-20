#include <runtime/inmatch/server_entity_state.h>

#include <runtime/devtools/tick_profile.h>
#include <net/npwire/ingame_encode.h>                   // encode_entity_remove
#include <net/npwire/ingame_message_id.h>               // s2c::ENTITY_REMOVE
#include <runtime/inmatch/game_config.h>               // kMaxFrameUpdateBodyBytes
#include <runtime/replication/connection_fan.h>         // build_connection_s2c
#include <runtime/replication/entity_wire_bridge.h>     // snapshot_world
#include <runtime/world/collision.h>                    // stable replication LOS view epoch
#include <runtime/world/world.h>

namespace opennova::inmatch {

void restamp_spectator_body(world::World &world, const NapiNPConnection &conn) {
	world::Entity *body = world.registry.get(conn.link.owned_entity);
	if (body == nullptr) return;
	body->flags |= 1u;          // @0x4FF71B
	body->damage_state = -1;    // @0x4FF71E
	body->health = 1;           // @0x4FF728
}

namespace {

// The writer's stores on the slot's body, in its order: the hide bit cleared;
// a spectator slot's restamp; while the pre-round timer runs, the dead-bit
// clear; then the header's re-set of the hide bit for a held slot (a
// spectator's repeats the restamp's). The +96481 arm between the clear and the
// restamp has no writer on this host. The cleared dead bit lives only in this
// 0x0A: an unhidden body at Health <= 0 latches it again in the frame's entity
// update, whose edge re-runs the death on the authority.
// [orig: NetPacket_WritePlayerState @0x4FF6B0 - the hide clear @0x4FF6D7, the
//  +96481 arm @0x4FF6DA..0x4FF6F5, the restamp @0x4FF6FA..0x4FF728,
//  `cmp g_PreRoundDelayTimer, 0` @0x4FF72F and the clear @0x4FF738, the
//  re-sets @0x4FF7A1 and (+89912 & 0x10) @0x4FF7B8;
//  Entity_UpdateInfantryPlayerBody @0x4B4BF1..0x4B4CEA]
void write_player_state_body_stores(world::World &world, const NapiNPConnection &conn,
		world::Entity &body) {
	body.flags &= ~world::kEntityFlagCarried;
	if (conn.link.spectator) restamp_spectator_body(world, conn);
	if (world.preround_delay_seconds != 0) {
		body.flags &= ~world::kEntityFlagDead;
		body.engine_flags &= ~world::kEntityFlagDead;
	}
	if (conn.link.respawn_pending) body.flags |= world::kEntityFlagCarried;
}

} // namespace

bool entity_state_takes_records(const world::World &world, const NapiNPConnection &conn) {
	return !(world.cached.local_player.valid() &&
			conn.link.owned_entity == world.cached.local_player);
}

std::vector<GameEntitySnapshot> snapshot_entity_states(const NapiNPServerCtx &ctx,
		world::World &world) {
	// The previous tick's movement/destruction is settled. Replication LOS
	// can retain each target's final section matrices across every entity
	// and recipient; never inherit a view built while the motor is moving.
	{
		const devtools::ProfileScope prep_scope(
				world.profile, devtools::Slot::SIM_REPLICATION_QUERY_PREP);
		if (world.collision != nullptr)
			world.collision->prepare_cached_raycast_queries(world);
	}
	const devtools::ProfileScope snapshot_scope(
			world.profile, devtools::Slot::SIM_REPLICATION_SNAPSHOT);
	std::vector<GameEntitySnapshot> ents = replication::snapshot_world(world);
	// Stamp owner-hidden rows: a spectator connection's own player entity
	// is admitted only to its owner's list (the JO reduction of the
	// slot+97536 hide byte — see replication_model.h owner_hidden)
	// [orig: Server_BuildEntityPriorityList @0x50e6fd].
	for (const NapiNPConnection &oc : ctx.np_protocol.connection_list) {
		if (!oc.link.spectator || !oc.link.owned_entity.valid()) continue;
		for (GameEntitySnapshot &es : ents) {
			if (es.wire_handle == oc.link.owned_entity.packed) {
				es.owner_hidden = true;
				break;
			}
		}
	}
	return ents;
}

bool write_entity_state(NapiNPServerCtx &ctx, world::World &world, NapiNPConnection &conn,
		std::vector<GameEntitySnapshot> &ents, std::vector<uint8_t> &frame_out) {
	const devtools::ProfileScope fan_scope(
			world.profile, devtools::Slot::SIM_REPLICATION_FAN);
	if (conn.link.transport == nullptr ||
			replication::owned_entity_for_emit(world, conn.link) == nullptr)
		return false;
	world::Entity &body = *world.registry.get(conn.link.owned_entity);
	conn.link.receive_silence_ms = conn.receive_inactive_ms;
	// The list (inside the builder) reads this body's row as `ents` holds it,
	// before the writer's stores; the header and this body's own record read the
	// body after them [orig: Server_SendEntityStateToPlayer - the list @0x517C1E,
	// the writer @0x517C76, the records @0x517C81].
	write_player_state_body_stores(world, conn, body);
	// The frame's CONTENT is the world as the caller found it. The server-fps and CPU
	// bytes are this tick's copies of the main loop's FR counter and CPU share
	// [orig: Server_TickUpdate @0x51D7E0..0x51D7E5, @0x51D7EA..0x51D7FB].
	std::vector<uint16_t> despawned;
	const bool built = replication::build_connection_s2c(
			world, conn.link, ents, frame_out, ctx.config.game_type,
			conn.type == NapiNPConnection::kTypeServerSide ? kMaxFrameUpdateBodyBytes : 0,
			ctx.stats_avg_fps, ctx.stats_cpu_percent, ctx.np_protocol.identity, &despawned);
	// The list's tracked-slot despawns, each an S2C 0x12 to this slot alone,
	// sent once (userParam 1: the queue's one-send lifetime), queued ahead of
	// its 0x0A [orig: Server_BuildEntityPriorityList @0x50E7F3..0x50E835 (pool 1
	//  @0x50ECE0), inside the list @0x517C1E; NapiNPServer_SendToConn passes the
	//  userParam as the message's user_param1 @0x4C4F52].
	for (const uint16_t handle : despawned) {
		EntityRemove removal;
		removal.entity_handle = handle;
		conn.link.transport->host_send(s2c::ENTITY_REMOVE, encode_entity_remove(removal),
				/*reliable=*/false);
	}
	// The slots written after this one read this body as the stores left it.
	for (GameEntitySnapshot &row : ents) {
		if (row.wire_handle != body.handle.packed) continue;
		row.state_flags = static_cast<uint8_t>(body.flags & 0xFF);
		row.health = body.health;
		break;
	}
	return built;
}

bool Server_SendEntityStateToPlayer(NapiNPServerCtx &ctx, world::World &world,
		NapiNPConnection &conn, std::vector<uint8_t> &frame_out) {
	std::vector<GameEntitySnapshot> ents;
	if (entity_state_takes_records(world, conn)) ents = snapshot_entity_states(ctx, world);
	return write_entity_state(ctx, world, conn, ents, frame_out);
}

} // namespace opennova::inmatch
