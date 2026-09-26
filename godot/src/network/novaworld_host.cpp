#include "network/novaworld_host.h"
#include "util/string_convert.h"

#include "network/novaworld_identity.h"
#include "network/random_id.h"
#include "rtxt/rtxt_string_file.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <base/io/tick_rate.h>
#include <net/napi/session.h>

#include <string>
#include <utility>
#include <vector>

namespace godot {

using opennova::to_gd;
using opennova::to_std;

NovaWorldHost::NovaWorldHost() :
		lobby_(make_lobby_hooks()) {}
NovaWorldHost::~NovaWorldHost() = default;

// The host's role-specific halves of the shared NwuLobbySession driver. The
// gate auth codes and the CU set are stashed/built by the driver itself; the
// host supplies the same client-environment Cookie set as the join path (the
// browser cookie jar + locale retail's ReadLocaleInfo rebuilds before every
// Cookie-bearing statement); role does not change the NW-S5 Cookie contract.
// NWUID is echoed from the ServerSessionInit by ClientSession.
NwuLobbySession::Hooks NovaWorldHost::make_lobby_hooks() {
	NwuLobbySession::Hooks hooks;
	hooks.cookie_vars = [this]() {
		return opennova::make_lobby_identity_vars(collect_lobby_identity_params(
				lobby_.client_index(), lobby_.client_key()));
	};
	hooks.on_session_state = [this]() { sync_session_state(); };
	hooks.on_fatal = [this](const String &message) {
		enter_state(STATE_ERROR, message);
	};
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
	ClassDB::bind_method(D_METHOD("set_server_message", "message"), &NovaWorldHost::set_server_message);
	ClassDB::bind_method(D_METHOD("get_server_message"), &NovaWorldHost::get_server_message);
	ClassDB::bind_method(D_METHOD("set_mission_name", "name"), &NovaWorldHost::set_mission_name);
	ClassDB::bind_method(D_METHOD("get_mission_name"), &NovaWorldHost::get_mission_name);
	ClassDB::bind_method(D_METHOD("set_game_type", "abbreviation"), &NovaWorldHost::set_game_type);
	ClassDB::bind_method(D_METHOD("get_game_type"), &NovaWorldHost::get_game_type);
	ClassDB::bind_method(D_METHOD("set_max_players", "n"), &NovaWorldHost::set_max_players);
	ClassDB::bind_method(D_METHOD("get_max_players"), &NovaWorldHost::get_max_players);
	ClassDB::bind_method(D_METHOD("set_region_index", "index"), &NovaWorldHost::set_region_index);
	ClassDB::bind_method(D_METHOD("get_region_index"), &NovaWorldHost::get_region_index);
	ClassDB::bind_method(D_METHOD("set_player_name", "name"), &NovaWorldHost::set_player_name);
	ClassDB::bind_method(D_METHOD("get_player_name"), &NovaWorldHost::get_player_name);
	ClassDB::bind_method(D_METHOD("set_password", "password"), &NovaWorldHost::set_password);
	ClassDB::bind_method(D_METHOD("get_password"), &NovaWorldHost::get_password);
	ClassDB::bind_method(D_METHOD("set_listen_host", "listen_host"), &NovaWorldHost::set_listen_host);
	ClassDB::bind_method(D_METHOD("get_listen_host"), &NovaWorldHost::get_listen_host);
	ClassDB::bind_method(D_METHOD("set_locked", "locked"), &NovaWorldHost::set_locked);
	ClassDB::bind_method(D_METHOD("get_locked"), &NovaWorldHost::get_locked);
	ClassDB::bind_method(D_METHOD("set_country", "country"), &NovaWorldHost::set_country);
	ClassDB::bind_method(D_METHOD("get_country"), &NovaWorldHost::get_country);
	ClassDB::bind_method(D_METHOD("set_expansion", "expansion"), &NovaWorldHost::set_expansion);
	ClassDB::bind_method(D_METHOD("get_expansion"), &NovaWorldHost::get_expansion);
	ClassDB::bind_method(D_METHOD("set_version", "version"), &NovaWorldHost::set_version);
	ClassDB::bind_method(D_METHOD("get_version"), &NovaWorldHost::get_version);
	ClassDB::bind_method(D_METHOD("set_time_of_day", "time_of_day"), &NovaWorldHost::set_time_of_day);
	ClassDB::bind_method(D_METHOD("get_time_of_day"), &NovaWorldHost::get_time_of_day);
	ClassDB::bind_method(D_METHOD("set_gametext", "gametext"), &NovaWorldHost::set_gametext);
	ClassDB::bind_method(D_METHOD("get_gametext"), &NovaWorldHost::get_gametext);
	ClassDB::bind_method(D_METHOD("set_game_port", "port"), &NovaWorldHost::set_game_port);
	ClassDB::bind_method(D_METHOD("get_game_port"), &NovaWorldHost::get_game_port);
	ClassDB::bind_method(D_METHOD("set_advertise_ip", "ip"), &NovaWorldHost::set_advertise_ip);
	ClassDB::bind_method(D_METHOD("get_advertise_ip"), &NovaWorldHost::get_advertise_ip);
	ClassDB::bind_method(D_METHOD("set_lobby_name", "lobby_name"), &NovaWorldHost::set_lobby_name);
	ClassDB::bind_method(D_METHOD("get_lobby_name"), &NovaWorldHost::get_lobby_name);
	ClassDB::bind_method(D_METHOD("get_app_id"), &NovaWorldHost::get_app_id);

	ClassDB::bind_method(D_METHOD("start"), &NovaWorldHost::start);
	ClassDB::bind_method(D_METHOD("stop"), &NovaWorldHost::stop);
	ClassDB::bind_method(D_METHOD("get_state"), &NovaWorldHost::get_state);
	ClassDB::bind_method(D_METHOD("is_hosting"), &NovaWorldHost::is_hosting);
	ClassDB::bind_method(D_METHOD("set_player_slot", "slot", "player_name", "ip_and_port", "pcid",
	                              "team", "type"), &NovaWorldHost::set_player_slot);
	ClassDB::bind_method(D_METHOD("clear_player_slot", "slot"), &NovaWorldHost::clear_player_slot);
	ClassDB::bind_method(D_METHOD("set_player_count", "n"), &NovaWorldHost::set_player_count);
	ClassDB::bind_method(D_METHOD("get_player_count"), &NovaWorldHost::get_player_count);
	ClassDB::bind_method(D_METHOD("get_gsid"), &NovaWorldHost::get_gsid);
	ClassDB::bind_method(D_METHOD("get_host_requires_join_ticket"),
	                     &NovaWorldHost::get_host_requires_join_ticket);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "host"), "set_host", "get_host");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "gate_port"), "set_gate_port", "get_gate_port");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "server_name"), "set_server_name", "get_server_name");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "server_message"), "set_server_message", "get_server_message");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "mission_name"), "set_mission_name", "get_mission_name");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "game_type"), "set_game_type", "get_game_type");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_players"), "set_max_players", "get_max_players");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "region_index"), "set_region_index", "get_region_index");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "player_name"), "set_player_name", "get_player_name");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "password"), "set_password", "get_password");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "listen_host"), "set_listen_host", "get_listen_host");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "locked"), "set_locked", "get_locked");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "country"), "set_country", "get_country");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "expansion"), "set_expansion", "get_expansion");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "version"), "set_version", "get_version");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "time_of_day"), "set_time_of_day", "get_time_of_day");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "gametext", PROPERTY_HINT_RESOURCE_TYPE, "RtxtStringFile"),
	             "set_gametext", "get_gametext");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "game_port"), "set_game_port", "get_game_port");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "advertise_ip"), "set_advertise_ip", "get_advertise_ip");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "lobby_name"), "set_lobby_name", "get_lobby_name");

	ADD_SIGNAL(MethodInfo("registered"));
	ADD_SIGNAL(MethodInfo("disconnected", PropertyInfo(Variant::STRING, "reason")));
	ADD_SIGNAL(MethodInfo("error_occurred", PropertyInfo(Variant::STRING, "message")));
	ADD_SIGNAL(MethodInfo("state_changed", PropertyInfo(Variant::INT, "state")));
	// The service ended our hosting (ServerStopHosting): the MsgCode and its
	// NWUSERVERMSGCODE_* key.
	ADD_SIGNAL(MethodInfo("hosting_stopped", PropertyInfo(Variant::INT, "msg_code"),
	                      PropertyInfo(Variant::STRING, "msg_key")));
	// The service punted us (ServerLeaveNovaWorld): the MsgCode the menutxt
	// ERR_PUNTEDFROMNOVAWORLD text substitutes for its [[$]].
	ADD_SIGNAL(MethodInfo("punted", PropertyInfo(Variant::INT, "msg_code")));
	// A ServerCommand from the service (the NovaWorld -> host administrative
	// channel): the verb name, the target selector ("", "ByIndex", "ByIpAndPort",
	// "ByName", "ByPCID"), and the argument tokens.
	ADD_SIGNAL(MethodInfo("server_command", PropertyInfo(Variant::STRING, "verb"),
	                      PropertyInfo(Variant::STRING, "target"),
	                      PropertyInfo(Variant::PACKED_STRING_ARRAY, "args")));
	// The service's ServerPlayerEnterResult for a player the host announced through
	// ClientPlayerEnterRequest: the joiner's ConnectionId, Success, MsgCode,
	// PlayerTicket and AccessCodeList.
	ADD_SIGNAL(MethodInfo("player_enter_result", PropertyInfo(Variant::INT, "connection_id"),
	                      PropertyInfo(Variant::INT, "success"),
	                      PropertyInfo(Variant::INT, "msg_code"),
	                      PropertyInfo(Variant::STRING, "player_ticket"),
	                      PropertyInfo(Variant::STRING, "access_code_list")));

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
void NovaWorldHost::set_server_name(const String &name) { cfg_.server_name = to_std(name); }
String NovaWorldHost::get_server_name() const { return to_gd(cfg_.server_name); }
void NovaWorldHost::set_server_message(const String &message) { cfg_.server_message = to_std(message); }
String NovaWorldHost::get_server_message() const { return to_gd(cfg_.server_message); }
void NovaWorldHost::set_mission_name(const String &name) { cfg_.mission_name = to_std(name); }
String NovaWorldHost::get_mission_name() const { return to_gd(cfg_.mission_name); }
void NovaWorldHost::set_game_type(const String &abbreviation) { cfg_.game_type = to_std(abbreviation); }
String NovaWorldHost::get_game_type() const { return to_gd(cfg_.game_type); }
void NovaWorldHost::set_max_players(int n) { cfg_.max_players = n; }
int NovaWorldHost::get_max_players() const { return cfg_.max_players; }
void NovaWorldHost::set_region_index(int index) { cfg_.region_index = index; }
int NovaWorldHost::get_region_index() const { return cfg_.region_index; }
void NovaWorldHost::set_player_name(const String &name) { player_name_ = name; }
String NovaWorldHost::get_player_name() const { return player_name_; }
void NovaWorldHost::set_password(bool password) { cfg_.password = password; }
bool NovaWorldHost::get_password() const { return cfg_.password; }
void NovaWorldHost::set_listen_host(bool listen_host) { cfg_.listen_host = listen_host; }
bool NovaWorldHost::get_listen_host() const { return cfg_.listen_host; }
void NovaWorldHost::set_locked(bool locked) { cfg_.locked = locked; }
bool NovaWorldHost::get_locked() const { return cfg_.locked; }
void NovaWorldHost::set_country(const String &country) { cfg_.country = to_std(country); }
String NovaWorldHost::get_country() const { return to_gd(cfg_.country); }
void NovaWorldHost::set_expansion(const String &expansion) { cfg_.expansion = to_std(expansion); }
String NovaWorldHost::get_expansion() const { return to_gd(cfg_.expansion); }
void NovaWorldHost::set_version(const String &version) { cfg_.version = to_std(version); }
String NovaWorldHost::get_version() const { return to_gd(cfg_.version); }
void NovaWorldHost::set_time_of_day(int time_of_day) { cfg_.time_of_day = time_of_day; }
int NovaWorldHost::get_time_of_day() const { return cfg_.time_of_day; }
void NovaWorldHost::set_round_time_remaining_ticks(int ticks) { cfg_.round_time_remaining_ticks = ticks; }
void NovaWorldHost::set_gametext(const Ref<RtxtStringFile> &gametext) { gametext_ = gametext; }
Ref<RtxtStringFile> NovaWorldHost::get_gametext() const { return gametext_; }
void NovaWorldHost::set_game_port(int port) { game_port_ = port; }
int NovaWorldHost::get_game_port() const { return game_port_; }
void NovaWorldHost::set_advertise_ip(const String &ip) { advertise_ip_ = ip; }
String NovaWorldHost::get_advertise_ip() const { return advertise_ip_; }
void NovaWorldHost::set_lobby_name(const String &lobby_name) { cfg_.lobby_name = to_std(lobby_name); }
String NovaWorldHost::get_lobby_name() const { return to_gd(cfg_.lobby_name); }

