#include "network/novaworld_gate_info.h"

#include "util/record_bind.h"
#include "util/string_convert.h"

using namespace godot;

String NovaWorldGateInfo::get_post_ip() const {
	return String::num_int64(value_.post_ip[0]) + "." + String::num_int64(value_.post_ip[1]) + "." +
			String::num_int64(value_.post_ip[2]) + "." + String::num_int64(value_.post_ip[3]);
}

// The gate response's text fields are the gate's wire bytes.
String NovaWorldGateInfo::get_startup_url() const { return opennova::cp1252_to_gd(value_.startup_url); }
String NovaWorldGateInfo::get_udp_novaworld() const { return opennova::cp1252_to_gd(value_.udp_novaworld); }
String NovaWorldGateInfo::get_udp_code1() const { return opennova::cp1252_to_gd(value_.udp_code1); }
String NovaWorldGateInfo::get_udp_code2() const { return opennova::cp1252_to_gd(value_.udp_code2); }
String NovaWorldGateInfo::get_met_label() const { return opennova::cp1252_to_gd(value_.met_label); }

void NovaWorldGateInfo::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(NovaWorldGateInfo, Variant::STRING, post_ip)
	OPENNOVA_RECORD_READ_ONLY(NovaWorldGateInfo, Variant::INT, post_port)
	OPENNOVA_RECORD_READ_ONLY(NovaWorldGateInfo, Variant::STRING, startup_url)
	OPENNOVA_RECORD_READ_ONLY(NovaWorldGateInfo, Variant::STRING, udp_novaworld)
	OPENNOVA_RECORD_READ_ONLY(NovaWorldGateInfo, Variant::STRING, udp_code1)
	OPENNOVA_RECORD_READ_ONLY(NovaWorldGateInfo, Variant::STRING, udp_code2)
	OPENNOVA_RECORD_READ_ONLY(NovaWorldGateInfo, Variant::STRING, met_label)
}
