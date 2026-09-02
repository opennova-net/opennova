#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>

namespace godot {

// The GSB response's list-wide totals (NovaWorldClient.get_server_totals):
// the service-wide server and player counts the retail browser shows as its
// population line. Zeros until the first list lands.
class NovaWorldServerTotals : public RefCounted {
	GDCLASS(NovaWorldServerTotals, RefCounted)

public:
	int get_total_servers() const { return total_servers_; }
	void set_total_servers(int p_count) { total_servers_ = p_count; }
	int get_total_players() const { return total_players_; }
	void set_total_players(int p_count) { total_players_ = p_count; }

protected:
	static void _bind_methods();

private:
	int total_servers_ = 0;
	int total_players_ = 0;
};

} // namespace godot
