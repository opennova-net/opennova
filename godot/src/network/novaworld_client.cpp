#include "network/novaworld_client.h"

#include "network/host_session_options.h"
#include "network/novaworld_identity.h"
#include "rtxt/rtxt_string_file.h"
#include "util/data_format.h"
#include "util/string_convert.h"

#include <array>
#include <godot_cpp/classes/http_client.hpp>
#include <godot_cpp/classes/http_request.hpp>
#include <godot_cpp/classes/ip.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <base/io/crt_ftol.h>
#include <base/os_random/os_random.h>
#include <net/napi/envelope.h>
#include <net/napi/session.h>
#include <net/novaworld/client_session.h>
#include <net/novaworld/connect_or_host.h>
#include <net/novaworld/gate_response.h>
#include <net/novaworld/gsb.h>
#include <net/novaworld/http_flow.h>
#include <net/novaworld/lobby_vars.h>
#include <net/novaworld/ping_sweep.h>
#include <net/novaworld/proxy_rendezvous.h>
#include <net/npwire/net_ports.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace godot {

namespace {

// HTTPRequest delivers response headers as "Name: value" lines; the flow finds Set-Cookie itself.
std::vector<std::string> pba_to_strvec(const PackedStringArray &arr) {
	std::vector<std::string> out;
	out.reserve(static_cast<size_t>(arr.size()));
	for (int i = 0; i < arr.size(); ++i) {
		out.emplace_back(opennova::to_std(arr[i]));
	}
	return out;
}

const char *state_name(NovaWorldClient::State s) {
	switch (s) {
	case NovaWorldClient::STATE_IDLE: return "idle";
	case NovaWorldClient::STATE_GATE_PROBING: return "gate_probing";
	case NovaWorldClient::STATE_SESSION_HELLO: return "session_hello";
	case NovaWorldClient::STATE_SESSION_JOIN: return "session_join";
	case NovaWorldClient::STATE_CONNECTED: return "connected";
	case NovaWorldClient::STATE_DISCONNECTED: return "disconnected";
	case NovaWorldClient::STATE_ERROR: return "error";
	case NovaWorldClient::STATE_JOINING: return "joining";
	case NovaWorldClient::STATE_IN_GAME_HELLO: return "in_game_hello";
	case NovaWorldClient::STATE_HOSTING_REQUESTED: return "hosting_requested";
	case NovaWorldClient::STATE_HOSTING: return "hosting";
	}
	return "unknown";
}

} // namespace

NovaWorldClient::NovaWorldClient() :
		lobby_(make_lobby_hooks(), make_lobby_environment()), host_role_(lobby_, make_host_hooks()) {}
// A hosted match may still hold the shared pump: its demux must not call into
// this lobby once it is gone.
NovaWorldClient::~NovaWorldClient() {
	if (nw_pump_.is_valid()) nw_pump_->demux().set_session_claim(nullptr);
}

void NovaWorldClient::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_host", "host"), &NovaWorldClient::set_host);
	ClassDB::bind_method(D_METHOD("get_host"), &NovaWorldClient::get_host);
	ClassDB::bind_method(D_METHOD("set_gate_port", "port"), &NovaWorldClient::set_gate_port);
	ClassDB::bind_method(D_METHOD("get_gate_port"), &NovaWorldClient::get_gate_port);
	ClassDB::bind_method(D_METHOD("set_player_name", "name"), &NovaWorldClient::set_player_name);
	ClassDB::bind_method(D_METHOD("get_player_name"), &NovaWorldClient::get_player_name);
	ClassDB::bind_method(D_METHOD("set_gametext", "gametext"), &NovaWorldClient::set_gametext);
	ClassDB::bind_method(D_METHOD("get_gametext"), &NovaWorldClient::get_gametext);

	ClassDB::bind_method(D_METHOD("start"), &NovaWorldClient::start);
	ClassDB::bind_method(D_METHOD("stop"), &NovaWorldClient::stop);
	ClassDB::bind_method(D_METHOD("get_state"), &NovaWorldClient::get_state);
	ClassDB::bind_method(D_METHOD("is_session_active"), &NovaWorldClient::is_session_active);
	ClassDB::bind_method(D_METHOD("is_authenticated"), &NovaWorldClient::is_authenticated);
	ClassDB::bind_method(D_METHOD("get_server_info"), &NovaWorldClient::get_server_info);
	ClassDB::bind_method(D_METHOD("get_server_rows"), &NovaWorldClient::get_server_rows);
	ClassDB::bind_method(D_METHOD("refresh_servers"), &NovaWorldClient::refresh_servers);
	ClassDB::bind_method(D_METHOD("get_total_servers"), &NovaWorldClient::get_total_servers);
	ClassDB::bind_method(D_METHOD("get_total_players"), &NovaWorldClient::get_total_players);
	ClassDB::bind_method(D_METHOD("get_server_ping_rids"), &NovaWorldClient::get_server_ping_rids);
	ClassDB::bind_method(D_METHOD("get_server_ping_values"),
			&NovaWorldClient::get_server_ping_values);
	ClassDB::bind_method(D_METHOD("_apply_ping_results", "results", "generation"),
	                     &NovaWorldClient::apply_ping_results);
	ClassDB::bind_method(D_METHOD("login", "username", "password"), &NovaWorldClient::login);
	ClassDB::bind_method(D_METHOD("join", "rid"), &NovaWorldClient::join);
	ClassDB::bind_method(D_METHOD("start_hosting", "options"), &NovaWorldClient::start_hosting);
	ClassDB::bind_method(D_METHOD("is_hosting"), &NovaWorldClient::is_hosting);
	ClassDB::bind_method(D_METHOD("stop_hosting"), &NovaWorldClient::stop_hosting);
	ClassDB::bind_method(D_METHOD("stop_playing"), &NovaWorldClient::stop_playing);
	ClassDB::bind_method(D_METHOD("has_join_proxy"), &NovaWorldClient::has_join_proxy);
	ClassDB::bind_method(D_METHOD("get_join_proxy_node"), &NovaWorldClient::get_join_proxy_node);
	ClassDB::bind_method(D_METHOD("get_join_proxy_cookie"), &NovaWorldClient::get_join_proxy_cookie);
	ClassDB::bind_method(D_METHOD("get_join_proxy_relay"), &NovaWorldClient::get_join_proxy_relay);
	ClassDB::bind_method(D_METHOD("get_join_lobby_number"), &NovaWorldClient::get_join_lobby_number);
	// Bound so the HTTPRequest.request_completed signals can target them.
	ClassDB::bind_method(
		D_METHOD("on_gsb_request_completed", "result", "response_code", "headers", "body"),
		&NovaWorldClient::on_gsb_request_completed);
	ClassDB::bind_method(
		D_METHOD("on_login_request_completed", "result", "response_code", "headers", "body"),
		&NovaWorldClient::on_login_request_completed);
	ClassDB::bind_method(
		D_METHOD("on_join_request_completed", "result", "response_code", "headers", "body"),
		&NovaWorldClient::on_join_request_completed);
	ClassDB::bind_method(
		D_METHOD("on_host_request_completed", "result", "response_code", "headers", "body"),
		&NovaWorldClient::on_host_request_completed);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "host"),     "set_host", "get_host");
	ADD_PROPERTY(PropertyInfo(Variant::INT,    "gate_port"), "set_gate_port", "get_gate_port");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "player_name"), "set_player_name", "get_player_name");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "gametext", PROPERTY_HINT_RESOURCE_TYPE, "RtxtStringFile"),
	             "set_gametext", "get_gametext");

	ADD_SIGNAL(MethodInfo("server_list_updated", PropertyInfo(Variant::ARRAY, "rows")));
	ADD_SIGNAL(MethodInfo("server_list_failed", PropertyInfo(Variant::STRING, "reason")));
	// One ping-sweep pass landed; read get_server_ping_rids/values() for the results.
	ADD_SIGNAL(MethodInfo("server_pings_updated"));
	ADD_SIGNAL(MethodInfo("connected"));
	ADD_SIGNAL(MethodInfo("disconnected", PropertyInfo(Variant::STRING, "reason")));
	ADD_SIGNAL(MethodInfo("error_occurred", PropertyInfo(Variant::STRING, "message")));
	ADD_SIGNAL(MethodInfo("state_changed", PropertyInfo(Variant::INT, "state")));
	ADD_SIGNAL(MethodInfo("login_succeeded", PropertyInfo(Variant::STRING, "nwhandle")));
	ADD_SIGNAL(MethodInfo("login_failed", PropertyInfo(Variant::STRING, "reason")));
	ADD_SIGNAL(MethodInfo("join_failed", PropertyInfo(Variant::STRING, "reason")));
	ADD_SIGNAL(MethodInfo("joined_game", PropertyInfo(Variant::STRING, "host"),
	                      PropertyInfo(Variant::INT, "port"),
	                      PropertyInfo(Variant::STRING, "app_id"),
	                      PropertyInfo(Variant::PACKED_BYTE_ARRAY, "cd_cookie")));
	// The service punted us (ServerLeaveNovaWorld): the MsgCode the menutxt
	// ERR_PUNTEDFROMNOVAWORLD text substitutes for its [[$]].
	ADD_SIGNAL(MethodInfo("punted", PropertyInfo(Variant::INT, "msg_code")));
	// The host leg (start_hosting): the session is hosting (the shell loads the
	// mission now), or the NWEC tag the NovaWorld error dialog shows.
	ADD_SIGNAL(MethodInfo("hosting_started"));
	ADD_SIGNAL(MethodInfo("host_failed", PropertyInfo(Variant::STRING, "reason")));
	// The service stopped the hosting (ServerStopHosting): its message key.
	ADD_SIGNAL(MethodInfo("hosting_stopped", PropertyInfo(Variant::STRING, "reason")));
	// A ServerCommand from the service to the hosting session: the verb name, the
	// target selector ("", "ByIndex", "ByIpAndPort", "ByName", "ByPCID") and the
	// argument tokens.
	ADD_SIGNAL(MethodInfo("server_command", PropertyInfo(Variant::STRING, "verb"),
	                      PropertyInfo(Variant::STRING, "target"),
	                      PropertyInfo(Variant::PACKED_STRING_ARRAY, "args")));
	// The service's ServerPlayerEnterResult for a joiner the hosting session
	// announced: the joiner's ConnectionId, Success, MsgCode, PlayerTicket and
	// AccessCodeList.
	ADD_SIGNAL(MethodInfo("player_enter_result", PropertyInfo(Variant::INT, "connection_id"),
	                      PropertyInfo(Variant::INT, "success"),
	                      PropertyInfo(Variant::INT, "msg_code"),
	                      PropertyInfo(Variant::STRING, "player_ticket"),
	                      PropertyInfo(Variant::STRING, "access_code_list")));

	BIND_ENUM_CONSTANT(STATE_IDLE);
	BIND_ENUM_CONSTANT(STATE_GATE_PROBING);
	BIND_ENUM_CONSTANT(STATE_SESSION_HELLO);
	BIND_ENUM_CONSTANT(STATE_SESSION_JOIN);
	BIND_ENUM_CONSTANT(STATE_CONNECTED);
	BIND_ENUM_CONSTANT(STATE_DISCONNECTED);
	BIND_ENUM_CONSTANT(STATE_ERROR);
	BIND_ENUM_CONSTANT(STATE_JOINING);
	BIND_ENUM_CONSTANT(STATE_IN_GAME_HELLO);
	BIND_ENUM_CONSTANT(STATE_HOSTING_REQUESTED);
	BIND_ENUM_CONSTANT(STATE_HOSTING);
}

