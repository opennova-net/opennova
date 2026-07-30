// The co-op-LAN fan-out guard (Increment A).
//
// One authoritative host World, ONE per-frame world snapshot, fanned to a CONNECTION TABLE:
// the host's own client (a LoopbackChannel, transport mode 1) plus a joiner (a socket-free
// UdpSessionTransport, mode 2). This proves the netsim core ports the original's per-connection
// fan [orig: NapiNPServer_SendFiltered @0x4C87E0 -> SendToConn @0x4c4f20] and per-connection
// receive drain [orig: PumpRecvQueues / NapiNPConnection_ParseMessages @0x625BC0] WITHOUT any
// Godot or OS sockets — the harness carries raw datagrams between the two transports the way a
// real UDP socket pair would.
//
// Asserts: (a) one emit_s2c fans the SAME frame to both connections (byte-identical when both
// share an anchor) AND each connection's frame is anchored to ITS OWN owned entity; (b) a
// joiner's C2S 0x0C uplink drains through its connection and SNAPs its remote-peer entity; (c)
// a 0x0C for the host's own player is drained but REJECTED (the §5.38a host-SNAP split).

#include "netsim/connection.h"
#include "netsim/connection_fan.h"
#include "netsim/entity_wire_bridge.h"
#include "netsim/loopback_channel.h"
#include "netsim/net_client_view.h"
#include "netsim/session_transport.h"
#include "netsim/udp_session_transport.h"

#include "conn_fan_test_util.h"

#include <npwire/ingame_decode.h> // EntityPacketSubHeader / PlayerExtendedUplink
#include <npwire/ingame_message_id.h>
#include <npwire/ingame_encode.h> // network_compress_fixedpoint, encode_* uplink
#include <world/ai.h>                // AiSystem / AiEntity (engine-frame mirror)
#include <world/entity.h>
#include <world/geom.h>
#include <world/player_spawn.h> // spawn_player / spawn_remote_player
#include <world/vehicle_attach.h> // entity_process_vehicle_attach / detach (0x26/0x27)
#include <world/world.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

namespace nw = opennova;
namespace ns = opennova::netsim;
namespace w = opennova::world;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

// The exact value the host's encoder + codec produce for one axis: compress the (wire - anchor)
// delta, decompress it, add the anchor back. The client must land on precisely this.
int32_t codec_recon(int32_t wire, int32_t anchor) {
	return anchor +
	       nw::network_decompress_fixedpoint(nw::network_compress_fixedpoint(wire - anchor));
}

// Build a 48-byte C2S 0x0C extended uplink (5-B sub-header + 43-B body) from the encoders.
// carrier != 0xFFFF makes x/y/z + heading CARRIER-LOCAL (§5.10 grounded form, D-NET-151).
std::vector<uint8_t> make_0c_uplink(uint16_t handle, int32_t x, int32_t y, int32_t z,
                                    int16_t heading, int16_t pitch,
                                    uint16_t carrier = 0xFFFF, uint8_t state_flags = 0) {
	nw::EntityPacketSubHeader hdr;
	hdr.handle = handle;
	hdr.item_type_id = 0x14B9; // player infantry
	hdr.sub_op = 0x0A;         // extended (type 10)
	nw::PlayerExtendedUplink up;
	up.carrier_handle = carrier;
	up.pos_x = x;
	up.pos_y = y;
	up.pos_z = z;
	up.heading = heading;
	up.pitch = pitch;
	up.state_flags_byte = state_flags;
	std::vector<uint8_t> body = nw::encode_entity_packet_sub_header(hdr);
	const std::vector<uint8_t> tail = nw::encode_player_extended_uplink(up);
	body.insert(body.end(), tail.begin(), tail.end());
	return body;
}

// Carry every raw datagram one endpoint has staged to send into the other endpoint's inbound —
// the harness's stand-in for the UDP socket pair (sendto on one side = recvfrom on the other).
void carry(ns::UdpSessionTransport &from, ns::UdpSessionTransport &to) {
	std::vector<uint8_t> raw;
	while (from.pop_outbound(raw)) to.push_inbound(raw);
}

w::PlayerSpawn player_spawn(w::Vec3 pos, int16_t yaw, uint16_t net_id,
                            uint16_t min_entity_slot = 0) {
	w::PlayerSpawn s;
	s.position = pos;
	s.yaw = yaw;
	s.net_id = net_id;
	s.min_entity_slot = min_entity_slot;
	return s;
}

// (a) One emit fans to BOTH connections; same-anchor frames are byte-identical, and each
//     connection's frame is anchored to its OWN owned entity (per-connection anchoring).
bool run_fanout_and_per_connection_anchor() {
	w::World world;
	world.registry.configure_pool(0, 16);
	w::AiSystem ai;
	world.ai = &ai;

	// The host's own player (publishes cached.local_player). Anchor subject for conn_self.
	const w::EntityHandle host_h =
			w::spawn_player(world, player_spawn({5.0f, 10.0f, -3.0f}, 0, 0xFFF0));
	if (!expect(host_h.valid(), "host player spawned")) return false;
	// The 0x0A tail carries the RECIPIENT's live health; the byte-identity sub-case below
	// needs the owned-entity tail (host, live) to equal the no-entity default (150).
	world.registry.get(host_h)->health = 150;

	std::vector<ns::Connection> conns; // the host's connection table
	ns::LoopbackChannel self_ch;
	ns::UdpSessionTransport udp_host(ns::UdpSessionTransport::Role::Host);
	conns.push_back(ns::Connection{&self_ch, ns::TransportMode::Loopback, host_h, 0});
	const std::size_t conn_self = 0;
	conns.push_back(ns::Connection{&udp_host, ns::TransportMode::Client, {}, 0});
	const std::size_t conn_join = 1;
	if (!expect(conns.size() == 2, "two connections registered")) return false;
	(void)conn_self;

	// Fallback anchor = the host player position (what NovaSimulation::compute_net_anchor builds).
	nw::PlayerReplicationState fallback;
	fallback.spawn_x = static_cast<uint32_t>(w::to_fixed(5.0));
	fallback.spawn_y = static_cast<uint32_t>(w::to_fixed(10.0));
	fallback.spawn_z = static_cast<uint32_t>(w::to_fixed(-3.0));

	// --- sub-case a1: byte-identity. conn_join still has NO owned entity, so it rides the
	//     fallback anchor = the host position = conn_self's anchor. Both frames identical. ---
	ns::test::emit_all(world, conns, fallback);
	if (!expect(self_ch.s2c_pending() == 1, "loopback got one S2C frame")) return false;
	if (!expect(udp_host.outbound_pending() == 1, "udp got one S2C raw datagram")) return false;

	ns::Datagram self_dg;
	if (!expect(self_ch.client_recv(self_dg), "loopback S2C dequeued")) return false;
	std::vector<uint8_t> raw;
	if (!expect(udp_host.pop_outbound(raw), "udp S2C dequeued")) return false;
	// Identity framing: raw = [tag][body...].
	if (!expect(raw.size() == self_dg.body.size() + 1, "udp raw = tag + body length")) return false;
	if (!expect(raw[0] == self_dg.tag, "udp tag prefix == loopback tag")) return false;
	bool bodies_equal = true;
	for (std::size_t i = 0; i < self_dg.body.size(); ++i)
		bodies_equal = bodies_equal && (raw[i + 1] == self_dg.body[i]);
	if (!expect(bodies_equal, "fan-out bodies byte-identical (same anchor)")) return false;

	// --- sub-case a2: per-connection anchor. Admit the joiner -> conn_join now owns joiner_h
	//     and anchors to ITS position; conn_self stays anchored to host_h. ---
	const w::EntityHandle joiner_h =
			ns::test::admit_peer(world, conns, conn_join, player_spawn({50.0f, 60.0f, -20.0f}, 90, 0xFFF1));
	if (!expect(joiner_h.valid() && joiner_h != host_h, "joiner spawned, distinct handle"))
		return false;
	if (!expect(world.cached.local_player == host_h,
	            "admit_peer did NOT steal local_player from the host")) return false;

	ns::test::emit_all(world, conns, fallback);
	ns::UdpSessionTransport udp_join(ns::UdpSessionTransport::Role::Client);
	carry(udp_host, udp_join);

	ns::NetClientView self_view, join_view;
	self_view.pump(self_ch);
	join_view.pump(udp_join);
	if (!expect(self_view.frames_applied() == 1 && join_view.frames_applied() == 1,
	            "each view applied one frame")) return false;
	if (!expect(self_view.state().entities.size() == 2 && join_view.state().entities.size() == 2,
	            "each view decoded both players")) return false;

	const uint16_t host_handle = host_h.packed;
	const uint16_t joiner_handle = joiner_h.packed;
	const int32_t hx = w::to_fixed(5.0), hy = w::to_fixed(10.0), hz = w::to_fixed(-3.0);
	const int32_t jx = w::to_fixed(50.0), jy = w::to_fixed(60.0), jz = w::to_fixed(-20.0);

	// In the HOST's own view (anchored to the host), the HOST entity reconstructs exactly
	// (delta 0); the joiner reconstructs against the host anchor.
	const ns::ClientEntityState *sh = self_view.state().find(host_handle);
	const ns::ClientEntityState *sj = self_view.state().find(joiner_handle);
	if (!expect(sh != nullptr && sj != nullptr, "self view has both entities")) return false;
	if (!expect(sh->x == hx && sh->y == hy && sh->z == hz,
	            "self view: host entity exact (anchored to host)")) return false;
	if (!expect(sj->x == codec_recon(jx, hx) && sj->y == codec_recon(jy, hy) &&
	                    sj->z == codec_recon(jz, hz),
	            "self view: joiner reconstructed against the host anchor")) return false;

	// In the JOINER's view (anchored to the joiner), the JOINER entity reconstructs exactly.
	const ns::ClientEntityState *jh = join_view.state().find(host_handle);
	const ns::ClientEntityState *jj = join_view.state().find(joiner_handle);
	if (!expect(jh != nullptr && jj != nullptr, "join view has both entities")) return false;
	if (!expect(jj->x == jx && jj->y == jy && jj->z == jz,
	            "join view: joiner entity exact (anchored to joiner)")) return false;
	if (!expect(jh->x == codec_recon(hx, jx) && jh->y == codec_recon(hy, jy) &&
	                    jh->z == codec_recon(hz, jz),
	            "join view: host reconstructed against the joiner anchor")) return false;

	// The proof of PER-connection anchoring: each view reconstructs its OWN player exactly, which
	// is only possible if the two frames carried different anchors.
	if (!expect(sh->x == hx && jj->x == jx,
	            "per-connection anchor: each view exact on its own player")) return false;
	return true;
}

