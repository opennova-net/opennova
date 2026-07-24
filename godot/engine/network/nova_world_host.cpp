#include "nova_world_host.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <napi/session.h>

#include <string>
#include <utility>
#include <vector>

namespace godot {

namespace {

std::string to_std(const String &s) {
	return std::string(s.utf8().get_data());
}

} // namespace

NovaWorldHost::NovaWorldHost() :
		lobby_(make_lobby_hooks()) {}
NovaWorldHost::~NovaWorldHost() = default;

// The host's role-specific halves of the shared NwuLobbySession driver. The
// gate auth codes and the CU set are stashed/built by the driver itself; the
// host only supplies its minimal verify identity — the OpenNova gate is
// permissive (verify is not credential-gated, NW-S5), and NWUID is echoed from
// the ServerSessionInit by ClientSession so the request stays well-formed.
NwuLobbySession::Hooks NovaWorldHost::make_lobby_hooks() {
	NwuLobbySession::Hooks hooks;
	hooks.verify_cookie_vars = []() {
		return std::vector<std::pair<std::string, std::string>>{{"NWUID", ""}};
	};
	hooks.on_session_state = [this]() { sync_session_state(); };
	hooks.on_fatal = [this](const String &message) { enter_state(STATE_ERROR, message); };
	hooks.on_soft_error = [this](const String &message) {
		emit_signal("error_occurred", message);
	};
	return hooks;
}

void NovaWorldHost::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_host", "host"), &NovaWorldHost::set_host);
	ClassDB::bind_method(D_METHOD("get_host"), &NovaWorldHost::get_host);
	ClassDB::bind_method(D_METHOD("set_gate_port", "port"), &NovaWorldHost::set_gate_port);
	ClassDB::bind_method(D_METHOD("get_gate_port"), &NovaWorldHost::get_gate_port);
	ClassDB::bind_method(D_METHOD("set_server_name", "name"), &NovaWorldHost::set_server_name);
	ClassDB::bind_method(D_METHOD("get_server_name"), &NovaWorldHost::get_server_name);
	ClassDB::bind_method(D_METHOD("set_mission_name", "name"), &NovaWorldHost::set_mission_name);
	ClassDB::bind_method(D_METHOD("get_mission_name"), &NovaWorldHost::get_mission_name);
	ClassDB::bind_method(D_METHOD("set_max_players", "n"), &NovaWorldHost::set_max_players);
	ClassDB::bind_method(D_METHOD("get_max_players"), &NovaWorldHost::get_max_players);
	ClassDB::bind_method(D_METHOD("set_region", "region"), &NovaWorldHost::set_region);
	ClassDB::bind_method(D_METHOD("get_region"), &NovaWorldHost::get_region);
	ClassDB::bind_method(D_METHOD("set_player_name", "name"), &NovaWorldHost::set_player_name);
	ClassDB::bind_method(D_METHOD("get_player_name"), &NovaWorldHost::get_player_name);
	ClassDB::bind_method(D_METHOD("set_game_port", "port"), &NovaWorldHost::set_game_port);
	ClassDB::bind_method(D_METHOD("get_game_port"), &NovaWorldHost::get_game_port);
	ClassDB::bind_method(D_METHOD("set_advertise_ip", "ip"), &NovaWorldHost::set_advertise_ip);
	ClassDB::bind_method(D_METHOD("get_advertise_ip"), &NovaWorldHost::get_advertise_ip);
	ClassDB::bind_method(D_METHOD("set_app_id", "app_id"), &NovaWorldHost::set_app_id);
	ClassDB::bind_method(D_METHOD("get_app_id"), &NovaWorldHost::get_app_id);
	ClassDB::bind_method(D_METHOD("set_lobby_name", "lobby_name"), &NovaWorldHost::set_lobby_name);
	ClassDB::bind_method(D_METHOD("get_lobby_name"), &NovaWorldHost::get_lobby_name);

