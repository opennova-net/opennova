#include "nova_world_host.h"

#include <godot_cpp/classes/packet_peer_udp.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <napi/envelope.h>
#include <napi/session.h>
#include <novaworld/gate_probe.h>
#include <novaworld/gate_response.h>

#include <cstring>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace godot {

namespace {

PackedByteArray to_pba(const std::vector<uint8_t> &v) {
	PackedByteArray out;
	out.resize(static_cast<int>(v.size()));
	if (!v.empty()) {
		std::memcpy(out.ptrw(), v.data(), v.size());
	}
	return out;
}

std::vector<uint8_t> from_pba(const PackedByteArray &pba) {
	std::vector<uint8_t> out(pba.size());
	if (!out.empty()) {
		std::memcpy(out.data(), pba.ptr(), out.size());
	}
	return out;
}

uint32_t pick_random_uint32() {
	static thread_local std::mt19937 gen{std::random_device{}()};
	return std::uniform_int_distribution<uint32_t>(1)(gen);
}

std::string to_std(const String &s) {
	return std::string(s.utf8().get_data());
}

const char *host_state_name(NovaWorldHost::State s) {
	switch (s) {
	case NovaWorldHost::STATE_IDLE: return "idle";
	case NovaWorldHost::STATE_GATE_PROBING: return "gate_probing";
	case NovaWorldHost::STATE_SESSION_HELLO: return "session_hello";
	case NovaWorldHost::STATE_SESSION_JOIN: return "session_join";
	case NovaWorldHost::STATE_REGISTERING: return "registering";
	case NovaWorldHost::STATE_HOSTING: return "hosting";
	case NovaWorldHost::STATE_DISCONNECTED: return "disconnected";
	case NovaWorldHost::STATE_ERROR: return "error";
	}
	return "unknown";
}

} // namespace

NovaWorldHost::NovaWorldHost() = default;
NovaWorldHost::~NovaWorldHost() = default;

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
	client_index_ = pick_random_uint32();
	client_key_ = pick_random_uint32();
	session_.reset();
	server_nwuid_.clear();
	nw_udp_host_ = String();
	nw_udp_port_ = 0;
	handshake_elapsed_ = 0.0;
	keepalive_accum_ = 0.0;
	update_accum_ = 0.0;
	update_pending_ = false;

	gate_socket_.instantiate();
	nw_socket_.instantiate();
	if (gate_socket_->bind(0, "0.0.0.0") != OK) {
		enter_state(STATE_ERROR, String("gate UDP bind failed"));
		return;
	}
	if (nw_socket_->bind(0, "0.0.0.0") != OK) {
		enter_state(STATE_ERROR, String("nw UDP bind failed"));
		return;
	}

	enter_state(STATE_GATE_PROBING);
	send_gate_probe();
}

void NovaWorldHost::stop() {
	if (session_ && nw_socket_.is_valid() && session_->is_verified()) {
		// Tell the gate to drop the host row, then close the session.
		send_nw_datagram(session_->build_lobby_message(opennova::make_client_stop_hosting()));
		send_nw_datagram(session_->build_goodbye());
	}
	session_.reset();
	if (gate_socket_.is_valid()) {
		gate_socket_->close();
		gate_socket_.unref();
	}
	if (nw_socket_.is_valid()) {
		nw_socket_->close();
		nw_socket_.unref();
	}
	if (state_ != STATE_DISCONNECTED && state_ != STATE_IDLE) {
		enter_state(STATE_DISCONNECTED, String("stopped"));
	}
}

void NovaWorldHost::_process(double delta) {
	if (state_ == STATE_IDLE || state_ == STATE_DISCONNECTED || state_ == STATE_ERROR) {
		return;
	}
	if (gate_socket_.is_valid()) poll_gate();
	if (nw_socket_.is_valid()) poll_session();

	if (state_ == STATE_HOSTING && session_) {
		// Keep the NWU session alive (the gate times out idle sessions) and push
		// a ClientHostUpdate on the refresh interval or whenever the player count
		// changed.
		keepalive_accum_ += delta;
		if (keepalive_accum_ >= keepalive_interval_s_) {
			keepalive_accum_ = 0.0;
			send_nw_datagram(session_->build_heartbeat());
		}
		update_accum_ += delta;
		if (update_pending_ || update_accum_ >= update_interval_s_) {
			update_accum_ = 0.0;
			update_pending_ = false;
			send_host_update();
		}
	}

	if (state_ == STATE_GATE_PROBING || state_ == STATE_SESSION_HELLO ||
	    state_ == STATE_SESSION_JOIN) {
		handshake_elapsed_ += delta;
		if (handshake_elapsed_ >= handshake_timeout_s_) {
			enter_state(STATE_ERROR,
			            String("handshake timeout in state ") + host_state_name(state_));
		}
	}
}

void NovaWorldHost::send_gate_probe() {
	if (!gate_socket_.is_valid()) return;
	auto probe = opennova::gate_probe_build(opennova::GATE_PROBE_TAG_JOINTOPS);
	std::vector<uint8_t> packet(probe.size() + 4);
	size_t out_size = 0;
	if (opennova::napi_envelope_encode(probe.data(), probe.size(),
	                                   packet.data(), packet.size(), &out_size) != 0) {
		enter_state(STATE_ERROR, String("gate probe envelope encode failed"));
		return;
	}
	packet.resize(out_size);
	gate_socket_->set_dest_address(host_, gate_port_);
	gate_socket_->put_packet(to_pba(packet));
}