// (b) A joiner's C2S 0x0C uplink drains through its connection and SNAPs its remote-peer entity;
//     the host's own player is untouched.
bool run_joiner_uplink_snaps_peer() {
	w::World world;
	world.registry.configure_pool(0, 16);
	w::AiSystem ai;
	world.ai = &ai;
	const w::EntityHandle host_h =
			w::spawn_player(world, player_spawn({0.0f, 0.0f, 0.0f}, 0, 0xFFF0));

	std::vector<ns::Connection> conns;
	ns::LoopbackChannel self_ch;
	ns::UdpSessionTransport udp_host(ns::UdpSessionTransport::Role::Host);
	conns.push_back(ns::Connection{&self_ch, ns::TransportMode::Loopback, host_h, 0});
	conns.push_back(ns::Connection{&udp_host, ns::TransportMode::Client, {}, 0});
	const std::size_t conn_join = 1;
	const w::EntityHandle joiner_h =
			ns::test::admit_peer(world, conns, conn_join, player_spawn({1.0f, 1.0f, 1.0f}, 0, 0xFFF1));
	if (!expect(joiner_h.valid(), "joiner admitted")) return false;

	// The joiner reports a new pose from its client endpoint.
	const int32_t wx = w::to_fixed(100.0), wy = w::to_fixed(200.0), wz = w::to_fixed(-50.0);
	const int16_t wheading = 0x2000; // -> mission yaw 45
	ns::UdpSessionTransport udp_join(ns::UdpSessionTransport::Role::Client);
	udp_join.client_send(0x0C, make_0c_uplink(joiner_h.packed, wx, wy, wz, wheading, 0x0100));
	carry(udp_join, udp_host);
	if (!expect(udp_host.inbound_pending() == 1, "joiner uplink reached the host endpoint"))
		return false;

	ns::test::drain_all(world, conns, /*is_authority=*/true);
	if (!expect(udp_host.inbound_pending() == 0, "host drained the joiner connection")) return false;

	// The joiner's registry entity SNAPPED to the wire pose.
	const w::Entity *je = world.registry.get(joiner_h);
	if (!expect(je != nullptr, "joiner entity present")) return false;
	if (!expect(je->position.x == static_cast<float>(w::from_fixed(wx)) &&
	                    je->position.y == static_cast<float>(w::from_fixed(wy)) &&
	                    je->position.z == static_cast<float>(w::from_fixed(wz)),
	            "joiner registry position snapped to the wire pose")) return false;
	if (!expect(je->yaw == 45, "joiner yaw = 90 - BAM/deg (== 45)")) return false;
	const w::AiEntity *jae = ai.for_handle(joiner_h);
	if (!expect(jae != nullptr && jae->net_is_remote_peer,
	            "joiner AiEntity marked net-snapped")) return false;

	// The host's own player was NOT touched by the drain.
	const w::Entity *he = world.registry.get(host_h);
	if (!expect(he->position.x == 0.0f && he->position.y == 0.0f && he->position.z == 0.0f,
	            "host player pose unchanged")) return false;
	const w::AiEntity *hae = ai.for_handle(host_h);
	if (!expect(hae != nullptr && !hae->net_is_remote_peer,
	            "host player not net-snapped")) return false;
	return true;
}

// (c) A 0x0C for the host's OWN player (over its loopback connection) is drained but REJECTED —
//     the host never read-applies its own player (§5.38a / ADR-0012).
bool run_self_uplink_rejected() {
	w::World world;
	world.registry.configure_pool(0, 16);
	w::AiSystem ai;
	world.ai = &ai;
	const w::EntityHandle host_h =
			w::spawn_player(world, player_spawn({7.0f, 8.0f, 9.0f}, 0, 0xFFF0));

	std::vector<ns::Connection> conns;
	ns::LoopbackChannel self_ch;
	conns.push_back(ns::Connection{&self_ch, ns::TransportMode::Loopback, host_h, 0});

	// A (malicious/echo) 0x0C naming the host's own player.
	self_ch.client_send(0x0C, make_0c_uplink(host_h.packed, w::to_fixed(999.0), 0, 0, 0x4000, 0));
	ns::test::drain_all(world, conns, /*is_authority=*/true);
	if (!expect(self_ch.c2s_pending() == 0, "self 0x0C drained even when rejected")) return false;

	const w::Entity *he = world.registry.get(host_h);
	if (!expect(he->position.x == 7.0f && he->position.y == 8.0f && he->position.z == 9.0f,
	            "host own-player pose NOT overwritten by a read-apply")) return false;
	const w::AiEntity *hae = ai.for_handle(host_h);
	if (!expect(hae != nullptr && !hae->net_is_remote_peer,
	            "host own-player not marked net-snapped")) return false;
	return true;
}

// (d) [D-NET-119] OWNER GATE: a connection may SNAP only its OWN entity. A 0x0C whose handle names a
//     DIFFERENT peer (spoofed or stale) is silently ignored — even though that handle resolves to a
//     live, non-local entity — and the gate ADMITS the same connection's uplink for its own entity, so
//     it discriminates by owner rather than rejecting unconditionally [orig:
//     dispatch_entity_packet_callback @0x4D6A80 `entity == *owner_ctx`].
bool run_cross_peer_uplink_rejected() {
	w::World world;
	world.registry.configure_pool(0, 16);
	w::AiSystem ai;
	world.ai = &ai;
	const w::EntityHandle peer_a =
			w::spawn_remote_player(world, player_spawn({1.0f, 2.0f, 3.0f}, 0, 0xFFF1));
	const w::EntityHandle peer_b =
			w::spawn_remote_player(world, player_spawn({4.0f, 5.0f, 6.0f}, 0, 0xFFF2));
	if (!expect(peer_a.valid() && peer_b.valid() && peer_a != peer_b, "two distinct peers spawned"))
		return false;

	// Connection A owns peer A. Its first uplink SPOOFS peer B's handle.
	std::vector<ns::Connection> conns;
	ns::UdpSessionTransport udp_a(ns::UdpSessionTransport::Role::Host);
	conns.push_back(ns::Connection{&udp_a, ns::TransportMode::Client, peer_a, 0});
	ns::UdpSessionTransport udp_a_client(ns::UdpSessionTransport::Role::Client);
	udp_a_client.client_send(0x0C, make_0c_uplink(peer_b.packed, w::to_fixed(999.0), 0, 0, 0x4000, 0));
	carry(udp_a_client, udp_a);

	ns::test::drain_all(world, conns, /*is_authority=*/true);
	if (!expect(udp_a.inbound_pending() == 0, "spoofed 0x0C drained even when rejected")) return false;

	// Peer B was NOT snapped by peer A's connection (owner gate), and peer A is untouched (its
	// uplink named B, not A) — neither entity moved.
	const w::Entity *be = world.registry.get(peer_b);
	if (!expect(be != nullptr && be->position.x == 4.0f && be->position.y == 5.0f &&
	                    be->position.z == 6.0f,
	            "peer B pose NOT overwritten by peer A's spoofed uplink")) return false;
	const w::Entity *ae = world.registry.get(peer_a);
	if (!expect(ae != nullptr && ae->position.x == 1.0f && ae->position.y == 2.0f &&
	                    ae->position.z == 3.0f,
	            "peer A pose unchanged (its uplink named B, not A)")) return false;
	const w::AiEntity *bae = ai.for_handle(peer_b);
	if (!expect(bae != nullptr && !bae->net_is_remote_peer, "peer B not net-snapped by the spoof"))
		return false;

	// Positive control: the SAME connection naming its OWN entity (A) DOES snap A — proving the gate
	// admits the owner, it does not reject every uplink.
	udp_a_client.client_send(0x0C, make_0c_uplink(peer_a.packed, w::to_fixed(100.0), 0, 0, 0x0000, 0));
	carry(udp_a_client, udp_a);
	ns::test::drain_all(world, conns, /*is_authority=*/true);
	const w::Entity *ae2 = world.registry.get(peer_a);
	if (!expect(ae2 != nullptr && ae2->position.x == 100.0f,
	            "peer A snapped by ITS OWN connection's uplink (owner gate admits the owner)"))
		return false;
	return true;
}