void NovaWorldClient::set_host(const String &host) { host_ = host; }
String NovaWorldClient::get_host() const { return host_; }
void NovaWorldClient::set_gate_port(int port) { gate_port_ = port; }
int NovaWorldClient::get_gate_port() const { return gate_port_; }
void NovaWorldClient::set_player_name(const String &name) { player_name_ = name; }
String NovaWorldClient::get_player_name() const { return player_name_; }
// The Host list's STRNOVA / TimeOfDay tokens resolve through the gametext table (the engine's
// key table, make_host_lobby_text); the role keeps the strings.
void NovaWorldClient::set_gametext(const Ref<RtxtStringFile> &gametext) {
	gametext_ = gametext;
	host_role_.set_lobby_text(opennova::make_host_lobby_text(
			[&gametext](const char *section, const char *key, std::string &out) {
				if (gametext.is_null() || !gametext->has_string_in_section(section, StringName(key))) return false;
				out = opennova::to_std(gametext->get_string_in_section(section, StringName(key)));
				return true;
			}));
}

Ref<NovaWorldGateInfo> NovaWorldClient::get_server_info() const { return server_info_; }

// The proxy addresses are the service's .joi bytes.
String NovaWorldClient::get_join_proxy_node() const {
	if (!join_proxy_.enabled()) return String();
	return opennova::cp1252_to_gd(join_proxy_node_ip_) + ":" +
	       String::num_int64(static_cast<int64_t>(join_proxy_.node_port));
}

String NovaWorldClient::get_join_proxy_relay() const {
	if (!join_proxy_.enabled()) return String();
	return opennova::cp1252_to_gd(join_proxy_relay_ip_) + ":" +
	       String::num_int64(static_cast<int64_t>(join_proxy_.relay_port));
}

std::string NovaWorldClient::get_login_pcid() const {
	const std::string *pcid = flow_.cookies().find("PCID");
	return pcid != nullptr ? *pcid : std::string();
}

void NovaWorldClient::trace(const String &line) {
	UtilityFunctions::print_verbose(line);
}

void NovaWorldClient::_ready() {
	set_process(true);
}

