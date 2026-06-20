#pragma once

#include <cstdint>
#include <deque>
#include <vector>

namespace opennova::netsim {

// One framed in-process datagram: an in-match message tag + its body bytes — the
// unit the host serializes S2C and the local client decodes. The byte path is
// identical to the socket transport; only `sendto`/`recvfrom` are skipped. This is
// the socketless transport-mode-1 loopback the original uses for the host's own
// local client [orig: CNapiNetwork_SetTransportMode @ 0x4c8750 mode 1 —
// CNapiNetwork_OpenTransportSocket @ 0x4c6a40 is reached only for modes 2/3/4]
// (ADR 0011 Decision 2).
struct Datagram {
	uint8_t tag = 0;
	std::vector<uint8_t> body;
};

// In-process bidirectional channel between the host (authoritative World tick) and
// the host's own local client. Two byte FIFOs: S2C (host -> client) and C2S
// (client -> host). Phase 4 swaps this for the witnessed UDP transport behind the
// same shape so MP is a transport swap, not a rewrite.
class LoopbackChannel {
public:
	// host -> client (server replication frames).
	void host_send(uint8_t tag, std::vector<uint8_t> body);
	// client -> host (the C2S 0x0C input uplink, Phase 2+).
	void client_send(uint8_t tag, std::vector<uint8_t> body);

	// Drain one pending datagram FIFO-order; false when the queue is empty.
	bool host_recv(Datagram &out);   // pulls a C2S datagram (host side)
	bool client_recv(Datagram &out); // pulls an S2C datagram (client side)

	void clear();
	std::size_t s2c_pending() const { return s2c_.size(); }
	std::size_t c2s_pending() const { return c2s_.size(); }

private:
	std::deque<Datagram> s2c_; // host -> client
	std::deque<Datagram> c2s_; // client -> host
};

} // namespace opennova::netsim