// Retail loads .bms-resident pool-0 organics before network players. The listen-server path
// therefore starts host/joiner player allocation at slot 4 so live 0x0A records do not collide
// with the retail client's local pool-0 slots 0..3.
bool run_retail_player_slots_start_after_bms_organics() {
	w::World world;
	world.registry.configure_pool(0, 16);
	w::AiSystem ai;
	world.ai = &ai;

	const w::EntityHandle host_h =
			w::spawn_player(world, player_spawn({0.0f, 0.0f, 0.0f}, 0, 0xFFF0, 4));
	if (!expect(host_h == w::EntityHandle::make(0, 4), "retail host player uses slot 4"))
		return false;

	std::vector<ns::Connection> conns;
	ns::LoopbackChannel self_ch;
	ns::UdpSessionTransport udp_host(ns::UdpSessionTransport::Role::Host);
	conns.push_back(ns::Connection{&self_ch, ns::TransportMode::Loopback, host_h, 0});
	conns.push_back(ns::Connection{&udp_host, ns::TransportMode::Client, {}, 0});
	const std::size_t conn_join = 1;
	const w::EntityHandle joiner_h =
			ns::test::admit_peer(world, conns, conn_join,
			                     player_spawn({10.0f, 0.0f, 0.0f}, 0, 0xFFF1, 4));
	if (!expect(joiner_h == w::EntityHandle::make(0, 5), "retail joiner player uses slot 5"))
		return false;

	nw::PlayerReplicationState fallback;
	fallback.spawn_x = static_cast<uint32_t>(w::to_fixed(0.0));
	fallback.spawn_y = static_cast<uint32_t>(w::to_fixed(0.0));
	fallback.spawn_z = static_cast<uint32_t>(w::to_fixed(0.0));
	ns::test::emit_all(world, conns, fallback);

	ns::UdpSessionTransport udp_join(ns::UdpSessionTransport::Role::Client);
	carry(udp_host, udp_join);
	ns::NetClientView join_view;
	join_view.pump(udp_join);
	if (!expect(join_view.frames_applied() == 1, "join view applied retail-slot frame"))
		return false;
	if (!expect(join_view.state().find(0x0004) != nullptr,
	            "live frame contains host player handle 0x0004")) return false;
	if (!expect(join_view.state().find(0x0005) != nullptr,
	            "live frame contains joiner player handle 0x0005")) return false;
	if (!expect(join_view.state().find(0x0000) == nullptr &&
	                    join_view.state().find(0x0001) == nullptr,
	            "live frame does not advertise player handles 0x0000/0x0001")) return false;
	return true;
}

// (f) The per-connection S2C 0x0A sub-block PHASE CYCLE — a faithful port of the original's
//     per-player-slot send counter [orig: ++playerSlot+100566 @0x517be8; NetPacket_WritePlayerState
//     writes it as flags2 and phase&3 selects the header sub-block @0x4ff6b0]. We cycle the SAFE
//     subset {1 server-status, 0 weapon, 3 gametype}; env (2) is deferred (host does not author
//     world.env). First send is phase 1 so the load-bearing fall-damage tolerance reaches the client
//     on frame 1.
bool run_0a_subblock_phase_cycle() {
	w::World world;
	world.registry.configure_pool(0, 8);
	w::AiSystem ai;
	world.ai = &ai;
	const w::EntityHandle host_h =
			w::spawn_player(world, player_spawn({1.0f, 2.0f, 3.0f}, 0, 0xFFF0));
	if (!expect(host_h.valid(), "host player spawned")) return false;

	std::vector<ns::Connection> conns;
	ns::LoopbackChannel ch;
	conns.push_back(ns::Connection{&ch, ns::TransportMode::Loopback, host_h, 0});

	nw::PlayerReplicationState fallback;
	fallback.spawn_x = static_cast<uint32_t>(w::to_fixed(1.0));
	fallback.spawn_y = static_cast<uint32_t>(w::to_fixed(2.0));
	fallback.spawn_z = static_cast<uint32_t>(w::to_fixed(3.0));

	// Two full cycles: status(1) -> weapon(0) -> gametype(3), repeating.
	const uint8_t want_flags2[6] = {1, 0, 3, 1, 0, 3};
	for (int i = 0; i < 6; ++i) {
		ns::test::emit_all(world, conns, fallback);
		ns::Datagram dg;
		if (!expect(ch.client_recv(dg), "0x0A frame dequeued")) return false;
		if (!expect(dg.tag == nw::s2c::PER_FRAME_UPDATE, "tag 0x0A")) return false;
		nw::FrameUpdate fu;
		if (!expect(nw::decode_frame_update(dg.body.data(), dg.body.size(), ns::class_for_type_id, fu),
		            "0x0A frame decodes as a well-formed frame")) return false;
		if (!expect(fu.flags2 == want_flags2[i], "flags2 cycles {1,0,3}")) return false;
		if ((want_flags2[i] & 3u) == 1) {
			if (!expect(fu.timer.present && fu.timer.state1 == 13,
			            "phase 1 = server-status carrying fall-damage tolerance 13")) return false;
		} else if ((want_flags2[i] & 3u) == 0) {
			if (!expect(fu.weapon.present, "phase 0 = weapon sub-block present")) return false;
			if (!expect(fu.weapon.uniform_team_mask == 0x8,
			            "phase 0 uniform team mask = 8 (golden steady value)")) return false;
		}
		// phase 3 (gametype) = 0 bytes for a non-objective gametype: nothing to assert.
	}
	// The connection's phase counter advanced once per send.
	if (!expect(conns[0].s2c_phase == 6, "phase counter advanced once per send")) return false;

	// Co-op's shared g_GameType (0x30020) turns phase 3 into a 16-byte
	// objective block. The gate is not encoded in flags2, so both fan and view
	// must receive the same session value.
	world.subgoals.won = 0x00000102u;
	world.subgoals.lost = 0x00000204u;
	world.subgoals.show_win = 0x00000408u;
	world.subgoals.show_lose = 0x00000810u;
	conns[0].s2c_phase = 2; // next safe-cycle entry is phase 3
	ns::test::emit_all(world, conns, fallback, 0x30020u);
	ns::Datagram objective_dg;
	if (!expect(ch.client_recv(objective_dg), "objective 0x0A frame dequeued")) return false;
	nw::FrameUpdate objective_fu;
	if (!expect(nw::decode_frame_update(objective_dg.body.data(), objective_dg.body.size(),
	                    ns::class_for_type_id, objective_fu,
	                    /*is_objective_gametype=*/true),
	            "objective 0x0A frame decodes with the session hint")) return false;
	if (!expect(objective_fu.flags2 == 3 && objective_fu.objective.present,
	            "co-op phase 3 carries the required 16-byte objective block")) return false;
	if (!expect(uint32_t(objective_fu.objective.state[0]) == world.subgoals.won &&
	                    uint32_t(objective_fu.objective.state[1]) == world.subgoals.lost &&
	                    uint32_t(objective_fu.objective.state[2]) == world.subgoals.show_win &&
	                    uint32_t(objective_fu.objective.state[3]) == world.subgoals.show_lose,
	            "phase 3 carries won/lost/show-win/show-lose in retail order")) return false;
	if (!expect(objective_fu.local_tail_present,
	            "objective bytes cannot be mistaken for the recipient health tail")) return false;
	std::printf("PASS 0a_subblock_phase_cycle\n");
	return true;
}

