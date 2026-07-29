// nw_replay role-partition regression.
//
// The replay tool splits a captured session into roles purely from the NAPI
// session opcode + UDP ports (no SCRK / inner decode). This pins that:
//   - partition_roles finds the host (the common host-side port) and the
//     distinct client ports, sorted;
//   - gate/lobby/SSDP-style noise (bad envelope OR non-session opcode) is
//     ignored;
//   - each client role carries its ClientAuth + ServerAuth handshake (the SCRK
//     precondition — without it that role's stream decodes to nothing).
//
// Runs on an inline two-session capture (CI, no fixtures). The real probe3_again
// partition (host :32768 + clients :32769/:32770) is covered by the lifecycle +
// nw_replay --validate path; this test pins the algorithm.

#include "nw_replay_partition.h"

#include <napi/envelope.h>
#include <novacrypto/nwu.h>
#include <npwire/session_hello.h>
#include <npwire/session_keys.h>

#include <pcapio/pcap_reader.h>

#include <cstdint>
#include <cstdio>
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

std::vector<uint8_t> nwu_outer_encode(uint8_t opcode, std::vector<uint8_t> body) {
	if (!body.empty()) nwu_decrypt(body.data(), body.size(), SESSION_NWU_KEY);
	std::vector<uint8_t> stripped;
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

// Add one session's handshake + a protocol message each way (host port 32768).
void add_session(std::vector<net::PcapDatagram> &pkts, int &f, int client_port,
                 const std::string &scrk) {
	ClientAuth ca; ca.na = "p"; ca.ci = 1; ca.ck = 2; ca.scrk = scrk;
	ServerAuth sa = build_server_auth(ca, 0x7F000001u, 32768, 0x55, scrk);
	// ServerAuth (S2C): host -> client.
	pkts.push_back({32768, client_port, f++,
	                nwu_outer_encode(SESSION_OPCODE_SERVER_AUTH, server_auth_to_bytes(sa))});
	// ClientAuth (C2S): client -> host.
	pkts.push_back({client_port, 32768, f++,
	                nwu_outer_encode(SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(ca))});
	// One protocol message each way (opcode-only matters to the partition).
	pkts.push_back({32768, client_port, f++,
	                nwu_outer_encode(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, {1, 2, 3})});
	pkts.push_back({client_port, 32768, f++,
	                nwu_outer_encode(SESSION_OPCODE_PROTOCOL_MESSAGE, {4, 5, 6})});
}

int count_kind(const std::vector<net::PcapDatagram> &pkts, int client_port,
               replay::SessionKind want) {
	int n = 0;
	for (const auto &d : pkts) {
		if (d.srcport != client_port && d.dstport != client_port) continue;
		if (replay::classify_session(d.payload) == want) n++;
	}
	return n;
}

} // namespace

int main() {
	std::vector<net::PcapDatagram> pkts;
	int f = 1;
	add_session(pkts, f, 32769, "UNIT_TEST_SCRK_A");
	add_session(pkts, f, 32770, "UNIT_TEST_SCRK_B");

	// --- noise the capture also contains, which the partition must ignore -----
	// (a) a valid NAPI envelope with a NON-session opcode (gate-style, 0x01).
	pkts.push_back({49152, 7597, f++, nwu_outer_encode(0x01, {0xAA, 0xBB})});
	// (b) garbage that isn't a valid envelope at all (SSDP/mDNS-style).
	pkts.push_back({1900, 1900, f++, std::vector<uint8_t>{'N', 'O', 'T', 'I', 'F', 'Y'}});

	EXPECT(replay::classify_session(pkts[pkts.size() - 2].payload) ==
	       replay::SessionKind::NotSession); // non-session opcode rejected
	EXPECT(replay::classify_session(pkts[pkts.size() - 1].payload) ==
	       replay::SessionKind::NotSession); // bad envelope rejected

	const replay::Roles roles = replay::partition_roles(pkts);
	std::printf("partition: host=:%d clients=%zu\n", roles.host_port,
	            roles.client_ports.size());

	EXPECT(roles.ok());
	EXPECT(roles.host_port == 32768);
	EXPECT(roles.client_ports.size() == 2);
	if (roles.client_ports.size() == 2) {
		EXPECT(roles.client_ports[0] == 32769);
		EXPECT(roles.client_ports[1] == 32770);
	}
	EXPECT(roles.role_count() == 3);

	// session_port_of: client-kind -> src, server-kind -> dst.
	EXPECT(replay::session_port_of(pkts[0]) == 32769); // ServerAuth host->32769
	EXPECT(replay::session_port_of(pkts[1]) == 32769); // ClientAuth 32769->host
	EXPECT(replay::session_port_of(pkts[4]) == 32770); // session B ServerAuth

	// SCRK precondition: each client role carries its auth handshake both ways.
	for (int port : {32769, 32770}) {
		EXPECT(count_kind(pkts, port, replay::SessionKind::ClientAuth) >= 1);
		EXPECT(count_kind(pkts, port, replay::SessionKind::ServerAuth) >= 1);
	}

	if (g_failures) {
		std::printf("\n%d assertion(s) failed\n", g_failures);
		return 1;
	}
	std::printf("\nPASS: nw_replay partitions host + 2 clients from session opcodes, "
	            "ignores gate/SSDP noise, and each role keeps its handshake.\n");
	return 0;
}
