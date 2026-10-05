#pragma once

#include "network/udp_pump.h"
#include "util/string_convert.h"

#include <net/npwire/idatagram_socket.h>
#include <net/npwire/peer_addr.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace godot {

// UdpPump-backed opennova::IDatagramSocket — the Godot adapter the engine
// roles pump (the host owner loop's drain and fan, the joiner frame's deposit
// and send). A null/closed pump yields recv 0 / send no-op, so a role's socket
// legs go inert exactly as the old flag-gated code did. PeerAddr <->
// "a.b.c.d" uses the LE octet packing PeerAddr documents (octet 0 in the low
// byte; 127.0.0.1 -> 0x0100007F).
class UdpPumpDatagramSocket : public opennova::IDatagramSocket {
public:
	explicit UdpPumpDatagramSocket(UdpPump *pump) : pump_(pump) {}

	int recv_from(uint8_t *buf, std::size_t cap, opennova::PeerAddr &from) override {
		if (pump_ == nullptr || !pump_->is_open()) return 0;
		if (!pump_->has_inbound()) {
			pump_->poll();
			if (!pump_->has_inbound()) return 0;
		}
		PackedByteArray bytes;
		if (!pump_->take_inbound_native(from, bytes)) return 0;
		const std::size_t n = std::min(cap, static_cast<std::size_t>(bytes.size()));
		if (n > 0) std::memcpy(buf, bytes.ptr(), n);
		return static_cast<int>(n);
	}

	void send_to(const opennova::PeerAddr &to, const uint8_t *data, std::size_t len) override {
		if (pump_ == nullptr || !pump_->is_open() || len == 0) return;
		const std::string ip = opennova::peer_addr_ip_to_string(to);
		PackedByteArray bytes;
		bytes.resize(static_cast<int64_t>(len));
		std::memcpy(bytes.ptrw(), data, len);
		pump_->send_to(opennova::to_gd(ip), to.port, bytes);
	}

private:
	UdpPump *pump_;
};

// The hosted match's side of a pump a NovaWorld lobby session shares
// (UdpPump::demux, D-NET-346): the game's protocol is attached to the socket
// for this adapter's lifetime, so the game's queue fills only while a match
// reads it, and it keeps the shared pump alive.
class UdpPumpGameSocket : public opennova::IDatagramSocket {
public:
	explicit UdpPumpGameSocket(const Ref<UdpPump> &pump) : pump_(pump) {
		if (pump_.is_valid()) pump_->demux().set_game_attached(true);
	}
	~UdpPumpGameSocket() override {
		if (pump_.is_valid()) pump_->demux().set_game_attached(false);
	}

	int recv_from(uint8_t *buf, std::size_t cap, opennova::PeerAddr &from) override {
		if (pump_.is_null() || !pump_->is_open()) return 0;
		return pump_->demux().game().recv_from(buf, cap, from);
	}

	void send_to(const opennova::PeerAddr &to, const uint8_t *data, std::size_t len) override {
		if (pump_.is_null() || !pump_->is_open()) return;
		pump_->demux().game().send_to(to, data, len);
	}

private:
	Ref<UdpPump> pump_;
};

} // namespace godot