// (g) [D-NET-138] The §5.10 field-17 health-classification byte is PACKED
//     `(tier << 4) | (playerClass & 0xF)` — tier boundaries 49152 (0.75) / 28671 (0.4375) in
//     16.16 of health/healthMax [orig: Entity_GetHealthClassification @ 0x4AD4E0]. A raw-health
//     byte here mis-classes remote players every applied frame (the client apply @0x4AD580
//     writes the low nibble back to playerClass and re-resolves itemDef from it) — the
//     retail-join C2S 0x0F flood.
bool run_0a_health_class_byte_packed() {
	// Quantizer boundaries, exact: health_max 65536 makes the 16.16 ratio == health.
	if (!expect(ns::health_classification_byte(28671, 65536, 8) == 0x08,
	            "ratio 28671 (boundary) -> tier 0")) return false;
	if (!expect(ns::health_classification_byte(28672, 65536, 8) == 0x18,
	            "ratio 28672 -> tier 1")) return false;
	if (!expect(ns::health_classification_byte(49152, 65536, 8) == 0x18,
	            "ratio 49152 (boundary) -> tier 1")) return false;
	if (!expect(ns::health_classification_byte(49153, 65536, 8) == 0x28,
	            "ratio 49153 -> tier 2")) return false;
	if (!expect(ns::health_classification_byte(0, 65536, 8) == 0x08,
	            "health 0 -> tier 0 | class (death is signalled elsewhere, faithful)")) return false;
	if (!expect(ns::health_classification_byte(150, 150, 5) == 0x25,
	            "full health -> tier 2 | class 5")) return false;
	if (!expect(ns::health_classification_byte(1, 0, 9) == 0x29,
	            "healthMax 0 rides the divide guard [orig: healthMax ? healthMax : 1]")) return false;
	if (!expect(ns::health_classification_byte(100, 150, 0x18) == 0x18,
	            "class nibble masked & 0xF")) return false;

	// End-to-end: the emitted 0x0A player record carries the packed byte. With the traits sweep
	// resolved (player_item_hp = 150), the spawn seeds FULL health 150/150 [orig:
	// Entity_InitFromItemDef @0x49e550] -> tier 2 -> 0x28, the golden joiner byte (D-NET-144).
	w::World world;
	world.registry.configure_pool(0, 8);
	w::AiSystem ai;
	world.ai = &ai;
	world.player_item_hp = 150; // the items.def class-8 Player hp (the traits-sweep stamp)
	const w::EntityHandle host_h =
			w::spawn_player(world, player_spawn({1.0f, 2.0f, 3.0f}, 0, 0xFFF0));
	if (!expect(host_h.valid(), "host player spawned")) return false;

	std::vector<ns::Connection> conns;
	ns::LoopbackChannel ch;
	conns.push_back(ns::Connection{&ch, ns::TransportMode::Loopback, host_h, 0});
	nw::PlayerReplicationState fallback;

	const auto emitted_health_byte = [&](uint8_t &out) -> bool {
		ns::test::emit_all(world, conns, fallback);
		ns::Datagram dg;
		if (!expect(ch.client_recv(dg), "0x0A frame dequeued")) return false;
		nw::FrameUpdate fu;
		if (!expect(nw::decode_frame_update(dg.body.data(), dg.body.size(), ns::class_for_type_id, fu),
		            "0x0A frame decodes")) return false;
		for (const auto &rec : fu.records) {
			if (rec.handle != host_h.packed) continue;
			out = rec.player.health_class_byte;
			return true;
		}
		return expect(false, "player record present in the frame");
	};

	uint8_t byte = 0;
	if (!emitted_health_byte(byte)) return false;
	if (!expect(byte == 0x28, "spawn-at-full 150/150 emits packed 0x28 (tier 2 | class 8) — the "
	                          "golden joiner byte (D-NET-144)"))
		return false;

	world.registry.get(host_h)->health = 100; // damaged below the 0.75 boundary -> tier 1
	if (!emitted_health_byte(byte)) return false;
	if (!expect(byte == 0x18, "health 100/150 emits packed 0x18 (tier 1 | class 8), not raw 0x64"))
		return false;
	std::printf("PASS 0a_health_class_byte_packed\n");
	return true;
}

// (h) [D-NET-134 step 2] The 0x0A entity loop: pool-1 vehicles replicate as Vehicle compact
//     records, the byte budget caps each frame [orig: g_entity_send_budget @0xC8FC50 = 600,
//     soft cap @0x50f34b], and AGING gives budget-starved entities the next frame's slots
//     [orig: paddusb sweep @0x50e60f, age reset @0x50f168] — the original's round-robin has
//     no cursor; it is emergent from the age term of the priority key.
bool run_0a_vehicle_budget_round_robin() {
	w::World world;
	world.registry.configure_pool(0, 8);
	world.registry.configure_pool(1, 64);
	w::AiSystem ai;
	world.ai = &ai;
	const w::EntityHandle host_h =
			w::spawn_player(world, player_spawn({100.0f, 100.0f, 10.0f}, 0, 0xFFF0));
	if (!expect(host_h.valid(), "host player spawned")) return false;

	// 40 pool-1 vehicles clustered at one spot (uniform distance -> deterministic ordering:
	// within a frame the tie-break is snapshot order; across frames age dominates). The
	// replication class comes from the items.def *_function tag stamped as net_class_code by
	// the host's item-traits sweep — pool membership alone NEVER selects the vehicle record.
	constexpr int kVehicles = 40;
	for (int i = 0; i < kVehicles; ++i) {
		w::Entity veh;
		veh.kind = w::EntityKind::Item;
		veh.item_id = 0x050B; // dune buggy
		veh.net_class_code = uint8_t(nw::EntityClass::Vehicle); // ai_function cveh
		veh.health = 3000;                                      // items.def hp (full spawn)
		veh.health_max = 3000;
		veh.position = {120.0f, 100.0f, 10.0f};
		veh.yaw = 90;
		veh.team = 1;
		if (!expect(world.registry.spawn(1, veh).valid(), "vehicle spawned")) return false;
	}
	// One UNRESOLVED pool-1 item (an ewep-like emplacement without a stamped class): it must
	// NOT be serialized at all — emitting it with the vehicle record desyncs the client
	// mid-frame (the retail-join v13 regression).
	{
		w::Entity ewep;
		ewep.kind = w::EntityKind::Item;
		ewep.item_id = 0x074D; // Mounted Grenade Launcher on Tripod (ai_function ewep)
		ewep.position = {121.0f, 100.0f, 10.0f};
		if (!expect(world.registry.spawn(1, ewep).valid(), "emplacement spawned")) return false;
	}

	std::vector<ns::Connection> conns;
	ns::LoopbackChannel ch;
	conns.push_back(ns::Connection{&ch, ns::TransportMode::Loopback, host_h, 0});
	nw::PlayerReplicationState fallback;

	// Frame math (flags2=1 first send): header 28 B + player 23 B + N x 26-B vehicle records
	// (tail B, flags 0); the soft cap completes the record crossing 600 -> 22 vehicles/frame.
	const auto pump_frame = [&](nw::FrameUpdate &fu) -> bool {
		ns::test::emit_all(world, conns, fallback);
		ns::Datagram dg;
		if (!expect(ch.client_recv(dg), "0x0A frame dequeued")) return false;
		const auto resolver = [](uint16_t tid) {
			return tid == 0x14B9 ? nw::EntityClass::Player : nw::EntityClass::Vehicle;
		};
		if (!expect(nw::decode_frame_update(dg.body.data(), dg.body.size(), resolver, fu),
		            "0x0A frame decodes")) return false;
		if (!expect(dg.body.size() <= 600 + 26, "frame stays within the soft byte cap"))
			return false;
		return true;
	};

	std::vector<bool> seen(kVehicles, false);
	int seen_count = 0;
	const auto fold_frame = [&](const nw::FrameUpdate &fu, int &vehicles, bool &player) {
		vehicles = 0;
		player = false;
		for (const auto &rec : fu.records) {
			if (rec.cls == nw::EntityClass::Player && rec.handle == host_h.packed) player = true;
			if (rec.cls != nw::EntityClass::Vehicle) continue;
			++vehicles;
			const int slot = rec.handle & 0xFFF;
			if (slot < kVehicles && !seen[slot]) { seen[slot] = true; ++seen_count; }
		}
	};

	nw::FrameUpdate f1, f2;
	int v1 = 0, v2 = 0;
	bool p1 = false, p2 = false;
	if (!pump_frame(f1)) return false;
	fold_frame(f1, v1, p1);
	if (!expect(p1, "own player present in frame 1 (+1000 boost)")) return false;
	if (!expect(v1 == 22, "frame 1 carries 22 budget-capped vehicle records")) return false;

	// Frame 2 rides the phase-0 weapon sub-block (11-B vs 6-B header -> 33+23 = 56 header+player
	// bytes), so one fewer vehicle fits: 56 + 21*26 = 602 crosses the soft cap at 21.
	if (!pump_frame(f2)) return false;
	fold_frame(f2, v2, p2);
	if (!expect(p2, "own player present in frame 2")) return false;
	if (!expect(v2 == 21, "frame 2 carries 21 vehicle records (bigger sub-block)")) return false;
	if (!expect(seen_count == kVehicles,
	            "aging round-robin: two frames cover ALL 40 vehicles (18 starved + repeats)"))
		return false;

	// Decoded vehicle position reconstructs against the frame anchor (codec sanity).
	const int32_t vx = w::to_fixed(120.0), vy = w::to_fixed(100.0), vz = w::to_fixed(10.0);
	const int32_t ax = w::to_fixed(100.0), ay = w::to_fixed(100.0), az = w::to_fixed(10.0);
	bool vehicle_pos_ok = false;
	for (const auto &rec : f1.records) {
		if (rec.cls != nw::EntityClass::Vehicle) continue;
		// entity+286 vehicle health word — MUST be the live healthMax-scale health, never 0
		// (0 kills the vehicle every frame — v12) and never a small stopgap (renders it
		// burning — v13): the client stores it back verbatim (@0x460aff).
		if (!expect(rec.vehicle.health_word == 3000, "vehicle record carries live full health"))
			return false;
		vehicle_pos_ok =
				f1.anchor_x + nw::network_decompress_fixedpoint(rec.vehicle.pos_x_compressed) ==
						codec_recon(vx, ax) &&
				f1.anchor_y + nw::network_decompress_fixedpoint(rec.vehicle.pos_y_compressed) ==
						codec_recon(vy, ay) &&
				f1.anchor_z + nw::network_decompress_fixedpoint(rec.vehicle.pos_z_compressed) ==
						codec_recon(vz, az);
		break;
	}
	if (!expect(vehicle_pos_ok, "vehicle record position reconstructs against the anchor"))
		return false;

	// NetClientView LEARNS the vehicle class from the 0x0D pool-1 spawn batch and then decodes
	// the vehicle compact bodies with its DEFAULT resolver (which alone cannot know them).
	ns::NetClientView view;
	view.apply(0x0D, nw::encode_pool_spawn_batch(ns::build_pool1_spawn_batch(world)));
	ns::test::emit_all(world, conns, fallback);
	view.pump(ch);
	if (!expect(view.frames_applied() == 1, "view applied the 0x0A frame")) return false;
	int view_vehicles = 0;
	for (const auto &es : view.state().entities)
		if (es.cls == nw::EntityClass::Vehicle && (es.handle >> 12) == 1) ++view_vehicles;
	// +1: the 0x0D spawn batch carries the emplacement too, and the view's learned table
	// treats every pool-1 spawn as vehicle-CLASS for decode purposes (safe: no host —
	// retail or ours — emits 0x0A records for null-callback classes, so the guess is
	// never exercised against a record body).
	if (!expect(view_vehicles == kVehicles + 1,
	            "view holds all pool-1 spawns as Vehicle class (0x0D-learned)")) return false;
	std::printf("PASS 0a_vehicle_budget_round_robin\n");
	return true;
}

