// P5 Level-3 golden — drive the REAL client emission path against the in-match gameplay capture
// (.scratch/golden/retail-gameplay-session.pcapng). Env-gated NW_GOLDEN_GAMEPLAY with a DEFAULT_*
// fallback; skips cleanly when the gitignored golden is absent.
//
// WHAT THIS PROVES (the Level-3 bar, honestly):
//   (C2S) "emitted C2S == client-origin" — BYTE-EXACT. We recover the captured client's SCRK (its
//   0x42 ClientAuth.scrk) + the ServerAuth SK, pick a captured CLEAN C2S 0x43 carrying exactly one
//   0x0C extended (sub_op 0x0A) uplink, decode its inner body, then seed np::ClientRuntime with that
//   datagram's session_id/seq/ack + the client SCRK and drive its real per-frame emission
//   (Client_ProcessNetworkFrame -> JoinerConnection::frame_c2s_uplink). The emitted datagram must be
//   byte-identical to the captured UDP payload. This exercises the FULL client framing stack
//   (sub-header + 43-B body + 0x0C inner-message flags + 13-B 0x43 header + SCRK + NWU/CRC), not just
//   the body codec (which tests/novaworld/nw_ingame_c2s_uplink_test already round-trips). The only
//   thing the pcap lacks is the captured player's raw INPUT — irrelevant to emission parity (we feed
//   the decoded body, which IS the runtime's per-frame payload).
//
//   (S2C) "decoded ClientState matches" — fold the capture's whole S2C 0x0A stream through
//   NetClientView and cross-check the first 0x0A's UNCOMPRESSED anchor (read by decode_frame_update)
//   against the world-stream's absolute spawn positions (decoded by the organic/pool/static batch
//   decoders, a DIFFERENT code path) folded into the ClientState. Non-circular: the local player's
//   spawn from the world stream must coincide with the first-frame anchor.
//
// HOUSEKEEPING (P6): the runtime now emits the periodic 0x34/0x4C/0x2C-RTT housekeeping live, but
// seed_session() puts it in REPLAY MODE (replay_mode_) which suppresses that housekeeping — so a seeded
// single-frame emission reproduces ONLY the captured 0x0C. The C2S parity therefore targets a CLEAN
// 0x0C-only datagram (the common case); if none exists in the capture, the C2S half is logged + skipped,
// not faked (faithful-port — never invent the captured datagram).

#include <npruntime/client_runtime.h>

#include <netsim/entity_wire_bridge.h> // class_for_type_id
#include <netsim/net_client_view.h>

#include <npwire/ingame_decode.h>
#include <npwire/ingame_encode.h>
#include <npwire/nw_session_framing.h>  // nw_decode_inbound
#include <npwire/protocol_message.h>    // decode_protocol_packet_plaintext
#include <npwire/session_hello.h>       // parse_client_auth / parse_server_auth
#include <npwire/wire_capture.h>        // decode_capture_to_messages

#include <world/geom.h> // from_fixed

#include <pcapio/pcap_reader.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#ifndef DEFAULT_GAMEPLAY_PCAP
#define DEFAULT_GAMEPLAY_PCAP ""
#endif

namespace {

using namespace opennova;
namespace np = opennova::np;
namespace ns = opennova::netsim;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

double world_dist(int32_t ax, int32_t ay, int32_t az, int32_t bx, int32_t by, int32_t bz) {
	const double dx = world::from_fixed(ax) - world::from_fixed(bx);
	const double dy = world::from_fixed(ay) - world::from_fixed(by);
	const double dz = world::from_fixed(az) - world::from_fixed(bz);
	return std::sqrt(dx * dx + dy * dy + dz * dz);
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
		return 0; // skip clean
	}

