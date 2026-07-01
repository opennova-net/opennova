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
// Lifted into netsim from the retired build_tag_0a_world_reference (P8); the wire bytes
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
	// Sub-block 1 (server-status/timer), NOT sub-block 0 (aim): the golden cycles flags2 1/2/3 and
	// never sends the aim sub-block in gameplay (the local player's aim is client-authoritative).
	// Sub-block 1 is LOAD-BEARING — it carries the client's fall-damage tolerance dword_C6EAE4. Left at
	// its BSS default 0, the body motor's landing-impact check `velZ <= C6EAE4 * -1057` has threshold 0,
	// so the per-frame micro-gravity velocity trips fall damage EVERY grounded frame -> constant
	// screen-red + camera-shake + minimap-red (Player_OnDamageReceived) though the player never dies
	// (health loss is authority-gated). Send the retail default 13. [orig: Entity_UpdateInfantryPlayerBody
	// landing check @0x4b7cf4-0x4b7d2d; NapiNPClientMsg_0x00A sub-block-1 read @0x4301a1-0x4301bc;
	// defaults @0x4f638b C6EAE0=20/C6EAE4=13; grill 2026-06-28]. (env sub-block 2 / objective 3 cycling is
	// a follow-up — the client keeps its mission-loaded env meanwhile.)
	fu.flags2 = 0x01;            // sub-block 1: [u8 C6EAE0][u8 C6EAE4][u8 serverFps][u8 serverCpuPct][i16 timer]
	fu.timer.present = true;
	fu.timer.state0 = 20;        // dword_C6EAE0 (retail default)
	fu.timer.state1 = 13;        // dword_C6EAE4 = fall-damage tolerance (retail default; 0 => constant fall dmg)
	fu.timer.state2 = 62;        // g_serverFps (cosmetic netgraph)
	fu.timer.state3 = 0;         // g_serverCpuPct (cosmetic netgraph)
	fu.timer.timer_seconds = -1; // dword_24C1958 = -1 -> no round time limit
	fu.state_flag_byte = 0x00; // 7-byte tail: not mounted, no stance bits yet (crouch/prone echo TBD)
	fu.mount_handle = 0xFFFF;
	// TAIL health (v121) -> g_local_player_entity->Health [orig: @0x4305df]. Send the player's healthMax
	// so the HUD reads 100% and the tail decrease-detector (`if v121 < Health` -> a brief damage flash
	// [orig: @0x43059a]) cannot self-trigger from an under-max tail. NOTE: this is only the minor health
	// flash — the CONSTANT screen-red/shake/minimap-red was the fall-damage tolerance C6EAE4 (the
	// flags2=1 sub-block above), NOT the tail. The per-entity record health byte is a DON'T-CARE for the
	// LOCAL player (NetPacket_SerializePlayerState skips the health-byte apply for g_local_player_entity
	// [orig: @0x4c11ac]). class-8 healthMax=150; real damage-driven health is a follow-up (thread the
	// connection's live health here).
	constexpr int kPlayerHealthMax = 150;
	fu.health = kPlayerHealthMax;
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
		// The authoritative wire handle carried off the registry Entity (snapshot_of), not
		// re-derived here — EntityRegistry stays the one source of handle truth. Equals the
		// former (pool<<12 | slot) reconstruction by construction, so the bytes are unchanged.
		rec.handle = e.wire_handle;
		rec.type_id = e.type_id;
		rec.cls = e.entity_class;
		switch (e.entity_class) {
		case EntityClass::Player:
			rec.player.vehicle_handle = 0xFFFF;
			rec.player.pos_x_compressed = cx;
			rec.player.pos_y_compressed = cy;
			rec.player.pos_z_compressed = cz;
			rec.player.yaw_byte = yaw_byte;
			// §5.10 health-classification byte (field 17 -> Entity_SetHealthFromDifficultyByte). A
			// living player MUST replicate non-zero or the client marks its own player dead and the
			// C2S 0x0C move uplink (Player_BuildTag0CInputBody, gated on entity->Health != 0) never
			// fires — i.e. the joiner spawns but cannot move. Send the entity's health clamped to a
			// byte (dead -> 0). The exact difficulty-byte classification is a follow-up grill of
			// Entity_SetHealthFromDifficultyByte @0x4AD580; non-zero is the deploy/move requirement.
			rec.player.health_class_byte =
					e.health > 0 ? static_cast<uint8_t>(e.health < 255 ? e.health : 255) : 0;
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