// (i) The witnessed player compact-record field sources [orig: NetPacket_SerializePlayerState
//     @0x4C09C0 case 1]: TRUNCATED yaw byte (@0x4c0c5d — not rounded), rounded pitch byte
//     (@0x4c0c77), anim slot low (entity+0x12C @0x4c0c9c), unmasked state flags (entity+0x24
//     @0x4c0c7d).
bool run_0a_player_record_field_sources() {
	w::World world;
	world.registry.configure_pool(0, 8);
	w::AiSystem ai;
	world.ai = &ai;
	const w::EntityHandle host_h =
			w::spawn_player(world, player_spawn({1.0f, 2.0f, 3.0f}, 0, 0xFFF0));
	w::Entity *e = world.registry.get(host_h);
	if (!expect(e != nullptr, "host entity resolvable")) return false;
	w::AiEntity *ae = ai.for_handle(host_h);
	if (!expect(ae != nullptr, "host AI pose resolvable")) return false;
	e->net_move_input = 0x21; // the uplink-ingested +0x12C movement-input byte the record echoes
	e->flags |= 0x40; // an arbitrary entity+0x24 bit rides the wire unmasked
	e->pitch = -45; // deliberately disagree with the live split-store look
	ae->pitch = 0x1F800000;
	e->equipped_adm_index = 0x09; // the equipped-weapon adm index the record's off-16 echoes

	std::vector<ns::Connection> conns;
	ns::LoopbackChannel ch;
	conns.push_back(ns::Connection{&ch, ns::TransportMode::Loopback, host_h, 0});
	nw::PlayerReplicationState fallback;
	ns::test::emit_all(world, conns, fallback);

	ns::Datagram dg;
	if (!expect(ch.client_recv(dg), "0x0A frame dequeued")) return false;
	nw::FrameUpdate fu;
	if (!expect(nw::decode_frame_update(dg.body.data(), dg.body.size(), ns::class_for_type_id, fu),
	            "0x0A frame decodes")) return false;
	const nw::FrameUpdateRecord *rec = nullptr;
	for (const auto &r : fu.records)
		if (r.handle == host_h.packed) rec = &r;
	if (!expect(rec != nullptr, "player record present")) return false;

	// yaw 0 -> engine BAM (90-0)*11930464 = 0x3FFFFFC0: TRUNCATED high byte = 0x3F (rounding
	// would give 0x40 — the exact bit the witness corrected).
	if (!expect(rec->player.yaw_byte == 0x3F, "yaw byte is the TRUNCATED high byte")) return false;
	// Live AiEntity pitch 0x1F800000 rounds to high byte 0x20; the
	// deliberately disagreeing registry pitch above must be ignored.
	if (!expect(rec->player.pitch_byte == 0x20, "pitch byte is the ROUNDED high byte")) return false;
	if (!expect(rec->player.move_input_byte == 0x21, "movement-input byte echoed")) return false;
	if (!expect((rec->player.state_flags & 0x40) != 0, "state flags carried unmasked")) return false;
	if (!expect(rec->player.carrier_handle == 0xFFFF, "unmounted anchor handle 0xFFFF")) return false;
	// ADM anim-def index (off-16) = the entity's equipped-weapon adm index (the uplink echo /
	// spawn default); 0 is a VALID adm entry — the none sentinel is 0xFF [orig: echo @0x4C20A3,
	// apply-skip @0x4c11f2] (D-NET-143).
	if (!expect(rec->player.anim_def_index == 0x09, "anim-def index echoes equipped_adm_index"))
		return false;
	// Anim-STATE id (off-14): the wire carries the entity's live body-anim state (the motor
	// mirror; pending-wins [orig: @0x4c0cc7]) — a fresh spawn reads the Entity_ResetToSpawnState
	// default 44, never 0 (the null clip; the v15 flicker) [orig: @0x4b9714] (D-NET-159).
	if (!expect(rec->player.anim_state_id == 44, "anim-state id is the spawn default 44"))
		return false;
	// Pending-wins selection [orig: reads +0x2B8 ?: +0x2BC @0x4c0cc7] + the channel ratio byte.
	e->net_anim_pending = 11; // a queued crouch-walk commit
	e->net_anim_phase = 37;
	ns::test::emit_all(world, conns, fallback);
	if (!expect(ch.client_recv(dg), "second 0x0A frame dequeued")) return false;
	if (!expect(nw::decode_frame_update(dg.body.data(), dg.body.size(), ns::class_for_type_id, fu),
	            "second 0x0A frame decodes")) return false;
	rec = nullptr;
	for (const auto &r : fu.records)
		if (r.handle == host_h.packed) rec = &r;
	if (!expect(rec != nullptr, "player record present (frame 2)")) return false;
	if (!expect(rec->player.anim_state_id == 11, "pending anim state wins the off-14 byte"))
		return false;
	if (!expect(rec->player.anim_channel_ratio == 37, "anim channel ratio rides off-15"))
		return false;
	std::printf("PASS 0a_player_record_field_sources\n");
	return true;
}

// (i2) D-NET-156 — the deploy-screen hold: a respawn-pending connection's 0x0A header carries
//      flags1 bit1 EVERY frame (the client's deploy screen is g_deploy_screen_active = (flags1 & 2) each
//      frame [orig: NetPacket_WritePlayerState @0x4ff7bd / NapiNPClientMsg_0x00A @0x42ff82]),
//      and dropping the flag closes it. The recipient's own stance echoes in the tail state
//      byte bits 0-1 [orig: tail read @0x4303e5 -> latches @0x430562/@0x430570].
bool run_0a_deploy_hold_and_tail_stance() {
	w::World world;
	world.registry.configure_pool(0, 8);
	w::AiSystem ai;
	world.ai = &ai;
	const w::EntityHandle h =
			w::spawn_player(world, player_spawn({1.0f, 2.0f, 3.0f}, 0, 0xFFF0));
	w::Entity *e = world.registry.get(h);
	if (!expect(e != nullptr, "player entity resolvable")) return false;

	std::vector<ns::Connection> conns;
	ns::LoopbackChannel ch;
	conns.push_back(ns::Connection{&ch, ns::TransportMode::Loopback, h, 0});
	conns[0].respawn_pending = true;
	e->flags |= 1u; // the hidden bit the join sets with the pending flag [orig: @0x4ff7dd]
	e->net_stance_bits = 2; // crouched — the tail must echo it (bit1)

	nw::PlayerReplicationState fallback;
	ns::test::emit_all(world, conns, fallback);
	ns::Datagram dg;
	if (!expect(ch.client_recv(dg), "0x0A frame dequeued")) return false;
	nw::FrameUpdate fu;
	if (!expect(nw::decode_frame_update(dg.body.data(), dg.body.size(), ns::class_for_type_id, fu),
	            "0x0A frame decodes")) return false;
	if (!expect(fu.flags1 == 0x02, "flags1 bit1 held while respawn-pending")) return false;
	if (!expect(fu.state_flag_byte == 0x02, "tail state byte echoes the crouch bit")) return false;
	const nw::FrameUpdateRecord *rec = nullptr;
	for (const auto &r : fu.records)
		if (r.handle == h.packed) rec = &r;
	if (!expect(rec != nullptr, "player record present")) return false;
	if (!expect((rec->player.state_flags & 0x01) != 0,
	            "record byte13 carries the pending hidden bit (golden 0x01)")) return false;

	// Deploy clears the hold: flags1 drops to 0 on the very next frame (one bit1=0 frame
	// closes the retail deploy screen).
	conns[0].respawn_pending = false;
	e->flags &= ~1u;
	e->net_stance_bits = 0;
	ns::test::emit_all(world, conns, fallback);
	if (!expect(ch.client_recv(dg), "post-deploy 0x0A dequeued")) return false;
	if (!expect(nw::decode_frame_update(dg.body.data(), dg.body.size(), ns::class_for_type_id, fu),
	            "post-deploy 0x0A decodes")) return false;
	if (!expect(fu.flags1 == 0x00, "flags1 drops after the deploy clears pending")) return false;
	if (!expect(fu.state_flag_byte == 0x00, "tail stance echo cleared")) return false;
	if (!expect(fu.health > 0, "tail carries the live (alive) health")) return false;

	// The victim's own death signal (v33 "killee never knows"): a dead recipient's frame
	// carries tail health 0 [orig: stored as the client's own Health @0x4305df] and its
	// record byte13 dead bit 0x02 [orig: the local apply's dead path @0x4c1005 — anim
	// stores + Health = 0; the 1->0 edge is the spawn hook @0x4c1109].
	e->health = 0;
	e->flags |= 2u;
	ns::test::emit_all(world, conns, fallback);
	if (!expect(ch.client_recv(dg), "dead-state 0x0A dequeued")) return false;
	if (!expect(nw::decode_frame_update(dg.body.data(), dg.body.size(), ns::class_for_type_id, fu),
	            "dead-state 0x0A decodes")) return false;
	if (!expect(fu.health == 0, "tail health 0 tells the victim it died")) return false;
	rec = nullptr;
	for (const auto &r : fu.records)
		if (r.handle == h.packed) rec = &r;
	if (!expect(rec != nullptr, "dead player record present")) return false;
	if (!expect((rec->player.state_flags & 0x02) != 0,
	            "record byte13 carries the dead bit")) return false;
	std::printf("PASS 0a_deploy_hold_and_tail_stance\n");
	return true;
}

