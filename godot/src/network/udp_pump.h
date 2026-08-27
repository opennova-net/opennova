#pragma once

#include <godot_cpp/classes/packet_peer_udp.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <base/pcapio/pcap_writer.h>

#include <deque>
#include <memory>

namespace godot {

// A thin raw-UDP datagram pump for the co-op-LAN net path. It wraps ONE PacketPeerUDP and moves
// RAW datagrams between the socket and the in-process net core — nothing more. Sockets + signals
// live here (the Godot binding); the protocol framing + crypto live in libs (ADR 0010), so this
// class knows nothing about message tags, the 0x0A frame, NWU/SCRK, or connections: it ships
// bytes to an address and surfaces received bytes WITH their source address.
//
// A HOST binds a listen port (bind_listen) and learns each joiner's address from the source of
// its first datagram; a JOINER dials the host (dial). Simulation owns the pump and drives
// poll() before its receive step and the sends after its emit, routing each datagram to the
// right engine/net/netsim UdpSessionTransport by source address (a later increment). This is a
// RefCounted, not a Node — it never self-processes; its owner drives the cadence (the witnessed
// poll-before-logic / send-after order, [orig: Game_ProcessMainFrame @0x5263f0]).
class UdpPump : public RefCounted {
	GDCLASS(UdpPump, RefCounted)

public:
	UdpPump();
	~UdpPump();

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

	// The pcap every datagram this pump moves is appended to, opened by the
	// next bind_listen/dial; "" (the default) records nothing. The runtime
	// passes `--capture-pcap <path>` (LaunchFlags) through Simulation.
	void set_capture_path(const String &path) { capture_path_ = path; }
	String get_capture_path() const { return capture_path_; }
	bool is_capturing() const { return capture_ != nullptr; }

protected:
	static void _bind_methods();

private:
	// SELF-CAPTURE. When a capture path is set, every datagram this pump moves
	// is appended to a pcap in the shape the repo's own readers and
	// `apps/nw_pp` consume — the same legacy/DLT_RAW shape the retail-side hook
	// writes, so a session recorded here and one recorded from the original
	// game can be decoded by the same tool and compared per (direction, tag).
	//
	// This records what the APPLICATION sent and received, not what left the
	// NIC: no OS fragmentation, no retransmit timing, checksums synthesized.
	// For wire-coverage work that is the more useful cut (and it needs no
	// Npcap install); for questions about the network stack itself, an
	// external dumpcap capture is still the right instrument.
	void record_(bool inbound, const String &peer_ip, int peer_port,
			const PackedByteArray &bytes);

	std::unique_ptr<opennova::net::PcapUdpWriter> capture_;
	String capture_path_;

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
