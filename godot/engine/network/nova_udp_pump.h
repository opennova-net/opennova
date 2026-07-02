#pragma once

#include <godot_cpp/classes/packet_peer_udp.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <deque>

namespace godot {

// A thin raw-UDP datagram pump for the co-op-LAN net path. It wraps ONE PacketPeerUDP and moves
// RAW datagrams between the socket and the in-process net core — nothing more. Sockets + signals
// live here (the Godot binding); the protocol framing + crypto live in libs (ADR 0010), so this
// class knows nothing about message tags, the 0x0A frame, NWU/SCRK, or connections: it ships
// bytes to an address and surfaces received bytes WITH their source address.
//
// A HOST binds a listen port (bind_listen) and learns each joiner's address from the source of
// its first datagram; a JOINER dials the host (dial). NovaSimulation owns the pump and drives
// poll() before its receive step and the sends after its emit, routing each datagram to the
// right libs/netsim UdpSessionTransport by source address (a later increment). This is a
// RefCounted, not a Node — it never self-processes; its owner drives the cadence (the witnessed
// poll-before-logic / send-after order, [orig: Game_ProcessMainFrame @0x5263f0]).
class NovaUdpPump : public RefCounted {
	GDCLASS(NovaUdpPump, RefCounted)

public:
	NovaUdpPump();
	~NovaUdpPump();

	// HOST: bind a listen socket on `port` (all interfaces; 0 = an OS-assigned ephemeral port).
	// Returns a Godot Error (OK == 0).
	int bind_listen(int port);
	// JOINER: bind an ephemeral local socket and set the default destination to host:port.
	int dial(const String &host, int port);

	bool is_open() const { return socket_.is_valid(); }
	int local_port() const { return local_port_; }
	void close();

	// Drain every datagram currently available on the socket into the inbound queue, tagging each
	// with its source address. Returns the number read. Call before the receive step.
	int poll();

	bool has_inbound() const { return !inbound_.empty(); }
	int inbound_count() const { return static_cast<int>(inbound_.size()); }
	// Pop the next received datagram as {ip:String, port:int, bytes:PackedByteArray}; an empty
	// Dictionary when none. The source address routes it to its connection's transport.
	Dictionary take_inbound();

	// Send `bytes` to a specific peer (host side, per joiner) — set_dest_address + put_packet.
	int send_to(const String &ip, int port, const PackedByteArray &bytes);
	// Send `bytes` to the dialed default destination (joiner side).
	int send_to_host(const PackedByteArray &bytes);

protected:
	static void _bind_methods();

private:
	Ref<PacketPeerUDP> socket_;
	int local_port_ = 0;
	String dest_ip_;
	int dest_port_ = 0;

	struct Inbound {
		String ip;
		int port = 0;
		PackedByteArray bytes;
	};
	std::deque<Inbound> inbound_;
};

} // namespace godot