// (i3) D-NET-157 — the 0x26 attach acceptance + the 0x0A mounted-branch echo: the accepted
//      occupant's record carries the RAW wire bone at byte 0, the vehicle as its carrier, and
//      CARRIER-LOCAL position; the recipient's own tail mount handle names its carrier. The
//      detach drops it all back to the free-standing form. [orig: Entity_ProcessVehicleAttach
//      @0x435AA0 / Entity_AttachToVehicleSlot @0x4946D0 tail @0x494752-75; record op1 @0x4c0a08]
// Type resolver for the attach test's mixed frame: the player type decodes as Player,
// the 0x1004 buggy as Vehicle (the default phase-1 resolver knows only the player type).
nw::EntityClass attach_test_class(uint16_t type_id) {
	if (type_id == 0x14B9) return nw::EntityClass::Player;
	if (type_id == 0x1004) return nw::EntityClass::Vehicle;
	return nw::EntityClass::Infantry;
}

bool run_0x26_attach_mounted_echo() {
	w::World world;
	world.registry.configure_pool(0, 8);
	world.registry.configure_pool(1, 8);
	w::AiSystem ai;
	world.ai = &ai;
	const w::EntityHandle ph =
			w::spawn_player(world, player_spawn({10.0f, 20.0f, 3.0f}, 0, 0xFFF0));
	w::Entity *player = world.registry.get(ph);
	if (!expect(player != nullptr, "player entity resolvable")) return false;

	// A pool-1 vehicle with one driver seat at bone 1 (the v31 wire bone).
	w::EntityHandle vh;
	{
		w::Entity veh;
		veh.kind = w::EntityKind::Item;
		veh.item_id = 0x1004;
		veh.position = {12.0f, 20.0f, 3.0f};
		veh.yaw = 0;
		veh.health = 3000;
		veh.health_max = 3000;
		veh.net_class_code = static_cast<uint8_t>(nw::EntityClass::Vehicle);
		w::Seat drv;
		drv.type = w::SeatType::Driver;
		drv.bone_index = 1;
		veh.seats.push_back(drv);
		vh = world.registry.spawn_from(1, 0, veh);
	}
	if (!expect(vh.valid(), "vehicle spawned")) return false;

	// The 0x26 acceptance path (dispatch calls this after the word0 anti-spoof overwrite).
	if (!expect(w::entity_process_vehicle_attach(world, ph, vh, 1), "attach accepted"))
		return false;
	if (!expect(player->mounted && player->mount_target == vh, "mount fields written"))
		return false;
	if (!expect(player->mount_bone == 1, "raw wire bone recorded (entity+0x157)")) return false;
	if (!expect((player->flags & 0x40u) != 0, "mounted flag 0x40 set")) return false;
	// A second occupant cannot take the held seat [orig: @0x435ba9].
	const w::EntityHandle ph2 =
			w::spawn_player(world, player_spawn({11.0f, 20.0f, 3.0f}, 1, 0xFFF1));
	if (!expect(!w::entity_process_vehicle_attach(world, ph2, vh, 1), "occupied seat rejects"))
		return false;

	std::vector<ns::Connection> conns;
	ns::LoopbackChannel ch;
	conns.push_back(ns::Connection{&ch, ns::TransportMode::Loopback, ph, 0});
	nw::PlayerReplicationState fallback;
	ns::test::emit_all(world, conns, fallback);
	ns::Datagram dg;
	if (!expect(ch.client_recv(dg), "0x0A frame dequeued")) return false;
	nw::FrameUpdate fu;
	if (!expect(nw::decode_frame_update(dg.body.data(), dg.body.size(), attach_test_class, fu),
	            "0x0A frame decodes")) return false;
	if (!expect(fu.mount_handle == vh.packed, "tail mount handle names the recipient's carrier"))
		return false;
	const nw::FrameUpdateRecord *rec = nullptr;
	for (const auto &r : fu.records)
		if (r.handle == ph.packed) rec = &r;
	if (!expect(rec != nullptr, "player record present")) return false;
	if (!expect(rec->player.vehicle_bone == 1, "mounted record byte0 = the wire bone"))
		return false;
	if (!expect(rec->player.carrier_handle == vh.packed, "mounted record carrier = the vehicle"))
		return false;
	if (!expect((rec->player.state_flags & 0x40u) != 0, "record byte13 carries mounted 0x40"))
		return false;

	// Detach: seat freed, mount fields cleared, record back to free-standing.
	if (!expect(w::entity_detach_from_vehicle(world, ph), "detach applies")) return false;
	if (!expect(!player->mounted && player->mount_bone == 0, "mount fields cleared"))
		return false;
	w::Entity *veh = world.registry.get(vh);
	if (!expect(veh != nullptr && !veh->seats[0].occupant.valid(), "seat occupant freed"))
		return false;
	ns::test::emit_all(world, conns, fallback);
	if (!expect(ch.client_recv(dg), "post-detach 0x0A dequeued")) return false;
	if (!expect(nw::decode_frame_update(dg.body.data(), dg.body.size(), attach_test_class, fu),
	            "post-detach 0x0A decodes")) return false;
	rec = nullptr;
	for (const auto &r : fu.records)
		if (r.handle == ph.packed) rec = &r;
	if (!expect(rec != nullptr, "player record present post-detach")) return false;
	if (!expect(rec->player.vehicle_bone == 0 && rec->player.carrier_handle == 0xFFFF,
	            "post-detach record is free-standing")) return false;
	std::printf("PASS 0x26_attach_mounted_echo\n");
	return true;
}

