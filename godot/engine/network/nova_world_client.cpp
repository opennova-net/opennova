#include "nova_world_client.h"

#include <godot_cpp/classes/http_client.hpp>
#include <godot_cpp/classes/http_request.hpp>
#include <godot_cpp/classes/ip.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <napi/envelope.h>
#include <novaworld/client_session.h>
#include <novaworld/gate_response.h>
#include <novaworld/gsb.h>
#include <novaworld/http_flow.h>
#include <novaworld/lobby_vars.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace godot {

namespace {

std::vector<uint8_t> from_pba(const PackedByteArray &pba) {
	std::vector<uint8_t> out(pba.size());
	if (!out.empty()) {
		std::memcpy(out.data(), pba.ptr(), out.size());
	}
	return out;
}

// HTTPRequest delivers response headers as "Name: value" lines; the flow finds Set-Cookie itself.
std::vector<std::string> pba_to_strvec(const PackedStringArray &arr) {
	std::vector<std::string> out;
	out.reserve(static_cast<size_t>(arr.size()));
	for (int i = 0; i < arr.size(); ++i) {
		out.emplace_back(String(arr[i]).utf8().get_data());
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
	ClassDB::bind_method(D_METHOD("get_server_info"), &NovaWorldClient::get_server_info);
	ClassDB::bind_method(D_METHOD("get_session_debug"), &NovaWorldClient::get_session_debug);
	ClassDB::bind_method(D_METHOD("get_server_rows"), &NovaWorldClient::get_server_rows);
	ClassDB::bind_method(D_METHOD("refresh_server_list"), &NovaWorldClient::refresh_server_list);
	ClassDB::bind_method(D_METHOD("login", "username", "password"), &NovaWorldClient::login);
	ClassDB::bind_method(D_METHOD("join", "rid"), &NovaWorldClient::join);
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

	ADD_SIGNAL(MethodInfo("server_info_received", PropertyInfo(Variant::DICTIONARY, "info")));
	ADD_SIGNAL(MethodInfo("server_list_updated", PropertyInfo(Variant::ARRAY, "rows")));
	ADD_SIGNAL(MethodInfo("connected"));
	ADD_SIGNAL(MethodInfo("disconnected", PropertyInfo(Variant::STRING, "reason")));
	ADD_SIGNAL(MethodInfo("error_occurred", PropertyInfo(Variant::STRING, "message")));
	ADD_SIGNAL(MethodInfo("state_changed", PropertyInfo(Variant::INT, "state")));
	ADD_SIGNAL(MethodInfo("login_succeeded", PropertyInfo(Variant::STRING, "nwhandle")));
	ADD_SIGNAL(MethodInfo("login_failed", PropertyInfo(Variant::STRING, "reason")));
	ADD_SIGNAL(MethodInfo("joined_game", PropertyInfo(Variant::STRING, "host"),
	                      PropertyInfo(Variant::INT, "port")));

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

Dictionary NovaWorldClient::get_server_info() const { return server_info_; }

void NovaWorldClient::trace(const String &line) {
	if (trace_ring_.size() >= kTraceRingCap) {
		trace_ring_.remove_at(0);
	}
	trace_ring_.push_back(line);
}

Dictionary NovaWorldClient::get_session_debug() const {
	Dictionary out;
	out["state"] = String(state_name(state_));
	out["gate"] = host_ + String(":") + String::num_int64(gate_port_);
	out["session_endpoint"] =
			lobby_.nw_udp_host() + String(":") + String::num_int64(static_cast<int64_t>(lobby_.nw_udp_port()));
	out["web_domain"] = nw_web_domain_;
	out["server_rows"] = server_rows_.size();
	out["gsb_in_flight"] = gsb_request_in_flight_;
	out["trace"] = trace_ring_;
	return out;
}

void NovaWorldClient::_ready() {
	set_process(true);
}

void NovaWorldClient::start() {
	if (state_ != STATE_IDLE && state_ != STATE_DISCONNECTED && state_ != STATE_ERROR) {
		return;
	}
	server_info_.clear();
	server_rows_.clear();
	gsb_request_in_flight_ = false;
	flow_.reset();
	nw_web_domain_ = String();
	identity_vars_.clear();

	// HTTP fetchers are child nodes. Created once and reused; each
	// request_completed signal drives its own bound callback. browser_http_ is
	// the GSB server browser (Phase 2); login_http_ runs the EPASK login chain
	// (Phase 3); join_http_ runs the NWJoin handshake (Phase 5).
	if (browser_http_ == nullptr) {
		browser_http_ = memnew(HTTPRequest);
		add_child(browser_http_);
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
	if (lobby_.session() && lobby_.sockets_open() &&
	    (state_ == STATE_CONNECTED || state_ == STATE_SESSION_HELLO || state_ == STATE_SESSION_JOIN)) {
		lobby_.send(lobby_.session()->build_goodbye());
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

	// The driver pumps the gate + session sockets and the handshake timeout;
	// role progress arrives through the hooks (sync_session_state / traces).
	lobby_.process(delta);

	tick_accum_ += delta;
	if (state_ == STATE_CONNECTED && tick_accum_ >= heartbeat_interval_s_) {
		tick_accum_ = 0.0;
		if (lobby_.session()) {
			lobby_.send(lobby_.session()->build_heartbeat());
		}
	}
}

// The role-specific halves of the shared NwuLobbySession driver: what to do
// with the gate response, which verify identity to ship, and the trace lines
// that keep the retail _connectlog shape (get_session_debug()).
NwuLobbySession::Hooks NovaWorldClient::make_lobby_hooks() {
	NwuLobbySession::Hooks hooks;
	hooks.on_gate_response = [this](const opennova::GateResponse &parsed) {
		on_gate_response(parsed);
	};
	hooks.verify_cookie_vars = [this]() { return make_verify_cookie_vars(); };
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
	hooks.on_fatal = [this](const String &message) { enter_state(STATE_ERROR, message); };
	hooks.on_soft_error = [this](const String &message) {
		emit_signal("error_occurred", message);
	};
	return hooks;
}

// The gate replied: surface the response to GDScript (server_info_received) and
// stash the fields the lobby HTTP legs resolve their base URL from.
void NovaWorldClient::on_gate_response(const opennova::GateResponse &parsed) {
	Dictionary info;
	info["post_ip"] = String(std::to_string(parsed.post_ip[0]).c_str()) + "." +
	                  String(std::to_string(parsed.post_ip[1]).c_str()) + "." +
	                  String(std::to_string(parsed.post_ip[2]).c_str()) + "." +
	                  String(std::to_string(parsed.post_ip[3]).c_str());
	info["post_port"] = parsed.post_port;
	info["startup_url"] = String(parsed.startup_url.c_str());
	info["udp_novaworld"] = String(parsed.udp_novaworld.c_str());
	// NW-S3: the gate-issued session-auth codes the 0x42 join must carry as
	// CU chunks (UDPCODE1/UDPCODE2 -> UdpCode1/UdpCode2). On live NW these
	// are issued only to an authenticated request (web login); their
	// presence/absence here tells us whether login is the remaining blocker.
	info["udp_code1"] = String(parsed.udp_code1.c_str());
	info["udp_code2"] = String(parsed.udp_code2.c_str());
	info["met_label"] = String(parsed.met_label.c_str());
	server_info_ = info;
	trace(String("gate response: udp_code1='")
	    + String(parsed.udp_code1.c_str()) + "' udp_code2='"
	    + String(parsed.udp_code2.c_str()) + "' (empty => live NW likely needs login)");
	emit_signal("server_info_received", info);
}

// Verify "Cookie" var-list (NW-S5) — the identity set the 892B
// ClientRequestVerifyResult carries (capture frame 10166). NWUID is filled by
// ClientSession from the ServerSessionInit. CD-key fields are empty (retail
// sent them empty and still validated); the hardware fingerprints + NWHWI are
// best-effort telemetry the lobby verify does not gate on. Built once in libs,
// reused for BOTH the UDP verify var-list and the HTTP login cookies (the .204
// capture shows the same identity set in both places). TimeZoneBias is the only
// OS-sourced field — read it here and pass it in.
std::vector<std::pair<std::string, std::string>> NovaWorldClient::make_verify_cookie_vars() {
	opennova::LobbyIdentityParams idp;
	idp.client_index = lobby_.client_index();
	idp.client_key = lobby_.client_key();
	{
		Dictionary tz = Time::get_singleton()->get_time_zone_from_system();
		if (tz.has(String("bias"))) {
			int64_t bias = tz[String("bias")];
			idp.tz_bias = std::to_string(bias);
		}
	}
	identity_vars_ = opennova::make_lobby_identity_vars(idp);
	return identity_vars_;
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
		nw_web_domain_ = String(session->server_web_domain().c_str());
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
		msg += String(" err='") + String(session->last_error().c_str()) + "'";
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
		if (state_ != STATE_CONNECTED) {
			enter_state(STATE_CONNECTED);
			tick_accum_ = 0.0;
			// Session is lobby-ready — fetch the server browser (Phase 2).
			trigger_gsb();
		}
		break;
	case S::Error:
		enter_state(STATE_ERROR, String(session->last_error().c_str()));
		break;
	default:
		break;
	}
}

// ---- Server browser (GSB over HTTP) — ADR 0010 Phase 2 ------------------

Array NovaWorldClient::get_server_rows() const {
	return server_rows_;
}

void NovaWorldClient::refresh_server_list() {
	trigger_gsb();
}

// ---- Lobby HTTP pump (over libs/novaworld LobbyHttpFlow) ----------------

// Snapshot the gate/session outputs into the flow context. The gate values
// (startup_url/post_ip/post_port) are frozen once the gate replies; web_domain +
// server_nwuid are frozen once the session is Verified — so re-syncing before each
// leg is idempotent and cannot shift http_base() mid-login. set_context does NOT
// touch the flow's cookie jar, so NWHANDLE/PCID survive login -> GSB -> join.
void NovaWorldClient::sync_flow_context() {
	opennova::LobbyHttpContext ctx;
	if (server_info_.has("startup_url")) {
		ctx.startup_url = std::string(String(server_info_["startup_url"]).utf8().get_data());
	}
	if (server_info_.has("post_ip")) {
		ctx.post_ip = std::string(String(server_info_["post_ip"]).utf8().get_data());
	}
	if (server_info_.has("post_port")) {
		// post_port is stored as an int Variant in server_info_ (see poll_gate).
		ctx.post_port = std::to_string(static_cast<int>(server_info_["post_port"]));
	}
	ctx.web_domain = std::string(nw_web_domain_.utf8().get_data());
	ctx.server_nwuid = lobby_.session() ? lobby_.session()->server_nwuid() : std::string();
	ctx.locale = std::string(OS::get_singleton()->get_locale().utf8().get_data());
	ctx.identity_vars = identity_vars_;
	flow_.set_context(std::move(ctx));
}

// Map one HttpRequestSpec onto a given HTTPRequest child node (method/url/headers/body).
Error NovaWorldClient::ship_spec(HTTPRequest *http, const opennova::HttpRequestSpec &spec) {
	PackedStringArray headers;
	for (const std::string &h : spec.headers) {
		headers.push_back(String(h.c_str()));
	}
	const HTTPClient::Method method = (spec.method == opennova::HttpMethod::Post)
		? HTTPClient::METHOD_POST : HTTPClient::METHOD_GET;
	return http->request(String(spec.url.c_str()), headers, method, String(spec.body.c_str()));
}

void NovaWorldClient::trigger_gsb() {
	if (browser_http_ == nullptr) {
		return;
	}
	sync_flow_context();
	const opennova::HttpRequestSpec spec = flow_.gsb_request();
	if (!spec.valid) {
		return; // no base URL yet — nothing to fetch
	}
	if (gsb_request_in_flight_) {
		browser_http_->cancel_request();
	}
	if (ship_spec(browser_http_, spec) != OK) {
		gsb_request_in_flight_ = false;
		UtilityFunctions::push_warning(String("[NovaWorldClient] GSB request did not start: ")
			+ String(spec.url.c_str()));
		return;
	}
	gsb_request_in_flight_ = true;
}

void NovaWorldClient::on_gsb_request_completed(int result, int response_code,
                                               const PackedStringArray &headers,
                                               const PackedByteArray &body) {
	(void)headers; // GSB does not merge Set-Cookie (the flow's contract)
	gsb_request_in_flight_ = false;

	// NOTE: on_gsb_response's arg order differs from login/join — body 3rd, out 4th, no headers.
	opennova::GsbResponse parsed;
	if (!flow_.on_gsb_response(result == HTTPRequest::RESULT_SUCCESS, response_code,
	                           from_pba(body), parsed)) {
		UtilityFunctions::push_warning(String("[NovaWorldClient] GSB fetch failed result=")
			+ String::num_int64(result) + " code=" + String::num_int64(response_code));
		return;
	}

	Array rows;
	for (const auto &s : parsed.servers) {
		Dictionary row;
		row["rid"] = static_cast<int64_t>(s.rid);
		row["name"] = String(s.server_name.c_str());
		row["game_type"] = String(s.game_type.c_str());
		row["mission_name"] = String(s.mission_name.c_str());
		row["players"] = s.players;
		row["max_players"] = s.max_players;
		row["dedicated"] = String(s.dedicated.c_str());
		row["password"] = String(s.password.c_str());
		row["country"] = String(s.country.c_str());
		row["region"] = String(s.region.c_str());
		// Row dword1 is the host's IPv4 — retail's browser pings it on the XXXX
		// finalize [orig: NapiGameList_StartPingSweep @ 0x63BCF0]. The connect
		// address is still resolved on join via the NK token (/NWJoin.dll?rid=).
		row["ip"] = String(s.ip.c_str());
		rows.push_back(row);
	}
	server_rows_ = rows;
	trace(String("server browser: ") + String::num_int64(rows.size()) + " server(s)");
	emit_signal("server_list_updated", server_rows_);
}

// ---- Account login (EPASK) — ADR 0010 Phase 3 --------------------------

void NovaWorldClient::login(const String &username, const String &password) {
	if (login_http_ == nullptr) {
		emit_signal("login_failed", String("client not started"));
		return;
	}
	if (flow_.login_active()) {
		return;  // a login is already in flight
	}
	sync_flow_context();
	const opennova::LoginResult r = flow_.login(
		std::string(username.utf8().get_data()), std::string(password.utf8().get_data()));
	switch (r.kind) {
	case opennova::LoginResult::Kind::NeedRequest:
		// Prepare GET: sets the EPASK cookie (the bundle credentials encrypt under).
		if (ship_spec(login_http_, r.request) != OK) {
			flow_.on_login_response(false, 0, {}, {});  // drive the machine back to Idle
			emit_signal("login_failed", String("prepare request failed"));
		}
		break;
	case opennova::LoginResult::Kind::Failed:
		// e.g. "no gate startup_url yet — connect first".
		emit_signal("login_failed", String(r.reason.c_str()));
		break;
	default:
		break;  // Succeeded is impossible synchronously
	}
}

void NovaWorldClient::on_login_request_completed(int result, int response_code,
                                                 const PackedStringArray &headers,
                                                 const PackedByteArray &body) {
	const opennova::LoginResult r = flow_.on_login_response(
		result == HTTPRequest::RESULT_SUCCESS, response_code, pba_to_strvec(headers), from_pba(body));
	switch (r.kind) {
	case opennova::LoginResult::Kind::NeedRequest:
		// Re-ship on login_http_ (PREPARE -> NWSTART -> POST -> POLL, all in the flow).
		if (ship_spec(login_http_, r.request) != OK) {
			flow_.on_login_response(false, 0, {}, {});
			emit_signal("login_failed", String("login request failed to start"));
		}
		break;
	case opennova::LoginResult::Kind::Succeeded:
		trace(String("logged in as ")
			+ String(r.nwhandle.c_str()) + " (PCID " + String(r.pcid.c_str()) + ")");
		emit_signal("login_succeeded", String(r.nwhandle.c_str()));
		trigger_gsb();  // re-fetch the browser now authenticated (NWHANDLE/PCID ride along)
		break;
	case opennova::LoginResult::Kind::Failed:
		emit_signal("login_failed", String(r.reason.c_str()));
		break;
	}
}

// ---- Join a hosted game — ADR 0010 Phase 5 -----------------------------

void NovaWorldClient::join(int rid) {
	if (join_http_ == nullptr) {
		emit_signal("error_occurred", String("client not started"));
		return;
	}
	if (flow_.join_active()) {
		return;  // a join is already in flight
	}
	sync_flow_context();
	const opennova::JoinResult r = flow_.join(static_cast<uint32_t>(rid));
	switch (r.kind) {
	case opennova::JoinResult::Kind::NeedRequest:
		enter_state(STATE_JOINING);
		// First NWJoin call: stores a NWJOINSESSIONTAG and returns the relay page.
		if (ship_spec(join_http_, r.request) != OK) {
			flow_.on_join_response(false, 0, {}, {});  // drive the machine back to Idle
			emit_signal("error_occurred", String("join request failed to start"));
		}
		break;
	case opennova::JoinResult::Kind::Failed:
		// Synchronous failure (e.g. "no server base URL — connect first") — no state change.
		emit_signal("error_occurred", String(r.reason.c_str()));
		break;
	default:
		break;  // Resolved is impossible synchronously
	}
}

void NovaWorldClient::on_join_request_completed(int result, int response_code,
                                                const PackedStringArray &headers,
                                                const PackedByteArray &body) {
	const opennova::JoinResult r = flow_.on_join_response(
		result == HTTPRequest::RESULT_SUCCESS, response_code, pba_to_strvec(headers), from_pba(body));
	switch (r.kind) {
	case opennova::JoinResult::Kind::NeedRequest:
		// Re-ship on join_http_ (FIRST -> SECOND, both resolved in the flow).
		if (ship_spec(join_http_, r.request) != OK) {
			flow_.on_join_response(false, 0, {}, {});
			emit_signal("error_occurred", String("join resolve failed to start"));
		}
		break;
	case opennova::JoinResult::Kind::Resolved:
		trace(String("join resolved host ") + String(r.host_ip.c_str()) + ":"
			+ String::num_int64(static_cast<int64_t>(r.host_port)));
		resolve_join_target(String(r.host_ip.c_str()), r.host_port);
		break;
	case opennova::JoinResult::Kind::Failed:
		// D-1: any async join failure falls back to the lobby (CONNECTED) — consolidates
		// the old non-200 (formerly stuck JOINING) and bad-.joi (CONNECTED) into one path.
		emit_signal("error_occurred", String(r.reason.c_str()));
		enter_state(STATE_CONNECTED);
		break;
	}
}

// The NWJoin handshake has resolved the in-match host:port. Hand that off to the game layer and
// stop — the panel routes joined_game into NovaSimulation's joiner (load_mission_as_joiner ->
// enable_join), which owns the SINGLE in-match ClientHello (joiner_pump's runtime_->start(), the
// witnessed CNapiGameSession_InitNPConnection path). We deliberately do NOT send our own in-match
// hello here: that would be a second, conflicting handshake on a third socket (the old "send one
// hello and stop" dead-end that never reached gameplay). LAN, NW-routed, and env joins now converge
// on the one joiner seam (ADR 0009; .agents/README.md "do not create a second gameplay network path").
void NovaWorldClient::resolve_join_target(const String &host, uint16_t port) {
	trace(String("join target resolved ") + host + ":"
		+ String::num_int64(static_cast<int64_t>(port))
		+ " — handing off to the in-match joiner (NovaSimulation owns the ClientHello)");
	enter_state(STATE_IN_GAME_HELLO);
	emit_signal("joined_game", host, static_cast<int>(port));
}

void NovaWorldClient::enter_state(State next, const String &reason) {
	if (state_ == next) return;
	const State previous = state_;
	state_ = next;
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
