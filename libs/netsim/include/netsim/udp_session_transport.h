#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include "netsim/session_transport.h" // Datagram, ISessionTransport

namespace opennova::netsim {

// A socket-free ISessionTransport: the same {tag,body} datagram shape as LoopbackChannel, but
// its OUTBOUND side stages raw datagram bytes for an owner to ship over a real socket and its
// INBOUND side accepts raw datagram bytes the owner received. The owner — the Godot binding's
// PacketPeerUDP pump (a later increment) or a unit-test harness — does the actual sendto /
// recvfrom; this class holds no socket so it unit-tests with no OS dependency. It is the
// witnessed mode-2/3/4 socket transport of the SAME shape as the mode-1 loopback (ADR 0011;
// [orig: CNapiNetwork_SetTransportMode @0x4c8750 / CNapiNetwork_OpenTransportSocket @0x4c6a40
// open a socket for 2/3/4 — only sendto/recvfrom differ from the in-process loopback]).
//
// FRAMING: this increment uses the identity codec (a datagram serializes as [tag][body...]
// and parses back). The real ProtocolMessage / NWU / CRC / SCRK framing (libs/novaworld +
// napi + novacrypto) is layered by the OWNER outside this class — a documented swap point for
// the real-socket increment, keeping the transport a byte mover and the framing/crypto in the
// reusable libs (ADR 0010).
//
// The four ISessionTransport methods are symmetric over one outbound + one inbound FIFO: any
// SEND stages outbound, any RECV pops inbound. A consumer uses only the two that match its
// side (a host endpoint: host_send / host_recv; a client endpoint: client_send / client_recv).
// The owner carries one endpoint's outbound into the other endpoint's inbound — exactly what a
// real UDP socket pair does.
class UdpSessionTransport : public ISessionTransport {
public:
	// Which side of the connection this endpoint sits on (informational; the queues are
	// symmetric). A Host endpoint ships S2C and receives C2S; a Client endpoint ships C2S and
	// receives S2C.
	enum class Role : uint8_t { Host, Client };

	explicit UdpSessionTransport(Role role) : role_(role) {}

	// --- ISessionTransport: the netsim core uses ONLY these four ---
	void host_send(uint8_t tag, std::vector<uint8_t> body) override;   // -> outbound (S2C)
	void client_send(uint8_t tag, std::vector<uint8_t> body) override; // -> outbound (C2S)
	bool host_recv(Datagram &out) override;   // <- inbound (C2S, host endpoint)
	bool client_recv(Datagram &out) override; // <- inbound (S2C, client endpoint)
	void deliver_c2s(uint8_t tag, std::vector<uint8_t> body) override; // inject into inbound FIFO

	// --- owner byte boundary (the PacketPeerUDP pump / the harness) ---
	// Stage a raw datagram that arrived on the wire; reframed into the inbound FIFO.
	void push_inbound(const std::vector<uint8_t> &raw);
	// Pop one raw datagram to ship on the wire; false when nothing is queued.
	bool pop_outbound(std::vector<uint8_t> &raw);

	bool has_outbound() const { return !outbound_.empty(); }
	std::size_t outbound_pending() const { return outbound_.size(); }
	std::size_t inbound_pending() const { return inbound_.size(); }
	Role role() const { return role_; }
	void clear();

private:
	// Identity framing this increment: [tag][body...]. The swap point for the real codec.
	static std::vector<uint8_t> frame(uint8_t tag, std::vector<uint8_t> body);
	static bool unframe(const std::vector<uint8_t> &raw, Datagram &out);
	bool pop_inbound(Datagram &out);

	Role role_;
	std::deque<std::vector<uint8_t>> outbound_; // raw bytes to ship over the wire
	std::deque<Datagram> inbound_;              // datagrams parsed from received raw bytes
};

} // namespace opennova::netsim
