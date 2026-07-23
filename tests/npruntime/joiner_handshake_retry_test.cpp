// A lossy UDP transport must not turn either pre-session join leg into a one-shot. Drive the public
// ClientRuntime seam with the first 0x41 and first 0x42 dropped, then prove the timed retries are the
// exact same retail wire datagrams (identity, HK, CI/CK, and SCRK included) and complete the real
// npruntime host handshake.

#include <npruntime/client_runtime.h>
#include <npruntime/napi_np_protocol.h>

#include "host_test_setup.h"

#include <npwire/nw_session_framing.h>
#include <npwire/session_keys.h>

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using namespace opennova;
namespace np = opennova::np;

bool expect(bool condition, const char *message) {
	if (condition) return true;
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

bool is_opcode(const std::vector<uint8_t> &datagram, uint8_t expected) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	return nw_decode_inbound(datagram.data(), datagram.size(), opcode, body) && opcode == expected;
}

bool run_dropped_hello_and_auth_recover() {
	constexpr uint64_t kRetailActiveSendIntervalMs = 1000;
	const PeerAddr peer{0x0100007Fu, 32769};

	np::NapiNPServerCtx host;
	uint64_t now_ms = 0;
	np::ClientRuntime client("RetryJoiner", [&now_ms] { return now_ms; });
	const std::vector<uint8_t> first_hello = client.start();
	if (!expect(is_opcode(first_hello, SESSION_OPCODE_CLIENT_HELLO),
	            "start emits retail ClientHello 0x41")) return false;
	// The first datagram reaches a host whose session is not up yet, which retail silently drops.
	np::HandleResult cold_result = np::handle_server_datagram(
	        host, peer, first_hello.data(), first_hello.size(), 0);
	if (!expect(cold_result.outbound.empty(), "cold host drops the initial ClientHello")) return false;
	// The initial send anchored the wall-clock deadline. Keep the unrelated simulation tick fixed to
	// prove render/simulation cadence cannot shorten or lengthen this pre-session timeout.
	if (!expect(client.Client_ProcessNetworkFrame(7).empty(),
	            "handshake retry timer arms without an immediate duplicate")) return false;
	now_ms = kRetailActiveSendIntervalMs - 1;
	if (!expect(client.Client_ProcessNetworkFrame(7).empty(),
	            "ClientHello is not retried before the active-send interval")) return false;

	// Bring the host session up before the retry becomes eligible.
	np::test::bring_up_host(host, np::ConnectionMode::HostClient,
	                        np::SocketMode::Socketless, 0x0FE0E112u);
	now_ms = kRetailActiveSendIntervalMs;
	auto hello_retry = client.Client_ProcessNetworkFrame(7);
	if (!expect(hello_retry.size() == 1 && hello_retry.front() == first_hello,
	            "dropped ClientHello retries byte-identically at the interval")) return false;

	// Deliver only the retry. The real host answers it; processing that answer emits ClientAuth.
	np::HandleResult hello_result = np::handle_server_datagram(
	        host, peer, hello_retry.front().data(), hello_retry.front().size(), 1);
	if (!expect(hello_result.outbound.size() == 1 &&
	                    is_opcode(hello_result.outbound.front(), SESSION_OPCODE_SERVER_HELLO),
	            "real host accepts the retried ClientHello")) return false;
	client.receive(hello_result.outbound.front().data(), hello_result.outbound.front().size());
	auto auth_out = client.Client_ProcessNetworkFrame(9999);
	if (!expect(auth_out.size() == 1 &&
	                    is_opcode(auth_out.front(), SESSION_OPCODE_CLIENT_AUTH),
	            "ServerHello advances the joiner to ClientAuth 0x42")) return false;
	const std::vector<uint8_t> first_auth = auth_out.front();

	// Drop first_auth too. Reusing the original bytes is important: changing CI/CK/SCRK would make
	// the host's retail retransmit-dedup path a different connection rather than a resend.
	now_ms = 2 * kRetailActiveSendIntervalMs - 1;
	if (!expect(client.Client_ProcessNetworkFrame(1).empty(),
	            "ClientAuth is not retried before the active-send interval")) return false;
	now_ms = 2 * kRetailActiveSendIntervalMs;
	auto auth_retry = client.Client_ProcessNetworkFrame(1);
	if (!expect(auth_retry.size() == 1 && auth_retry.front() == first_auth,
	            "dropped ClientAuth retries byte-identically at the interval")) return false;

	np::HandleResult auth_result = np::handle_server_datagram(
	        host, peer, auth_retry.front().data(), auth_retry.front().size(), 2);
	if (!expect(!auth_result.outbound.empty(), "real host accepts the retried ClientAuth")) return false;
	for (const std::vector<uint8_t> &reply : auth_result.outbound)
		client.receive(reply.data(), reply.size());
	auto post_auth = client.Client_ProcessNetworkFrame(300);
	if (!expect(client.phase() == np::JoinerConnection::Phase::Driving,
	            "retried pre-session handshake reaches the in-match drive")) return false;
	for (const std::vector<uint8_t> &datagram : post_auth) {
		if (!expect(!is_opcode(datagram, SESSION_OPCODE_CLIENT_AUTH),
		            "ServerAuth cancels the ClientAuth retry")) return false;
	}
	return true;
}

} // namespace

int main() {
	if (!run_dropped_hello_and_auth_recover()) return 1;
	std::printf("OK\n");
	return 0;
}