void NovaWorldClient::start() {
	if (state_ != STATE_IDLE && state_ != STATE_DISCONNECTED && state_ != STATE_ERROR) {
		return;
	}
	server_info_.unref();
	server_entries_.clear();
	server_pings_ = Dictionary();
	total_servers_ = 0;
	total_players_ = 0;
	authenticated_ = false;
	gsb_request_in_flight_ = false;
	flow_.reset();
	nw_web_domain_ = String();
	identity_vars_.clear();
	play_in_flight_ = false;
	join_proxy_ = opennova::ProxyRendezvousConfig{};

	// HTTP fetchers are child nodes. Created once and reused; each
	// request_completed signal drives its own bound callback. browser_http_ is
	// the GSB server browser (Phase 2); login_http_ runs the EPASK login chain
	// (Phase 3); join_http_ runs the NWJoin handshake (Phase 5).
	if (browser_http_ == nullptr) {
		browser_http_ = memnew(HTTPRequest);
		add_child(browser_http_);
		browser_http_->set_timeout(10.0);
		browser_http_->connect("request_completed",
		                       Callable(this, "on_gsb_request_completed"));
	}
	if (login_http_ == nullptr) {
		login_http_ = memnew(HTTPRequest);
		add_child(login_http_);
		login_http_->set_timeout(10.0);
		login_http_->connect("request_completed",
		                     Callable(this, "on_login_request_completed"));
	}
	if (join_http_ == nullptr) {
		join_http_ = memnew(HTTPRequest);
		add_child(join_http_);
		join_http_->set_timeout(10.0);
		join_http_->connect("request_completed",
		                    Callable(this, "on_join_request_completed"));
	}
	if (host_http_ == nullptr) {
		host_http_ = memnew(HTTPRequest);
		add_child(host_http_);
		host_http_->set_timeout(10.0);
		host_http_->connect("request_completed",
		                    Callable(this, "on_host_request_completed"));
	}

	// The gate worker's socket (an OS-assigned port), and the NovaWorld network
	// type's one socket: bound from the mpnovaworld range, it carries the NWU
	// session now and, when this session hosts, the hosted match's game traffic
	// too (the pump's demux, D-NET-346; engine: net/npwire/net_ports.h
	// novaworld_bind_ports). Then the driver mints the ci/ck pair on them; a
	// bind failure is the error state.
	gate_pump_.instantiate();
	nw_pump_.instantiate();
	if (gate_pump_->bind_listen(0) != OK) {
		enter_state(STATE_ERROR, String("gate UDP bind failed"));
		return;
	}
	bool nw_bound = false;
	for (const uint16_t port : opennova::novaworld_bind_ports()) {
		if (nw_pump_->bind_listen(port) == OK) {
			nw_bound = true;
			break;
		}
	}
	if (!nw_bound) {
		enter_state(STATE_ERROR, String("nw UDP bind failed"));
		return;
	}
	gate_socket_ = std::make_unique<UdpPumpDatagramSocket>(gate_pump_.ptr());
	// No game protocol rides the socket until a hosted match attaches.
	opennova::DatagramDemux &demux = nw_pump_->demux();
	demux.set_game_attached(false);
	demux.set_session_claim([this](const opennova::PeerAddr &from, const uint8_t *data, std::size_t len) {
		return lobby_.claims(from, data, len);
	});
	clock_accum_s_ = 0.0;
	clock_ms_ = 0;
	lobby_.open(*gate_socket_, demux.session());

	enter_state(STATE_GATE_PROBING);
	lobby_.probe(opennova::to_std(host_), static_cast<uint16_t>(gate_port_));
}

void NovaWorldClient::stop() {
	if (lobby_.session() && lobby_.is_open()) {
		// A play or a hosting in flight is cancelled the retail way
		// (ClientStopPlaying / ClientStopHosting, the ConnectOrHost escape/timeout
		// legs), and an established one leaves the same way the NovaWorld menu's
		// re-entry after a match leaves it (each statement empty outside its own
		// states); then the session goes.
		lobby_.session()->stop_playing();
		play_in_flight_ = false;
		host_role_.stop();
		// The stop statements leave on one send pump before the goodbye.
		lobby_.flush();
		opennova::ClientSession *session = lobby_.session();
		if (session->is_verified()) {
			// The reset's teardown of a connected connection sends its disconnect burst.
			const std::vector<uint8_t> goodbye = session->build_goodbye();
			for (size_t i = 0; i < session->disconnect_burst_count(); ++i) {
				lobby_.send(goodbye);
			}
		} else if (!session->reconnecting() &&
		           (state_ == STATE_SESSION_HELLO || state_ == STATE_SESSION_JOIN)) {
			// A first connect still in its handshake says goodbye once; a connection a
			// reconnect is still re-establishing has nothing to tear down.
			lobby_.send(session->build_goodbye());
		}
	}
	if (browser_http_ != nullptr) {
		browser_http_->cancel_request();
	}
	if (login_http_ != nullptr) {
		login_http_->cancel_request();
	}
	if (join_http_ != nullptr) {
		join_http_->cancel_request();
	}
	if (host_http_ != nullptr) {
		host_http_->cancel_request();
	}
	gsb_request_in_flight_ = false;
	authenticated_ = false;
	server_entries_.clear();
	server_pings_ = Dictionary();
	++ping_generation_; // a sweep still running reports into a list that is gone
	total_servers_ = 0;
	total_players_ = 0;
	flow_.reset();
	lobby_.close();
	gate_socket_.reset();
	if (gate_pump_.is_valid()) gate_pump_->close();
	if (nw_pump_.is_valid()) {
		nw_pump_->demux().set_session_claim(nullptr); // the lobby is gone
		nw_pump_->close();
	}
	gate_pump_.unref();
	nw_pump_.unref();
	if (state_ != STATE_DISCONNECTED && state_ != STATE_IDLE) {
		enter_state(STATE_DISCONNECTED, String("stopped"));
	}
}

void NovaWorldClient::_process(double delta) {
	if (state_ == STATE_IDLE || state_ == STATE_DISCONNECTED || state_ == STATE_ERROR) {
		return;
	}

	// The driver pumps the gate + session sockets, the stage retransmits, the
	// connect deadlines and the session's negotiated keepalive/reap; role
	// progress arrives through the hooks (sync_session_state / traces).
	// The ms wall clock every retail deadline and interval runs on (GetTickCount).
	clock_accum_s_ += delta;
	clock_ms_ = static_cast<uint32_t>(clock_accum_s_ * 1000.0);
	lobby_.tick(clock_ms_);
	drain_session_notices();
	host_role_.tick(clock_ms_);

	// The start-playing poll: the ServerPlayResult must land within the connect
	// window (SESSION_CONNECT_TIMEOUT_MS), else the play is cancelled (NWEC02).
	if (play_in_flight_ && lobby_.session() &&
	    lobby_.clock_ms() - play_started_ms_ > opennova::SESSION_CONNECT_TIMEOUT_MS) {
		abort_playing(String(opennova::NWEC_PLAY_TIMEOUT));
	}
}

// The role-specific halves of the shared NwuLobbySession driver: what to do
// with the gate response, which verify identity to ship, and the trace lines
// that keep the retail _connectlog shape (trace()).
opennova::NwuLobbySession::Hooks NovaWorldClient::make_lobby_hooks() {
	opennova::NwuLobbySession::Hooks hooks;
	hooks.on_gate_response = [this](const opennova::GateResponse &parsed) {
		on_gate_response(parsed);
	};
	hooks.cookie_vars = [this]() { return make_cookie_vars(); };
	hooks.on_session_created = [this](std::size_t cu_vars, std::size_t cookie_vars) {
		trace(String("0x42 join carries ")
		    + String::num_int64(static_cast<int64_t>(cu_vars))
		    + " CU chunks; verify carries "
		    + String::num_int64(static_cast<int64_t>(cookie_vars))
		    + " Cookie vars");
	};
	hooks.on_sent = [this](const std::vector<uint8_t> &dg) { trace_sent_datagram(dg); };
	hooks.on_received = [this](const opennova::NwuLobbySession::RxInfo &rx) { on_session_datagram(rx); };
	hooks.on_session_state = [this]() { sync_session_state(); };
	hooks.on_fatal = [this](const std::string &message) {
		enter_state(STATE_ERROR, opennova::cp1252_to_gd(message));
	};
	hooks.on_soft_error = [this](const std::string &message) {
		emit_signal("error_occurred", opennova::cp1252_to_gd(message));
	};
	return hooks;
}