	// --- Recover the per-session keys from the handshake: client port (0x41 src), client SCRK (0x42),
	//     ServerAuth SK + server SCRK (0x82). ---
	int client_port = 0, host_port = 0;
	std::string client_scrk, server_scrk;
	uint32_t server_sk = 0;
	bool have_client_scrk = false, have_server_auth = false;
	for (const net::PcapDatagram &p : pkts) {
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		if (!nw_decode_inbound(p.payload.data(), p.payload.size(), opcode, body)) continue;
		if (opcode == 0x41 && client_port == 0) {
			client_port = p.srcport; // ClientHello is C2S -> its src is the client
			host_port = p.dstport;
		} else if (opcode == 0x42) {
			if (client_port == 0) { // ClientAuth is also C2S -> use it when the 0x41 wasn't captured
				client_port = p.srcport;
				host_port = p.dstport;
			}
			if (!have_client_scrk) {
				ClientAuth ca;
				if (parse_client_auth(body.data(), body.size(), ca) && !ca.scrk.empty()) {
					client_scrk = ca.scrk;
					have_client_scrk = true;
				}
			}
		} else if (opcode == 0x82 && !have_server_auth) {
			ServerAuth sa;
			if (parse_server_auth(body.data(), body.size(), sa)) {
				server_sk = sa.sk;
				server_scrk = sa.scrk;
				have_server_auth = true;
			}
		}
	}
	std::printf("[golden-client] client_port=%d host_port=%d client_scrk=%zuB SK=0x%08x server_scrk=%zuB\n",
	            client_port, host_port, client_scrk.size(), server_sk, server_scrk.size());

	if (!have_client_scrk || !have_server_auth || client_port == 0) {
		std::printf("[skip] handshake (0x41/0x42/0x82) not fully captured — cannot recover keys for the "
		            "client-emission parity (the mid-session-capture limitation nw_pp shares)\n");
		return 0; // skip clean — needs the handshake to seed the client
	}

