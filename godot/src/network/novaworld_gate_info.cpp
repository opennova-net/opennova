#include "network/novaworld_gate_info.h"

#include "util/record_bind.h"

#include <string>

using namespace godot;

String NovaWorldGateInfo::get_post_ip() const {
	return String(std::to_string(value_.post_ip[0]).c_str()) + "." +
			String(std::to_string(value_.post_ip[1]).c_str()) + "." +
			String(std::to_string(value_.post_ip[2]).c_str()) + "." +
			String(std::to_string(value_.post_ip[3]).c_str());
}

String NovaWorldGateInfo::get_startup_url() const { return String(value_.startup_url.c_str()); }
String NovaWorldGateInfo::get_udp_novaworld() const { return String(value_.udp_novaworld.c_str()); }
String NovaWorldGateInfo::get_udp_code1() const { return String(value_.udp_code1.c_str()); }
String NovaWorldGateInfo::get_udp_code2() const { return String(value_.udp_code2.c_str()); }
String NovaWorldGateInfo::get_met_label() const { return String(value_.met_label.c_str()); }

void NovaWorldGateInfo::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(NovaWorldGateInfo, Variant::STRING, post_ip)
	OPENNOVA_RECORD_READ_ONLY(NovaWorldGateInfo, Variant::INT, post_port)
	OPENNOVA_RECORD_READ_ONLY(NovaWorldGateInfo, Variant::STRING, startup_url)
	OPENNOVA_RECORD_READ_ONLY(NovaWorldGateInfo, Variant::STRING, udp_novaworld)
	OPENNOVA_RECORD_READ_ONLY(NovaWorldGateInfo, Variant::STRING, udp_code1)
	OPENNOVA_RECORD_READ_ONLY(NovaWorldGateInfo, Variant::STRING, udp_code2)
	OPENNOVA_RECORD_READ_ONLY(NovaWorldGateInfo, Variant::STRING, met_label)
}