// The driver's device side: the random draws (ci/ck, the reconnect's CK, the AppId and
// cookie-key seeds; the OS CSPRNG, never 0) and the gate / UDPNOVAWORLD host lookup.
opennova::NwuLobbySession::Environment NovaWorldClient::make_lobby_environment() {
	opennova::NwuLobbySession::Environment env;
	env.random_u32 = []() { return opennova::os_random_nonzero_u32(); };
	env.resolve_ipv4 = [](const std::string &host, opennova::PeerAddr &out) {
		IP *ip = IP::get_singleton();
		if (ip == nullptr) return false;
		const String resolved = ip->resolve_hostname(opennova::to_gd(host), IP::TYPE_IPV4);
		const PackedStringArray octets = resolved.split(".");
		if (octets.size() != 4) return false;
		std::array<uint8_t, 4> bytes{};
		for (int i = 0; i < 4; ++i) {
			if (!octets[i].is_valid_int()) return false;
			const int64_t value = octets[i].to_int();
			if (value < 0 || value > 255) return false;
			bytes[static_cast<size_t>(i)] = static_cast<uint8_t>(value);
		}
		out = opennova::peer_addr_from_octets(bytes, out.port);
		return true;
	};
	return env;
}

// The gate replied: stash the fields the lobby HTTP legs resolve their base
// URL from (get_server_info).
void NovaWorldClient::on_gate_response(const opennova::GateResponse &parsed) {
	// NW-S3: the gate-issued session-auth codes the 0x42 join must carry as
	// CU chunks (UDPCODE1/UDPCODE2 -> UdpCode1/UdpCode2) ride the record. On
	// live NW these are issued only to an authenticated request (web login);
	// their presence/absence tells us whether login is the remaining blocker.
	Ref<NovaWorldGateInfo> info;
	info.instantiate();
	info->assign(parsed);
	server_info_ = info;
	trace(String("gate response: udp_code1='")
	    + opennova::cp1252_to_gd(parsed.udp_code1) + "' udp_code2='"
	    + opennova::cp1252_to_gd(parsed.udp_code2) + "' (empty => live NW likely needs login)");
}

// The locale/hardware snapshot seeds both the initial verify and HTTP login.
// Each later Cookie-bearing statement reads the current HTTP jar through the
// engine flow, so cookies issued during login/NWJoin reach the UDP session too.
std::vector<std::pair<std::string, std::string>> NovaWorldClient::make_cookie_vars() {
	if (identity_vars_.empty()) {
		const opennova::LobbyIdentityParams idp = collect_lobby_identity_params(
				lobby_.client_index(), lobby_.client_key());
		identity_vars_ = opennova::make_lobby_identity_vars(idp);
	}
	sync_flow_context();
	return flow_.session_cookie_vars();
}

// Short name for a ClientSession::State int (for the handshake diagnostics).
static const char *cs_state_name(int s) {
	switch (s) {
	case 0: return "idle";    case 1: return "hello";    case 2: return "auth";
	case 3: return "verifying"; case 4: return "verified"; case 5: return "closed";
	case 6: return "error";   default: return "?";
	}
}

void NovaWorldClient::trace_sent_datagram(const std::vector<uint8_t> &dg) {
	// Peek the plaintext outbound opcode (envelope byte 0) so the log lines up
	// 1:1 with retail's _connectlog "SENDING N BYTES ... [0xNN]" — lets the user
	// diff our wire against the .204 capture (fixtures/novaworld/nw204_lobby).
	char op_buf[8] = "0x??";
	{
		std::vector<uint8_t> stripped(dg.size());
		size_t ssz = 0;
		if (opennova::napi_envelope_decode(dg.data(), dg.size(), stripped.data(),
		                                   stripped.size(), &ssz) == 0 && ssz >= 1) {
			std::snprintf(op_buf, sizeof(op_buf), "0x%02x", stripped[0]);
		}
	}
	// Diagnostics (NW-S3): which datagram, where to, and our source port —
	// the server keys its session by our (ip, port), so a changing local_port
	// between the ClientHello and ClientAuth would make the server drop the
	// join with no reply (opennova-int handle_client_join: "No session found").
	const opennova::ClientSession *session = lobby_.session();
	trace(String(">> sent ")
	    + String::num_int64(static_cast<int64_t>(dg.size())) + "B op=" + String(op_buf)
	    + " to " + opennova::cp1252_to_gd(lobby_.nw_udp_host()) + ":"
	    + String::num_int64(static_cast<int64_t>(lobby_.nw_udp_port())) + " local_port="
	    + String::num_int64(static_cast<int64_t>(nw_pump_.is_valid() ? nw_pump_->local_port() : 0))
	    + " state=" + cs_state_name(session ? static_cast<int>(session->state()) : -1));
}

// Per-datagram session diagnostics (the driver already handled the datagram
// and will ship the replies right after this hook returns).
void NovaWorldClient::on_session_datagram(const opennova::NwuLobbySession::RxInfo &rx) {
	// Peek the plaintext session opcode (read-only; handle_datagram
	// re-decoded independently). 0x81=ServerHello, 0x82=ServerAuth,
	// 0x83=ServerProtocolMessage. Tells us exactly what the server sent.
	char op_buf[8] = "0x??";
	{
		std::vector<uint8_t> stripped(rx.bytes.size());
		size_t ssz = 0;
		if (opennova::napi_envelope_decode(rx.bytes.data(), rx.bytes.size(),
		        stripped.data(), stripped.size(), &ssz) == 0 && ssz >= 1) {
			std::snprintf(op_buf, sizeof(op_buf), "0x%02x", stripped[0]);
		} else {
			std::snprintf(op_buf, sizeof(op_buf), "BADENV");
		}
	}

	const opennova::ClientSession *session = lobby_.session();

	// Capture the real web host from the ServerSessionInit (0x82) — it carries
	// the NovaworldWebDomainNameAndPortNumber the gate's startupurl leaves as
	// "[domainname]". This is what makes the HTTP login/GSB/join target real NW.
	if (nw_web_domain_.is_empty() && session && !session->server_web_domain().empty()) {
		nw_web_domain_ = opennova::cp1252_to_gd(session->server_web_domain());
		trace(String("web host (SessionInit): ") + nw_web_domain_);
	}

	// Diagnostics: a recv line for every inbound datagram. If state doesn't
	// advance (e.g. auth->auth) with ok=1 and replies=0, the datagram was
	// an unexpected opcode the session ignored; if no recv line appears
	// after the ClientAuth send, the server sent nothing (or it was lost).
	String msg = String("<< recv ")
	    + String::num_int64(static_cast<int64_t>(rx.bytes.size())) + "B op=" + String(op_buf)
	    + " from " + opennova::to_gd(opennova::peer_addr_ip_to_string(rx.from)) + ":"
	    + String::num_int64(static_cast<int64_t>(rx.from.port)) + " state "
	    + cs_state_name(rx.state_before) + "->" + cs_state_name(rx.state_after)
	    + " ok=" + (rx.ok ? "1" : "0") + " replies="
	    + String::num_int64(static_cast<int64_t>(rx.replies));
	if (!rx.ok && session) {
		msg += String(" err='") + opennova::to_gd(session->last_error()) + "'";
	}
	// After the 0x82 ServerAuth, expose the parsed server SK + scrk: the SK
	// becomes the session_id on our outbound 0x43, and opennova-int's 0x43
	// handler DROPS the packet (no reply) unless that session_id == the SK
	// it issued. A zero/garbage SK here would explain the missing 0x83.
	if (session) {
		char sk_buf[40];
		std::snprintf(sk_buf, sizeof(sk_buf), " server_sk=0x%08x sscrk=%dB",
		              static_cast<unsigned>(session->server_key()),
		              static_cast<int>(session->server_scrk().size()));
		msg += String(sk_buf);
	}
	trace(msg);
}

