#pragma once

// An enumerated join's first C2S 0x42, for the tests that drive a joiner through the host's 0x81
// (D-NET-NEW-L2-25): the 0x81's receive records the session and sends nothing; the record's own
// pass pumps nothing; the join state machine's next pass starts the connect state (start = now,
// last send one resend interval back) and pumps it, so the 0x42 leaves at the first pump whose
// clock reads past that start.
// [orig: UI_EnumerateAndJoinSession @0x569F50 - g_PreMenuState = 4 @0x56A2D9;
//  MultiPlayer_JoinSessionStateMachine @0x56A320 - state 4 @0x56A4B3 -> UI_JoinSelectedSession
//  @0x5699D0 - CNapiNetwork_PumpAndCheckState (the call @0x569E60) -> PumpStateMachine case 3
//  @0x629508, the resend test @0x629588]

#include <net/npwire/nw_session_framing.h>
#include <net/npwire/session_keys.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/joiner_connection.h>

#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

namespace opennova::inmatch::test {

// A joiner on a stepped clock: the record's pass, the connect state's start, then `now_ms` one
// millisecond on and the pump that sends the 0x42. Empty when any pump disagrees.
inline std::vector<uint8_t> enumerated_join_auth(JoinerConnection &joiner, uint64_t &now_ms) {
	if (!joiner.pump(0).empty() || !joiner.pump(0).empty()) return {};
	now_ms += 1;
	std::vector<std::vector<uint8_t>> sent = joiner.pump(0);
	return sent.size() == 1 ? std::move(sent[0]) : std::vector<uint8_t>{};
}

// A joiner on the steady clock: the record's pass, then pumps until the real clock reads past the
// connect state's start (a millisecond), bounded at two seconds. Empty on a failed join.
inline std::vector<uint8_t> enumerated_join_auth(JoinerConnection &joiner) {
	if (!joiner.pump(0).empty()) return {};
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (std::chrono::steady_clock::now() < deadline) {
		std::vector<std::vector<uint8_t>> sent = joiner.pump(0);
		if (!sent.empty()) return sent.size() == 1 ? std::move(sent[0]) : std::vector<uint8_t>{};
		if (joiner.phase() == JoinerConnection::Phase::Error) return {};
	}
	return {};
}

// True when `datagram` is a C2S 0x42.
inline bool is_client_auth_datagram(const std::vector<uint8_t> &datagram) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	return nw_decode_inbound(datagram.data(), datagram.size(), opcode, body) &&
			opcode == SESSION_OPCODE_CLIENT_AUTH;
}

// A ClientRuntime on the steady clock after it received the host's 0x81: runs client frames
// until one sends the 0x42 (the 0x81's frame, the connect state's start, then the first frame
// past it in real time), bounded at two seconds. Returns that frame's datagrams, empty when no
// frame sent the 0x42 or a frame before it sent anything.
inline std::vector<std::vector<uint8_t>> frames_to_enumerated_join(
		ClientRuntime &client, uint32_t now_tick) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (std::chrono::steady_clock::now() < deadline) {
		std::vector<std::vector<uint8_t>> sent = client.Client_ProcessNetworkFrame(now_tick);
		if (sent.empty()) {
			if (client.phase() == JoinerConnection::Phase::Error) return {};
			continue;
		}
		for (const std::vector<uint8_t> &datagram : sent)
			if (is_client_auth_datagram(datagram)) return sent;
		return {};
	}
	return {};
}

} // namespace opennova::inmatch::test