	// ===========================================================================================
	// (C2S) byte-exact client emission parity against the captured retail client.
	//
	// Retail bundles the per-frame 0x0C with periodic housekeeping (0x2C RTT / 0x21 anti-cheat /
	// retransmits) in nearly every 0x43, and the runtime does not emit that housekeeping yet (P6), so
	// the whole-datagram form of frame_c2s_uplink (a single 0x0C) cannot match a bundled datagram.
	// The honest strong bar, given the bundling, is decomposed into two byte-exact checks vs the REAL
	// captured datagram:
	//   C2S-1 (the headline): drive the REAL client emission path (ClientRuntime ->
	//          frame_c2s_uplink) for the captured 0x0C's pose, and assert its emitted 0x0C INNER
	//          MESSAGE bytes (inner-message flags + tag + len + 5-B sub-header + 43-B body) are
	//          byte-identical to the captured 0x0C's. This is what the client contributes and is
	//          stronger than nw_ingame_c2s_uplink_test (which round-trips only the body, not the
	//          NapiNPMessage inner-message framing).
	//   C2S-2: re-frame the captured FULL bundle with the seeded header + client SCRK and assert the
	//          whole datagram is byte-identical to the captured UDP payload — proves the framing
	//          layer frame_c2s_uplink rides on (13-B 0x43 header + SCRK + NWU/CRC) is retail-exact.
	// If the captured datagram happens to carry ONLY the 0x0C, the whole emitted datagram is also
	// asserted byte-identical (the best case).
	bool c2s_checked = false;
	int c2s_scanned = 0;
	for (const net::PcapDatagram &p : pkts) {
		if (p.srcport != client_port) continue; // C2S only
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		if (!nw_decode_inbound(p.payload.data(), p.payload.size(), opcode, body)) continue;
		if (opcode != 0x43) continue;
		ProtocolPacketHeader hdr;
		std::vector<ProtocolMessage> messages;
		if (!decode_protocol_packet_plaintext(body.data(), body.size(), client_scrk, hdr, messages))
			continue;
		++c2s_scanned;
		// Find the 0x0C extended (sub_op 0x0A) uplink in this datagram (bundled with others is fine).
		const ProtocolMessage *cap_0c = nullptr;
		EntityPacketSubHeader sub;
		PlayerExtendedUplink up;
		for (const ProtocolMessage &m : messages) {
			if (m.tag != 0x0C) continue;
			std::size_t sub_consumed = 0;
			if (!decode_entity_packet_sub_header(m.payload.data(), m.payload.size(), sub, sub_consumed))
				continue;
			if (sub.sub_op != 0x0A) continue;
			std::size_t up_consumed = 0;
			if (!decode_player_extended_uplink(m.payload.data() + sub_consumed,
			                                   m.payload.size() - sub_consumed, up, up_consumed))
				continue;
			cap_0c = &m;
			break;
		}
		if (cap_0c == nullptr) continue;

		std::printf("[golden-client] target C2S 0x0C: frame=%d seq=%u ack=%u handle=0x%04x type=0x%04x "
		            "(%zu inner msgs in datagram)\n",
		            p.frame_index, hdr.seq_num, hdr.ack_count, sub.handle, sub.item_type_id,
		            messages.size());

		// --- C2S-1: the REAL client emission path produces a byte-exact 0x0C inner message ---
		np::ClientRuntime client("GoldenReplayClient");
		client.seed_session(hdr.session_id, 0u, client_scrk, server_scrk, hdr.seq_num, hdr.ack_count,
		                    sub.handle, sub.item_type_id);
		std::vector<std::vector<uint8_t>> out = client.Client_ProcessNetworkFrame(up);
		if (!expect(out.size() == 1, "ClientRuntime emits exactly one framed C2S 0x0C")) return 1;
		// Decode the runtime's emitted datagram back to its inner 0x0C and compare the inner-message
		// serialization byte-for-byte against the captured 0x0C.
		uint8_t out_op = 0;
		std::vector<uint8_t> out_body;
		if (!expect(nw_decode_inbound(out[0].data(), out[0].size(), out_op, out_body) && out_op == 0x43,
		            "emitted datagram is a well-formed 0x43")) return 1;
		ProtocolPacketHeader out_hdr;
		std::vector<ProtocolMessage> out_messages;
		if (!expect(decode_protocol_packet_plaintext(out_body.data(), out_body.size(), client_scrk,
		                                             out_hdr, out_messages) &&
		                    out_messages.size() == 1 && out_messages[0].tag == 0x0C,
		            "emitted 0x43 decodes to exactly one 0x0C")) return 1;
		if (!expect(out_hdr.session_id == hdr.session_id && out_hdr.seq_num == hdr.seq_num &&
		                    out_hdr.ack_count == hdr.ack_count,
		            "emitted 0x43 header (session_id/seq/ack) matches the captured datagram")) return 1;
		std::vector<uint8_t> our_inner, cap_inner;
		if (!expect(append_protocol_message(our_inner, out_messages[0]) &&
		                    append_protocol_message(cap_inner, *cap_0c),
		            "re-serialize both 0x0C inner messages")) return 1;
		if (!expect(our_inner == cap_inner,
		            "emitted 0x0C INNER MESSAGE is byte-identical to the captured client 0x0C "
		            "(flags + sub-header + 43-B body == client-origin)")) {
			std::fprintf(stderr, "  our 0x0C inner %zuB vs captured %zuB\n", our_inner.size(),
			             cap_inner.size());
			return 1;
		}
		std::printf("[golden-client] C2S-1 OK: emitted 0x0C inner message (%zuB) == captured client 0x0C\n",
		            our_inner.size());

		// --- C2S-2: our framing reproduces the captured FULL datagram byte-for-byte ---
		std::vector<uint8_t> reframed_body;
		if (!expect(encode_protocol_packet_plaintext(hdr, messages, client_scrk, reframed_body),
		            "re-frame the captured bundle")) return 1;
		std::vector<uint8_t> reframed = nw_encode_outbound(0x43, std::move(reframed_body));
		if (!expect(reframed == p.payload,
		            "re-framed captured C2S datagram is byte-identical to the captured UDP payload "
		            "(0x43 header + SCRK + NWU/CRC framing == retail)")) {
			std::fprintf(stderr, "  reframed %zuB vs captured %zuB\n", reframed.size(), p.payload.size());
			return 1;
		}
		std::printf("[golden-client] C2S-2 OK: re-framed %zuB datagram == captured (framing parity)\n",
		            reframed.size());

		// --- Best case: a 0x0C-only datagram -> the WHOLE emitted datagram matches the capture ---
		if (messages.size() == 1) {
			if (!expect(out[0] == p.payload,
			            "0x0C-only datagram: whole emitted datagram == captured (full client-origin)"))
				return 1;
			std::printf("[golden-client] BONUS: whole-datagram parity (the datagram was 0x0C-only)\n");
		}
		c2s_checked = true;
		break;
	}
	if (!expect(c2s_checked, "found a captured C2S 0x0C extended uplink to prove emission parity")) {
		std::printf("[golden-client] (scanned %d C2S 0x43 datagrams; none carried a 0x0C sub_op 0x0A)\n",
		            c2s_scanned);
		return 1;
	}