// Map the libs-side session state onto our public State + signals. CONNECTED
// means the lobby verify handshake completed (ServerVerifyResult) — that's
// when the panel enables Host-a-Game.
void NovaWorldClient::sync_session_state() {
	const opennova::ClientSession *session = lobby_.session();
	if (!session) return;
	using S = opennova::ClientSession::State;
	// A reconnect runs under the leg the session was in: a play or a hosting keeps
	// its public state while the session re-probes, re-joins and re-verifies
	// (ClientSession::reconnecting); a lobby session reads as connecting again.
	const bool in_leg = state_ == STATE_JOINING || state_ == STATE_IN_GAME_HELLO ||
			state_ == STATE_HOSTING_REQUESTED || state_ == STATE_HOSTING;
	switch (session->state()) {
	case S::Hello:
		if (!in_leg) enter_state(STATE_SESSION_HELLO);
		break;
	case S::Auth:
	case S::Verifying:
		if (!in_leg && state_ != STATE_SESSION_JOIN) {
			enter_state(STATE_SESSION_JOIN);
		}
		break;
	case S::Verified:
		// A verified lobby remains active while the separate HTTP join runs.
		// Keep transactional public states from being overwritten by keepalives.
		if (state_ != STATE_CONNECTED && state_ != STATE_JOINING &&
		    state_ != STATE_IN_GAME_HELLO && state_ != STATE_HOSTING_REQUESTED &&
		    state_ != STATE_HOSTING) {
			enter_state(STATE_CONNECTED);
		}
		break;
	case S::Closed:
		// The peer closed / punted us, or the receive-silence reap fired: the
		// latched disconnect record's tag is the reason -- unless the session is
		// reconnecting, which it does on its own while its word is set.
		if (session->reconnecting()) break;
		if (session->disconnected_by_peer() && state_ != STATE_DISCONNECTED) {
			play_in_flight_ = false;
			enter_state(STATE_DISCONNECTED, opennova::to_gd(session->last_error()));
		}
		break;
	case S::Error:
		enter_state(STATE_ERROR, opennova::to_gd(session->last_error()));
		break;
	default:
		break;
	}
}

// The server notifications the session parsed this frame (ServerPlayResult,
// ServerStopPlaying, ServerLeaveNovaWorld, ServerGLSVSSResults: the client
// msginfo rows ClientSession dispatches, engine/net/novaworld/session).
void NovaWorldClient::drain_session_notices() {
	opennova::ClientSession *session = lobby_.session();
	if (!session) return;
	using Notice = opennova::ClientSession::Notice;
	for (const Notice &notice : session->take_notices()) {
		if (host_role_.handle_notice(notice)) continue;
		switch (notice.kind) {
		case Notice::Kind::PlayResult:
			if (!play_in_flight_) break;
			if (notice.fields.success) {
				play_in_flight_ = false;
				resolve_join_target();
			} else {
				// The rejected play maps through the dword_B60110 switch (NWEC04..14).
				abort_playing(opennova::to_gd(opennova::novaworld_error_tag(
						opennova::novaworld_error_from_code(notice.fields.msg_code))));
			}
			break;
		case Notice::Kind::StopPlaying:
			play_in_flight_ = false;
			trace(String("ServerStopPlaying msgcode=")
			    + String::num_int64(notice.fields.msg_code));
			break;
		case Notice::Kind::LeaveNovaWorld:
			play_in_flight_ = false;
			trace(String("ServerLeaveNovaWorld msgcode=")
			    + String::num_int64(notice.fields.msg_code));
			emit_signal("punted", notice.fields.msg_code);
			break;
		case Notice::Kind::GlsvssResults:
			trace(String("ServerGLSVSSResults: ")
			    + String::num_int64(static_cast<int64_t>(notice.glsvss_results.size())) + "B");
			break;
		default:
			break;
		}
	}
}

// ---- Server browser (GSB over HTTP) — ADR 0010 Phase 2 ------------------

TypedArray<NovaWorldServerRow> NovaWorldClient::get_server_rows() const {
	TypedArray<NovaWorldServerRow> out;
	for (const opennova::GsbServerEntry &entry : server_entries_) {
		Ref<NovaWorldServerRow> row;
		row.instantiate();
		row->assign(entry);
		out.push_back(row);
	}
	return out;
}

// ---- Lobby HTTP pump (over engine/net/novaworld LobbyHttpFlow) ----------------

// Snapshot the gate/session outputs into the flow context. The gate values
// (startup_url/post_ip/post_port) are frozen once the gate replies; web_domain +
// server_nwuid are frozen once the session is Verified — so re-syncing before each
// leg is idempotent and cannot shift http_base() mid-login. set_context does NOT
// touch the flow's cookie jar, so NWHANDLE/PCID survive login -> GSB -> join.
void NovaWorldClient::sync_flow_context() {
	opennova::LobbyHttpContext ctx;
	if (server_info_.is_valid()) {
		ctx.startup_url = opennova::to_std(server_info_->get_startup_url());
		ctx.post_ip = opennova::to_std(server_info_->get_post_ip());
		ctx.post_port = std::to_string(server_info_->get_post_port());
	}
	ctx.web_domain = opennova::to_std(nw_web_domain_);
	ctx.server_nwuid = lobby_.session() ? lobby_.session()->server_nwuid() : std::string();
	ctx.locale = opennova::to_std(OS::get_singleton()->get_locale());
	ctx.identity_vars = identity_vars_;
	flow_.set_context(std::move(ctx));
}

// Map one HttpRequestSpec onto a given HTTPRequest child node (method/url/headers/body).
Error NovaWorldClient::ship_spec(HTTPRequest *http, const opennova::HttpRequestSpec &spec) {
	PackedStringArray headers;
	for (const std::string &h : spec.headers) {
		headers.push_back(opennova::to_gd(h));
	}
	const HTTPClient::Method method = (spec.method == opennova::HttpMethod::Post)
		? HTTPClient::METHOD_POST : HTTPClient::METHOD_GET;
	return http->request(opennova::to_gd(spec.url), headers, method, opennova::to_gd(spec.body));
}

void NovaWorldClient::trigger_gsb() {
	if (!authenticated_) {
		emit_signal("server_list_failed", String("Sign in before loading games."));
		return;
	}
	if (browser_http_ == nullptr) {
		emit_signal("server_list_failed", String("The game browser is unavailable."));
		return;
	}
	sync_flow_context();
	const opennova::HttpRequestSpec spec = flow_.gsb_request();
	if (!spec.valid) {
		emit_signal("server_list_failed", String("The matchmaking service did not provide a game-list address."));
		return;
	}
	if (gsb_request_in_flight_) {
		browser_http_->cancel_request();
	}
	if (ship_spec(browser_http_, spec) != OK) {
		gsb_request_in_flight_ = false;
		UtilityFunctions::push_warning(String("[NovaWorldClient] GSB request did not start: ")
			+ opennova::to_gd(spec.url));
		emit_signal("server_list_failed", String("Could not request the game list."));
		return;
	}
	gsb_request_in_flight_ = true;
}

