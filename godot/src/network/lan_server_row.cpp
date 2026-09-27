#include "network/lan_server_row.h"
#include "util/record_bind.h"

using namespace godot;

void LanServerRow::_bind_methods() {
	LAN_SERVER_ROW_FIELDS(OPENNOVA_RECORD_FIELD)
	ClassDB::bind_static_method("LanServerRow", D_METHOD("make", "server_name", "host_ip", "port"),
			&LanServerRow::make);
}

Ref<LanServerRow> LanServerRow::make(const String &p_server_name, const String &p_host_ip,
		int p_port) {
	Ref<LanServerRow> out;
	out.instantiate();
	out->set_server_name(p_server_name);
	out->set_host_ip(p_host_ip);
	out->set_port(p_port);
	return out;
}