// The GSID is the service's wire bytes (ServerHostResult HostCommands).
String NovaWorldHost::get_gsid() const {
	const opennova::ClientSession *session = lobby_.session();
	return session ? opennova::cp1252_to_gd(session->host_gsid()) : String();
}

bool NovaWorldHost::get_host_requires_join_ticket() const {
	const opennova::ClientSession *session = lobby_.session();
	return session != nullptr && session->host_requires_join_ticket() != 0;
}

void NovaWorldHost::request_player_enter(int64_t connection_id, int64_t ip_address, int port,
                                         const String &join_ticket) {
	if (!lobby_.session()) return;
	const std::vector<uint8_t> dg = lobby_.session()->build_player_enter_request(
			static_cast<uint32_t>(connection_id), static_cast<uint32_t>(ip_address),
			static_cast<uint32_t>(port), to_std(join_ticket));
	if (!dg.empty()) lobby_.send(dg);
}

// A changed column is dirty in the Host var-list and rides the next refresh (the
// dirty delta): retail republishes on its 1860-tick timer, not on the edit.
void NovaWorldHost::set_player_slot(int slot, const String &player_name, const String &ip_and_port,
                                    const String &pcid, const String &team, const String &type) {
	opennova::HostPlayerSlot player;
	player.slot = slot;
	player.player_name = to_std(player_name);
	player.ip_and_port = to_std(ip_and_port);
	player.pcid = to_std(pcid);
	player.team = to_std(team);
	player.type = to_std(type);
	players_[slot] = player;
	cfg_.player_count = static_cast<int>(players_.size());
	// ClientHostPlayerAdded fires immediately while hosting is established (state 6).
	if (state_ == STATE_HOSTING && lobby_.session()) {
		lobby_.send(lobby_.session()->build_host_player_added(player));
	}
}