void NovaWorldClient::on_gsb_request_completed(int result, int response_code,
                                               const PackedStringArray &headers,
                                               const PackedByteArray &body) {
	(void)headers; // GSB does not merge Set-Cookie (the flow's contract)
	gsb_request_in_flight_ = false;
	if (!authenticated_) {
		return; // cancelled by stop/reconnect
	}

	// NOTE: on_gsb_response's arg order differs from login/join — body 3rd, out 4th, no headers.
	opennova::GsbResponse parsed;
	if (!flow_.on_gsb_response(result == HTTPRequest::RESULT_SUCCESS, response_code,
	                           from_pba(body), parsed)) {
		UtilityFunctions::push_warning(String("[NovaWorldClient] GSB fetch failed result=")
			+ String::num_int64(result) + " code=" + String::num_int64(response_code));
		if (result != HTTPRequest::RESULT_SUCCESS) {
			emit_signal("server_list_failed", String("Could not reach the game browser."));
		} else if (response_code != 200) {
			emit_signal("server_list_failed", String("The matchmaking service could not load the game list (HTTP ")
				+ String::num_int64(response_code) + String(")."));
		} else {
			emit_signal("server_list_failed", String("The matchmaking service returned an unreadable game list."));
		}
		return;
	}

	server_entries_ = parsed.servers;
	total_servers_ = parsed.total_servers;
	total_players_ = parsed.total_players;
	trace(String("server browser: ") + String::num_int64(static_cast<int64_t>(server_entries_.size())) + " server(s)");
	emit_signal("server_list_updated", get_server_rows());
	// Retail pings every accumulated row's IPv4 on the list finalize
	// (docs/net/novaworld-net-re.md; the semantics live in
	// engine/net/novaworld/ping_sweep.h).
	start_ping_sweep();
}

void NovaWorldClient::refresh_servers() {
	trigger_gsb();
}

PackedInt64Array NovaWorldClient::get_server_ping_rids() const {
	PackedInt64Array out;
	const Array rids = server_pings_.keys();
	for (int64_t i = 0; i < rids.size(); ++i) {
		out.push_back(static_cast<int64_t>(rids[i]));
	}
	return out;
}

PackedInt32Array NovaWorldClient::get_server_ping_values() const {
	PackedInt32Array out;
	const Array rids = server_pings_.keys();
	for (int64_t i = 0; i < rids.size(); ++i) {
		out.push_back(static_cast<int32_t>(static_cast<int>(server_pings_[rids[i]])));
	}
	return out;
}

void NovaWorldClient::start_ping_sweep() {
	++ping_generation_;
	server_pings_ = Dictionary();
	// Every row goes to the sweep; the engine decides which it can drive (an
	// unreported 0.0.0.0 host address folds to never-attempted there).
	std::vector<std::pair<int64_t, std::string>> targets;
	for (const opennova::GsbServerEntry &entry : server_entries_) {
		targets.emplace_back(static_cast<int64_t>(entry.rid), entry.ip);
	}
	if (targets.empty()) {
		emit_signal("server_pings_updated");
		return;
	}
	// A refreshed list's sweep supersedes the running one at once (the engine's
	// PingSweepRunner, net/novaworld/ping_sweep_runner.h).
	ping_worker_.start(std::move(targets), Callable(this, "_apply_ping_results"),
	                   ping_generation_);
}

void NovaWorldClient::apply_ping_results(const Dictionary &results, int64_t generation) {
	// A result of an older generation (its list was replaced or the session
	// stopped while its deferred call was queued) is stale.
	if (generation != ping_generation_) return;
	const Array rids = results.keys();
	for (int i = 0; i < rids.size(); ++i)
		server_pings_[rids[i]] = results[rids[i]];
	emit_signal("server_pings_updated");
}

// ---- Account login (EPASK) — ADR 0010 Phase 3 --------------------------

void NovaWorldClient::login(const String &username, const String &password) {
	authenticated_ = false;
	if (state_ != STATE_CONNECTED) {
		emit_signal("login_failed", String("The matchmaking service is still connecting."));
		return;
	}
	if (login_http_ == nullptr) {
		emit_signal("login_failed", String("The matchmaking client is unavailable."));
		return;
	}
	if (flow_.login_active()) {
		return;  // a login is already in flight
	}
	sync_flow_context();
	const opennova::LoginResult r = flow_.login(
		opennova::to_std(username), opennova::to_std(password));
	switch (r.kind) {
	case opennova::LoginResult::Kind::NeedRequest:
		// Prepare GET: sets the EPASK cookie (the bundle credentials encrypt under).
		if (ship_spec(login_http_, r.request) != OK) {
			flow_.on_login_response(false, 0, {}, {});  // drive the machine back to Idle
			emit_signal("login_failed", String("Could not start sign-in."));
		}
		break;
	case opennova::LoginResult::Kind::Failed:
		// e.g. "no gate startup_url yet — connect first".
		emit_signal("login_failed", opennova::to_gd(r.reason));
		break;
	default:
		break;  // Succeeded is impossible synchronously
	}
}

void NovaWorldClient::on_login_request_completed(int result, int response_code,
                                                 const PackedStringArray &headers,
                                                 const PackedByteArray &body) {
	if (state_ != STATE_CONNECTED || !flow_.login_active()) {
		return; // cancelled by stop/reconnect, or superseded by another lifecycle
	}
	const opennova::LoginResult r = flow_.on_login_response(
		result == HTTPRequest::RESULT_SUCCESS, response_code, pba_to_strvec(headers), from_pba(body));
	switch (r.kind) {
	case opennova::LoginResult::Kind::NeedRequest:
		// Re-ship on login_http_ (PREPARE -> NWSTART -> POST -> POLL, all in the flow).
		if (ship_spec(login_http_, r.request) != OK) {
			flow_.on_login_response(false, 0, {}, {});
			emit_signal("login_failed", String("Could not continue sign-in."));
		}
		break;
	case opennova::LoginResult::Kind::Succeeded:
		authenticated_ = true;
		// NWHANDLE / PCID are the service's cookie bytes.
		trace(String("logged in as ")
			+ opennova::cp1252_to_gd(r.nwhandle) + " (PCID " + opennova::cp1252_to_gd(r.pcid) + ")");
		emit_signal("login_succeeded", opennova::cp1252_to_gd(r.nwhandle));
		trigger_gsb();  // re-fetch the browser now authenticated (NWHANDLE/PCID ride along)
		break;
	case opennova::LoginResult::Kind::Failed:
		authenticated_ = false;
		emit_signal("login_failed", opennova::to_gd(r.reason));
		break;
	}
}

// ---- Join a hosted game — ADR 0010 Phase 5 -----------------------------