void NovaWorldHost::poll_gate() {
	if (!gate_socket_.is_valid()) return;
	while (gate_socket_->get_available_packet_count() > 0) {
		auto bytes = from_pba(gate_socket_->get_packet());
		std::vector<uint8_t> inner(bytes.size());
		size_t inner_size = 0;
		if (opennova::napi_envelope_decode(bytes.data(), bytes.size(),
		                                   inner.data(), inner.size(), &inner_size) != 0) {
			emit_signal("error_occurred", String("bad gate envelope"));
			continue;
		}
		inner.resize(inner_size);

		opennova::GateResponse parsed;
		if (!opennova::gate_response_decrypt_and_parse(inner.data(), inner.size(), parsed)) {
			emit_signal("error_occurred", String("bad gate response"));
			continue;
		}
		// Gate-issued session-auth codes the 0x42 join must carry as CU chunks
		// (NW-S3): METLABEL -> MetTag, UDPCODE1/2 -> UdpCode1/2. Empty against the
		// OpenNova gate; populated when registering against live NovaWorld.
		gate_met_tag_ = parsed.met_label;
		gate_udp_code1_ = parsed.udp_code1;
		gate_udp_code2_ = parsed.udp_code2;
		std::string udp_host;
		uint16_t udp_port = 0;
		if (!opennova::parse_host_port(parsed.udp_novaworld, udp_host, udp_port)) {
			enter_state(STATE_ERROR, String("UDPNOVAWORLD malformed"));
			return;
		}
		nw_udp_host_ = String(udp_host.c_str());
		nw_udp_port_ = udp_port;
		begin_session();
	}
}

void NovaWorldHost::begin_session() {
	opennova::ClientSession::Config cfg;
	cfg.client_index = client_index_;
	cfg.client_key = client_key_;

	// 0x42-join CU set (NW-S3) — the same set retail builds in
	// CNapiGameSession_ConnectToNovaWorld @ 0x4d4640 for any NovaWorld connection,
	// host or join. Empty codes against the permissive OpenNova gate; populated
	// from the gate response when registering against live NovaWorld so the
	// server's join callbacks accept the host's session. GateTag = cfg.na (the
	// const protocol/gate tag).
	opennova::NovaWorldJoinCu cu;
	cu.gate_tag = cfg.na;
	cu.met_tag = gate_met_tag_;
	cu.udp_code1 = gate_udp_code1_;
	cu.udp_code2 = gate_udp_code2_;
	cfg.cu_vars = opennova::make_novaworld_join_cu(cu);

	// Minimal lobby identity — the OpenNova gate is permissive (verify is not
	// credential-gated, NW-S5). NWUID is echoed from the ServerSessionInit so the
	// verify request is well-formed.
	cfg.verify_cookie_vars = {{"NWUID", ""}};
	session_ = std::make_unique<opennova::ClientSession>(cfg);
	enter_state(STATE_SESSION_HELLO);
	handshake_elapsed_ = 0.0;
	send_nw_datagram(session_->start());
}

void NovaWorldHost::send_nw_datagram(const std::vector<uint8_t> &dg) {
	if (!nw_socket_.is_valid() || dg.empty()) return;
	nw_socket_->set_dest_address(nw_udp_host_, nw_udp_port_);
	nw_socket_->put_packet(to_pba(dg));
}

void NovaWorldHost::poll_session() {
	if (!nw_socket_.is_valid() || !session_) return;
	while (nw_socket_->get_available_packet_count() > 0) {
		auto bytes = from_pba(nw_socket_->get_packet());
		std::vector<std::vector<uint8_t>> replies;
		const bool ok = session_->handle_datagram(bytes.data(), bytes.size(), replies);
		if (server_nwuid_.empty() && !session_->server_nwuid().empty()) {
			server_nwuid_ = session_->server_nwuid();
		}
		for (const auto &dg : replies) {
			send_nw_datagram(dg);
		}
		if (!ok) {
			enter_state(STATE_ERROR, String(session_->last_error().c_str()));
			return;
		}
		sync_session_state();
	}
}

void NovaWorldHost::sync_session_state() {
	if (!session_) return;
	using S = opennova::ClientSession::State;
	switch (session_->state()) {
	case S::Hello:
		enter_state(STATE_SESSION_HELLO);
		break;
	case S::Auth:
	case S::Verifying:
		if (state_ != STATE_SESSION_JOIN) {
			enter_state(STATE_SESSION_JOIN);
			handshake_elapsed_ = 0.0;
		}
		break;
	case S::Verified:
		// Register exactly once (the host-request), then heartbeat from _process.
		if (state_ != STATE_REGISTERING && state_ != STATE_HOSTING) {
			send_host_request();
		}
		break;
	case S::Error:
		enter_state(STATE_ERROR, String(session_->last_error().c_str()));
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
	if (!session_ || !session_->is_verified()) return;
	// [orig: CNapiGameSession_SendHostRequest @ 0x4d3700]
	auto req = opennova::make_host_request(host_cfg(), server_nwuid_);
	send_nw_datagram(session_->build_lobby_message(req));

	enter_state(STATE_HOSTING);
	keepalive_accum_ = 0.0;
	update_accum_ = 0.0;
	emit_signal("registered");
	UtilityFunctions::print(String("[NovaWorldHost] registered '") + server_name_ +
	                        "' game_port=" + String::num_int64(game_port_) +
	                        " -> gate " + nw_udp_host_ + ":" +
	                        String::num_int64(nw_udp_port_));
}

void NovaWorldHost::send_host_update() {
	if (!session_ || !session_->is_verified()) return;
	// [orig: CNapiGameSession_SendHostUpdate @ 0x4d3860]
	auto upd = opennova::make_host_update(host_cfg());
	send_nw_datagram(session_->build_lobby_message(upd));
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
