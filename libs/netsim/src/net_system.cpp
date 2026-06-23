#include "netsim/net_system.h"

#include <cstddef>
#include <utility>
#include <vector>

#include <novaworld/ingame_decode.h> // decode_entity_packet_sub_header / decode_player_extended_uplink
#include <world/geom.h>              // to_fixed
#include <world/player_spawn.h>      // spawn_remote_player

namespace opennova::netsim {

namespace {

// The S2C 0x0A anchor for one connection: its owned entity's live position (so the compact
// records compress small deltas around that client's own player), or the passed fallback when
// the connection has no owned entity yet (the host's own loopback, or a pre-spawn joiner).
// [orig: the per-player send descriptor / 12-byte frame anchor = player.spawn_x/y/z, §5.2a.]
PlayerReplicationState anchor_for_connection(const world::World &w, const Connection &conn,
                                             const PlayerReplicationState &fallback) {
	if (!conn.owned_entity.valid()) return fallback;
	const world::Entity *e = w.registry.get(conn.owned_entity);
	if (e == nullptr) return fallback;
	PlayerReplicationState a = fallback; // keep the non-position fields
	a.spawn_x = static_cast<uint32_t>(world::to_fixed(e->position.x));
	a.spawn_y = static_cast<uint32_t>(world::to_fixed(e->position.y));
	a.spawn_z = static_cast<uint32_t>(world::to_fixed(e->position.z));
	return a;
}

// Drain + read-apply the queued C2S 0x0C player uplinks on one connection's transport.
void drain_connection_c2s(world::World &world, ISessionTransport &transport) {
	Datagram dg;
	while (transport.host_recv(dg)) {
		// §5.10 player-input uplink only this increment (other in-match C2S tags TBD).
		if (dg.tag != 0x0C) continue;

		// 5-byte sub-header [u16 handle][u16 itemTypeId][u8 sub_op], then the 43-B body.
		std::size_t consumed = 0;
		EntityPacketSubHeader hdr;
		if (!decode_entity_packet_sub_header(dg.body.data(), dg.body.size(), hdr, consumed))
			continue;
		if (hdr.sub_op != 0x0A) continue; // 0x0A=extended (type 10); 0x0B compact = later

		PlayerExtendedUplink up;
		std::size_t body_consumed = 0;
		if (!decode_player_extended_uplink(dg.body.data() + consumed, dg.body.size() - consumed,
		                                   up, body_consumed))
			continue;

		PlayerIntent intent;
		intent.entity_handle = hdr.handle;
		intent.item_type_id = hdr.item_type_id;
		intent.vehicle_handle = up.vehicle_handle;
		intent.pos_x = up.pos_x;
		intent.pos_y = up.pos_y;
		intent.pos_z = up.pos_z;
		intent.heading = up.heading;
		intent.pitch = up.pitch;
		intent.anim = up.anim_slot_low;
		intent.buttons = 0; // extended uplink carries flagsXor/anim-defs, not a buttons word
		apply_player_intent(world, intent);
	}
}

} // namespace

void NetSystem::tick(world::World &world, const world::TickContext &ctx) {
	// Drain queued C2S datagrams on EVERY connection and read-apply each player uplink to its
	// (remote peer) entity, BEFORE WAC/BMS/AI run [orig: net-before-logic, Game_ProcessMainFrame
	// @ 0x5263f0; PumpRecvQueues walks all connections, NapiNPConnection_ParseMessages @0x625BC0].
	// Only the host (authority) receives C2S — a joiner never drains one
	// [orig: dispatch_entity_packet_callback @0x4D6A80 gates on g_napi_np_ctx.is_authority].
	if (!ctx.is_authority) return;
	for (Connection &conn : connections_) {
		if (conn.transport == nullptr) continue;
		drain_connection_c2s(world, *conn.transport);
	}
}

void NetSystem::emit_s2c(const world::World &w, const PlayerReplicationState &fallback_anchor) {
	// Build the world snapshot ONCE, then fan a per-connection-anchored 0x0A to each connection
	// [orig: NapiNPServer_SendFiltered @0x4C87E0 builds once, SendToConn @0x4c4f20 per node].
	// No per-connection visibility cull this increment (broadcast the whole world to every
	// connection — a filter==1 send); the per-connection send_mask is present but unread.
	const std::vector<GameEntitySnapshot> ents = snapshot_world(w);
	for (const Connection &conn : connections_) {
		if (conn.transport == nullptr) continue;
		const PlayerReplicationState anchor = anchor_for_connection(w, conn, fallback_anchor);
		std::vector<uint8_t> body = build_tag_0a_world_reference(anchor, ents);
		conn.transport->host_send(kTag0aFrameUpdate, std::move(body));
	}
}

std::size_t NetSystem::add_connection(const Connection &c) {
	connections_.push_back(c);
	return connections_.size() - 1;
}

void NetSystem::clear_connections() { connections_.clear(); }

world::EntityHandle NetSystem::admit_peer(world::World &w, std::size_t conn_index,
                                          const world::PlayerSpawn &spawn) {
	if (conn_index >= connections_.size()) return world::EntityHandle{};
	// A remote peer's entity is NOT the host's own player: spawn_remote_player runs the same
	// faithful §5.2b sequence but leaves inf.is_local_player false and does NOT republish
	// World::cached.local_player — so apply_player_intent accepts (snaps) it and the motor
	// skips it once net-snapped. [orig: player_ServerAdd @0x51cbc0 registers a joined player's
	// entity without assigning g_local_player_entity.]
	const world::EntityHandle h = world::spawn_remote_player(w, spawn);
	if (h.valid()) connections_[conn_index].owned_entity = h;
	return h;
}

} // namespace opennova::netsim
