#include <runtime/inmatch/server_entity_state.h>

#include <runtime/devtools/tick_profile.h>
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
		const std::vector<GameEntitySnapshot> &ents, std::vector<uint8_t> &frame_out) {
	const devtools::ProfileScope fan_scope(
			world.profile, devtools::Slot::SIM_REPLICATION_FAN);
	conn.link.receive_silence_ms = conn.receive_inactive_ms;
	// The frame's CONTENT is the world as the caller found it. The server-fps and CPU
	// bytes are this tick's copies of the main loop's FR counter and CPU share
	// [orig: Server_TickUpdate @0x51D7E0..0x51D7E5, @0x51D7EA..0x51D7FB].
	return replication::build_connection_s2c(
			world, conn.link, ents, frame_out, ctx.config.game_type,
			conn.type == NapiNPConnection::kTypeServerSide ? kMaxFrameUpdateBodyBytes : 0,
			ctx.stats_avg_fps, ctx.stats_cpu_percent, ctx.np_protocol.identity);
}

bool Server_SendEntityStateToPlayer(NapiNPServerCtx &ctx, world::World &world,
		NapiNPConnection &conn, std::vector<uint8_t> &frame_out) {
	if (conn.link.spectator) restamp_spectator_body(world, conn);
	std::vector<GameEntitySnapshot> ents;
	if (entity_state_takes_records(world, conn)) ents = snapshot_entity_states(ctx, world);
	return write_entity_state(ctx, world, conn, ents, frame_out);
}

} // namespace opennova::inmatch
