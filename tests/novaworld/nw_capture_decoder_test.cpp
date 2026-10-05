// CaptureDecoder streaming-equivalence regression.
//
// The in-engine net client (godot NovaWorldClient) cannot re-run the whole-capture
// decode_capture_to_messages() over a growing buffer every frame — that is
// O(n^2) on a multi-MB session. CaptureDecoder is the resumable form: push()
// one datagram, get the messages that completed on it, with the per-session SCRK
// pair + per-direction reassembly held across calls.
//
// This test pins the contract that makes the refactor safe: a SEQUENCE of push()
// calls produces byte-identical InGameMessages (same order, same fields, same
// payload bytes) to ONE batch decode_capture_to_messages() over the same
// datagrams. It exercises that on an inline two-session capture (runs in CI with
// no fixtures) and, when present, on the real 3-player probe3_again capture (the
// strong multi-client / fragmented / real-SCRK case; SKIP-LEG when absent).

#include <net/napi/envelope.h>
#include <net/novacrypto/nwu.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>
#include <net/npwire/wire_capture.h>

#include <base/pcapio/pcap_reader.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace opennova;

namespace {

int g_failures = 0;
#define EXPECT(cond)                                                            \
	do {                                                                        \
		if (!(cond)) {                                                          \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
			g_failures++;                                                       \
		}                                                                       \
	} while (0)

#ifndef DEFAULT_PROBE3AGAIN_PCAP
#define DEFAULT_PROBE3AGAIN_PCAP ""
#endif

// Wrap a post-opcode plaintext body into the on-wire UDP payload: outer NWU
// transform + opcode prefix + NAPI envelope (mirrors nw_replay_timeline_test).
std::vector<uint8_t> nwu_outer_encode(uint8_t opcode, std::vector<uint8_t> body) {
	if (!body.empty()) nwu_decrypt(body.data(), body.size(), SESSION_NWU_KEY);
	std::vector<uint8_t> stripped;
	stripped.reserve(body.size() + 1);
	stripped.push_back(opcode);
	stripped.insert(stripped.end(), body.begin(), body.end());
	std::vector<uint8_t> raw(stripped.size() + 16);
	size_t out = 0;
	if (napi_envelope_encode(stripped.data(), stripped.size(), raw.data(), raw.size(),
	                         &out) != 0)
		return {};
	raw.resize(out);
	return raw;
}

// SCRK-encrypted 0x43/0x83 protocol packet -> nwu_outer_encode, for a given key.
// `session_id` follows the witnessed stamping (session/protocol_message.cpp:
// hdr.session_id = crypto.session_id — the PEER's local key): S2C carries the
// client's ClientAuth.ck, C2S carries that connection's ServerAuth.sk. The
// wire_capture demux keys sessions on it (measured on the Kutu gateway capture,
// 28/28 S2C ids == ck), so a crafted capture must stamp it the same way.
std::vector<uint8_t> make_proto_payload(uint8_t opcode, uint8_t tag,
                                        const std::vector<uint8_t> &inner,
                                        const std::string &scrk,
                                        uint32_t session_id = 0x1234,
                                        uint32_t sequence = 0,
                                        uint32_t acknowledgement = 0) {
	ProtocolMessage msg = make_protocol_message(tag, inner);
	ProtocolPacketHeader hdr{};
	hdr.session_id = session_id;
	hdr.seq_num = sequence;
	hdr.ack_count = acknowledgement;
	std::vector<uint8_t> proto;
	encode_protocol_packet_plaintext(hdr, {msg}, scrk, proto);
	return nwu_outer_encode(opcode, proto);
}

std::vector<uint8_t> make_proto_payload(
		uint8_t opcode, const std::vector<ProtocolMessage> &messages,
		const std::string &scrk, uint32_t session_id = 0x1234,
		uint32_t sequence = 0,
		uint32_t acknowledgement = 0) {
	ProtocolPacketHeader hdr{};
	hdr.session_id = session_id;
	hdr.seq_num = sequence;
	hdr.ack_count = acknowledgement;
	std::vector<uint8_t> proto;
	encode_protocol_packet_plaintext(hdr, messages, scrk, proto);
	return nwu_outer_encode(opcode, proto);
}

bool msg_equal(const InGameMessage &a, const InGameMessage &b) {
	return a.frame_index == b.frame_index && a.dir == b.dir && a.tag == b.tag &&
	       a.settings_update == b.settings_update && a.session == b.session &&
	       a.participant == b.participant && a.payload == b.payload;
}

// The core assertion: pushing the datagrams one at a time through CaptureDecoder,
// concatenating each push()'s output in order, equals the batch decode.
bool streaming_equals_batch(const std::vector<CaptureDatagram> &caps,
                            const char *label) {
	const std::vector<InGameMessage> batch = decode_capture_to_messages(caps);

	CaptureDecoder dec;
	std::vector<InGameMessage> streamed;
	for (const auto &d : caps) {
		std::vector<InGameMessage> got = dec.push(d);
		for (auto &m : got) streamed.push_back(std::move(m));
	}

	if (streamed.size() != batch.size()) {
		std::printf("FAIL [%s]: streamed %zu msgs vs batch %zu\n", label,
		            streamed.size(), batch.size());
		return false;
	}
	for (size_t i = 0; i < batch.size(); ++i) {
		if (!msg_equal(streamed[i], batch[i])) {
			std::printf("FAIL [%s]: message %zu differs (frame %d/%d dir %c/%c "
			            "tag 0x%02x/0x%02x sess %d/%d payload %zu/%zu)\n",
			            label, i, streamed[i].frame_index, batch[i].frame_index,
			            streamed[i].dir, batch[i].dir, streamed[i].tag, batch[i].tag,
			            streamed[i].session, batch[i].session,
			            streamed[i].payload.size(), batch[i].payload.size());
			return false;
		}
	}
	std::printf("[%s] streaming == batch (%zu messages)\n", label, batch.size());
	return true;
}

// Two clients A and B (their own SCRK pairs, server keys 0x55/0x56) talking
// to the host on 32768. Inner payloads are opaque to wire_capture (it
// reassembles + emits them), so arbitrary bytes suffice. The three crafters
// below differ only in the client ports, the client keys, and the wire order.
struct TwoClientCapture {
	const std::string scrk[2] = {"UNIT_TEST_SCRK_A", "UNIT_TEST_SCRK_B"};
	const uint32_t server_key[2] = {0x55, 0x56};
	ClientAuth ca[2];
	ServerAuth sa[2];
	int port[2];
	std::vector<CaptureDatagram> caps;
	int f = 1;