void NovaWorldHost::set_player_count(int n) {
	player_count_override_ = n < 0 ? 0 : n;
}

int NovaWorldHost::get_player_count() const {
	if (player_count_override_ >= 0) return player_count_override_;
	// The host itself is the first player: its own slot lands at registration.
	return players_.empty() ? 1 : static_cast<int>(players_.size());
}

void NovaWorldHost::clear_player_slot(int slot) {
	if (players_.erase(slot) == 0) return;
	cfg_.player_count = static_cast<int>(players_.size());
	// ClientHostPlayerRemoved fires immediately while hosting is established (state 6).
	if (state_ == STATE_HOSTING && lobby_.session()) {
		lobby_.send(lobby_.session()->build_host_player_removed(slot));
	}
}

void NovaWorldHost::_ready() {
	set_process(true);
}

void NovaWorldHost::start() {
	if (state_ != STATE_IDLE && state_ != STATE_DISCONNECTED && state_ != STATE_ERROR) {
		return;
	}
	refresh_ticks_ = 0.0;
	uptime_s_ = 0.0;
	last_sent_host_.clear();
	last_sent_players_.clear();
	players_.clear();
	pcid_ring_ = opennova::SessionIdRing{};
	// The per-registration AppId: the engine's make_session_app_id (tick + rand
	// folded into [1000, 9999]).
	cfg_.app_id = opennova::make_session_app_id(lobby_.clock_ms(),
			static_cast<int>(pick_random_uint32() & 0x7FFFu));

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
		// Tell the gate to drop the host row (ClientStopHosting), then close the session.
		lobby_.send(lobby_.session()->build_stop_hosting());
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
	// The driver pumps the gate + session sockets, the connect deadlines and the
	// session's negotiated keepalive/reap; role progress arrives through the hooks.
	lobby_.process(delta);
	drain_session_notices();

	if (state_ == STATE_REGISTERING && lobby_.session()) {
		// The host poll: no ServerHostResult within SESSION_CONNECT_TIMEOUT_MS ->
		// ClientStopHosting + NWEC52.
		if (lobby_.clock_ms() - register_started_ms_ > opennova::SESSION_CONNECT_TIMEOUT_MS) {
			lobby_.send(lobby_.session()->build_stop_hosting());
			enter_state(STATE_ERROR, String(opennova::NWEC_HOST_TIMEOUT));
			return;
		}
	}

	if (state_ == STATE_HOSTING && lobby_.session()) {
		uptime_s_ += delta;
		// The server-info refresh runs on the logic clock: every
		// SESSION_HOST_INFO_REFRESH_TICKS the cookie-key ring (SessionIdRing) advances
		// and the Host list is republished as the dirty delta.
		refresh_ticks_ += delta * opennova::io::kTickHz;
		if (refresh_ticks_ >= static_cast<double>(opennova::SESSION_HOST_INFO_REFRESH_TICKS)) {
			refresh_ticks_ -= static_cast<double>(opennova::SESSION_HOST_INFO_REFRESH_TICKS);
			pcid_ring_.advance(lobby_.clock_ms(), static_cast<int>(pick_random_uint32() & 0x7FFFu));
			cfg_.pcid_key = pcid_ring_.current();
			send_host_update(/*full=*/false);
			send_status_blob();
		}
	}
}

