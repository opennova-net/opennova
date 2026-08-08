#include "network/nova_udp_pump.h"

#include <godot_cpp/classes/ip.hpp>
#include <godot_cpp/core/error_macros.hpp>

#include <utility>

namespace godot {

NovaUdpPump::NovaUdpPump() {}
NovaUdpPump::~NovaUdpPump() { close(); }

int NovaUdpPump::bind_listen(int port) {
	close();
	socket_.instantiate();
	const Error err = socket_->bind(port, "0.0.0.0");
	if (err != OK) {
		socket_.unref();
		return static_cast<int>(err);
	}
	local_port_ = static_cast<int>(socket_->get_local_port());
	return static_cast<int>(OK);
}

int NovaUdpPump::dial(const String &host, int port) {
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
	return static_cast<int>(OK);
}

void NovaUdpPump::close() {
	if (socket_.is_valid()) socket_->close();
	socket_.unref();
	local_port_ = 0;
	dest_ip_ = String();
	dest_port_ = 0;
	inbound_.clear();
}

int NovaUdpPump::poll() {
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
		inbound_.push_back(std::move(in));
		++n;
	}
	return n;
}

Dictionary NovaUdpPump::take_inbound() {
	Dictionary d;
	if (inbound_.empty()) return d;
	Inbound in = std::move(inbound_.front());
	inbound_.pop_front();
	d["ip"] = in.ip;
	d["port"] = in.port;
	d["bytes"] = in.bytes;
	return d;
}

int NovaUdpPump::send_to(const String &ip, int port, const PackedByteArray &bytes) {
	if (!socket_.is_valid()) return static_cast<int>(ERR_UNCONFIGURED);
	socket_->set_dest_address(ip, port);
	return static_cast<int>(socket_->put_packet(bytes));
}

int NovaUdpPump::send_to_host(const PackedByteArray &bytes) {
	if (!socket_.is_valid() || dest_port_ == 0) return static_cast<int>(ERR_UNCONFIGURED);
	socket_->set_dest_address(dest_ip_, dest_port_);
	return static_cast<int>(socket_->put_packet(bytes));
}

void NovaUdpPump::_bind_methods() {
	ClassDB::bind_method(D_METHOD("bind_listen", "port"), &NovaUdpPump::bind_listen);
	ClassDB::bind_method(D_METHOD("dial", "host", "port"), &NovaUdpPump::dial);
	ClassDB::bind_method(D_METHOD("is_open"), &NovaUdpPump::is_open);
	ClassDB::bind_method(D_METHOD("local_port"), &NovaUdpPump::local_port);
	ClassDB::bind_method(D_METHOD("close"), &NovaUdpPump::close);
	ClassDB::bind_method(D_METHOD("poll"), &NovaUdpPump::poll);
	ClassDB::bind_method(D_METHOD("has_inbound"), &NovaUdpPump::has_inbound);
	ClassDB::bind_method(D_METHOD("inbound_count"), &NovaUdpPump::inbound_count);
	ClassDB::bind_method(D_METHOD("take_inbound"), &NovaUdpPump::take_inbound);
	ClassDB::bind_method(D_METHOD("send_to", "ip", "port", "bytes"), &NovaUdpPump::send_to);
	ClassDB::bind_method(D_METHOD("send_to_host", "bytes"), &NovaUdpPump::send_to_host);
}

} // namespace godot
