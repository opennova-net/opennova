#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

// One received datagram popped from UdpPump.take_inbound: the source address
// and the bytes verbatim. Read-only, assigned by the pump.
class UdpDatagram : public RefCounted {
	GDCLASS(UdpDatagram, RefCounted)

public:
	void assign(const String &p_ip, int p_port, const PackedByteArray &p_bytes) {
		ip_ = p_ip;
		port_ = p_port;
		bytes_ = p_bytes;
	}
	String get_ip() const { return ip_; }
	int get_port() const { return port_; }
	PackedByteArray get_bytes() const { return bytes_; }

protected:
	static void _bind_methods();

private:
	String ip_;
	int port_ = 0;
	PackedByteArray bytes_;
};

} // namespace godot
