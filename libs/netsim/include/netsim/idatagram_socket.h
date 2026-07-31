#pragma once

#include <cstddef>
#include <cstdint>

#include <npwire/peer_addr.h> // opennova::PeerAddr

namespace opennova::netsim {

// The real-socket seam. A minimal datagram-socket abstraction the in-match owner loop (the
// libs/npruntime host/client per-frame loops) pumps, so the loop itself holds NO socket — libs
// stay socket-free (libs/CLAUDE.md). The socket owners provide the adapter: apps/nw_server wraps
// net::Socket (apps/common/net_datagram_socket.h), godot/engine wraps NovaUdpPump. This is the
// ONE owner-loop implementation's only door to the wire — drift between the headless server and
// the Godot layer (the host_owner_loop.h <-> nova_simulation.cpp copy) is what promoting the loop
// over this interface eliminates.
//
// Distinct from the in-process transports (Loopback/UdpSessionTransport): those move INNER
// identity {tag,body} frames between endpoints; this moves WHOLE datagrams (NWU/CRC envelope
// onward) over a real wire. The owner loop speaks PeerAddr; each adapter does the
// PeerAddr<->native-address conversion.
class IDatagramSocket {
public:
	virtual ~IDatagramSocket() = default;

	// Drain ONE pending datagram into `buf` (capacity `cap`); set `from` to its source. Returns
	// bytes received (>0), 0 when nothing is pending, <0 on error. NON-BLOCKING "drain what's
	// available" — the owner loop calls it in a loop until <= 0. Maps onto both
	// net::udp_recv_from(timeout) and NovaUdpPump.poll()+take_inbound().
	virtual int recv_from(uint8_t *buf, std::size_t cap, PeerAddr &from) = 0;

	// Send `len` bytes to `to`.
	virtual void send_to(const PeerAddr &to, const uint8_t *data, std::size_t len) = 0;
};

} // namespace opennova::netsim
