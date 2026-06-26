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
#include "netsim/entity_wire_bridge.h"
#include "netsim/loopback_channel.h"
#include "netsim/net_client_view.h"
#include "netsim/net_system.h"
#include "netsim/session_transport.h"
#include "netsim/udp_session_transport.h"

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

	ns::NetSystem net; // default ctor: empty table
	ns::LoopbackChannel self_ch;
	ns::UdpSessionTransport udp_host(ns::UdpSessionTransport::Role::Host);
	const std::size_t conn_self =
			net.add_connection(ns::Connection{&self_ch, ns::TransportMode::Loopback, host_h, 0});
	const std::size_t conn_join =
			net.add_connection(ns::Connection{&udp_host, ns::TransportMode::Client, {}, 0});
	if (!expect(net.connection_count() == 2, "two connections registered")) return false;
	(void)conn_self;

	// Fallback anchor = the host player position (what NovaSimulation::compute_net_anchor builds).
	nw::PlayerReplicationState fallback;
	fallback.spawn_x = static_cast<uint32_t>(w::to_fixed(5.0));
	fallback.spawn_y = static_cast<uint32_t>(w::to_fixed(10.0));
	fallback.spawn_z = static_cast<uint32_t>(w::to_fixed(-3.0));

	// --- sub-case a1: byte-identity. conn_join still has NO owned entity, so it rides the
	//     fallback anchor = the host position = conn_self's anchor. Both frames identical. ---
	net.emit_s2c(world, fallback);
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
			net.admit_peer(world, conn_join, player_spawn({50.0f, 60.0f, -20.0f}, 90, 0xFFF1));
	if (!expect(joiner_h.valid() && joiner_h != host_h, "joiner spawned, distinct handle"))
		return false;
	if (!expect(world.cached.local_player == host_h,
	            "admit_peer did NOT steal local_player from the host")) return false;

	net.emit_s2c(world, fallback);
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

	ns::NetSystem net;
	ns::LoopbackChannel self_ch;
	ns::UdpSessionTransport udp_host(ns::UdpSessionTransport::Role::Host);
	net.add_connection(ns::Connection{&self_ch, ns::TransportMode::Loopback, host_h, 0});
	const std::size_t conn_join =
			net.add_connection(ns::Connection{&udp_host, ns::TransportMode::Client, {}, 0});
	const w::EntityHandle joiner_h =
			net.admit_peer(world, conn_join, player_spawn({1.0f, 1.0f, 1.0f}, 0, 0xFFF1));
	if (!expect(joiner_h.valid(), "joiner admitted")) return false;

	// The joiner reports a new pose from its client endpoint.
	const int32_t wx = w::to_fixed(100.0), wy = w::to_fixed(200.0), wz = w::to_fixed(-50.0);
	const int16_t wheading = 0x2000; // -> mission yaw 45
	ns::UdpSessionTransport udp_join(ns::UdpSessionTransport::Role::Client);
	udp_join.client_send(0x0C, make_0c_uplink(joiner_h.packed, wx, wy, wz, wheading, 0x0100));
	carry(udp_join, udp_host);
	if (!expect(udp_host.inbound_pending() == 1, "joiner uplink reached the host endpoint"))
		return false;

	w::TickContext ctx;
	ctx.world = &world;
	ctx.is_authority = true;
	net.tick(world, ctx);
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

	ns::NetSystem net;
	ns::LoopbackChannel self_ch;
	net.add_connection(ns::Connection{&self_ch, ns::TransportMode::Loopback, host_h, 0});

	// A (malicious/echo) 0x0C naming the host's own player.
	self_ch.client_send(0x0C, make_0c_uplink(host_h.packed, w::to_fixed(999.0), 0, 0, 0x4000, 0));
	w::TickContext ctx;
	ctx.world = &world;
	ctx.is_authority = true;
	net.tick(world, ctx);
	if (!expect(self_ch.c2s_pending() == 0, "self 0x0C drained even when rejected")) return false;

	const w::Entity *he = world.registry.get(host_h);
	if (!expect(he->position.x == 7.0f && he->position.y == 8.0f && he->position.z == 9.0f,
	            "host own-player pose NOT overwritten by a read-apply")) return false;
	const w::AiEntity *hae = ai.for_handle(host_h);
	if (!expect(hae != nullptr && !hae->net_is_remote_peer,
	            "host own-player not marked net-snapped")) return false;
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

	ns::NetSystem net;
	ns::LoopbackChannel self_ch;
	ns::UdpSessionTransport udp_host(ns::UdpSessionTransport::Role::Host);
	net.add_connection(ns::Connection{&self_ch, ns::TransportMode::Loopback, host_h, 0});
	const std::size_t conn_join =
			net.add_connection(ns::Connection{&udp_host, ns::TransportMode::Client, {}, 0});
	const w::EntityHandle joiner_h =
			net.admit_peer(world, conn_join,
			               player_spawn({10.0f, 0.0f, 0.0f}, 0, 0xFFF1, 4));
	if (!expect(joiner_h == w::EntityHandle::make(0, 5), "retail joiner player uses slot 5"))
		return false;

	nw::PlayerReplicationState fallback;
	fallback.spawn_x = static_cast<uint32_t>(w::to_fixed(0.0));
	fallback.spawn_y = static_cast<uint32_t>(w::to_fixed(0.0));
	fallback.spawn_z = static_cast<uint32_t>(w::to_fixed(0.0));
	net.emit_s2c(world, fallback);

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

} // namespace

int main() {
	const bool ok = run_fanout_and_per_connection_anchor() && run_joiner_uplink_snaps_peer() &&
	                run_self_uplink_rejected() &&
	                run_retail_player_slots_start_after_bms_organics();
	std::fprintf(stderr, ok ? "OK\n" : "FAIL\n");
	return ok ? 0 : 1;
}