void NovaWorldClient::join(int rid) {
	if (!authenticated_) {
		emit_signal("join_failed", String("Sign in before joining a game."));
		return;
	}
	if (join_http_ == nullptr) {
		emit_signal("join_failed", String("The matchmaking join service is unavailable."));
		return;
	}
	if (flow_.join_active() || play_in_flight_) {
		return;  // a join is already in flight
	}
	sync_flow_context();
	// The browsed row's name rides the PlaySetup as ServerName (g_NapiNPCtx.field_11AC).
	pending_join_rid_ = static_cast<uint32_t>(rid);
	pending_join_server_name_.clear();
	for (const opennova::GsbServerEntry &entry : server_entries_) {
		if (entry.rid == pending_join_rid_) {
			pending_join_server_name_ = entry.server_name;
			break;
		}
	}
	const opennova::JoinResult r = flow_.join(static_cast<uint32_t>(rid));
	switch (r.kind) {
	case opennova::JoinResult::Kind::NeedRequest:
		enter_state(STATE_JOINING);
		// First NWJoin call: stores a NWJOINSESSIONTAG and returns the relay page.
		if (ship_spec(join_http_, r.request) != OK) {
			flow_.on_join_response(false, 0, {}, {});  // drive the machine back to Idle
			enter_state(STATE_CONNECTED);
			emit_signal("join_failed", String("Could not start the join request."));
		}
		break;
	case opennova::JoinResult::Kind::Failed:
		// Synchronous failure (e.g. "no server base URL — connect first") — no state change.
		emit_signal("join_failed", opennova::to_gd(r.reason));
		break;
	default:
		break;  // Resolved is impossible synchronously
	}
}

void NovaWorldClient::on_join_request_completed(int result, int response_code,
                                                const PackedStringArray &headers,
                                                const PackedByteArray &body) {
	if (!authenticated_ || !flow_.join_active()) {
		return; // cancelled by stop/reconnect, or superseded by another lifecycle
	}
	const opennova::JoinResult r = flow_.on_join_response(
		result == HTTPRequest::RESULT_SUCCESS, response_code, pba_to_strvec(headers), from_pba(body));
	switch (r.kind) {
	case opennova::JoinResult::Kind::NeedRequest:
		// Re-ship on join_http_ (FIRST -> SECOND, both resolved in the flow).
		if (ship_spec(join_http_, r.request) != OK) {
			flow_.on_join_response(false, 0, {}, {});
			enter_state(STATE_CONNECTED);
			emit_signal("join_failed", String("Could not resolve the selected game."));
		}
		break;
	case opennova::JoinResult::Kind::Resolved:
		trace(String("join resolved host ") + opennova::cp1252_to_gd(r.host_ip) + ":"
			+ String::num_int64(static_cast<int64_t>(r.host_port)));
		start_playing(r);
		break;
	case opennova::JoinResult::Kind::Failed:
		// D-1: any async join failure falls back to the lobby (CONNECTED) — consolidates
		// the old non-200 (formerly stuck JOINING) and bad-.joi (CONNECTED) into one path.
		enter_state(STATE_CONNECTED);
		emit_signal("join_failed", opennova::to_gd(r.reason));
		break;
	}
}

// The .joi resolved: register the play with the service before dialing the host.
// PlaySetup carries the browsed row's name, the .joi-decoded endpoint, the CK join
// token and the LN lobby number; the ServerPlayResult (state 8) releases the
// in-match handoff, a rejection or the 60 s poll cancels it with ClientStopPlaying
// (the PlaySetup shape and the state machine are engine-side: make_play_setup_vars,
// ClientSession::request_playing).
void NovaWorldClient::start_playing(const opennova::JoinResult &resolved) {
	pending_join_ = resolved;
	// NI/NP/BK next to NK: the proxy-assisted join fields the in-match joiner
	// installs on its NP connection (engine/net/novaworld/proxy_rendezvous.h).
	join_proxy_ = opennova::ProxyRendezvousConfig{};
	join_proxy_.node_addr = opennova::proxy_inet_addr(resolved.ni);
	// atol(NP) and atol(BK), the CRT's (io::retail_atol; D-NET-384) [orig:
	//  CNapiGameSession_InitTransportConnection @0x4c9e10 — _atol @0x4ca05b..0x4ca0b9].
	join_proxy_.node_port = static_cast<uint32_t>(opennova::io::retail_atol(resolved.np.c_str()));
	join_proxy_.cookie = static_cast<uint32_t>(opennova::io::retail_atol(resolved.bk.c_str()));
	join_proxy_.relay_addr = opennova::proxy_inet_addr(resolved.host_ip);
	join_proxy_.relay_port = resolved.host_port;
	join_proxy_node_ip_ = resolved.ni;
	join_proxy_relay_ip_ = resolved.host_ip;
	opennova::ClientSession *session = lobby_.session();
	if (!session || !session->is_verified()) {
		enter_state(STATE_CONNECTED);
		emit_signal("join_failed", String(opennova::NWEC_WRONG_SESSION_STATE));
		return;
	}
	const std::vector<opennova::ClientVar> play_setup = opennova::make_play_setup_vars(
			pending_join_server_name_, resolved.host_ip, std::to_string(resolved.host_port),
			resolved.app_id, resolved.ln);
	if (!session->request_playing(play_setup)) {
		enter_state(STATE_CONNECTED);
		emit_signal("join_failed", String(opennova::NWEC_PLAY_START_FAILED));
		return;
	}
	play_in_flight_ = true;
	play_started_ms_ = lobby_.clock_ms();
}

void NovaWorldClient::abort_playing(const String &tag) {
	if (play_in_flight_ && lobby_.session()) {
		lobby_.session()->stop_playing();
	}
	play_in_flight_ = false;
	enter_state(STATE_CONNECTED);
	emit_signal("join_failed", tag);
}

// The hosting half's outcomes, mapped onto our State + signals.
opennova::NwuHostRole::Hooks NovaWorldClient::make_host_hooks() {
	opennova::NwuHostRole::Hooks hooks;
	hooks.on_hosting = [this]() {
		enter_state(STATE_HOSTING);
		emit_signal("hosting_started");
	};
	hooks.on_failed = [this](const std::string &tag) {
		if (state_ == STATE_HOSTING_REQUESTED || state_ == STATE_HOSTING) {
			enter_state(STATE_CONNECTED);
		}
		emit_signal("host_failed", opennova::to_gd(tag));
	};
	hooks.on_stopped = [this](const std::string &reason) {
		if (state_ == STATE_HOSTING_REQUESTED || state_ == STATE_HOSTING) {
			enter_state(STATE_CONNECTED);
		}
		emit_signal("hosting_stopped", opennova::to_gd(reason));
	};
	// The verb's arguments are the service's wire bytes.
	hooks.on_command = [this](const opennova::ServerCommand &command) {
		PackedStringArray args;
		for (const std::string &arg : command.args) args.push_back(opennova::cp1252_to_gd(arg));
		emit_signal("server_command", String(opennova::server_command_verb_name(command.verb)),
				String(opennova::server_command_target_name(command.target)), args);
	};
	hooks.on_player_enter_result = [this](const opennova::ClientSession::PlayerEnterResult &result) {
		emit_signal("player_enter_result", static_cast<int64_t>(result.connection_id),
				result.success, result.msg_code, opennova::cp1252_to_gd(result.player_ticket),
				opennova::cp1252_to_gd(result.access_code_list));
	};
	return hooks;
}

