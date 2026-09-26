#include "network/novaworld_client.h"

#include "network/novaworld_identity.h"
#include "util/data_format.h"
#include "util/string_convert.h"

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

#include <net/napi/envelope.h>
#include <net/napi/session.h>
#include <net/novaworld/client_session.h>
#include <net/novaworld/gate_response.h>
#include <net/novaworld/gsb.h>
#include <net/novaworld/http_flow.h>
#include <net/novaworld/lobby_vars.h>
#include <net/novaworld/ping_sweep.h>
#include <net/novaworld/proxy_rendezvous.h>

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
	}
	return "unknown";
}

} // namespace

NovaWorldClient::NovaWorldClient() :
		lobby_(make_lobby_hooks()) {}
NovaWorldClient::~NovaWorldClient() = default;

void NovaWorldClient::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_host", "host"), &NovaWorldClient::set_host);
	ClassDB::bind_method(D_METHOD("get_host"), &NovaWorldClient::get_host);
	ClassDB::bind_method(D_METHOD("set_gate_port", "port"), &NovaWorldClient::set_gate_port);
	ClassDB::bind_method(D_METHOD("get_gate_port"), &NovaWorldClient::get_gate_port);
	ClassDB::bind_method(D_METHOD("set_player_name", "name"), &NovaWorldClient::set_player_name);
	ClassDB::bind_method(D_METHOD("get_player_name"), &NovaWorldClient::get_player_name);

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

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "host"),     "set_host", "get_host");
	ADD_PROPERTY(PropertyInfo(Variant::INT,    "gate_port"), "set_gate_port", "get_gate_port");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "player_name"), "set_player_name", "get_player_name");

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
	// The service ended our play (ServerStopPlaying); mission exit reason 12.
	ADD_SIGNAL(MethodInfo("play_stopped", PropertyInfo(Variant::INT, "msg_code")));

	BIND_ENUM_CONSTANT(STATE_IDLE);
	BIND_ENUM_CONSTANT(STATE_GATE_PROBING);
	BIND_ENUM_CONSTANT(STATE_SESSION_HELLO);
	BIND_ENUM_CONSTANT(STATE_SESSION_JOIN);
	BIND_ENUM_CONSTANT(STATE_CONNECTED);
	BIND_ENUM_CONSTANT(STATE_DISCONNECTED);
	BIND_ENUM_CONSTANT(STATE_ERROR);
	BIND_ENUM_CONSTANT(STATE_JOINING);
	BIND_ENUM_CONSTANT(STATE_IN_GAME_HELLO);
}

void NovaWorldClient::set_host(const String &host) { host_ = host; }
String NovaWorldClient::get_host() const { return host_; }
void NovaWorldClient::set_gate_port(int port) { gate_port_ = port; }
int NovaWorldClient::get_gate_port() const { return gate_port_; }
void NovaWorldClient::set_player_name(const String &name) { player_name_ = name; }
String NovaWorldClient::get_player_name() const { return player_name_; }

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

	// The driver binds the gate/NW sockets and mints the ci/ck pair; a bind
	// failure lands in STATE_ERROR through the on_fatal hook.
	if (!lobby_.open()) {
		return;
	}

	enter_state(STATE_GATE_PROBING);
	lobby_.probe(host_, gate_port_);
}

