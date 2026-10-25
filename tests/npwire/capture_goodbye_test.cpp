// The capture decoder's session-opcode goodbyes: a teardown's C2S 0x46
// ClientGoodBye and S2C 0x86 ServerGoodBye carry the receiver's key dword and
// the sender's latched disconnect record, its seven fields in the writer's
// order [orig: CNapiNPConnection_SendDisconnectPacket @0x61F2A0 (the key dword
// @0x61F3AF, the fields @0x61F3D0..0x61F4AA); the receiver Nwu_HandleDisconnect
// @0x623CE0]. Such a datagram is classified and decoded; one without that
// shape stays Unknown.
//
// The datagrams are minted with the functions our senders use (the joiner's
// burst and the host's: client_goodbye_to_bytes / server_goodbye_to_bytes
// wrapped by nw_encode_outbound). Retail's own bursts are proven in place by
// the parity judge over the suite's x4 kick cells (parity-20261021-x4
// tdh-i3a-tdm-kick RR, RR-aa, RO), whose goodbyes decode with these classes.

#include <net/npwire/nw_session_framing.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>
#include <net/npwire/wire_capture.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace opennova;

namespace {

int g_failures = 0;
#define EXPECT(cond)                                                    \
	do {                                                                \
		if (!(cond)) {                                                  \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
			g_failures++;                                               \
		}                                                               \
	} while (0)

constexpr int kJoinerPort = 32768;
constexpr int kHostPort = 32787;

CapturedDatagramResult classify(const std::vector<uint8_t> &payload, int src, int dst) {
	CaptureDecoder decoder;
	const CaptureDecodeResult result = decoder.push_detailed({7, src, dst, payload});
	EXPECT(result.datagrams.size() == 1);
	return result.datagrams.empty() ? CapturedDatagramResult{} : result.datagrams[0];
}

// A goodbye the decoder classifies, whose key dword and record read back as written.
void expect_goodbye(const std::vector<uint8_t> &payload, int src, int dst, uint8_t opcode,
		CaptureDatagramClass cls, uint32_t key, const DisconnectEvent &written) {
	const CapturedDatagramResult d = classify(payload, src, dst);
	EXPECT(d.outer_decoded);
	EXPECT(d.opcode == opcode);
	EXPECT(d.datagram_class == cls);
	EXPECT(d.decoded);
	uint8_t read_opcode = 0;
	std::vector<uint8_t> body;
	EXPECT(nw_decode_inbound(payload.data(), payload.size(), read_opcode, body));
	EXPECT(read_opcode == opcode && body.size() > 4);
	if (body.size() <= 4) return;
	EXPECT(uint32_t(body[0] | body[1] << 8 | body[2] << 16 | uint32_t(body[3]) << 24) == key);
	DisconnectEvent read;
	EXPECT(parse_disconnect_event(body.data() + 4, body.size() - 4, read));
	EXPECT(read.ds == written.ds && read.dc == written.dc && read.dp1 == written.dp1 &&
			read.dp2 == written.dp2 && read.dstr == written.dstr && read.dpc == written.dpc &&
			read.ddstr == written.ddstr);
}

void expect_unknown(const std::vector<uint8_t> &payload, int src, int dst, uint8_t opcode) {
	const CapturedDatagramResult d = classify(payload, src, dst);
	EXPECT(d.outer_decoded);
	EXPECT(d.opcode == opcode);
	EXPECT(d.datagram_class == CaptureDatagramClass::Unknown);
	EXPECT(!d.decoded);
}

// The punt's record as the host latches it and the joiner echoes it, a user
// leave's, the zero record; the host's goodbye of the punt.
void test_minted_bursts_decode() {
	const DisconnectEvent punt = make_disconnect_event(1, 2, 0, 0, "", 33, "S.C:SPA");
	const DisconnectEvent leave = make_disconnect_event(2, 2, 0, 0, "I.C:CIDEMIS", 0, "");
	const uint32_t host_sk = 0x329324E5u, joiner_ck = 0x3DAE4F20u;
	expect_goodbye(nw_encode_outbound(SESSION_OPCODE_CLIENT_GOODBYE,
						   client_goodbye_to_bytes(host_sk, punt)),
			kJoinerPort, kHostPort, SESSION_OPCODE_CLIENT_GOODBYE,
			CaptureDatagramClass::ClientGoodBye, host_sk, punt);
	expect_goodbye(nw_encode_outbound(SESSION_OPCODE_CLIENT_GOODBYE,
						   client_goodbye_to_bytes(host_sk, leave)),
			kJoinerPort, kHostPort, SESSION_OPCODE_CLIENT_GOODBYE,
			CaptureDatagramClass::ClientGoodBye, host_sk, leave);
	expect_goodbye(nw_encode_outbound(SESSION_OPCODE_CLIENT_GOODBYE,
						   client_goodbye_to_bytes(host_sk)),
			kJoinerPort, kHostPort, SESSION_OPCODE_CLIENT_GOODBYE,
			CaptureDatagramClass::ClientGoodBye, host_sk, DisconnectEvent{});
	expect_goodbye(nw_encode_outbound(SESSION_OPCODE_SERVER_GOODBYE,
						   server_goodbye_to_bytes(joiner_ck, punt)),
			kHostPort, kJoinerPort, SESSION_OPCODE_SERVER_GOODBYE,
			CaptureDatagramClass::ServerGoodBye, joiner_ck, punt);
}

// Bytes no stock peer writes stay Unknown: no key dword, the fields out of the
// writer's order (which the receiver's lenient walk still reads), a record cut
// inside its last field.
void test_other_shapes_stay_unknown() {
	expect_unknown(nw_encode_outbound(SESSION_OPCODE_CLIENT_GOODBYE, {1, 2, 3}), kJoinerPort,
			kHostPort, SESSION_OPCODE_CLIENT_GOODBYE);
	const DisconnectEvent punt = make_disconnect_event(1, 2, 0, 0, "", 33, "S.C:SPA");
	std::vector<uint8_t> written = server_goodbye_to_bytes(0x3DAE4F20u, punt);
	const std::vector<uint8_t> record = connection_description_to_bytes(punt);
	// DS and DC are each "XX\0", a u16 length 4 and a dword: 9 bytes. Swap them.
	std::vector<uint8_t> reordered(written.begin(), written.begin() + 4);
	reordered.insert(reordered.end(), record.begin() + 9, record.begin() + 18);
	reordered.insert(reordered.end(), record.begin(), record.begin() + 9);
	reordered.insert(reordered.end(), record.begin() + 18, record.end());
	EXPECT(reordered.size() == written.size());
	DisconnectEvent lenient;
	EXPECT(parse_disconnect_event(reordered.data() + 4, reordered.size() - 4, lenient) &&
			lenient.dc == 2 && lenient.dpc == 33);
	expect_unknown(nw_encode_outbound(SESSION_OPCODE_SERVER_GOODBYE, reordered), kHostPort,
			kJoinerPort, SESSION_OPCODE_SERVER_GOODBYE);
	written.resize(written.size() - 3);
	expect_unknown(nw_encode_outbound(SESSION_OPCODE_SERVER_GOODBYE, written), kHostPort,
			kJoinerPort, SESSION_OPCODE_SERVER_GOODBYE);
}

} // namespace

int main() {
	test_minted_bursts_decode();
	test_other_shapes_stay_unknown();
	if (g_failures == 0) std::printf("capture_goodbye: all checks passed\n");
	return g_failures == 0 ? 0 : 1;
}