// The NovaWorld menu's Host, on this logged-in session: ConnectOrHost's
// teamId != 0 leg (engine: net/novaworld/connect_or_host.h). Each failure
// leaves its NWEC tag for the error dialog, and only a hosting session (state 6)
// lets the shell start the mission.
void NovaWorldClient::start_hosting(const Ref<HostSessionOptions> &options) {
	opennova::ClientSession *session = lobby_.session();
	const opennova::GateResponse &gate = lobby_.gate_response();
	const bool set_up = options.is_valid() && session != nullptr && lobby_.is_open();
	const char *refusal = opennova::host_leg_refusal(set_up,
			set_up ? session->session_flags() : 0u, server_info_.is_valid(), gate.lobby_name);
	if (refusal != nullptr) {
		emit_signal("host_failed", String(refusal));
		return;
	}
	opennova::HostRegistration cfg;
	cfg.lobby_name = gate.lobby_name;
	cfg.server_name = opennova::to_std(options->get_server_name());
	cfg.max_players = opennova::host_leg_max_players(options->get_max_players(),
			!options->get_serve_and_play());
	cfg.password = !options->get_server_password().is_empty();
	cfg.listen_host = options->get_serve_and_play();
	cfg.lan_only = options->get_server_lan_only();
	cfg.expansion = opennova::to_std(options->get_expansion());
	cfg.region_index = options->get_region_index();
	const String mission = options->get_mission_name().is_empty()
			? options->get_mission_file().get_file().get_basename()
			: options->get_mission_name();
	cfg.mission_name = opennova::to_std(mission);
	enter_state(STATE_HOSTING_REQUESTED);
	// The hosting page first: the menu opens the main page's @HOST_URL@, and the page whose
	// title carries [HOSTKEY=...&] completes the registration. Without the login there is no
	// main page and so no host. [orig: UI_HandleHostSessionStart @0x556d00 @0x556f0d..0x556f98;
	//  UI_ProcessWebResponseContent @0x63d5bb..0x63d604 -> UI_DispatchScreenEvent @0x54f2fa]
	pending_host_cfg_ = cfg;
	sync_flow_context();
	const opennova::HostKeyResult first = flow_.host();
	if (first.kind != opennova::HostKeyResult::Kind::NeedRequest ||
	    ship_spec(host_http_, first.request) != OK) {
		fail_host_leg(opennova::to_gd(first.reason.empty() ? "host request failed" : first.reason));
	}
}

void NovaWorldClient::fail_host_leg(const String &reason) {
	if (state_ == STATE_HOSTING_REQUESTED) enter_state(STATE_CONNECTED);
	emit_signal("host_failed", reason);
}

// The hosting page's chain: relay pages refresh to the next, the [HOSTKEY=...&] title resolves
// the key and the host request goes out with it. A completion that lands after the hosting leg
// was left (stop_hosting, stop) is dropped.
void NovaWorldClient::on_host_request_completed(int result, int response_code,
		const PackedStringArray &headers, const PackedByteArray &body) {
	if (state_ != STATE_HOSTING_REQUESTED || !flow_.host_active()) return;
	const opennova::HostKeyResult r = flow_.on_host_response(
		result == HTTPRequest::RESULT_SUCCESS, response_code, pba_to_strvec(headers), from_pba(body));
	switch (r.kind) {
	case opennova::HostKeyResult::Kind::NeedRequest:
		if (ship_spec(host_http_, r.request) != OK) fail_host_leg(String("host request failed"));
		return;
	case opennova::HostKeyResult::Kind::Resolved: {
		opennova::HostRegistration cfg = pending_host_cfg_;
		cfg.host_key = r.host_key;
		host_role_.request(cfg);
		return;
	}
	case opennova::HostKeyResult::Kind::Failed:
	default:
		fail_host_leg(opennova::cp1252_to_gd(r.reason));
		return;
	}
}

// The NovaWorld menu's re-entry after a match leaves the hosting and the play
// and keeps the verified session (CGameSession_StopHosting / StopPlaying, the
// ClientSession statements).
void NovaWorldClient::stop_hosting() {
	if (host_http_ != nullptr) host_http_->cancel_request();
	host_role_.stop();
	if (state_ == STATE_HOSTING_REQUESTED || state_ == STATE_HOSTING) {
		enter_state(STATE_CONNECTED);
	}
}

void NovaWorldClient::stop_playing() {
	opennova::ClientSession *session = lobby_.session();
	if (session != nullptr && lobby_.is_open()) {
		session->stop_playing();
	}
	play_in_flight_ = false;
	if (state_ == STATE_JOINING || state_ == STATE_IN_GAME_HELLO) {
		enter_state(STATE_CONNECTED);
	}
}

// The service admitted the play (state 8). Hand the in-match host:port off to the game layer
// and stop — the panel routes joined_game into Simulation's joiner (load_mission_as_joiner ->
// enable_join), which owns the SINGLE in-match ClientHello (the joiner role's runtime_->start(), the
// witnessed CNapiGameSession_InitNPConnection path). We deliberately do NOT send our own in-match
// hello here: that would be a second, conflicting handshake on a third socket (the old "send one
// hello and stop" dead-end that never reached gameplay). LAN, NW-routed, and env joins now converge
// on the one joiner seam (ADR 0009; .agents/README.md "do not create a second gameplay network path").
void NovaWorldClient::resolve_join_target() {
	// The in-match join refuses an NK endpoint whose port reads 0 or whose host
	// string is under eight characters (the engine's joi_endpoint_usable); the
	// player is back in the lobby, which leaves the play.
	if (!opennova::joi_endpoint_usable(pending_join_.host_ip, pending_join_.host_port)) {
		stop_playing();
		emit_signal("join_failed", String(opennova::kJoinEndpointRejectTag));
		return;
	}
	// The .joi endpoint and APPID are the service's bytes.
	const String host = opennova::cp1252_to_gd(pending_join_.host_ip);
	const uint16_t port = pending_join_.host_port;
	trace(String("join target resolved ") + host + ":"
		+ String::num_int64(static_cast<int64_t>(port))
		+ " — handing off to the in-match joiner (Simulation owns the ClientHello)");
	enter_state(STATE_IN_GAME_HELLO);
	PackedByteArray cd;
	if (!pending_join_.cd_cookie.empty()) {
		cd.resize(static_cast<int64_t>(pending_join_.cd_cookie.size()));
		std::memcpy(cd.ptrw(), pending_join_.cd_cookie.data(), pending_join_.cd_cookie.size());
	}
	// The APPID join token (decoded .joi CK) and the CD identity cookie (packed
	// PUB* blob) travel with the address: a NovaWorld host validates the APPID in
	// the ClientAuth (code 9) and the CD cookie in the 0x00 JOIN (codes 23/24/25).
	emit_signal("joined_game", host, static_cast<int>(port),
	            opennova::cp1252_to_gd(pending_join_.app_id), cd);
}

void NovaWorldClient::enter_state(State next, const String &reason) {
	if (state_ == next) return;
	const State previous = state_;
	state_ = next;
	if (next == STATE_DISCONNECTED || next == STATE_ERROR) {
		authenticated_ = false;
	}
	trace(String(state_name(previous)) + " -> " + state_name(next)
		+ (reason.is_empty() ? String() : (String(" (") + reason + String(")"))));
	emit_signal("state_changed", static_cast<int>(state_));
	if (next == STATE_CONNECTED) {
		emit_signal("connected");
	} else if (next == STATE_DISCONNECTED) {
		emit_signal("disconnected", reason);
	} else if (next == STATE_ERROR) {
		emit_signal("error_occurred", reason);
	}
}

} // namespace godot