void NovaWorldClient::stop() {
	if (lobby_.session() && lobby_.sockets_open()) {
		// A play in flight is cancelled the retail way (ClientStopPlaying, the
		// ConnectOrHost escape/timeout leg), then the session leaves.
		if (play_in_flight_) {
			lobby_.send(lobby_.session()->build_stop_playing());
			play_in_flight_ = false;
		}
		if (state_ == STATE_CONNECTED || state_ == STATE_SESSION_HELLO ||
		    state_ == STATE_SESSION_JOIN || state_ == STATE_JOINING ||
		    state_ == STATE_IN_GAME_HELLO) {
			lobby_.send(lobby_.session()->build_goodbye());
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
	gsb_request_in_flight_ = false;
	authenticated_ = false;
	server_entries_.clear();
	server_pings_ = Dictionary();
	total_servers_ = 0;
	total_players_ = 0;
	flow_.reset();
	lobby_.close();
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
	lobby_.process(delta);
	drain_session_notices();

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
NwuLobbySession::Hooks NovaWorldClient::make_lobby_hooks() {
	NwuLobbySession::Hooks hooks;
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
	hooks.on_received = [this](const NwuLobbySession::RxInfo &rx) { on_session_datagram(rx); };
	hooks.on_session_state = [this]() { sync_session_state(); };
	hooks.on_fatal = [this](const String &message) {
		enter_state(STATE_ERROR, message);
	};
	hooks.on_soft_error = [this](const String &message) {
		emit_signal("error_occurred", message);
	};
	return hooks;
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
	    + " to " + lobby_.nw_udp_host() + ":"
	    + String::num_int64(static_cast<int64_t>(lobby_.nw_udp_port())) + " local_port="
	    + String::num_int64(static_cast<int64_t>(lobby_.nw_local_port()))
	    + " state=" + cs_state_name(session ? static_cast<int>(session->state()) : -1));
}

// Per-datagram session diagnostics (the driver already handled the datagram
// and will ship the replies right after this hook returns).
void NovaWorldClient::on_session_datagram(const NwuLobbySession::RxInfo &rx) {
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
	    + " from " + rx.src_ip + ":"
	    + String::num_int64(static_cast<int64_t>(rx.src_port)) + " state "
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
		// A verified lobby remains active while the separate HTTP join runs.
		// Keep transactional public states from being overwritten by keepalives.
		if (state_ != STATE_CONNECTED && state_ != STATE_JOINING &&
		    state_ != STATE_IN_GAME_HELLO) {
			enter_state(STATE_CONNECTED);
		}
		break;
	case S::Closed:
		// The peer closed / punted us, or the receive-silence reap fired: the
		// latched disconnect record's tag is the reason.
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
			emit_signal("play_stopped", notice.fields.msg_code);
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
	if (!ping_worker_.start(std::move(targets), Callable(this, "_apply_ping_results"),
	                        ping_generation_)) {
		// One sweep at a time: the in-flight one is now stale (its generation
		// no longer matches), and its landing re-issues this one.
		ping_resweep_pending_ = true;
	}
}

void NovaWorldClient::apply_ping_results(const Dictionary &results, int64_t generation) {
	if (generation == ping_generation_) {
		const Array rids = results.keys();
		for (int i = 0; i < rids.size(); ++i)
			server_pings_[rids[i]] = results[rids[i]];
		emit_signal("server_pings_updated");
	}
	// A pass from a superseded sweep (the list refreshed underneath it) is
	// stale; if that refresh asked for a sweep while this one ran, run it now.
	if (ping_resweep_pending_) {
		ping_resweep_pending_ = false;
		start_ping_sweep();
	}
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
	// The browsed row's name rides the PlaySetup as ServerName (g_napi_np_ctx.field_11AC).
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
// ClientSession::build_play_request).
void NovaWorldClient::start_playing(const opennova::JoinResult &resolved) {
	pending_join_ = resolved;
	// NI/NP/BK next to NK: the proxy-assisted join fields the in-match joiner
	// installs on its NP connection (engine/net/novaworld/proxy_rendezvous.h).
	join_proxy_ = opennova::ProxyRendezvousConfig{};
	join_proxy_.node_addr = opennova::proxy_inet_addr(resolved.ni);
	join_proxy_.node_port = static_cast<uint32_t>(std::strtol(resolved.np.c_str(), nullptr, 10));
	join_proxy_.cookie = static_cast<uint32_t>(std::strtol(resolved.bk.c_str(), nullptr, 10));
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
	const std::vector<uint8_t> dg = session->build_play_request(play_setup);
	if (dg.empty()) {
		enter_state(STATE_CONNECTED);
		emit_signal("join_failed", String(opennova::NWEC_PLAY_START_FAILED));
		return;
	}
	lobby_.send(dg);
	play_in_flight_ = true;
	play_started_ms_ = lobby_.clock_ms();
}

void NovaWorldClient::abort_playing(const String &tag) {
	if (play_in_flight_ && lobby_.session()) {
		lobby_.send(lobby_.session()->build_stop_playing());
	}
	play_in_flight_ = false;
	enter_state(STATE_CONNECTED);
	emit_signal("join_failed", tag);
}

// The service admitted the play (state 8). Hand the in-match host:port off to the game layer
// and stop — the panel routes joined_game into Simulation's joiner (load_mission_as_joiner ->
// enable_join), which owns the SINGLE in-match ClientHello (the joiner role's runtime_->start(), the
// witnessed CNapiGameSession_InitNPConnection path). We deliberately do NOT send our own in-match
// hello here: that would be a second, conflicting handshake on a third socket (the old "send one
// hello and stop" dead-end that never reached gameplay). LAN, NW-routed, and env joins now converge
// on the one joiner seam (ADR 0009; .agents/README.md "do not create a second gameplay network path").
void NovaWorldClient::resolve_join_target() {
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
