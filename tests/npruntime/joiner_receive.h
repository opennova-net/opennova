#pragma once

// A test's datagram through the joiner's production receive, as ClientRuntime's frame runs it
// (ClientRuntime::manager_receive_pass, then the client pump): the NAPI manager's receive of
// the datagram, whose socket legs (the 0x84 resend answers, the 0x85 pong) write their
// datagrams at once, collected in immediate_outbound; then the protocol's client pump until it
// drains, its packets' results in the PollResult.
// [orig: Game_ProcessMainFrame @0x5263F0 - CNapiNetwork_PumpManagerReceive @0x4C4D10 (the call
//  @0x526528), then CNapiNetwork_PumpClientProtocolRecv @0x4C4FE0 (the call @0x526692)]

#include <runtime/inmatch/joiner_connection.h>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace joiner_receive {

struct Received : opennova::inmatch::JoinerConnection::PollResult {
	std::vector<std::vector<uint8_t>> immediate_outbound;
};

inline Received receive(opennova::inmatch::JoinerConnection &joiner, const uint8_t *raw,
		std::size_t len) {
	Received out;
	opennova::inmatch::JoinerConnection::ManagerPass pass = joiner.manager_receive(raw, len);
	out.immediate_outbound = std::move(pass.immediate_outbound);
	while (joiner.pump_client_queue(out)) {
	}
	return out;
}

} // namespace joiner_receive