	ClassDB::bind_method(D_METHOD("start"), &NovaWorldHost::start);
	ClassDB::bind_method(D_METHOD("stop"), &NovaWorldHost::stop);
	ClassDB::bind_method(D_METHOD("get_state"), &NovaWorldHost::get_state);
	ClassDB::bind_method(D_METHOD("is_hosting"), &NovaWorldHost::is_hosting);
	ClassDB::bind_method(D_METHOD("set_player_count", "n"), &NovaWorldHost::set_player_count);
	ClassDB::bind_method(D_METHOD("get_player_count"), &NovaWorldHost::get_player_count);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "host"), "set_host", "get_host");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "gate_port"), "set_gate_port", "get_gate_port");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "server_name"), "set_server_name", "get_server_name");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "mission_name"), "set_mission_name", "get_mission_name");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_players"), "set_max_players", "get_max_players");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "region"), "set_region", "get_region");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "player_name"), "set_player_name", "get_player_name");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "game_port"), "set_game_port", "get_game_port");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "advertise_ip"), "set_advertise_ip", "get_advertise_ip");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "app_id"), "set_app_id", "get_app_id");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "lobby_name"), "set_lobby_name", "get_lobby_name");

	ADD_SIGNAL(MethodInfo("registered"));
	ADD_SIGNAL(MethodInfo("host_update_sent"));
	ADD_SIGNAL(MethodInfo("disconnected", PropertyInfo(Variant::STRING, "reason")));
	ADD_SIGNAL(MethodInfo("error_occurred", PropertyInfo(Variant::STRING, "message")));
	ADD_SIGNAL(MethodInfo("state_changed", PropertyInfo(Variant::INT, "state")));

	BIND_ENUM_CONSTANT(STATE_IDLE);
	BIND_ENUM_CONSTANT(STATE_GATE_PROBING);
	BIND_ENUM_CONSTANT(STATE_SESSION_HELLO);
	BIND_ENUM_CONSTANT(STATE_SESSION_JOIN);
	BIND_ENUM_CONSTANT(STATE_REGISTERING);
	BIND_ENUM_CONSTANT(STATE_HOSTING);
	BIND_ENUM_CONSTANT(STATE_DISCONNECTED);
	BIND_ENUM_CONSTANT(STATE_ERROR);
}

void NovaWorldHost::set_host(const String &host) { host_ = host; }
String NovaWorldHost::get_host() const { return host_; }
void NovaWorldHost::set_gate_port(int port) { gate_port_ = port; }
int NovaWorldHost::get_gate_port() const { return gate_port_; }
void NovaWorldHost::set_server_name(const String &name) { server_name_ = name; }
String NovaWorldHost::get_server_name() const { return server_name_; }
void NovaWorldHost::set_mission_name(const String &name) { mission_name_ = name; }
String NovaWorldHost::get_mission_name() const { return mission_name_; }
void NovaWorldHost::set_max_players(int n) { max_players_ = n; }
int NovaWorldHost::get_max_players() const { return max_players_; }
void NovaWorldHost::set_region(const String &region) { region_ = region; }
String NovaWorldHost::get_region() const { return region_; }
void NovaWorldHost::set_player_name(const String &name) { player_name_ = name; }
String NovaWorldHost::get_player_name() const { return player_name_; }
void NovaWorldHost::set_game_port(int port) { game_port_ = port; }
int NovaWorldHost::get_game_port() const { return game_port_; }
void NovaWorldHost::set_advertise_ip(const String &ip) { advertise_ip_ = ip; }
String NovaWorldHost::get_advertise_ip() const { return advertise_ip_; }
void NovaWorldHost::set_app_id(const String &app_id) { app_id_ = app_id; }
String NovaWorldHost::get_app_id() const { return app_id_; }
void NovaWorldHost::set_lobby_name(const String &lobby_name) { lobby_name_ = lobby_name; }
String NovaWorldHost::get_lobby_name() const { return lobby_name_; }

void NovaWorldHost::set_player_count(int n) {
	if (n < 0) n = 0;
	if (n == player_count_) return;
	player_count_ = n;
	if (state_ == STATE_HOSTING) update_pending_ = true;  // push on the next tick
}

void NovaWorldHost::_ready() {
	set_process(true);
}

void NovaWorldHost::start() {
	if (state_ != STATE_IDLE && state_ != STATE_DISCONNECTED && state_ != STATE_ERROR) {
		return;
	}
	keepalive_accum_ = 0.0;
	update_accum_ = 0.0;
	update_pending_ = false;

	// The driver binds the gate/NW sockets and mints the ci/ck pair; a bind
	// failure lands in STATE_ERROR through the on_fatal hook.
	if (!lobby_.open()) {
		return;
	}

	enter_state(STATE_GATE_PROBING);
	lobby_.probe(host_, gate_port_);
}