// (j) D-NET-151 — the grounded-on-entity replication loop. A joiner standing ON another
//     entity (building floor / vehicle deck) uplinks carrier_handle + CARRIER-LOCAL pos and
//     heading [orig: op3 reads groundEntity(+0x28); the extended body's pose is
//     Entity_TransformWorldToLocal output]. The host apply must lift local -> world through
//     the carrier pose [orig: op4 @0x4c1de1 Entity_TransformLocalToWorld; heading add
//     @0x43be7e], REPLACE flags bits 2-4 from the raw wire byte [orig: @0x4c1e4d], and the
//     0x0A echo must re-emit the carrier + local pos + local yaw byte [orig: op1 @0x4c0a08 /
//     @0x4c0b07 / sar-24 yaw @0x4c0b85] — echoing 0xFFFF at a grounded retail client
//     detaches + hard-snaps it to local-as-world coords (the v26 origin teleport).
bool run_grounded_uplink_apply_and_echo() {
	w::World world;
	world.registry.configure_pool(0, 8);
	world.registry.configure_pool(2, 8);
	w::AiSystem ai;
	world.ai = &ai;

	// The carrier: a pool-2 building at (100, 200, 10), mission yaw 90 -> engine BAM 0
	// (identity rotation — every 22-bit product below is exact).
	w::EntityHandle bld_h;
	{
		w::Entity bld;
		bld.kind = w::EntityKind::Building;
		bld.item_id = 0x044c;
		bld.position = {100.0f, 200.0f, 10.0f};
		bld.yaw = 90;
		bld_h = world.registry.spawn(2, bld);
	}
	if (!expect(bld_h.valid() && bld_h.pool() == 2, "pool-2 carrier spawned")) return false;

	const w::EntityHandle host_h =
			w::spawn_player(world, player_spawn({0.0f, 0.0f, 0.0f}, 0, 0xFFF0));
	std::vector<ns::Connection> conns;
	ns::LoopbackChannel self_ch, join_ch;
	conns.push_back(ns::Connection{&self_ch, ns::TransportMode::Loopback, host_h, 0});
	conns.push_back(ns::Connection{&join_ch, ns::TransportMode::Client, {}, 0});
	const w::EntityHandle joiner_h = ns::test::admit_peer(
			world, conns, 1, player_spawn({1.0f, 1.0f, 1.0f}, 0, 0xFFF1));
	if (!expect(joiner_h.valid(), "joiner admitted")) return false;
	{
		// Pre-set a stance bit INSIDE the replace mask: the uplink below carries bit 4 only,
		// so a faithful REPLACE clears bit 3; the old xor-delta would have kept it.
		w::Entity *je = world.registry.get(joiner_h);
		je->flags |= 0x08u;
	}

	// Grounded uplink: local (2.0, 0.5, 1.0) on the building, local heading 0x2000<<16
	// (mission yaw 45 after the identity-carrier add), flags byte bit 4.
	const int32_t lx = w::to_fixed(2.0), ly = w::to_fixed(0.5), lz = w::to_fixed(1.0);
	join_ch.client_send(0x0C, make_0c_uplink(joiner_h.packed, lx, ly, lz, 0x2000, 0,
	                                         bld_h.packed, /*state_flags=*/0x10));
	ns::test::drain_all(world, conns, /*is_authority=*/true);

	// (1) HOST APPLY: world pos = carrier ⊕ local (identity rotation -> exact adds).
	const w::Entity *je = world.registry.get(joiner_h);
	if (!expect(je != nullptr, "joiner entity present")) return false;
	if (!expect(je->position.x == 102.0f && je->position.y == 200.5f && je->position.z == 11.0f,
	            "grounded uplink lifted local -> world through the carrier pose"))
		return false;
	if (!expect(je->yaw == 45, "grounded heading composed with the carrier heading")) return false;
	if (!expect(je->ground_target == bld_h, "uplinked carrier mirrored into ground_target"))
		return false;
	if (!expect((je->flags & 0x1Cu) == 0x10u,
	            "flags bits 2-4 REPLACED from the wire byte (bit 3 cleared, not xor-kept)"))
		return false;

	// (2) ECHO: the joiner's 0x0A record re-emits the carrier + compressed LOCAL pos +
	// local yaw byte (single-bit locals survive the 12-bit-mantissa compressor exactly).
	ns::test::emit_all(world, conns, nw::PlayerReplicationState{});
	ns::Datagram dg;
	if (!expect(join_ch.client_recv(dg), "joiner 0x0A frame dequeued")) return false;
	nw::FrameUpdate fu;
	if (!expect(nw::decode_frame_update(dg.body.data(), dg.body.size(), ns::class_for_type_id, fu),
	            "0x0A frame decodes")) return false;
	const nw::FrameUpdateRecord *rec = nullptr;
	for (const auto &r : fu.records)
		if (r.handle == joiner_h.packed) rec = &r;
	if (!expect(rec != nullptr, "joiner player record present")) return false;
	if (!expect(rec->player.carrier_handle == bld_h.packed,
	            "record echoes the ground carrier (echoing 0xFFFF is the v26 snap)"))
		return false;
	if (!expect(nw::network_decompress_fixedpoint(rec->player.pos_x_compressed) == lx &&
	                    nw::network_decompress_fixedpoint(rec->player.pos_y_compressed) == ly &&
	                    nw::network_decompress_fixedpoint(rec->player.pos_z_compressed) == lz,
	            "record position is the CARRIER-LOCAL offset, not anchor-relative world"))
		return false;
	// Local heading hi-byte: yaw 45 -> engine BAM (90-45)*11930464 = 0x1FFFFFE0; carrier BAM 0.
	if (!expect(rec->player.yaw_byte == 0x1F, "yaw byte is the LOCAL heading's high byte"))
		return false;

	// (3) The free-standing form is unchanged: a later 0xFFFF uplink returns to world coords.
	join_ch.client_send(0x0C, make_0c_uplink(joiner_h.packed, w::to_fixed(50.0),
	                                         w::to_fixed(60.0), w::to_fixed(12.0), 0x2000, 0));
	ns::test::drain_all(world, conns, /*is_authority=*/true);
	je = world.registry.get(joiner_h);
	if (!expect(je->position.x == 50.0f && je->position.y == 60.0f && je->position.z == 12.0f,
	            "free-standing uplink applies world coords raw")) return false;
	if (!expect(!je->ground_target.valid(), "ground_target cleared by a free-standing uplink"))
		return false;
	std::printf("PASS grounded_uplink_apply_and_echo\n");
	return true;
}

// (k) The 22-bit pose-transform pair inverts: world_to_local(local_to_world(v)) recovers v
//     exactly at identity and within fixed-point rounding for an arbitrary pose
//     [orig: Entity_TransformLocalToWorld @0x43BD00 / Entity_TransformWorldToLocal @0x43BB50].
bool run_pose_transform_roundtrip() {
	// Identity pose: exact.
	{
		const nw::WorldPose w = nw::network_transform_local_to_world(
				w::to_fixed(2.0), w::to_fixed(0.5), w::to_fixed(1.0), w::to_fixed(100.0),
				w::to_fixed(200.0), w::to_fixed(10.0), 0u, 0u, 0u);
		if (!expect(w.x == w::to_fixed(102.0) && w.y == w::to_fixed(200.5) &&
		                    w.z == w::to_fixed(11.0),
		            "identity-pose lift is exact")) return false;
		const nw::WorldPose l = nw::network_transform_world_to_local(
				w.x, w.y, w.z, w::to_fixed(100.0), w::to_fixed(200.0), w::to_fixed(10.0), 0u,
				0u, 0u);
		if (!expect(l.x == w::to_fixed(2.0) && l.y == w::to_fixed(0.5) && l.z == w::to_fixed(1.0),
		            "identity-pose round-trip is exact")) return false;
	}
	// Arbitrary pose (yaw+pitch+roll): round-trips within 22-bit chained-mul rounding.
	{
		const uint32_t yaw = 0x1F340000u, pitch = 0x02ABCDEFu, roll = 0xFE000123u;
		const int32_t px = w::to_fixed(-433.7), py = w::to_fixed(371.5), pz = w::to_fixed(12.4);
		const int32_t lx = w::to_fixed(3.25), ly = w::to_fixed(-1.5), lz = w::to_fixed(0.75);
		const nw::WorldPose w2 = nw::network_transform_local_to_world(lx, ly, lz, px, py, pz,
		                                                              yaw, pitch, roll);
		const nw::WorldPose l2 = nw::network_transform_world_to_local(w2.x, w2.y, w2.z, px, py,
		                                                              pz, yaw, pitch, roll);
		const auto near_eq = [](int32_t a, int32_t b) {
			const int32_t d = a - b;
			return d >= -4 && d <= 4; // <= 4/65536 world units of chained rounding
		};
		if (!expect(near_eq(l2.x, lx) && near_eq(l2.y, ly) && near_eq(l2.z, lz),
		            "arbitrary-pose round-trip within fixed-point rounding")) return false;
	}
	std::printf("PASS pose_transform_roundtrip\n");
	return true;
}

} // namespace

// (l) [D-NET-152] The §5.9.1 tag-2 ROUND-EVENT fan: an accepted fire appends a world
//     round-ring event; the per-recipient sweep serves it to every OTHER in-match
//     connection exactly once (watermark), skipping the shooter's own rounds (its client
//     already simulated them [orig: @0x4fff97]) and never replaying the pre-join backlog
//     (the arm gate [orig: the playerSlot+97544 non-zero gate]). The origin compresses
//     against the RECIPIENT's anchor and the direction BAM high words survive rounded
//     [orig: Server_BuildRoundEventListForPlayer @0x4ffee0 ->
//     NetPacket_SerializeRoundEvent @0x504820].
bool run_round_event_fanout() {
	w::World world;
	world.registry.configure_pool(0, 8);
	w::AiSystem ai;
	world.ai = &ai;
	const w::EntityHandle host_h =
			w::spawn_player(world, player_spawn({1.0f, 2.0f, 3.0f}, 0, 0xFFF0));
	const w::EntityHandle peer_h =
			w::spawn_remote_player(world, player_spawn({4.0f, 5.0f, 6.0f}, 0, 0xFFF1));
	if (!expect(host_h.valid() && peer_h.valid(), "host + peer spawned")) return false;

	std::vector<ns::Connection> conns;
	ns::LoopbackChannel ch_host;
	ns::LoopbackChannel ch_peer; // loopback transports keep the harness socket-free
	conns.push_back(ns::Connection{&ch_host, ns::TransportMode::Loopback, host_h, 0});
	conns.push_back(ns::Connection{&ch_peer, ns::TransportMode::Client, peer_h, 0});

	nw::PlayerReplicationState fallback;
	fallback.spawn_x = static_cast<uint32_t>(w::to_fixed(1.0));
	fallback.spawn_y = static_cast<uint32_t>(w::to_fixed(2.0));
	fallback.spawn_z = static_cast<uint32_t>(w::to_fixed(3.0));

	auto next_frame = [&](ns::LoopbackChannel &ch, nw::FrameUpdate &fu) {
		ns::Datagram dg;
		if (!ch.client_recv(dg) || dg.tag != nw::s2c::PER_FRAME_UPDATE) return false;
		return nw::decode_frame_update(dg.body.data(), dg.body.size(), ns::class_for_type_id,
		                               fu);
	};

	// A round fired BEFORE either connection's first emit = the pre-join backlog; the
	// arm gate must swallow it for both.
	{
		w::RoundEvent backlog;
		backlog.shooter_handle = peer_h.packed;
		backlog.adm_index = 9;
		world.rounds.add(backlog);
	}
	ns::test::emit_all(world, conns, fallback);
	{
		nw::FrameUpdate fh, fp;
		if (!expect(next_frame(ch_host, fh) && next_frame(ch_peer, fp), "arm frames decode"))
			return false;
		if (!expect(fh.round_events.empty() && fp.round_events.empty(),
		            "pre-arm backlog never replays to a joiner"))
			return false;
	}

	// The PEER fires: origin near its own pos, direction words as the C2S 0x06 carries
	// them (<< 16), a claimed target stamped on the shooter entity.
	const int32_t ox = w::to_fixed(4.5), oy = w::to_fixed(5.5), oz = w::to_fixed(6.5);
	{
		w::Entity *shooter = world.registry.get(peer_h);
		if (!expect(shooter != nullptr, "shooter entity live")) return false;
		shooter->last_fire_target = host_h; // [orig: @0x50c2ad]
		w::RoundEvent ev;
		ev.shooter_handle = peer_h.packed;
		ev.origin_x = ox;
		ev.origin_y = oy;
		ev.origin_z = oz;
		ev.dir_yaw = int32_t(0x1234u << 16);
		ev.dir_pitch = int32_t(0xFEDCu << 16);
		ev.shot_seq = 77;
		ev.mode_flags = 0x22;
		ev.subtype = 12;
		ev.slot_byte = 0;
		ev.adm_index = 11;
		world.rounds.add(ev);
	}
	ns::test::emit_all(world, conns, fallback);
	{
		nw::FrameUpdate fh;
		if (!expect(next_frame(ch_host, fh), "host frame decodes")) return false;
		if (!expect(fh.round_events.size() == 1, "host (observer) gets ONE round event"))
			return false;
		const nw::RoundEventRecord &re = fh.round_events[0];
		if (!expect(re.shooter_handle == peer_h.packed, "round shooter = the firing peer"))
			return false;
		if (!expect((re.flags & 0x40) != 0 && re.target_handle == host_h.packed,
		            "live fire target rides the 0x40 word"))
			return false;
		if (!expect((re.flags & 0x80) == 0, "zero slot byte stays un-gated")) return false;
		if (!expect((re.flags & 0x3F) == 0x22 && re.adm_index == 11 && re.subtype == 12 &&
		                    re.shot_seq == 77,
		            "mode/adm/subtype/shot_seq round-trip"))
			return false;
		// Origin reconstructs against the RECIPIENT's own anchor.
		const int32_t hax = int32_t(w::to_fixed(1.0));
		if (!expect(nw::network_decompress_fixedpoint(re.pos_x_compressed) + fh.anchor_x ==
		                    codec_recon(ox, hax),
		            "fire origin decompresses against the recipient anchor"))
			return false;
		if (!expect(re.yaw_bam_high == 0x1234 && re.pitch_bam_high == 0xFEDC,
		            "direction BAM high words intact"))
			return false;
		nw::FrameUpdate fp;
		if (!expect(next_frame(ch_peer, fp), "peer frame decodes")) return false;
		if (!expect(fp.round_events.empty(), "the shooter's OWN round is never echoed back"))
			return false;
	}

	// Watermark: the same round never repeats on the next frame.
	ns::test::emit_all(world, conns, fallback);
	{
		nw::FrameUpdate fh, fp;
		if (!expect(next_frame(ch_host, fh) && next_frame(ch_peer, fp),
		            "watermark frames decode"))
			return false;
		if (!expect(fh.round_events.empty() && fp.round_events.empty(),
		            "a swept round never repeats (per-connection watermark)"))
			return false;
	}
	std::printf("PASS round_event_fanout\n");
	return true;
}

