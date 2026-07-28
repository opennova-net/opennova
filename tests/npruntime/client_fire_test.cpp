// D-NET-152 — the C2S 0x06 client-fired-round pipeline. The dispatch case must validate the
// shooter (anti-spoof vs the connection's own entity [orig: @0x51358d]), enforce the clip for
// armory-known primary fire ("NO AMMO!" reject [orig: @0x50c15c]; decrement [orig:
// consume_weapon_ammo @0x540913]), mirror the equipped weapon [orig: @0x50bd56] + the claimed
// target [orig: @0x50c2ad], and append ONE g_round_ring event carrying the client's exact
// pre-spread fire pose [orig: RoundData_AddRound @0x4fdb40] — with NO reactive reply (the echo
// rides the per-frame 0x0A tag-2 fan, §5.9.1). The 0x25 relay refills the same slot's clip
// [orig: WeaponSlot_ReloadAmmo @0x541720 @0x514F03].
//
// Coverage: a valid primary fire (ring fields + clip 30->29 + mirrors + no replies/sends); a
// spoofed shooter handle and an unknown adm are dropped; alt fire appends without touching the
// clip; the host's own loopback 0x06 is a net-path no-op [orig: @0x50c18d]; clip exhaustion
// rejects further fire until the 0x25 relay refills; a no-clip weapon (clipsize -1) never
// rejects; a table-less host accepts without bookkeeping.

#include <npruntime/napi_np_connection.h>
#include <npruntime/napi_np_protocol.h>
#include <npruntime/napi_np_server_ctx.h>
#include <npruntime/server_message_dispatch.h>

#include <netsim/connection.h>
#include <netsim/loopback_channel.h>
#include <netsim/session_transport.h>
#include <netsim/udp_session_transport.h>

#include <npwire/ingame_decode.h>
#include <npwire/nw_session_framing.h>
#include <npwire/protocol_message.h>
#include <npwire/session_keys.h>

#include <world/ai.h>
#include <world/player_spawn.h>
#include <world/world.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace opennova;
namespace np = opennova::np;
namespace ns = opennova::netsim;
namespace w = opennova::world;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

w::PlayerSpawn player_spawn(uint16_t net_id) {
	w::PlayerSpawn s;
	s.position = {0, 0, 0};
	s.net_id = net_id;
	return s;
}

np::NapiNPConnection make_conn(uint32_t id, int type, ns::ISessionTransport *t,
                               ns::TransportMode mode, w::EntityHandle owned, bool spawned) {
	np::NapiNPConnection c;
	c.connection_id = id;
	c.type = type;
	c.link.transport = t;
	c.link.mode = mode;
	c.link.owned_entity = owned;
	c.burst.spawned = spawned;
	c.spawned_announced = spawned;
	c.phase = spawned ? np::ConnectionPhase::InMatch : np::ConnectionPhase::New;
	return c;
}

void put_u16(std::vector<uint8_t> &b, uint16_t v) {
	b.push_back(uint8_t(v & 0xFF));
	b.push_back(uint8_t(v >> 8));
}
void put_u32(std::vector<uint8_t> &b, uint32_t v) {
	b.push_back(uint8_t(v & 0xFF));
	b.push_back(uint8_t((v >> 8) & 0xFF));
	b.push_back(uint8_t((v >> 16) & 0xFF));
	b.push_back(uint8_t((v >> 24) & 0xFF));
}

// Craft the fixed 45-B C2S 0x06 body (§5.16 field order).
std::vector<uint8_t> fire_body(uint16_t shooter, uint8_t fire_flags, uint8_t adm,
                               int32_t px, int32_t py, int32_t pz, int32_t dx, int32_t dy,
                               uint16_t target, uint16_t hit_part, uint8_t extra2,
                               uint8_t misc) {
	std::vector<uint8_t> b;
	put_u32(b, 12345);            // current_tick
	put_u16(b, shooter);
	b.push_back(fire_flags);
	b.push_back(adm);
	put_u32(b, uint32_t(px));
	put_u32(b, uint32_t(py));
	put_u32(b, uint32_t(pz));
	put_u32(b, uint32_t(dx));
	put_u32(b, uint32_t(dy));
	put_u16(b, target);
	put_u16(b, hit_part);
	b.push_back(0x07);            // extra_byte1
	b.push_back(extra2);          // extra_byte2 -> ring subtype composite
	b.push_back(misc);            // misc_byte -> ring slot_byte
	put_u16(b, 0);                // delta_x
	put_u16(b, 0);                // delta_y
	put_u16(b, 0);                // delta_z
	put_u16(b, 0);                // delta_yaw
	put_u16(b, 0);                // delta_pitch
	return b;
}

