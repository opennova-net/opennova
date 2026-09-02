#include "network/novaworld_server_totals.h"

using namespace godot;

void NovaWorldServerTotals::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_total_servers"), &NovaWorldServerTotals::get_total_servers);
	ClassDB::bind_method(D_METHOD("set_total_servers", "count"),
			&NovaWorldServerTotals::set_total_servers);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "total_servers"), "set_total_servers",
			"get_total_servers");
	ClassDB::bind_method(D_METHOD("get_total_players"), &NovaWorldServerTotals::get_total_players);
	ClassDB::bind_method(D_METHOD("set_total_players", "count"),
			&NovaWorldServerTotals::set_total_players);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "total_players"), "set_total_players",
			"get_total_players");
}