	TwoClientCapture(int port_a, int port_b, uint32_t ck_a, uint32_t ck_b)
			: port{port_a, port_b} {
		ca[0].na = "alpha"; ca[0].ci = 1; ca[0].ck = ck_a; ca[0].scrk = scrk[0];
		ca[1].na = "bravo"; ca[1].ci = 3; ca[1].ck = ck_b; ca[1].scrk = scrk[1];
		for (int c = 0; c < 2; ++c)
			sa[c] = build_server_auth(ca[c], 0x7F000001u, 32768, server_key[c], scrk[c]);
	}
	void add(int sp, int dp, std::vector<uint8_t> payload) {
		caps.push_back({f++, sp, dp, std::move(payload)});
	}
	void server_auth(int c) {
		add(32768, port[c], nwu_outer_encode(SESSION_OPCODE_SERVER_AUTH, server_auth_to_bytes(sa[c])));
	}
	void client_auth(int c) {
		add(port[c], 32768, nwu_outer_encode(SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(ca[c])));
	}
	// S2C: the header session_id is the client's own key (ClientAuth.ck).
	void s2c(int c, uint8_t tag, uint8_t fill, size_t n, uint32_t seq, uint32_t ack) {
		add(32768, port[c], make_proto_payload(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, tag,
		                                       std::vector<uint8_t>(n, fill), scrk[c],
		                                       ca[c].ck, seq, ack));
	}
	// C2S: the header session_id is the connection's server key (ServerAuth.sk).
	void c2s(int c, uint8_t tag, uint8_t fill, size_t n, uint32_t seq, uint32_t ack) {
		add(port[c], 32768, make_proto_payload(SESSION_OPCODE_PROTOCOL_MESSAGE, tag,
		                                       std::vector<uint8_t>(n, fill), scrk[c],
		                                       server_key[c], seq, ack));
	}
};

// A crafted two-session capture: two clients (32769, 32770) interleaved in
// wire order so the streaming decoder must keep per-session state separate
// exactly as the batch loop does.
std::vector<CaptureDatagram> craft_two_session_capture() {
	TwoClientCapture t(32769, 32770, /*ck_a=*/2, /*ck_b=*/4);
	t.server_auth(0); t.client_auth(0); t.server_auth(1); t.client_auth(1);
	t.s2c(0, 0x0D, 0xA1, 24, 7, 5);
	t.c2s(1, 0x0C, 0xB2, 48, 11, 9);
	t.s2c(1, 0x16, 0xB3, 12, 10, 11);
	t.c2s(0, 0x06, 0xA2, 45, 6, 7);
	t.s2c(0, 0x0A, 0xA3, 64, 8, 6);
	t.s2c(1, 0x0A, 0xB4, 80, 11, 11);
	return t.caps;
}

// The server-side gateway shape that motivated the session_id demux: TWO
// clients whose datagrams all ride ONE client-side port (measured on a real
// gateway capture, where 42 clients' ClientAuths arrived on port 32768). A
// port-keyed session table overwrites the first client's SCRK when the second
// authenticates, so the first client's later traffic decrypts with the wrong
// key — and the tolerant plaintext parser emits garbage messages rather than
// failing. Keyed by the pre-SCRK header session_id (S2C = that client's
// ClientAuth.ck, C2S = the connection's ServerAuth.sk), both streams decode.
// Client B's auth lands BEFORE A's remaining traffic, which is exactly the
// ordering that made the port-keyed table decrypt A with B's key.
std::vector<CaptureDatagram> craft_shared_port_capture() {
	TwoClientCapture t(32769, 32769, /*ck_a=*/2, /*ck_b=*/4);
	t.server_auth(0); t.client_auth(0); t.server_auth(1); t.client_auth(1);
	t.s2c(0, 0x0D, 0xA1, 24, 7, 5);
	t.s2c(1, 0x16, 0xB3, 12, 10, 11);
	t.c2s(0, 0x06, 0xA2, 45, 6, 7);
	t.c2s(1, 0x0C, 0xB2, 48, 11, 9);
	t.s2c(0, 0x0A, 0xA3, 64, 8, 6);
	t.s2c(1, 0x0A, 0xB4, 80, 11, 11);
	return t.caps;
}

// The other half of the demux identity: session identity is (client port,
// client key), not the key alone. TWO different clients on DIFFERENT ports
// can reuse one ck value — key-alone keying merged them into a single
// session, truncating one client's stream. Each keeps its own SCRK here, so
// a merged table decrypts one of them with the wrong key.
std::vector<CaptureDatagram> craft_key_reuse_capture() {
	TwoClientCapture t(32769, 32770, /*ck_a=*/2, /*ck_b=*/2);
	t.server_auth(0); t.client_auth(0); t.server_auth(1); t.client_auth(1);
	t.s2c(0, 0x0D, 0xA1, 24, 7, 5);
	t.c2s(1, 0x0C, 0xB2, 48, 11, 9);
	t.s2c(1, 0x16, 0xB3, 12, 10, 11);
	t.c2s(0, 0x06, 0xA2, 45, 6, 7);
	t.s2c(0, 0x0A, 0xA3, 64, 8, 6);
	t.s2c(1, 0x0A, 0xB4, 80, 11, 11);
	return t.caps;
}

} // namespace

