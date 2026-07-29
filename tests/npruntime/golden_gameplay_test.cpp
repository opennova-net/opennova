// P4 golden — the per-frame host loop (np::Server_TickUpdate) round-trips a REAL retail in-match
// C2S 0x0C player uplink into an S2C 0x0A frame, sourced from the gameplay capture
// (.scratch/golden/retail-gameplay-session.pcapng). Env-gated on NW_GOLDEN_GAMEPLAY with a DEFAULT_*
// fallback; skips cleanly when the gitignored golden is absent (no committed derived oracle).
//
// WHAT THIS PROVES (the P4 bar, honestly scoped):
//   A retail client's extended (sub_op 0x0A) C2S 0x0C uplink decodes, drains through the SINGLE-OWNER
//   connection table (NapiNPProtocol.connection_list), SNAPs the owned entity, and the SAME frame's
//   per-connection S2C 0x0A fan emits a frame whose anchor IS that entity's post-SNAP position. This is
//   opennova<->opennova encoder/decoder self-consistency: 0x0C-in -> apply -> 0x0A-out reflects it.
//
// WHAT IS DEFERRED (documented, NOT faked green):
//   Full-datagram body byte-parity vs the capture's OWN S2C 0x0A is NOT asserted — our World holds only
//   the host + one peer, not the capture's full ASH_G3D entity set, so the bodies cannot byte-match
//   (the same deferral npruntime_golden_lan_join documents). Per-tag counts are printed for the record.

#include <npruntime/napi_np_connection.h>
#include <npruntime/napi_np_server_ctx.h>
#include <npruntime/server_tick.h>

#include <netsim/connection.h>           // TransportMode
#include <netsim/entity_wire_bridge.h>   // class_for_type_id
#include <netsim/udp_session_transport.h>

#include <npwire/ingame_decode.h> // decode_entity_packet_sub_header / decode_player_extended_uplink / decode_frame_update
#include <npwire/ingame_encode.h> // encode_entity_packet_sub_header / encode_player_extended_uplink
#include <npwire/wire_capture.h>  // CaptureDatagram / InGameMessage / decode_capture_to_messages

#include <world/ai.h>
#include <world/entity.h>
#include <world/geom.h>
#include <world/player_spawn.h>
#include <world/world.h>

#include <pcapio/pcap_reader.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#ifndef DEFAULT_GAMEPLAY_PCAP
#define DEFAULT_GAMEPLAY_PCAP ""
#endif

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

w::PlayerSpawn player_spawn(w::Vec3 pos, int16_t yaw, uint16_t net_id) {
	w::PlayerSpawn s;
	s.position = pos;
	s.yaw = yaw;
	s.net_id = net_id;
	return s;
}

} // namespace