void NovaWorldHost::send_status_blob() {
	const opennova::GateResponse &gate = lobby_.gate_response();
	std::vector<opennova::HostPlayerSlot> roster;
	for (const auto &entry : players_) roster.push_back(entry.second);
	const std::vector<uint8_t> packet = opennova::lobby_update_build_datagram(
			gate, opennova::make_host_status_blob(host_cfg(), lobby_text(), roster));
	if (packet.empty()) return;
	const String post_host = String::num_int64(gate.post_ip[0]) + "." + String::num_int64(gate.post_ip[1]) +
	                         "." + String::num_int64(gate.post_ip[2]) + "." + String::num_int64(gate.post_ip[3]);
	lobby_.send_to(post_host, static_cast<int>(gate.post_port), packet);
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
		// Register exactly once (the host-request); the ServerHostResult moves us on.
		if (state_ != STATE_REGISTERING && state_ != STATE_HOSTING) {
			send_host_request();
		}
		break;
	case S::Closed:
		if (session->disconnected_by_peer() && state_ != STATE_DISCONNECTED) {
			enter_state(STATE_DISCONNECTED, to_gd(session->last_error()));
		}
		break;
	case S::Error:
		enter_state(STATE_ERROR, to_gd(session->last_error()));
		break;
	default:
		break;
	}
}

