// build_player_uplink (libs/netsim) is the JOINER-side inverse of apply_player_intent: it
// synthesizes the C2S 0x0C extended (type-10) uplink BODY from the joiner's own live
// local-player state. This proves the full joiner->host round-trip — build the uplink from a
// source pose, encode it (+ the 5-B sub-header), decode it, and read-apply it to the joiner's
// peer entity on a host World: the peer SNAPs to exactly the source pose (i.e. the joiner
// moves on the host). [orig: Player_BuildTag0CInputBody @0x42A550; inverse of
// NetPacket_SerializePlayerState case 4 @0x4c2042-0x4c20a9.]

#include "netsim/connection_fan.h"
#include "netsim/entity_wire_bridge.h"
#include "netsim/loopback_channel.h"

#include "conn_fan_test_util.h"

#include <novaworld/ingame_decode.h> // EntityPacketSubHeader / PlayerExtendedUplink
#include <novaworld/ingame_encode.h> // encode_entity_packet_sub_header / encode_player_extended_uplink
#include <world/ai.h>
#include <world/entity.h>
#include <world/geom.h>
#include <world/world.h>

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

namespace w = opennova::world;
namespace ns = opennova::netsim;
namespace nw = opennova;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

// The state->struct field mapping (the inverse of apply_player_intent's reads).
bool run_field_mapping() {
	w::Entity src_e{};
	src_e.net_move_input = 3; // the +0x12C movement-input byte (NOT the visual anim slot)
	w::AiEntity src_ae{};
	src_ae.pos[0] = w::to_fixed(12.5);
	src_ae.pos[1] = w::to_fixed(-34.0);
	src_ae.pos[2] = w::to_fixed(5.25);
	src_ae.heading = 0x20000000; // BAM32 with zero low-16 -> exact i16 round-trip
	src_ae.pitch = 0x01000000;

	const nw::PlayerExtendedUplink up = ns::build_player_uplink(src_e, src_ae);
	if (!expect(up.vehicle_handle == 0xFFFF, "on-foot vehicle handle")) return false;
	if (!expect(up.pos_x == src_ae.pos[0] && up.pos_y == src_ae.pos[1] && up.pos_z == src_ae.pos[2],
	            "pos = live AiEntity.pos (16.16)")) return false;
	if (!expect(up.heading == static_cast<int16_t>(src_ae.heading >> 16),
	            "heading = BAM32 high half")) return false;
	if (!expect(up.pitch == static_cast<int16_t>(src_ae.pitch >> 16),
	            "pitch = BAM32 high half")) return false;
	if (!expect(up.move_input_byte == 3, "movement-input byte carried from Entity.net_move_input"))
		return false;
	if (!expect(up.weapon_id_0 == 0 && up.fire_counter_0 == 0,
	            "anti-cheat counters 0 (host receive ignores them)")) return false;
	return true;
}

// build -> encode -> decode -> host read-apply (drain_connection_c2s): the host peer SNAPs to the source pose.
bool run_roundtrip_to_host_snap() {
	// Source: the joiner's live local-player pose (zero low-16 heading/pitch for exact round-trip).
	w::Entity src_e{};
	src_e.anim_slot = -1;
	w::AiEntity src_ae{};
	const int32_t sx = w::to_fixed(100.0), sy = w::to_fixed(200.0), sz = w::to_fixed(-50.0);
	src_ae.pos[0] = sx;
	src_ae.pos[1] = sy;
	src_ae.pos[2] = sz;
	src_ae.heading = 0x20000000; // i16 high 0x2000 -> mission yaw 45
	src_ae.pitch = 0x01000000;
	const nw::PlayerExtendedUplink up = ns::build_player_uplink(src_e, src_ae);

	// Host World with the joiner's peer entity at handle H.
	w::World world;
	world.registry.configure_pool(0, 8);
	w::Entity peer;
	peer.kind = w::EntityKind::Organic;
	peer.item_id = 0x14B9; // player infantry template
	peer.position = {0.0f, 0.0f, 0.0f};
	const w::EntityHandle ph = world.registry.spawn(0, peer);
	if (!expect(ph.valid(), "host peer spawned")) return false;
	w::AiSystem ai;
	world.ai = &ai;
	ai.attach(ph);
	world.cached.local_player = w::EntityHandle::make(0, 7); // a DIFFERENT handle is the host's own

	// Encode the full 0x0C payload (5-B sub-header at handle H + 43-B uplink body).
	nw::EntityPacketSubHeader sub;
	sub.handle = ph.packed;
	sub.item_type_id = 0x14B9;
	sub.sub_op = 0x0A;
	std::vector<uint8_t> payload = nw::encode_entity_packet_sub_header(sub);
	const std::vector<uint8_t> body = nw::encode_player_extended_uplink(up);
	payload.insert(payload.end(), body.begin(), body.end());

	// Drain through the host's authority C2S read-apply.
	ns::LoopbackChannel channel;
	channel.client_send(0x0C, payload);
	std::vector<ns::Connection> conns;
	conns.push_back(ns::Connection{&channel, ns::TransportMode::Loopback, ph, 0}); // the connection owns
	                                     // peer ph — the owner gate (D-NET-119) requires the uplink
	                                     // handle to match conn.owned_entity
	ns::test::drain_all(world, conns, /*is_authority=*/true);

	const w::Entity *pe = world.registry.get(ph);
	if (!expect(pe != nullptr, "host peer present")) return false;
	if (!expect(pe->position.x == static_cast<float>(w::from_fixed(sx)) &&
	            pe->position.y == static_cast<float>(w::from_fixed(sy)) &&
	            pe->position.z == static_cast<float>(w::from_fixed(sz)),
	            "host peer snapped to the joiner's source position")) return false;
	if (!expect(pe->yaw == 45, "host peer yaw = 45 (from the joiner's heading)")) return false;
	const w::AiEntity *ae = ai.for_handle(ph);
	if (!expect(ae != nullptr, "host peer has an AiEntity")) return false;
	if (!expect(ae->pos[0] == sx && ae->pos[1] == sy && ae->pos[2] == sz,
	            "host peer AiEntity live pos = source (16.16 exact)")) return false;
	if (!expect(ae->heading == src_ae.heading && ae->pitch == src_ae.pitch,
	            "host peer heading/pitch round-trip exactly (zero low-16 source)")) return false;
	if (!expect(ae->net_is_remote_peer, "host peer marked net-snapped")) return false;
	return true;
}

} // namespace

int main() {
	bool ok = true;
	ok = run_field_mapping() && ok;
	ok = run_roundtrip_to_host_snap() && ok;
	return ok ? 0 : 1;
}
