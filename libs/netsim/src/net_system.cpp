#include "netsim/net_system.h"

#include <cstddef>
#include <utility>
#include <vector>

#include <novaworld/ingame_decode.h> // decode_entity_packet_sub_header / decode_player_extended_uplink

namespace opennova::netsim {

void NetSystem::tick(world::World &world, const world::TickContext &ctx) {
	// Drain queued C2S datagrams and read-apply each player uplink to its (remote peer)
	// entity, BEFORE WAC/BMS/AI run [orig: net-before-logic, Game_ProcessMainFrame
	// @ 0x5263f0]. Only the host (authority) receives C2S — a joiner never drains one
	// [orig: dispatch_entity_packet_callback @0x4D6A80 gates on g_napi_np_ctx.is_authority].
	if (!ctx.is_authority) return;

	Datagram dg;
	while (channel_.host_recv(dg)) {
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

void NetSystem::emit_s2c(const world::World &w, const PlayerReplicationState &anchor) {
	std::vector<GameEntitySnapshot> ents = snapshot_world(w);
	std::vector<uint8_t> body = build_tag_0a_world_reference(anchor, ents);
	channel_.host_send(kTag0aFrameUpdate, std::move(body));
}

} // namespace opennova::netsim