// The server notifications the session parsed this frame (ServerHostResult,
// ServerStopHosting, ServerLeaveNovaWorld, ServerCommand, ServerPlayerEnterResult:
// the client msginfo rows ClientSession dispatches, engine/net/novaworld/session).
void NovaWorldHost::drain_session_notices() {
	opennova::ClientSession *session = lobby_.session();
	if (!session) return;
	using Notice = opennova::ClientSession::Notice;
	for (const Notice &notice : session->take_notices()) {
		switch (notice.kind) {
		case Notice::Kind::HostResult:
			if (state_ != STATE_REGISTERING) break;
			if (notice.fields.success) {
				// Registered (state 6): the full Host list republishes at once, the host
				// itself is the roster's first player, and the refresh clock starts.
				enter_state(STATE_HOSTING);
				refresh_ticks_ = 0.0;
				uptime_s_ = 0.0;
				send_host_update(/*full=*/true);
				for (const auto &entry : players_) {
					lobby_.send(session->build_host_player_added(entry.second));
				}
				emit_signal("registered");
				// The `registered` signal is the structured notification; the console line
				// rides the verbose channel only.
				UtilityFunctions::print_verbose(String("[NovaWorldHost] registered '") +
				                        get_server_name() + "' gsid=" + get_gsid() +
				                        " game_port=" + String::num_int64(game_port_) +
				                        " -> gate " + lobby_.nw_udp_host() + ":" +
				                        String::num_int64(static_cast<int64_t>(lobby_.nw_udp_port())));
			} else {
				// Rejected: the MsgCode maps through the host switch (NWEC53..60).
				enter_state(STATE_ERROR,
						to_gd(opennova::novaworld_host_error_tag(notice.fields.msg_code)));
			}
			break;
		case Notice::Kind::StopHosting:
			emit_signal("hosting_stopped", notice.fields.msg_code, to_gd(notice.msg_key));
			if (state_ == STATE_HOSTING || state_ == STATE_REGISTERING) {
				enter_state(STATE_DISCONNECTED, to_gd(notice.msg_key));
			}
			break;
		case Notice::Kind::LeaveNovaWorld:
			emit_signal("punted", notice.fields.msg_code);
			break;
		case Notice::Kind::Command: {
			// The verb's arguments are the service's wire bytes.
			PackedStringArray args;
			for (const std::string &arg : notice.command.args) args.push_back(opennova::cp1252_to_gd(arg));
			const char *target = "";
			switch (notice.command.target) {
			case opennova::ServerCommandTarget::ByIndex: target = "ByIndex"; break;
			case opennova::ServerCommandTarget::ByIpAndPort: target = "ByIpAndPort"; break;
			case opennova::ServerCommandTarget::ByName: target = "ByName"; break;
			case opennova::ServerCommandTarget::ByPCID: target = "ByPCID"; break;
			default: break;
			}
			emit_signal("server_command",
			            String(opennova::server_command_verb_name(notice.command.verb)),
			            String(target), args);
			break;
		}
		case Notice::Kind::PlayerEnterResult:
			emit_signal("player_enter_result",
			            static_cast<int64_t>(notice.player_enter.connection_id),
			            notice.player_enter.success, notice.player_enter.msg_code,
			            opennova::cp1252_to_gd(notice.player_enter.player_ticket),
			            opennova::cp1252_to_gd(notice.player_enter.access_code_list));
			break;
		default:
			break;
		}
	}
}