int main() {
	std::string path;
	if (const char *env = std::getenv("NW_GOLDEN_GAMEPLAY"); env && *env)
		path = env;
	else
		path = DEFAULT_GAMEPLAY_PCAP;

	std::vector<net::PcapDatagram> pkts;
	if (path.empty() || !net::read_pcap_udp_file(path, pkts)) {
		std::printf("[skip] golden gameplay capture not found (set NW_GOLDEN_GAMEPLAY) — '%s'\n",
		            path.c_str());
		return 0; // skip clean — CI stays green without the gitignored golden
	}

	// Outer-decode the whole capture (recovers each side's SCRK from the in-stream handshake).
	std::vector<CaptureDatagram> caps;
	caps.reserve(pkts.size());
	for (const net::PcapDatagram &p : pkts) {
		CaptureDatagram c;
		c.frame_index = p.frame_index;
		c.src_port = p.srcport;
		c.dst_port = p.dstport;
		c.payload = p.payload;
		caps.push_back(std::move(c));
	}
	const std::vector<InGameMessage> msgs = decode_capture_to_messages(caps);

	// Step 0 — decodability gate. Find the first C2S 0x0C whose sub-header is an extended (sub_op 0x0A)
	// player uplink — the only form drain_connection_c2s applies this increment (0x0B compact deferred).
	// If the handshake wasn't captured (no SCRK -> no decodable protocol messages) or every 0x0C is
	// compact, skip clean (the LAN-join honesty precedent — the apply gap is documented, never faked).
	std::map<uint8_t, int> c2s_counts, s2c_counts;
	const InGameMessage *uplink = nullptr;
	for (const InGameMessage &m : msgs) {
		if (m.settings_update) continue;
		const uint8_t tag = static_cast<uint8_t>(m.tag & 0xFF);
		(m.dir == 'C' ? c2s_counts : s2c_counts)[tag]++;
		if (uplink != nullptr || m.dir != 'C' || tag != 0x0C) continue;
		EntityPacketSubHeader hdr;
		std::size_t consumed = 0;
		if (decode_entity_packet_sub_header(m.payload.data(), m.payload.size(), hdr, consumed) &&
		    hdr.sub_op == 0x0A)
			uplink = &m;
	}

	std::printf("[golden] gameplay capture: C2S 0x0C=%d, S2C 0x0A=%d, S2C 0x0C(spawn)=%d\n",
	            c2s_counts[0x0C], s2c_counts[0x0A], s2c_counts[0x0C]);

	if (uplink == nullptr) {
		std::printf("[skip] no decodable extended (sub_op 0x0A) C2S 0x0C in capture — apply path not "
		            "exercised (handshake absent or all-compact uplinks)\n");
		return 0; // skip clean — the round-trip needs a real extended uplink to drive
	}

	// Decode the real retail extended uplink (the captured player's per-frame movement).
	EntityPacketSubHeader hdr;
	std::size_t hdr_consumed = 0;
	if (!expect(decode_entity_packet_sub_header(uplink->payload.data(), uplink->payload.size(), hdr,
	                                            hdr_consumed),
	            "captured 0x0C sub-header decodes"))
		return 1;
	PlayerExtendedUplink up;
	std::size_t body_consumed = 0;
	if (!expect(decode_player_extended_uplink(uplink->payload.data() + hdr_consumed,
	                                          uplink->payload.size() - hdr_consumed, up, body_consumed),
	            "captured 0x0C extended (43 B) body decodes"))
		return 1;

	// Stand up a minimal authoritative host: a live World with the host's own player (sets
	// cached.local_player, which apply_player_intent refuses to snap) + one remote peer entity H.
	// No ISystems are registered, so run_logic_tick is a clean no-op tick — this isolates the net
	// round-trip (the motor's interaction with net-snapped peers is covered elsewhere).
	w::World world;
	world.registry.configure_pool(0, 16);
	w::AiSystem ai;
	world.ai = &ai;
	const w::EntityHandle host_h = w::spawn_player(world, player_spawn({0, 0, 0}, 0, 0xFFF0));
	if (!expect(host_h.valid(), "host's own player spawned")) return 1;
	const w::EntityHandle H = w::spawn_remote_player(world, player_spawn({0, 0, 0}, 0, 0xFFF1));
	if (!expect(H.valid() && H != host_h, "remote peer spawned at a distinct handle")) return 1;

	// The single owner: one in-match connection on connection_list, its link bound to the peer entity
	// + a host-role transport. burst.spawned marks it in-match so Server_TickUpdate drains + fans it.
	ns::UdpSessionTransport udp(ns::UdpSessionTransport::Role::Host);
	np::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1; // an in-match host: the S2C 0x0A replicate fan is is_in_session-gated [D-NET-120]
	np::NapiNPConnection conn;
	conn.connection_id = 3; // the joiner dcb (cosmetic here)
	conn.type = 1;          // server-side view of a remote client
	conn.link.transport = &udp;
	conn.link.mode = ns::TransportMode::Client;
	conn.link.owned_entity = H;
	conn.burst.spawned = true;
	conn.spawned_announced = true;
	conn.phase = np::ConnectionPhase::InMatch;
	ctx.np_protocol.connection_list.push_back(conn);

	// Re-target the captured uplink to OUR peer handle H (the capture's handle won't resolve in our
	// World), then deliver it as the owner would: [0x0C][sub-header][43-B body] onto the peer transport.
	EntityPacketSubHeader rt;
	rt.handle = static_cast<uint16_t>(H.packed);
	rt.item_type_id = hdr.item_type_id;
	rt.sub_op = 0x0A;
	std::vector<uint8_t> framed;
	framed.push_back(0x0C);
	{
		std::vector<uint8_t> sub = encode_entity_packet_sub_header(rt);
		std::vector<uint8_t> body = encode_player_extended_uplink(up);
		framed.insert(framed.end(), sub.begin(), sub.end());
		framed.insert(framed.end(), body.begin(), body.end());
	}
	udp.push_inbound(framed);

	// One host frame: drain the 0x0C (SNAP H), run_logic_tick (no-op), fan the per-connection 0x0A.
	np::Server_TickUpdate(ctx);

	if (!expect(udp.inbound_pending() == 0, "Server_TickUpdate drained the peer's C2S queue")) return 1;

	// The peer entity SNAPped to the uplink pose (the apply fired).
	const w::Entity *he = world.registry.get(H);
	if (!expect(he != nullptr, "peer entity present after the tick")) return 1;
	if (!expect(he->position.x == static_cast<float>(w::from_fixed(up.pos_x)) &&
	                    he->position.y == static_cast<float>(w::from_fixed(up.pos_y)) &&
	                    he->position.z == static_cast<float>(w::from_fixed(up.pos_z)),
	            "peer entity SNAPped to the captured C2S 0x0C position")) return 1;

	// Server_TickUpdate emitted exactly one S2C 0x0A on the peer's transport.
	std::vector<uint8_t> raw;
	if (!expect(udp.pop_outbound(raw), "Server_TickUpdate emitted an S2C datagram for the peer")) return 1;
	if (!expect(!raw.empty() && raw[0] == 0x0A, "emitted datagram is tag 0x0A")) return 1;
	if (!expect(!udp.pop_outbound(raw), "exactly one S2C 0x0A per connection per frame")) return 1;

	// Decode the emitted 0x0A (the witnessed §5.9 frame build_tag_0a_world_reference -> encode_frame_update
	// produces; decode_frame_update is its inverse).
	FrameUpdate fu;
	if (!expect(decode_frame_update(raw.data() + 1, raw.size() - 1, ns::class_for_type_id, fu),
	            "emitted 0x0A decodes as a well-formed §5.9 frame")) return 1;

	// SELF-CONSISTENCY: the emitted frame's anchor IS the peer's post-SNAP position (the per-connection
	// emit anchors to its owned entity). to_fixed(he->position) is exactly what anchor_for_connection
	// computed, so this ties the emitted 0x0A back to the applied C2S 0x0C — the round-trip.
	if (!expect(fu.anchor_x == w::to_fixed(he->position.x) &&
	                    fu.anchor_y == w::to_fixed(he->position.y) &&
	                    fu.anchor_z == w::to_fixed(he->position.z),
	            "emitted 0x0A anchor == peer's post-SNAP position (0x0C-in -> 0x0A-out round-trip)"))
		return 1;
	if (!expect(!fu.records.empty(), "emitted 0x0A carries the world's compact records")) return 1;

	// D-NET-143: the peer record's off-16 anim_def_index echoes the CAPTURED uplink's
	// equipped-weapon adm index (entity+0x2B0 ingest -> 0x0A echo [orig: @0x4C20A3]; this
	// table-less world accepts the byte verbatim), and off-14 carries retail's 0x2C idle2
	// spawn default (Entity_ResetToSpawnState @0x4B9714).
	for (const auto &frec : fu.records) {
		if (frec.handle != H.packed) continue;
		if (!expect(frec.player.anim_def_index == up.equipped_adm_index,
		            "emitted anim_def_index == the captured uplink's equipped adm index"))
			return 1;
		if (!expect(frec.player.anim_state_id == 0x2C,
		            "emitted anim state = 0x2C retail idle2 spawn default"))
			return 1;
	}

	// The host's own player was NOT touched by the peer's uplink (the apply was entity-scoped).
	const w::Entity *ho = world.registry.get(host_h);
	if (!expect(ho != nullptr && ho->position.x == 0.0f && ho->position.y == 0.0f &&
	                    ho->position.z == 0.0f,
	            "host's own player pose unchanged by the peer's C2S 0x0C")) return 1;

	std::printf("[golden] DEFERRED: full 0x0A body byte-parity vs the capture's own S2C 0x0A is not "
	            "asserted (our World holds host+peer only, not the capture's ASH_G3D entity set).\n");
	std::printf("OK\n");
	return 0;
}
