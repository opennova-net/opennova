// CaptureDecoder streaming-equivalence regression.
//
// The in-engine net client (godot NovaNetClient) cannot re-run the whole-capture
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
// strong multi-client / fragmented / real-SCRK case; skips clean when absent).

#include <napi/envelope.h>
#include <novacrypto/nwu.h>
#include <npwire/protocol_message.h>
#include <npwire/session_hello.h>
#include <npwire/session_keys.h>
#include <npwire/wire_capture.h>

#include <pcapio/pcap_reader.h>

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

void put_u16(std::vector<uint8_t> &b, uint16_t v) {
	b.push_back(uint8_t(v));
	b.push_back(uint8_t(v >> 8));
}

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
std::vector<uint8_t> make_proto_payload(uint8_t opcode, uint8_t tag,
                                        const std::vector<uint8_t> &inner,
                                        const std::string &scrk) {
	ProtocolMessage msg = make_protocol_message(tag, inner);
	ProtocolPacketHeader hdr{};
	hdr.session_id = 0x1234;
	std::vector<uint8_t> proto;
	encode_protocol_packet_plaintext(hdr, {msg}, scrk, proto);
	return nwu_outer_encode(opcode, proto);
}

bool msg_equal(const InGameMessage &a, const InGameMessage &b) {
	return a.frame_index == b.frame_index && a.dir == b.dir && a.tag == b.tag &&
	       a.settings_update == b.settings_update && a.session == b.session &&
	       a.payload == b.payload;
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

// A crafted two-session capture: two clients (32769, 32770) talking to the host
// (32768), each with its OWN SCRK pair, interleaved in wire order so the
// streaming decoder must keep per-session state separate exactly as the batch
// loop does. Inner payloads are opaque to wire_capture (it reassembles + emits
// them), so arbitrary bytes suffice.
std::vector<CaptureDatagram> craft_two_session_capture() {
	const std::string scrk_a = "UNIT_TEST_SCRK_A";
	const std::string scrk_b = "UNIT_TEST_SCRK_B";

	ClientAuth ca_a; ca_a.na = "alpha"; ca_a.ci = 1; ca_a.ck = 2; ca_a.scrk = scrk_a;
	ClientAuth ca_b; ca_b.na = "bravo"; ca_b.ci = 3; ca_b.ck = 4; ca_b.scrk = scrk_b;
	ServerAuth sa_a = build_server_auth(ca_a, 0x7F000001u, 32768, 0x55, scrk_a);
	ServerAuth sa_b = build_server_auth(ca_b, 0x7F000001u, 32768, 0x56, scrk_b);

	auto inner = [](uint8_t fill, size_t n) {
		return std::vector<uint8_t>(n, fill);
	};

	std::vector<CaptureDatagram> caps;
	int f = 1;
	auto add = [&](int sp, int dp, std::vector<uint8_t> payload) {
		caps.push_back({f++, sp, dp, std::move(payload)});
	};

	// Interleave the two sessions' handshakes and traffic.
	add(32768, 32769, nwu_outer_encode(SESSION_OPCODE_SERVER_AUTH, server_auth_to_bytes(sa_a)));
	add(32769, 32768, nwu_outer_encode(SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(ca_a)));
	add(32768, 32770, nwu_outer_encode(SESSION_OPCODE_SERVER_AUTH, server_auth_to_bytes(sa_b)));
	add(32770, 32768, nwu_outer_encode(SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(ca_b)));
	add(32768, 32769, make_proto_payload(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, 0x0D, inner(0xA1, 24), scrk_a));
	add(32770, 32768, make_proto_payload(SESSION_OPCODE_PROTOCOL_MESSAGE, 0x0C, inner(0xB2, 48), scrk_b));
	add(32768, 32770, make_proto_payload(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, 0x16, inner(0xB3, 12), scrk_b));
	add(32769, 32768, make_proto_payload(SESSION_OPCODE_PROTOCOL_MESSAGE, 0x06, inner(0xA2, 45), scrk_a));
	add(32768, 32769, make_proto_payload(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, 0x0A, inner(0xA3, 64), scrk_a));
	add(32768, 32770, make_proto_payload(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, 0x0A, inner(0xB4, 80), scrk_b));
	return caps;
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

	// A fresh CaptureDecoder must start empty (no leakage across instances).
	{
		CaptureDecoder dec;
		EXPECT(dec.push({1, 0, 0, {}}).empty()); // empty payload -> no message
	}

	// --- real 3-player capture (opt-in) --------------------------------------
	std::string pcap_path;
	if (const char *env = std::getenv("NW_PROBE3AGAIN_PCAP"); env && *env)
		pcap_path = env;
	else
		pcap_path = DEFAULT_PROBE3AGAIN_PCAP;

	std::vector<net::PcapDatagram> pkts;
	if (!pcap_path.empty() && net::read_pcap_udp_file(pcap_path, pkts)) {
		std::vector<CaptureDatagram> caps;
		caps.reserve(pkts.size());
		for (auto &pk : pkts)
			caps.push_back({pk.frame_index, pk.srcport, pk.dstport, std::move(pk.payload)});
		std::printf("probe3_again: %zu datagrams\n", caps.size());
		EXPECT(streaming_equals_batch(caps, "probe3_again"));
	} else {
		std::printf("[note] probe3_again capture not found (set NW_PROBE3AGAIN_PCAP) "
		            "— skipping the real-capture equivalence check\n");
	}

	if (g_failures) {
		std::printf("\n%d assertion(s) failed\n", g_failures);
		return 1;
	}
	std::printf("\nPASS: CaptureDecoder push() stream is byte-identical to the batch "
	            "decode_capture_to_messages() across sessions.\n");
	return 0;
}