opennova::HostLobbyText NovaWorldHost::lobby_text() const {
	opennova::HostLobbyText text;
	if (gametext_.is_null()) return text;
	auto lookup = [this](const char *section, const char *key, std::string &out) {
		if (gametext_->has_string_in_section(section, StringName(key)))
			out = to_std(gametext_->get_string_in_section(section, StringName(key)));
	};
	lookup("NovaWorld", "STRNOVA11", text.yes);
	lookup("NovaWorld", "STRNOVA12", text.no);
	lookup("NovaWorld", "STRNOVA10", text.no_time_limit);
	lookup("NovaWorld", "STRNOVA07", text.region[0]);
	lookup("NovaWorld", "STRNOVA08", text.region[1]);
	lookup("NovaWorld", "STRNOVA09", text.region[2]);
	// GameText_GetStringWithFallback("TimeOfDay", KEY, fallback): the fallback stands
	// when the table lacks the key.
	lookup("TimeOfDay", "UNKNOWN", text.time_of_day[0]);
	lookup("TimeOfDay", "DAWN", text.time_of_day[1]);
	lookup("TimeOfDay", "DAY", text.time_of_day[2]);
	lookup("TimeOfDay", "DUSK", text.time_of_day[3]);
	lookup("TimeOfDay", "NIGHT", text.time_of_day[4]);
	return text;
}