int main() {
	// --- inline two-session capture (always runs) ----------------------------
	const std::vector<CaptureDatagram> crafted = craft_two_session_capture();
	const std::vector<InGameMessage> crafted_batch = decode_capture_to_messages(crafted);
	// Sanity: the crafted capture actually produced the 6 protocol messages
	// (3 per session) — otherwise the equivalence check is vacuous.
	EXPECT(crafted_batch.size() == 6);
	int sess_a = 0, sess_b = 0;
	for (const auto &m : crafted_batch) {
		if (m.session == 32769) sess_a++;
		if (m.session == 32770) sess_b++;
	}
	EXPECT(sess_a == 3 && sess_b == 3);
	EXPECT(streaming_equals_batch(crafted, "inline-2-session"));

	// --- shared client-side port (the demux regression) ----------------------
	// Both sessions ride port 32769; only the header session_id separates them.
	// All 6 payloads must decode with their exact sizes — under port keying,
	// client B's auth overwrote A's SCRK and A's later traffic garbled.
	{
		const std::vector<CaptureDatagram> shared = craft_shared_port_capture();
		const std::vector<InGameMessage> shared_batch = decode_capture_to_messages(shared);
		EXPECT(shared_batch.size() == 6);
		size_t sizes_seen[6] = {0, 0, 0, 0, 0, 0};
		const size_t expected_sizes[6] = {24, 12, 45, 48, 64, 80};
		for (const auto &m : shared_batch) {
			for (int i = 0; i < 6; ++i) {
				if (m.payload.size() == expected_sizes[i]) sizes_seen[i]++;
			}
		}
		for (int i = 0; i < 6; ++i) EXPECT(sizes_seen[i] == 1);
		// The port cannot tell the two apart (both 32769); the participant key
		// can: A is participant 1, B is 2, three messages each, and B's are
		// exactly the 12/48/80-byte bodies.
		int by_participant[3] = {0, 0, 0};
		bool b_bodies_only = true;
		for (const auto &m : shared_batch) {
			EXPECT(m.session == 32769);
			if (m.participant >= 1 && m.participant <= 2) by_participant[m.participant]++;
			else by_participant[0]++;
			if (m.participant == 2 &&
			    !(m.payload.size() == 12 || m.payload.size() == 48 || m.payload.size() == 80))
				b_bodies_only = false;
		}
		EXPECT(by_participant[0] == 0 && by_participant[1] == 3 && by_participant[2] == 3);
		EXPECT(b_bodies_only);
		EXPECT(streaming_equals_batch(shared, "shared-port-2-session"));
	}

	// --- one client key reused across DIFFERENT client ports -----------------
	// Identity is (client port, client key): key-alone keying merged these two
	// sessions and decrypted one with the other's SCRK.
	{
		const std::vector<CaptureDatagram> reuse = craft_key_reuse_capture();
		const std::vector<InGameMessage> reuse_batch = decode_capture_to_messages(reuse);
		EXPECT(reuse_batch.size() == 6);
		int port_a = 0, port_b = 0;
		for (const auto &m : reuse_batch) {
			if (m.session == 32769) port_a++;
			if (m.session == 32770) port_b++;
		}
		EXPECT(port_a == 3 && port_b == 3);
		EXPECT(streaming_equals_batch(reuse, "key-reuse-2-port"));
	}

	// The detailed capture seam reports every decrypted 0x43/0x83 datagram,
	// including its session header, independently of message reassembly. This is
	// the deterministic sequence/ACK oracle used by opennova-wire --sequencing and the
	// retail/OpenNova semantic diff loop.
	{
		CaptureDecoder decoder;
		std::vector<CapturedSessionPacket> packets;
		std::vector<CapturedDatagramResult> datagrams;
		for (const CaptureDatagram &datagram : crafted) {
			CaptureDecodeResult decoded = decoder.push_detailed(datagram);
			EXPECT(decoded.datagrams.size() == 1);
			packets.insert(packets.end(), decoded.session_packets.begin(),
			               decoded.session_packets.end());
			datagrams.insert(datagrams.end(), decoded.datagrams.begin(),
			                 decoded.datagrams.end());
		}
		EXPECT(datagrams.size() == crafted.size());
		if (datagrams.size() == crafted.size()) {
			EXPECT(datagrams[0].frame_index == crafted[0].frame_index);
			EXPECT(datagrams[0].src_port == crafted[0].src_port);
			EXPECT(datagrams[0].dst_port == crafted[0].dst_port);
			EXPECT(datagrams[0].payload_length == crafted[0].payload.size());
			EXPECT(datagrams[0].outer_decoded);
			EXPECT(datagrams[0].opcode == SESSION_OPCODE_SERVER_AUTH);
			EXPECT(datagrams[0].datagram_class == CaptureDatagramClass::ServerAuth);
			EXPECT(datagrams[0].decoded);
			EXPECT(datagrams[4].datagram_class ==
			       CaptureDatagramClass::ServerProtocol);
			EXPECT(datagrams[4].decoded);
		}
		EXPECT(packets.size() == 6);
		if (packets.size() == 6) {
			EXPECT(packets[0].frame_index == 5);
			EXPECT(packets[0].dir == 'S');
			EXPECT(packets[0].session == 32769);
			EXPECT(packets[0].header.session_id == 2); // ca_a.ck, the witnessed S2C stamp
			EXPECT(packets[0].header.seq_num == 7);
			EXPECT(packets[0].header.ack_count == 5);
			EXPECT(packets[0].header.connection_flags == 0);
			EXPECT(packets[0].tags == std::vector<uint16_t>({0x0D}));
			EXPECT(packets[0].records.size() == 1);
			if (packets[0].records.size() == 1) {
				EXPECT(packets[0].records[0].full_tag == 0x0D);
				EXPECT(packets[0].records[0].raw_flags == PROTOCOL_MSG_FLAG_LEN8);
				EXPECT(packets[0].records[0].encoded_length == 24);
				EXPECT(packets[0].records[0].skip_bytes.empty());
			}
			EXPECT(packets[5].frame_index == 10);
			EXPECT(packets[5].dir == 'S');
			EXPECT(packets[5].session == 32770);
			EXPECT(packets[5].header.seq_num == 11);
			EXPECT(packets[5].header.ack_count == 11);
			EXPECT(packets[5].tags == std::vector<uint16_t>({0x0A}));
		}

		const CaptureDecodeResult batch = decode_capture(crafted);
		EXPECT(batch.session_packets.size() == packets.size());
		EXPECT(batch.messages.size() == crafted_batch.size());
		EXPECT(batch.datagrams.size() == crafted.size());
	}

	// Datagram accounting is total: malformed envelopes, unknown opcodes,
	// protocol packets captured before their SCRK, and packets that fail the
	// plaintext parser after auth each produce one explicit negative result.
	{
		CaptureDecoder decoder;
		const CaptureDecodeResult malformed = decoder.push_detailed({50, 1, 2, {}});
		EXPECT(malformed.datagrams.size() == 1);
		if (malformed.datagrams.size() == 1) {
			EXPECT(!malformed.datagrams[0].outer_decoded);
			EXPECT(malformed.datagrams[0].datagram_class ==
			       CaptureDatagramClass::Invalid);
			EXPECT(!malformed.datagrams[0].decoded);
		}

		const CaptureDecodeResult unknown = decoder.push_detailed(
				{51, 1, 2, nwu_outer_encode(0x45, {0x01})});
		EXPECT(unknown.datagrams.size() == 1);
		if (unknown.datagrams.size() == 1) {
			EXPECT(unknown.datagrams[0].outer_decoded);
			EXPECT(unknown.datagrams[0].opcode == 0x45);
			EXPECT(unknown.datagrams[0].datagram_class ==
			       CaptureDatagramClass::Unknown);
			EXPECT(!unknown.datagrams[0].decoded);
		}

		const CaptureDecodeResult missing_key = decoder.push_detailed(
				{52, 32769, 32768,
				 make_proto_payload(SESSION_OPCODE_PROTOCOL_MESSAGE, 0x0C,
				                    {0x01, 0x02}, "NOT_INSTALLED")});
		EXPECT(missing_key.datagrams.size() == 1);
		if (missing_key.datagrams.size() == 1) {
			EXPECT(missing_key.datagrams[0].outer_decoded);
			EXPECT(missing_key.datagrams[0].datagram_class ==
			       CaptureDatagramClass::ClientProtocol);
			EXPECT(!missing_key.datagrams[0].decoded);
		}

		const CaptureDecodeResult auth = decoder.push_detailed(crafted[1]);
		EXPECT(auth.datagrams.size() == 1 && auth.datagrams[0].decoded);
		const CaptureDecodeResult invalid_protocol = decoder.push_detailed(
				{53, 32769, 32768,
				 nwu_outer_encode(SESSION_OPCODE_PROTOCOL_MESSAGE, {0x01, 0x02})});
		EXPECT(invalid_protocol.datagrams.size() == 1);
		if (invalid_protocol.datagrams.size() == 1) {
			EXPECT(invalid_protocol.datagrams[0].outer_decoded);
			EXPECT(invalid_protocol.datagrams[0].datagram_class ==
			       CaptureDatagramClass::ClientProtocol);
			EXPECT(!invalid_protocol.datagrams[0].decoded);
		}
	}

	// Packet evidence must preserve physical record framing even when a
	// fragment does not yet produce a semantic InGameMessage. This is the
	// verifier's ordered LEN/SKIP/fragment witness, not a second parser.
	{
		CaptureDecoder decoder;
		for (size_t i = 0; i < 4; ++i) {
			(void)decoder.push_detailed(crafted[i]);
		}
		ProtocolMessage fragment = make_protocol_message(
				0x55, {0x01, 0x02, 0x03},
				PROTOCOL_MSG_FLAG_LEN8 | PROTOCOL_MSG_FLAG_SKIP2 |
						PROTOCOL_MSG_FLAG_FRAG_CONT);
		fragment.skip_bytes = {0xAA, 0xBB};
		CaptureDatagram datagram{
				99, 32768, 32769,
				make_proto_payload(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE,
				                   std::vector<ProtocolMessage>{fragment},
				                   "UNIT_TEST_SCRK_A", 2, 12, 10)};
		const CaptureDecodeResult decoded = decoder.push_detailed(datagram);
		EXPECT(decoded.messages.empty());
		EXPECT(decoded.session_packets.size() == 1);
		if (decoded.session_packets.size() == 1) {
			const CapturedSessionPacket &packet = decoded.session_packets[0];
			EXPECT(packet.tags == std::vector<uint16_t>({0x55}));
			EXPECT(packet.records.size() == 1);
			if (packet.records.size() == 1) {
				EXPECT(packet.records[0].full_tag == 0x55);
				EXPECT(packet.records[0].raw_flags == 0x34);
				EXPECT(packet.records[0].encoded_length == 3);
				EXPECT(packet.records[0].skip_bytes ==
				       std::vector<uint8_t>({0xAA, 0xBB}));
			}
		}
	}

	// A fresh CaptureDecoder must start empty (no leakage across instances).
	{
		CaptureDecoder dec;
		EXPECT(dec.push({1, 0, 0, {}}).empty()); // compatibility projection unchanged
	}

	if (g_failures) {
		std::printf("\n%d assertion(s) failed\n", g_failures);
		return 1;
	}
	std::printf("\nPASS: CaptureDecoder push() stream is byte-identical to the batch "
	            "decode_capture_to_messages() across sessions.\n");
	return 0;
}
