#include "network/udp_datagram.h"

using namespace godot;

void UdpDatagram::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_ip"), &UdpDatagram::get_ip);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "ip", PROPERTY_HINT_NONE, "",
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY),
			"", "get_ip");
	ClassDB::bind_method(D_METHOD("get_port"), &UdpDatagram::get_port);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "port", PROPERTY_HINT_NONE, "",
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY),
			"", "get_port");
	ClassDB::bind_method(D_METHOD("get_bytes"), &UdpDatagram::get_bytes);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "bytes", PROPERTY_HINT_NONE, "",
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY),
			"", "get_bytes");
}
