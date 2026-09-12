#pragma once

#include <cstdint>
#include <vector>

namespace opennova::replication {

// One framed in-match datagram: a message tag + its body bytes — the unit the host
// serializes S2C and a client decodes. The byte path is identical across transports;
// only `sendto`/`recvfrom` differ (ADR 0011).
struct Datagram {
	uint8_t tag = 0;
	std::vector<uint8_t> body;
	// Semantic send-queue metadata only; identity/raw framing does not encode it.
	bool reliable = true;
	// Exact inner ProtocolMessage flags selected by a semantic producer. Zero
	// lets the owner choose LEN8/LEN16 normally. A high-table record such as the
	// retail connection-description punt carries 0xA0 here so it cannot be
	// mistaken for ordinary low tag 0x03 while crossing the owner boundary.
	uint8_t protocol_flags_raw = 0;
	// Owner-side message-pool exemption. This survives semantic transport queues
	// but is never represented by identity framing or ProtocolMessage wire bits.
	bool capacity_exempt = false;
	// Zero retains a reliable record until ACK; a nonzero value expires it
	// after this many completed send flushes, including its first send.
	uint32_t retention_flushes = 0;
};

// The byte transport between the authoritative host and one client, abstracted so the
// in-match net core (the connection-fan primitives / ClientReplicaPipeline) stays
// transport-agnostic. The host's own local client is a LoopbackChannel — the witnessed
// socketless transport mode 1; a remote LAN/MP peer is a UDP-backed transport of the
// SAME shape (modes 2/3/4) [orig: CNapiNetwork_SetTransportMode @ 0x4c8750;
// CNapiNetwork_OpenTransportSocket @ 0x4c6a40 opens a socket only for 2/3/4]. Co-op over
// LAN is this one core with socket transports in place of the loopback — a transport
// swap, not a rewrite. A connection carries its original transport mode (ADR 0011).
class ISessionTransport {
public:
	virtual ~ISessionTransport() = default;

	// host -> client (server replication frames). `reliable=false` mirrors a
	// retail NapiNPMessage with userParam1=1: it is sent once, then pruned before
	// a later NACK can reconstruct the packet sequence.
	virtual void host_send(
			uint8_t tag, std::vector<uint8_t> body, bool reliable = true,
			uint8_t protocol_flags_raw = 0,
			bool capacity_exempt = false, uint32_t retention_flushes = 0) = 0;
	// client -> host (the C2S 0x0C input uplink).
	virtual void client_send(uint8_t tag, std::vector<uint8_t> body) = 0;

	// Drain one pending datagram in FIFO order; false when the queue is empty.
	virtual bool host_recv(Datagram &out) = 0;   // a C2S datagram (host side)
	virtual bool client_recv(Datagram &out) = 0; // an S2C datagram (client side)

	// Inject a C2S {tag,body} datagram directly into the host-side inbound queue (the one
	// host_recv pops). The owner-boundary consumer (inmatch::apply_in_match_c2s) uses this to route a
	// decoded in-match C2S 0x0C onto the owning connection's transport, so Server_TickUpdate's
	// drain_connection_c2s read-applies it — the production form of the manual push the joiner
	// test does inline. Distinct from client_send: a host endpoint's client_send would stage to
	// OUTBOUND (never reaches host_recv), so a uniform inject method is required. [PeerC2SInMatch
	// consumer; orig: NapiNPProtocol_HandleSessionPacket 0x0C surface -> the host recv FIFO]
	virtual void deliver_c2s(uint8_t tag, std::vector<uint8_t> body) = 0;
};

} // namespace opennova::replication
