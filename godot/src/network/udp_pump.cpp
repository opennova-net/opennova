#include "network/nova_udp_pump.h"

#include <godot_cpp/classes/ip.hpp>
#include <godot_cpp/core/error_macros.hpp>

#include <utility>

namespace godot {

namespace {

// The env var that names the capture file. Unset (the default) means the pump
// never opens a file and every record_ call returns on its first branch.
constexpr const char *kCaptureEnvVar = "NW_CAPTURE_WRITE";

// This end of every recorded datagram. The socket binds 0.0.0.0, so there is no
// single local address to report; loopback keeps the synthesized IP header valid
// and the PORTS — which is what partitions a session downstream — exact.
constexpr uint32_t kLocalIp = 0x7F000001u; // 127.0.0.1

// Dotted-quad -> host-order u32. Anything that is not plain IPv4 (a hostname
// that resolved elsewhere, or an IPv6 peer) records as 0.0.0.0 rather than
// guessing: the ports still identify the flow, and a wrong address would read
// as fact.
uint32_t ipv4_from_string(const String &ip) {
	const PackedStringArray parts = ip.split(".");
	if (parts.size() != 4) return 0u;
	uint32_t out = 0u;
	for (int i = 0; i < 4; ++i) {
		const String &part = parts[i];
		if (part.is_empty() || !part.is_valid_int()) return 0u;
		const int64_t v = part.to_int();
		if (v < 0 || v > 255) return 0u;
		out = (out << 8) | uint32_t(v);
	}
	return out;
}

} // namespace

UdpPump::UdpPump() {}
UdpPump::~UdpPump() { close(); }

void UdpPump::record_(bool inbound, const String &peer_ip, int peer_port,
		const PackedByteArray &bytes) {
	if (capture_ == nullptr || bytes.is_empty()) return;
	const uint32_t peer = ipv4_from_string(peer_ip);
	const uint16_t peer_p = uint16_t(peer_port);
	const uint16_t local_p = uint16_t(local_port_);
	const uint8_t *data = reinterpret_cast<const uint8_t *>(bytes.ptr());
	const size_t len = size_t(bytes.size());
	const uint64_t ts = opennova::net::pcap_now_nanos();
	if (inbound) {
		capture_->write(peer, peer_p, kLocalIp, local_p, data, len, ts);
	} else {
		capture_->write(kLocalIp, local_p, peer, peer_p, data, len, ts);
	}
}

int UdpPump::bind_listen(int port) {
	close();
	socket_.instantiate();
	const Error err = socket_->bind(port, "0.0.0.0");
	if (err != OK) {
		socket_.unref();
		return static_cast<int>(err);
	}
	local_port_ = static_cast<int>(socket_->get_local_port());
	capture_ = opennova::net::PcapUdpWriter::from_env(kCaptureEnvVar);
	if (capture_ != nullptr) {
		print_verbose(String("UdpPump: recording host traffic (port ") +
				itos(local_port_) + ")");
	}
	return static_cast<int>(OK);
}

int UdpPump::dial(const String &host, int port) {
	close();
	const String resolved_host = IP::get_singleton()->resolve_hostname(host, IP::TYPE_IPV4);
	if (resolved_host.is_empty()) return static_cast<int>(ERR_CANT_RESOLVE);

	socket_.instantiate();
	const Error err = socket_->bind(0, "0.0.0.0");
	if (err != OK) {
		socket_.unref();
		return static_cast<int>(err);
	}
	local_port_ = static_cast<int>(socket_->get_local_port());
	// Retain a numeric endpoint: packet sources are reported numerically, so
	// names such as "localhost" can be compared when poll() admits packets.
	dest_ip_ = resolved_host;
	dest_port_ = port;
	socket_->set_dest_address(dest_ip_, port);
	capture_ = opennova::net::PcapUdpWriter::from_env(kCaptureEnvVar);
	if (capture_ != nullptr) {
		print_verbose(String("UdpPump: recording joiner traffic to ") +
				dest_ip_ + ":" + itos(dest_port_));
	}
	return static_cast<int>(OK);
}

void UdpPump::close() {
	// Closed before the socket so a capture is complete on disk even if the
	// process is killed right after teardown starts.
	capture_.reset();
	if (socket_.is_valid()) socket_->close();
	socket_.unref();
	local_port_ = 0;
	dest_ip_ = String();
	dest_port_ = 0;
	inbound_.clear();
}

int UdpPump::poll() {
	if (!socket_.is_valid()) return 0;
	int n = 0;
	while (socket_->get_available_packet_count() > 0) {
		const PackedByteArray pkt = socket_->get_packet();
		// get_packet_ip/port report the source of the LAST get_packet() — read them after.
		Inbound in;
		in.ip = socket_->get_packet_ip();
		in.port = static_cast<int>(socket_->get_packet_port());
		// A listener accepts every source to discover joiners. A dialed pump is
		// bound to exactly one resolved endpoint; discard injected datagrams at
		// the UDP boundary before any protocol consumer can observe them.
		if (dest_port_ != 0 && (in.ip != dest_ip_ || in.port != dest_port_)) continue;
		in.bytes = pkt;
		// Recorded after the source filter, so a capture holds exactly the
		// datagrams a protocol consumer could observe — a rejected injection
		// never appears as traffic we processed.
		record_(true, in.ip, in.port, in.bytes);
		inbound_.push_back(std::move(in));
		++n;
	}
	return n;
}

Dictionary UdpPump::take_inbound() {
	Dictionary d;
	if (inbound_.empty()) return d;
	Inbound in = std::move(inbound_.front());
	inbound_.pop_front();
	d["ip"] = in.ip;
	d["port"] = in.port;
	d["bytes"] = in.bytes;
	return d;
}

int UdpPump::send_to(const String &ip, int port, const PackedByteArray &bytes) {
	if (!socket_.is_valid()) return static_cast<int>(ERR_UNCONFIGURED);
	socket_->set_dest_address(ip, port);
	const Error err = socket_->put_packet(bytes);
	// Only a datagram the socket accepted is recorded: a capture should show
	// what we put on the wire, not what we attempted.
	if (err == OK) record_(false, ip, port, bytes);
	return static_cast<int>(err);
}

int UdpPump::send_to_host(const PackedByteArray &bytes) {
	if (!socket_.is_valid() || dest_port_ == 0) return static_cast<int>(ERR_UNCONFIGURED);
	socket_->set_dest_address(dest_ip_, dest_port_);
	const Error err = socket_->put_packet(bytes);
	if (err == OK) record_(false, dest_ip_, dest_port_, bytes);
	return static_cast<int>(err);
}

void UdpPump::_bind_methods() {
	ClassDB::bind_method(D_METHOD("bind_listen", "port"), &UdpPump::bind_listen);
	ClassDB::bind_method(D_METHOD("dial", "host", "port"), &UdpPump::dial);
	ClassDB::bind_method(D_METHOD("is_open"), &UdpPump::is_open);
	ClassDB::bind_method(D_METHOD("local_port"), &UdpPump::local_port);
	ClassDB::bind_method(D_METHOD("close"), &UdpPump::close);
	ClassDB::bind_method(D_METHOD("poll"), &UdpPump::poll);
	ClassDB::bind_method(D_METHOD("is_capturing"), &UdpPump::is_capturing);
	ClassDB::bind_method(D_METHOD("inbound_count"), &UdpPump::inbound_count);
	ClassDB::bind_method(D_METHOD("take_inbound"), &UdpPump::take_inbound);
	ClassDB::bind_method(D_METHOD("send_to", "ip", "port", "bytes"), &UdpPump::send_to);
	ClassDB::bind_method(D_METHOD("send_to_host", "bytes"), &UdpPump::send_to_host);
}

} // namespace godot