int dispatch_fire(np::NapiNPConnection &conn, std::vector<np::NapiNPConnection> &roster,
                  w::World &world, const std::vector<uint8_t> &body) {
	std::vector<ProtocolMessage> msgs;
	msgs.push_back(make_protocol_message(0x06, body));
	std::vector<ProtocolMessage> replies =
			np::dispatch_session_replies(np::GameConfig{}, conn, msgs, 100, roster, &world);
	return int(replies.size());
}

// Exercise the actual 0x43 session receive boundary, not the message dispatcher in isolation.
// Retail's HandleSessionPacket drops seq <= recv_ack_seq before ParseMessages, so replaying the
// identical UDP datagram must neither append another round nor spend another cartridge.
bool check_duplicate_c2s_session_does_not_refire() {
	w::World world;
	world.registry.configure_pool(0, 8);
	w::AiSystem ai;
	world.ai = &ai;
	const w::EntityHandle shooter = w::spawn_remote_player(world, player_spawn(0xFFF1));
	if (!expect(shooter.valid(), "session replay shooter spawned")) return false;

	world.weapons.entries.resize(6);
	w::WeaponTableEntry &rifle = world.weapons.entries[5];
	rifle.name = "WPN_TESTRIFLE";
	rifle.category = 3;
	rifle.rank = 2;
	rifle.clipsize = 30;
	rifle.ammo_index = 0;
	rifle.valid = true;
	world.ammo.entries.resize(1);
	world.ammo.entries[0].name = "REMOTE_POWER_THROW";
	world.ammo.entries[0].velocity = 620;
	world.ammo.entries[0].max_age_ticks = 248;
	world.ammo.entries[0].valid = true;

	const PeerAddr peer{0x0100007Fu, 30123};
	const std::string client_scrk = "CLIENT-REPLAY-SCRK";
	const std::string server_scrk = "SERVER-REPLAY-SCRK";
	np::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	np::NapiNPConnection conn =
			make_conn(3, 1, nullptr, ns::TransportMode::Client, shooter, true);
	conn.peer = peer;
	conn.client_scrk = client_scrk;
	conn.server_scrk = server_scrk;
	conn.client_ck = 0x11223344u;
	conn.server_sk = 0x55667788u;
	ctx.np_protocol.connection_list.push_back(std::move(conn));

	const std::vector<uint8_t> shot =
			fire_body(shooter.packed, 0x22, 5, 0, 0, 0, 0, 0, 0xFFFF, 1, 12, 0);
	SessionSequencing client_tx{1, 0};
	std::vector<uint8_t> session_body;
	if (!expect(frame_session_packet(client_tx, SessionCrypto{client_scrk, {}, 0x55667788u},
	                                 {make_protocol_message(0x06, shot)}, session_body),
	            "frame C2S 0x06 session packet"))
		return false;
	const std::vector<uint8_t> fire_datagram =
			nw_encode_outbound(SESSION_OPCODE_PROTOCOL_MESSAGE, std::move(session_body));

	np::handle_server_datagram(ctx, peer, fire_datagram.data(), fire_datagram.size(), 100);
	if (!expect(world.rounds.count == 1, "first C2S 0x06 appends one authoritative round"))
		return false;
	const uint16_t combo = uint16_t(3 * 65 + 2);
	if (!expect(ctx.np_protocol.connection_list[0].weapon_slots[combo].clip == 29,
	            "first C2S 0x06 spends one cartridge"))
		return false;

	np::handle_server_datagram(ctx, peer, fire_datagram.data(), fire_datagram.size(), 101);
	if (!expect(world.rounds.count == 1, "exact duplicate C2S 0x06 does not append a second round"))
		return false;
	if (!expect(ctx.np_protocol.connection_list[0].weapon_slots[combo].clip == 29,
	            "exact duplicate C2S 0x06 does not spend a second cartridge"))
		return false;

	std::vector<uint8_t> heartbeat_body;
	if (!expect(frame_session_packet(client_tx, SessionCrypto{client_scrk, {}, 0x55667788u},
	                                 {make_protocol_message(0x34, {})}, heartbeat_body),
	            "frame next contiguous C2S session packet"))
		return false;
	const std::vector<uint8_t> heartbeat_datagram =
			nw_encode_outbound(SESSION_OPCODE_PROTOCOL_MESSAGE, std::move(heartbeat_body));
	np::handle_server_datagram(ctx, peer, heartbeat_datagram.data(), heartbeat_datagram.size(), 102);
	np::handle_server_datagram(ctx, peer, fire_datagram.data(), fire_datagram.size(), 103);
	if (!expect(ctx.np_protocol.connection_list[0].seq.last_inbound_seq == 2,
	            "replayed older C2S packet cannot regress the host ACK latch"))
		return false;

	std::vector<uint8_t> ack_datagram;
	if (!expect(np::frame_in_match_s2c(ctx, peer, 0x34, {}, ack_datagram),
	            "host frames an ACK-bearing S2C packet"))
		return false;
	uint8_t opcode = 0;
	std::vector<uint8_t> ack_body;
	ProtocolPacketHeader ack_hdr;
	std::vector<ProtocolMessage> ack_messages;
	if (!expect(nw_decode_inbound(ack_datagram.data(), ack_datagram.size(), opcode, ack_body) &&
	                    opcode == SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE &&
	                    decode_protocol_packet_plaintext(ack_body.data(), ack_body.size(), server_scrk,
	                                                     ack_hdr, ack_messages),
	            "decode host ACK-bearing S2C packet"))
		return false;
	return expect(ack_hdr.ack_count == 2, "host echoes the highest contiguous C2S sequence");
}

} // namespace

