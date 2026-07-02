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

#include <novaworld/ingame_decode.h> // EntityPacketSubHeader / PlayerExtendedUplink
#include <novaworld/ingame_encode.h> // network_compress_fixedpoint, encode_* uplink
#include <world/ai.h>                // AiSystem / AiEntity (engine-frame mirror)
#include <world/entity.h>
#include <world/geom.h>
#include <world/player_spawn.h> // spawn_player / spawn_remote_player
#include <world/world.h>

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
std::vector<uint8_t> make_0c_uplink(uint16_t handle, int32_t x, int32_t y, int32_t z,
                                    int16_t heading, int16_t pitch) {
	nw::EntityPacketSubHeader hdr;
	hdr.handle = handle;
	hdr.item_type_id = 0x14B9; // player infantry
	hdr.sub_op = 0x0A;         // extended (type 10)
	nw::PlayerExtendedUplink up;
	up.vehicle_handle = 0xFFFF; // unmounted
	up.pos_x = x;
	up.pos_y = y;
	up.pos_z = z;
	up.heading = heading;
	up.pitch = pitch;
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
		if (!expect(dg.tag == ns::kTag0aFrameUpdate, "tag 0x0A")) return false;
		nw::FrameUpdate fu;
		if (!expect(nw::decode_frame_update(dg.body.data(), dg.body.size(), ns::class_for_type_id, fu),
		            "0x0A frame decodes as a well-formed frame")) return false;
		if (!expect(fu.flags2 == want_flags2[i], "flags2 cycles {1,0,3}")) return false;
		if ((want_flags2[i] & 3u) == 1) {
			if (!expect(fu.timer.present && fu.timer.state1 == 13,
			            "phase 1 = server-status carrying fall-damage tolerance 13")) return false;
		} else if ((want_flags2[i] & 3u) == 0) {
			if (!expect(fu.weapon.present, "phase 0 = weapon sub-block present")) return false;
		}
		// phase 3 (gametype) = 0 bytes for a non-objective gametype: nothing to assert.
	}
	// The connection's phase counter advanced once per send.
	if (!expect(conns[0].s2c_phase == 6, "phase counter advanced once per send")) return false;
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

	// End-to-end: the emitted 0x0A player record carries the packed byte. spawn_player defaults:
	// class 8, health 100, snapshot healthMax 150 -> ratio 43690 -> tier 1 -> 0x18.
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
	if (!expect(byte == 0x18, "health 100/150 emits packed 0x18 (tier 1 | class 8), not raw 0x64"))
		return false;

	world.registry.get(host_h)->health = 150; // full health -> tier 2
	if (!emitted_health_byte(byte)) return false;
	if (!expect(byte == 0x28, "health 150/150 emits packed 0x28 (tier 2 | class 8)")) return false;
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
	// within a frame the tie-break is snapshot order; across frames age dominates).
	constexpr int kVehicles = 40;
	for (int i = 0; i < kVehicles; ++i) {
		w::Entity veh;
		veh.kind = w::EntityKind::Item;
		veh.item_id = 0x050B; // dune buggy
		veh.position = {120.0f, 100.0f, 10.0f};
		veh.yaw = 90;
		veh.team = 1;
		if (!expect(world.registry.spawn(1, veh).valid(), "vehicle spawned")) return false;
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
		// entity+286 vehicle health word — MUST be the live health (spawn default 100), never 0:
		// the client stores it back verbatim (@0x460aff), so a 0 kills the vehicle every frame
		// (the live v12 all-vehicles-dying regression).
		if (!expect(rec.vehicle.health_word == 100, "vehicle record carries live health"))
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
	if (!expect(view_vehicles == kVehicles,
	            "view holds all pool-1 vehicles as Vehicle class (0x0D-learned)")) return false;
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
	e->anim_slot = 0x21;
	e->flags |= 0x40; // an arbitrary entity+0x24 bit rides the wire unmasked
	e->pitch = 45;

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
	// pitch 45 deg -> BAM 0x1FFFFFE0 -> rounded high byte 0x20.
	if (!expect(rec->player.pitch_byte == 0x20, "pitch byte is the ROUNDED high byte")) return false;
	if (!expect(rec->player.anim_slot_low == 0x21, "anim slot low carried")) return false;
	if (!expect((rec->player.state_flags & 0x40) != 0, "state flags carried unmasked")) return false;
	if (!expect(rec->player.vehicle_handle == 0xFFFF, "unmounted anchor handle 0xFFFF")) return false;
	std::printf("PASS 0a_player_record_field_sources\n");
	return true;
}

} // namespace

int main() {
	const bool ok = run_fanout_and_per_connection_anchor() && run_joiner_uplink_snaps_peer() &&
	                run_self_uplink_rejected() && run_cross_peer_uplink_rejected() &&
	                run_retail_player_slots_start_after_bms_organics() &&
	                run_0a_subblock_phase_cycle() && run_0a_health_class_byte_packed() &&
	                run_0a_vehicle_budget_round_robin() && run_0a_player_record_field_sources();
	std::fprintf(stderr, ok ? "OK\n" : "FAIL\n");
	return ok ? 0 : 1;
}
