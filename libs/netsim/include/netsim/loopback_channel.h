#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include "netsim/session_transport.h" // Datagram, ISessionTransport

namespace opennova::netsim {

// In-process bidirectional transport between the host (authoritative World tick) and
// the host's own local client: two byte FIFOs, S2C (host -> client) and C2S
// (client -> host). This is the witnessed socketless transport-mode-1 loopback the
// original uses for the host's own client [orig: CNapiNetwork_SetTransportMode @
// 0x4c8750 mode 1 — no OpenTransportSocket] (ADR 0011 Decision 2). A remote peer uses a
// socket-backed ISessionTransport of the same shape (Phase 2); MP is a transport swap.
class LoopbackChannel : public ISessionTransport {
public:
	void host_send(uint8_t tag, std::vector<uint8_t> body) override;
	void client_send(uint8_t tag, std::vector<uint8_t> body) override;
	bool host_recv(Datagram &out) override;   // pulls a C2S datagram (host side)
	bool client_recv(Datagram &out) override; // pulls an S2C datagram (client side)
	void deliver_c2s(uint8_t tag, std::vector<uint8_t> body) override; // inject into the C2S FIFO

	void clear();
	std::size_t s2c_pending() const { return s2c_.size(); }
	std::size_t c2s_pending() const { return c2s_.size(); }

private:
	std::deque<Datagram> s2c_; // host -> client
	std::deque<Datagram> c2s_; // client -> host
};

} // namespace opennova::netsim
