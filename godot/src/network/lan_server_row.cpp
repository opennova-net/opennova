#include "network/lan_server_row.h"
#include "util/record_bind.h"
#include "util/string_convert.h"

#include <base/io/cp1252.h>
#include <net/npwire/lan_discovery.h>

using namespace godot;

void LanServerRow::_bind_methods() {
	LAN_SERVER_ROW_FIELDS(OPENNOVA_RECORD_FIELD)
	ClassDB::bind_static_method("LanServerRow", D_METHOD("make", "server_name", "host_ip", "port"),
			&LanServerRow::make);
	ClassDB::bind_method(D_METHOD("list_label"), &LanServerRow::list_label);
}

String LanServerRow::list_label() const {
	return opennova::cp1252_to_gd(opennova::lan_discovery_row_label(
			opennova::utf8_to_retail_text(opennova::to_std(server_name_)),
			opennova::utf8_to_retail_text(opennova::to_std(expansion_)),
			static_cast<int32_t>(players_), static_cast<int32_t>(max_players_)));
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