void NovaWorldHost::stop() {
	if (lobby_.sockets_open() && lobby_.session_verified()) {
		// Tell the gate to drop the host row, then close the session.
		lobby_.send(lobby_.session()->build_lobby_message(opennova::make_client_stop_hosting()));
		lobby_.send(lobby_.session()->build_goodbye());
	}
	lobby_.close();
	if (state_ != STATE_DISCONNECTED && state_ != STATE_IDLE) {
		enter_state(STATE_DISCONNECTED, String("stopped"));
	}
}

void NovaWorldHost::_process(double delta) {
	if (state_ == STATE_IDLE || state_ == STATE_DISCONNECTED || state_ == STATE_ERROR) {
		return;
	}
	// The driver pumps the gate + session sockets and the handshake timeout;
	// role progress arrives through the hooks (sync_session_state).
	lobby_.process(delta);

	if (state_ == STATE_HOSTING && lobby_.session()) {
		// Keep the NWU session alive (the gate times out idle sessions) and push
		// a ClientHostUpdate on the refresh interval or whenever the player count
		// changed.
		keepalive_accum_ += delta;
		if (keepalive_accum_ >= keepalive_interval_s_) {
			keepalive_accum_ = 0.0;
			lobby_.send(lobby_.session()->build_heartbeat());
		}
		update_accum_ += delta;
		if (update_pending_ || update_accum_ >= update_interval_s_) {
			update_accum_ = 0.0;
			update_pending_ = false;
			send_host_update();
		}
	}
}

void NovaWorldHost::sync_session_state() {
	const opennova::ClientSession *session = lobby_.session();
	if (!session) return;
	using S = opennova::ClientSession::State;
	switch (session->state()) {
	case S::Hello:
		enter_state(STATE_SESSION_HELLO);
		break;
	case S::Auth:
	case S::Verifying:
		if (state_ != STATE_SESSION_JOIN) {
			enter_state(STATE_SESSION_JOIN);
		}
		break;
	case S::Verified:
		// Register exactly once (the host-request), then heartbeat from _process.
		if (state_ != STATE_REGISTERING && state_ != STATE_HOSTING) {
			send_host_request();
		}
		break;
	case S::Error:
		enter_state(STATE_ERROR, String(session->last_error().c_str()));
		break;
	default:
		break;
	}
}

opennova::HostRegistration NovaWorldHost::host_cfg() const {
	opennova::HostRegistration cfg;
	cfg.server_name = to_std(server_name_);
	cfg.mission_name = to_std(mission_name_);
	cfg.max_players = max_players_;
	cfg.region = to_std(region_);
	cfg.player_name = to_std(player_name_);
	cfg.game_port = game_port_;
	cfg.advertise_ip = to_std(advertise_ip_);
	cfg.app_id = to_std(app_id_);
	cfg.lobby_name = to_std(lobby_name_);
	cfg.player_count = player_count_;
	return cfg;
}

void NovaWorldHost::send_host_request() {
	if (!lobby_.session_verified()) return;
	// [orig: CNapiGameSession_SendHostRequest @ 0x4d3700]
	// NWUID: echoed from the ServerSessionInit into the request's Cookie.
	auto req = opennova::make_host_request(host_cfg(), lobby_.session()->server_nwuid());
	lobby_.send(lobby_.session()->build_lobby_message(req));

	enter_state(STATE_HOSTING);
	keepalive_accum_ = 0.0;
	update_accum_ = 0.0;
	emit_signal("registered");
	// The `registered` signal is the structured notification; the console line
	// rides the verbose channel only.
	UtilityFunctions::print_verbose(String("[NovaWorldHost] registered '") + server_name_ +
	                        "' game_port=" + String::num_int64(game_port_) +
	                        " -> gate " + lobby_.nw_udp_host() + ":" +
	                        String::num_int64(static_cast<int64_t>(lobby_.nw_udp_port())));
}

void NovaWorldHost::send_host_update() {
	if (!lobby_.session_verified()) return;
	// [orig: CNapiGameSession_SendHostUpdate @ 0x4d3860]
	auto upd = opennova::make_host_update(host_cfg());
	lobby_.send(lobby_.session()->build_lobby_message(upd));
	emit_signal("host_update_sent");
}

void NovaWorldHost::enter_state(State next, const String &reason) {
	if (state_ == next) return;
	state_ = next;
	emit_signal("state_changed", static_cast<int>(next));
	if (next == STATE_ERROR) {
		emit_signal("error_occurred", reason);
	} else if (next == STATE_DISCONNECTED) {
		emit_signal("disconnected", reason);
	}
}

} // namespace godot
