#include "netsim/connection_fan.h"

#include <cstddef>
#include <utility>
#include <vector>

#include <novaworld/ingame_decode.h> // decode_entity_packet_sub_header / decode_player_extended_uplink
#include <novaworld/ingame_encode.h> // FrameUpdate / network_compress_fixedpoint / encode_frame_update
#include <world/geom.h>              // to_fixed

namespace opennova::netsim {

namespace {

// The per-frame S2C 0x0A field-driven §5.9 frame: a 12-byte position anchor + one tag=1 compact record
// per replicated entity, each position compressed relative to the anchor (network_compress_fixedpoint).
// Lifted into netsim from the retired replication_min build_tag_0a_world_reference (P8); the wire bytes
// are unchanged (decode_frame_update round-trips them). [orig: NapiNPClientMsg_0x00A @0x42FEC0 /
// NetPacket_SerializePlayerState case 1 @0x4C09C0]
std::vector<uint8_t> build_0a_frame(const PlayerReplicationState &ctx,
                                    const std::vector<GameEntitySnapshot> &entities) {
	FrameUpdate fu;
	const int32_t ax = int32_t(ctx.spawn_x);
	const int32_t ay = int32_t(ctx.spawn_y);
	const int32_t az = int32_t(ctx.spawn_z);
	fu.anchor_x = ax;
	fu.anchor_y = ay;
	fu.anchor_z = az;
	fu.flags1 = 0x00;
	fu.flags2 = 0x00;          // sub-block 0 (aim) — the common gameplay frame
	fu.aim.present = true;     // local-player view left zeroed (not authored yet)
	fu.state_flag_byte = 0x00; // 7-byte tail: not mounted, full health, no extra state
	fu.mount_handle = 0xFFFF;
	fu.health = 100;
	fu.state_word = 0;

	for (const GameEntitySnapshot &e : entities) {
		const uint16_t cx = network_compress_fixedpoint(e.x - ax);
		const uint16_t cy = network_compress_fixedpoint(e.y - ay);
		const uint16_t cz = network_compress_fixedpoint(e.z - az);
		// Coarse wire heading from the engine BAM (D-NET-86): Player/Infantry carry the rounded high
		// byte (v+0x800000)>>24; Vehicle carries the rounded high i16 (v+0x8000)>>16.
		const uint8_t yaw_byte = uint8_t((uint32_t(e.euler_z) + 0x00800000u) >> 24);
		const int16_t yaw_bam16 = int16_t((uint32_t(e.euler_z) + 0x00008000u) >> 16);
		FrameUpdateRecord rec;
		rec.handle = uint16_t((uint16_t(e.pool) << 12) | (e.slot & 0x0FFFu));
		rec.type_id = e.type_id;
		rec.cls = e.entity_class;
		switch (e.entity_class) {
		case EntityClass::Player:
			rec.player.vehicle_handle = 0xFFFF;
			rec.player.pos_x_compressed = cx;
			rec.player.pos_y_compressed = cy;
			rec.player.pos_z_compressed = cz;
			rec.player.yaw_byte = yaw_byte;
			break;
		case EntityClass::Vehicle:
			rec.vehicle.parent_slot_handle = 0xFFFF;
			rec.vehicle.flags_byte = 0x00; // unmounted (world-relative position)
			rec.vehicle.pos_x_compressed = cx;
			rec.vehicle.pos_y_compressed = cy;
			rec.vehicle.pos_z_compressed = cz;
			rec.vehicle.euler_z = yaw_bam16;
			break;
		case EntityClass::Infantry:
			rec.infantry.vehicle_slot_handle = 0xFFFF;
			rec.infantry.pos_x_compressed = cx;
			rec.infantry.pos_y_compressed = cy;
			rec.infantry.pos_z_compressed = cz;
			rec.infantry.yaw_byte = yaw_byte;
			break;
		case EntityClass::Guided:
		case EntityClass::NoNetworkCallback:
		case EntityClass::Unknown:
		default:
			continue; // no production 0x0A compact body
		}
		fu.records.push_back(std::move(rec));
	}
	return encode_frame_update(fu);
}

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

} // namespace

// Drain + read-apply the queued C2S 0x0C player uplinks on one connection's transport. Takes the
// whole Connection (not a bare transport) so it can enforce the per-connection owner gate and
// null-checks the transport internally — symmetric with emit_connection_s2c.
void drain_connection_c2s(world::World &world, const Connection &conn) {
	if (conn.transport == nullptr) return;
	Datagram dg;
	while (conn.transport->host_recv(dg)) {
		// §5.10 player-input uplink only this increment (other in-match C2S tags TBD).
		if (dg.tag != 0x0C) continue;

		// 5-byte sub-header [u16 handle][u16 itemTypeId][u8 sub_op], then the 43-B body.
		std::size_t consumed = 0;
		EntityPacketSubHeader hdr;
		if (!decode_entity_packet_sub_header(dg.body.data(), dg.body.size(), hdr, consumed))
			continue;
		if (hdr.sub_op != 0x0A) continue; // 0x0A=extended (type 10); 0x0B compact = later

		// [D-NET-119] Owner gate: a connection may only SNAP its OWN entity. The original resolves
		// the wire handle (pool<<12|slot) to an entity and verifies `entity == *owner_ctx` (the
		// connection's authorized entity) before invoking the +356 read-apply callback; a handle
		// naming any other entity is silently ignored — no apply, rejection, or disconnect (returns
		// 0 @0x4d6b7e). An invalid owner (the host's own loopback, or a pre-spawn joiner) matches
		// nothing, mirroring the original's `owner_ctx != null` guard @0x4d6ad3. [orig:
		// dispatch_entity_packet_callback @0x4D6A80 `entity == *owner_ctx` @0x4d6b08; owner_ctx <-
		// NapiNPServerMsg_0x00C @0x501c30 connCtx+0x160 -> +0xC0 -> *.]
		if (world::EntityHandle{hdr.handle} != conn.owned_entity) continue;

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

// Serialize the live world into one S2C 0x0A frame for `conn` and host_send it. anchor_for_connection
// is file-static; the per-connection emit body is shared by the legacy listen-server binding and
// npruntime's Server_TickUpdate fan over connection_list.
void emit_connection_s2c(const world::World &w, const Connection &conn,
                         const std::vector<GameEntitySnapshot> &ents,
                         const PlayerReplicationState &fallback_anchor) {
	if (conn.transport == nullptr) return;
	const PlayerReplicationState anchor = anchor_for_connection(w, conn, fallback_anchor);
	conn.transport->host_send(kTag0aFrameUpdate, build_0a_frame(anchor, ents));
}

} // namespace opennova::netsim