opennova::HostRegistration NovaWorldHost::host_cfg() const {
	opennova::HostRegistration cfg = cfg_;
	cfg.player_count = get_player_count();
	cfg.pcid_key = pcid_ring_.current();
	cfg.uptime_ms = static_cast<uint32_t>(uptime_s_ * 1000.0);
	return cfg;
}

std::vector<opennova::ClientVar> NovaWorldHost::player_list_vars() const {
	std::vector<opennova::HostPlayerSlot> roster;
	for (const auto &entry : players_) roster.push_back(entry.second);
	return opennova::make_player_list(roster);
}

void NovaWorldHost::send_host_request() {
	opennova::ClientSession *session = lobby_.session();
	if (!session || !session->is_verified()) return;
	// The host itself is the roster's first player: its game endpoint rides the
	// PlayerIpAndPort of its own ClientHostPlayerAdded once registered (retail's
	// Host list carries no address; Port = "-1").
	if (players_.count(0) == 0) {
		opennova::HostPlayerSlot self;
		self.slot = 0;
		self.player_name = to_std(player_name_);
		self.ip_and_port = to_std(advertise_ip_) + ":" + std::to_string(game_port_);
		players_[0] = self;
		cfg_.player_count = static_cast<int>(players_.size());
	}
	// A fresh host sends CurrentlyHosting = 0; only the re-host-after-reconnect path
	// sends 1. [orig: CNapiGameSession_SendHostRequest @ 0x4d3700, see docs/net/novaworld-net-re.md]
	const std::vector<uint8_t> dg = session->build_host_request(host_cfg(), /*currently_hosting=*/0);
	if (dg.empty()) return;
	lobby_.send(dg);
	register_started_ms_ = lobby_.clock_ms();
	enter_state(STATE_REGISTERING);
}

void NovaWorldHost::send_host_update(bool full) {
	opennova::ClientSession *session = lobby_.session();
	if (!session || !session->is_verified()) return;
	// includeAll right after registration, else only the vars whose value changed
	// (dirty_client_vars), and nothing at all when none did.
	// [orig: CNapiGameSession_SendHostUpdate @ 0x4d3860, see docs/net/novaworld-net-re.md]
	const std::vector<opennova::ClientVar> host =
			opennova::make_host_var_list(host_cfg(), lobby_text(), /*full=*/true);
	const std::vector<opennova::ClientVar> players = player_list_vars();
	const std::vector<opennova::ClientVar> dirty_host =
			full ? host : opennova::dirty_client_vars(last_sent_host_, host);
	const std::vector<opennova::ClientVar> dirty_players =
			full ? players : opennova::dirty_client_vars(last_sent_players_, players);
	if (dirty_host.empty() && dirty_players.empty()) return;
	lobby_.send(session->build_host_update(dirty_host, dirty_players));
	last_sent_host_ = host;
	last_sent_players_ = players;
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