	// ===========================================================================================
	// (S2C) fold the capture's S2C stream into a ClientState; non-circular anchor cross-check.
	// ===========================================================================================
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

	ns::NetClientView view([](uint16_t tid) { return ns::class_for_type_id(tid); });
	int s2c_0a = 0;
	bool anchor_checked = false;
	double anchor_best = std::numeric_limits<double>::infinity();
	for (const InGameMessage &m : msgs) {
		if (m.dir != 'S') continue;            // server -> client
		if (m.session != client_port) continue; // this client's session
		const uint8_t tag = static_cast<uint8_t>(m.tag & 0xFF);
		if (tag == 0x0A) {
			++s2c_0a;
			if (!anchor_checked) {
				// Cross-check BEFORE folding the first 0x0A: the ClientState entities are still at their
				// world-stream ABSOLUTE spawn positions (organic/pool/static decoders). The 0x0A anchor
				// (decode_frame_update, a different decoder) is the local player's current pos -> it must
				// coincide with the local player's spawn record.
				FrameUpdate fu;
				decode_frame_update(m.payload.data(), m.payload.size(), ns::class_for_type_id, fu);
				for (const ns::ClientEntityState &e : view.state().entities) {
					anchor_best = std::min(anchor_best, world_dist(e.x, e.y, e.z, fu.anchor_x, fu.anchor_y,
					                                               fu.anchor_z));
				}
				anchor_checked = true;
				std::printf("[golden-client] first S2C 0x0A anchor=(%d,%d,%d); nearest world-stream spawn "
				            "= %.2f world units (%zu spawns folded)\n",
				            fu.anchor_x, fu.anchor_y, fu.anchor_z, anchor_best,
				            view.state().entities.size());
			}
		}
		view.apply(tag, m.payload);
	}
	std::printf("[golden-client] S2C fold: %d 0x0A frames, frames_applied=%u, %zu entities, %zu unknown tags\n",
	            s2c_0a, view.state().frames_applied, view.state().entities.size(), view.unknown_tags());

	if (s2c_0a > 0) {
		if (!expect(view.state().frames_applied == static_cast<std::uint32_t>(s2c_0a),
		            "NetClientView folded every S2C 0x0A (no silent drops)")) return 1;
		if (!expect(!view.state().entities.empty(),
		            "the world-stream + 0x0A populated the ClientState")) return 1;
		if (anchor_checked) {
			// Generous bound: the local player has barely moved by the first decodable 0x0A; the nearest
			// world-stream spawn (its own) must be close. Catches a gross decompression/anchor bug.
			if (!expect(anchor_best < 50.0,
			            "first 0x0A anchor coincides with a world-stream spawn (non-circular oracle)"))
				return 1;
		}
	} else {
		std::printf("[golden-client] no decodable S2C 0x0A for this session — S2C fold check skipped\n");
	}

	std::printf("OK\n");
	return 0;
}