int main() {
	if (!check_duplicate_c2s_session_does_not_refire()) return 1;

	w::World world;
	world.registry.configure_pool(0, 16);
	w::AiSystem ai;
	world.ai = &ai;
	const w::EntityHandle ha = w::spawn_player(world, player_spawn(0xFFF0));        // host
	const w::EntityHandle hb = w::spawn_remote_player(world, player_spawn(0xFFF1)); // shooter
	const w::EntityHandle hc = w::spawn_remote_player(world, player_spawn(0xFFF2)); // target
	if (!expect(ha.valid() && hb.valid() && hc.valid(), "three players spawned")) return 1;

	// Armory: adm 5 = a 30-round rifle (category 3, rank 2 -> combo 197); adm 7 = a
	// no-clip weapon (clipsize -1, the knife/medpack shape).
	world.weapons.entries.resize(8);
	{
		w::WeaponTableEntry &rifle = world.weapons.entries[5];
		rifle.name = "WPN_TESTRIFLE";
		rifle.category = 3;
		rifle.rank = 2;
		rifle.clipsize = 30;
		rifle.ammo_index = 0;
		rifle.valid = true;
		w::WeaponTableEntry &knife = world.weapons.entries[7];
		knife.name = "WPN_TESTKNIFE";
		knife.category = 1;
		knife.rank = 0;
		knife.clipsize = -1;
		knife.valid = true;
	}
	world.ammo.entries.resize(1);
	world.ammo.entries[0].name = "REMOTE_POWER_THROW";
	world.ammo.entries[0].velocity = 620;
	world.ammo.entries[0].max_age_ticks = 248;
	world.ammo.entries[0].valid = true;

	ns::LoopbackChannel loop;
	ns::UdpSessionTransport udp_b(ns::UdpSessionTransport::Role::Host);
	ns::UdpSessionTransport udp_c(ns::UdpSessionTransport::Role::Host);

	np::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	auto &roster = ctx.np_protocol.connection_list;
	roster.push_back(make_conn(1, 2, &loop, ns::TransportMode::Loopback, ha, true));
	roster.push_back(make_conn(3, 1, &udp_b, ns::TransportMode::Client, hb, true));
	roster.push_back(make_conn(4, 1, &udp_c, ns::TransportMode::Client, hc, true));

	// --- 1. Valid primary fire: ring append + clip decrement + mirrors, no replies. ---
	const std::vector<uint8_t> body =
			fire_body(hb.packed, /*flags=*/0x22, /*adm=*/5, 0x100000, 0x200000, 0x30000,
	                  /*dx=*/0x1234, /*dy=*/int32_t(0xFFFFAAAA), hc.packed,
	                  /*hit_part=*/1025, /*extra2=*/12, /*misc=*/64);
	if (!expect(body.size() == 45, "crafted 0x06 body is 45 B"))
		return 1;
	int nreplies = dispatch_fire(roster[1], roster, world, body);
	if (!expect(nreplies == 0, "0x06 draws NO reactive reply")) return 1;
	if (!expect(world.rounds.count == 1, "one ring event appended")) return 1;
	{
		const w::RoundEvent &ev = world.rounds.records[0];
		if (!expect(ev.shooter_handle == hb.packed, "ring shooter = the connection's entity"))
			return 1;
		if (!expect(ev.origin_x == 0x100000 && ev.origin_y == 0x200000 && ev.origin_z == 0x30000,
		            "ring origin = the claimed fire origin"))
			return 1;
		if (!expect(ev.dir_yaw == int32_t(0x1234u << 16) &&
		                    ev.dir_pitch == int32_t(0xAAAAu << 16),
		            "ring direction = the raw dir words << 16"))
			return 1;
		if (!expect(ev.shot_seq == 1025, "ring shot_seq = the uplink hit_part")) return 1;
		if (!expect(ev.mode_flags == 0x22, "ring mode = the fire-flags byte")) return 1;
		if (!expect(ev.subtype == 12, "ring subtype = extra_byte2 composite")) return 1;
		if (!expect(ev.slot_byte == 64, "ring slot byte = PowerThrow charge")) return 1;
		if (!expect(ev.adm_index == 5, "ring adm index")) return 1;
	}
	{
		const uint16_t combo = 3 * 65 + 2;
		auto it = roster[1].weapon_slots.find(combo);
		if (!expect(it != roster[1].weapon_slots.end(), "weapon slot bound by combo")) return 1;
		if (!expect(it->second.clip == 29, "clip 30 -> 29 after one primary fire")) return 1;
	}
	{
		const w::LiveRound &round = world.round_sim.rounds[0];
		const float speed =
				std::sqrt(round.vel.x * round.vel.x + round.vel.y * round.vel.y +
				          round.vel.z * round.vel.z);
		if (!expect(std::fabs(speed - (620.0f / 62.0f) * (64.0f / 256.0f)) < 0.01f,
		            "remote PowerThrow charge scales the authoritative round"))
			return 1;
	}
	{
		const w::Entity *sh = world.registry.get(hb);
		if (!expect(sh != nullptr && sh->equipped_adm_index == 5,
		            "equipped adm mirrored [orig: entity+688]"))
			return 1;
		if (!expect(sh->last_fire_target == hc, "claimed target stamped on the shooter"))
			return 1;
	}
	{
		std::vector<uint8_t> raw;
		ns::Datagram dg;
		if (!expect(!udp_b.pop_outbound(raw) && !udp_c.pop_outbound(raw) && !loop.client_recv(dg),
		            "no direct sends from the 0x06 dispatch"))
			return 1;
	}

	// --- 2. Spoofed shooter (C's handle on B's connection) -> dropped. ---
	dispatch_fire(roster[1], roster, world,
	              fire_body(hc.packed, 0x22, 5, 0, 0, 0, 0, 0, 0xFFFF, 1, 12, 0));
	if (!expect(world.rounds.count == 1, "spoofed shooter handle dropped")) return 1;

	// --- 3. Unknown adm on an armory-fed host -> dropped [orig: NULL wpn -3]. ---
	dispatch_fire(roster[1], roster, world,
	              fire_body(hb.packed, 0x22, 6, 0, 0, 0, 0, 0, 0xFFFF, 2, 12, 0));
	if (!expect(world.rounds.count == 1, "unknown adm dropped")) return 1;

	// --- 4. Alt fire appends WITHOUT touching the clip [orig: @0x50bb0d/@0x50be2b]. ---
	dispatch_fire(roster[1], roster, world,
	              fire_body(hb.packed, 0x23, 5, 0, 0, 0, 0, 0, 0xFFFF, 3, 12, 0));
	if (!expect(world.rounds.count == 2, "alt fire appended")) return 1;
	if (!expect(roster[1].weapon_slots[uint16_t(3 * 65 + 2)].clip == 29,
	            "alt fire leaves the clip untouched"))
		return 1;

	// --- 5. The host's own loopback 0x06 is a no-op [orig: @0x50c18d]. ---
	dispatch_fire(roster[0], roster, world,
	              fire_body(ha.packed, 0x22, 5, 0, 0, 0, 0, 0, 0xFFFF, 4, 12, 0));
	if (!expect(world.rounds.count == 2, "loopback fire ignored on the net path")) return 1;

	// --- 6. Clip exhaustion rejects; the 0x25 relay refills [orig: @0x541720]. ---
	roster[1].weapon_slots[uint16_t(3 * 65 + 2)].clip = 1;
	dispatch_fire(roster[1], roster, world,
	              fire_body(hb.packed, 0x22, 5, 0, 0, 0, 0, 0, 0xFFFF, 5, 12, 0));
	if (!expect(world.rounds.count == 3, "last round fires")) return 1;
	dispatch_fire(roster[1], roster, world,
	              fire_body(hb.packed, 0x22, 5, 0, 0, 0, 0, 0, 0xFFFF, 6, 12, 0));
	if (!expect(world.rounds.count == 3, "empty clip rejects fire (NO AMMO)")) return 1;
	{
		// C2S 0x25 [u16 handle][u16 combo] from B -> relay + host-side refill.
		std::vector<uint8_t> reload;
		put_u16(reload, hb.packed);
		put_u16(reload, uint16_t(3 * 65 + 2));
		std::vector<ProtocolMessage> msgs;
		msgs.push_back(make_protocol_message(0x25, reload));
		np::dispatch_session_replies(np::GameConfig{}, roster[1], msgs, 101, roster, &world);
		// Drain the relayed 0x49s so later checks stay clean.
		std::vector<uint8_t> raw;
		ns::Datagram dg;
		while (udp_b.pop_outbound(raw)) {}
		while (udp_c.pop_outbound(raw)) {}
		while (loop.client_recv(dg)) {}
	}
	if (!expect(roster[1].weapon_slots[uint16_t(3 * 65 + 2)].clip == 30,
	            "0x25 relay refilled the clip to capacity"))
		return 1;
	dispatch_fire(roster[1], roster, world,
	              fire_body(hb.packed, 0x22, 5, 0, 0, 0, 0, 0, 0xFFFF, 7, 12, 0));
	if (!expect(world.rounds.count == 4, "fire works again after the reload")) return 1;

	// --- 7. A no-clip weapon (clipsize -1) never ammo-rejects [orig: adm+88 == -1]. ---
	for (int i = 0; i < 3; ++i)
		dispatch_fire(roster[1], roster, world,
		              fire_body(hb.packed, 0x12, 7, 0, 0, 0, 0, 0, 0xFFFF, uint16_t(10 + i),
		                        12, 0));
	if (!expect(world.rounds.count == 7, "no-clip weapon fires freely")) return 1;

	// --- 8. Table-less host accepts without bookkeeping (the 0x5A echo fallback shape). ---
	world.weapons.entries.clear();
	dispatch_fire(roster[1], roster, world,
	              fire_body(hb.packed, 0x22, 9, 0, 0, 0, 0, 0, 0xFFFF, 20, 12, 0));
	if (!expect(world.rounds.count == 8, "table-less host accepts the fire")) return 1;

	std::printf("OK\n");
	return 0;
}