// (i4) The drive-authority chain (net-re §5.13 witness 2026-07-04): vehicles have NO wire
//      uplink — the HOST simulates the ridden vehicle from the driver's replicated
//      MoveOrder/heading (the vehicle motor pass in AiSystem::tick), and the S2C 0x0A
//      vehicle record streams the LIVE (moving) pose. [orig: Client_ProcessNetworkFrame
//      @0x42c482 single-entity uplink; Entity_UpdateVehiclePhysics @0x48af00 drive gate
//      @0x48b0ff; Entity_SerializeVehicleState @0x460560 modes 3/4 return -1]
bool run_vehicle_drive_authority() {
	w::World world;
	world.registry.configure_pool(0, 8);
	world.registry.configure_pool(1, 8);
	w::AiSystem ai;
	world.ai = &ai;
	// The driver is a REMOTE joiner (the v33 rider topology): its MoveOrder/heading are
	// wire-owned — the motor consumes what the 0x0C apply landed, and mirror_wire_anim's
	// local-input export must NOT overwrite them (a local host player's input comes from
	// the input system instead).
	const w::EntityHandle ph =
			w::spawn_remote_player(world, player_spawn({10.0f, 20.0f, 3.0f}, 0, 0xFFF0));
	w::Entity *player = world.registry.get(ph);
	if (!expect(player != nullptr, "player entity resolvable")) return false;

	w::EntityHandle vh;
	{
		w::Entity veh;
		veh.kind = w::EntityKind::Item;
		veh.item_id = 0x1004;
		veh.position = {12.0f, 20.0f, 3.0f};
		veh.yaw = 0;
		veh.health = 3000;
		veh.health_max = 3000;
		veh.net_class_code = static_cast<uint8_t>(nw::EntityClass::Vehicle);
		w::Seat drv;
		drv.type = w::SeatType::Driver;
		drv.bone_index = 1;
		veh.seats.push_back(drv);
		vh = world.registry.spawn_from(1, 0, veh);
	}
	if (!expect(vh.valid(), "vehicle spawned")) return false;
	// The JOX dune buggy's pre-scaled physics block (vehicle_motor_test pins the parse).
	{
		w::VehicleTraits t;
		t.physics = 1;
		t.player_speed = 94 * 293;
		t.acceleration = 15 * 4;
		t.deceleration = 70 * 4;
		t.turn_rate = 65 * 192426;
		t.turn_rate2 = 41 * 192426;
		t.player_control = true;
		world.vehicle_traits.set(0x1004, t);
	}

	if (!expect(w::entity_process_vehicle_attach(world, ph, vh, 1), "attach accepted"))
		return false;
	// The driver's replicated input (landed by the 0x0C apply): forward + moving, heading
	// = the vehicle's own (drive straight).
	player->net_move_input = 0x08;
	player->yaw = 0;

	std::vector<ns::Connection> conns;
	ns::LoopbackChannel ch;
	conns.push_back(ns::Connection{&ch, ns::TransportMode::Loopback, ph, 0});
	nw::PlayerReplicationState fallback;

	// Frame A: parked pose.
	ns::test::emit_all(world, conns, fallback);
	ns::Datagram dg;
	if (!expect(ch.client_recv(dg), "frame A dequeued")) return false;
	nw::FrameUpdate fa;
	if (!expect(nw::decode_frame_update(dg.body.data(), dg.body.size(), attach_test_class, fa),
	            "frame A decodes")) return false;
	const nw::FrameUpdateRecord *ra = nullptr;
	for (const auto &r : fa.records)
		if (r.handle == vh.packed) ra = &r;
	if (!expect(ra != nullptr, "vehicle record present in frame A")) return false;

	// 62 authority ticks: the vehicle motor consumes the driver's input.
	w::TickContext ctx;
	ctx.world = &world;
	ctx.is_authority = true;
	for (int i = 0; i < 62; ++i) {
		ctx.logic_tick = static_cast<uint32_t>(i);
		ai.tick(world, ctx);
	}
	w::Entity *veh = world.registry.get(vh);
	if (!expect(veh != nullptr && veh->veh.speed > 0, "host vehicle motor spun up"))
		return false;
	// 62 ticks from standstill: the unclamped launch step (861) + 61 accel-clamped ticks
	// (+60) — deterministic [orig: the @0x48bb46 branch tree + ±itemDef->acceleration].
	if (!expect(veh->veh.speed == 861 + 60 * 61, "speed ramp matches the clamp math"))
		return false;
	const float moved = std::fabs(veh->position.x - 12.0f) + std::fabs(veh->position.y - 20.0f);
	if (!expect(moved > 0.5f, "vehicle moved under the driver's replicated input"))
		return false;

	// Frame B: the streamed record carries the LIVE pose (compressed coords changed while
	// the recipient anchor held still).
	ns::test::emit_all(world, conns, fallback);
	if (!expect(ch.client_recv(dg), "frame B dequeued")) return false;
	nw::FrameUpdate fb;
	if (!expect(nw::decode_frame_update(dg.body.data(), dg.body.size(), attach_test_class, fb),
	            "frame B decodes")) return false;
	const nw::FrameUpdateRecord *rb = nullptr;
	for (const auto &r : fb.records)
		if (r.handle == vh.packed) rb = &r;
	if (!expect(rb != nullptr, "vehicle record present in frame B")) return false;
	if (!expect(ra->vehicle.pos_x_compressed != rb->vehicle.pos_x_compressed ||
	                    ra->vehicle.pos_y_compressed != rb->vehicle.pos_y_compressed,
	            "vehicle record pose is LIVE (host-simulated)"))
		return false;
	if (!expect(rb->vehicle.health_word == 3000, "live record keeps the health word"))
		return false;
	std::printf("PASS vehicle_drive_authority\n");
	return true;
}

int main() {
	const bool ok = run_fanout_and_per_connection_anchor() && run_joiner_uplink_snaps_peer() &&
	                run_self_uplink_rejected() && run_cross_peer_uplink_rejected() &&
	                run_retail_player_slots_start_after_bms_organics() &&
	                run_0a_subblock_phase_cycle() && run_0a_health_class_byte_packed() &&
	                run_0a_vehicle_budget_round_robin() && run_0a_player_record_field_sources() &&
	                run_0a_deploy_hold_and_tail_stance() && run_0x26_attach_mounted_echo() &&
	                run_vehicle_drive_authority() &&
	                run_grounded_uplink_apply_and_echo() && run_pose_transform_roundtrip() &&
	                run_round_event_fanout();
	std::fprintf(stderr, ok ? "OK\n" : "FAIL\n");
	return ok ? 0 : 1;
}
