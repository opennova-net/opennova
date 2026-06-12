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
#include <novacrypto/epask.h>
#include <novaworld/client_session.h>
#include <novaworld/gate_probe.h>
#include <novaworld/gate_response.h>
#include <novaworld/gsb.h>
#include <novaworld/http_login.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
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

NovaWorldClient::NovaWorldClient() = default;
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
	client_index_ = pick_random_uint32();
	client_key_ = pick_random_uint32();
	session_.reset();
	game_session_.reset();
	cookie_jar_ = opennova::CookieJar{};
	login_step_ = LOGIN_IDLE;
	login_poll_count_ = 0;
	join_step_ = JOIN_IDLE;
	nwhandle_ = String();
	pcid_ = String();
	nw_udp_port_ = 0;
	nw_udp_host_ = String();
	nw_web_domain_ = String();
	identity_vars_.clear();
	handshake_elapsed_ = 0.0;

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

void NovaWorldClient::stop() {
	if (session_ && nw_socket_.is_valid() &&
	    (state_ == STATE_CONNECTED || state_ == STATE_SESSION_HELLO || state_ == STATE_SESSION_JOIN)) {
		send_nw_datagram(session_->build_goodbye());
	}
	session_.reset();
	game_session_.reset();
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
	login_step_ = LOGIN_IDLE;
	join_step_ = JOIN_IDLE;
	if (gate_socket_.is_valid()) {
		gate_socket_->close();
		gate_socket_.unref();
	}
	if (nw_socket_.is_valid()) {
		nw_socket_->close();
		nw_socket_.unref();
	}
	if (game_socket_.is_valid()) {
		game_socket_->close();
		game_socket_.unref();
	}
	if (state_ != STATE_DISCONNECTED && state_ != STATE_IDLE) {
		enter_state(STATE_DISCONNECTED, String("stopped"));
	}
}

void NovaWorldClient::_process(double delta) {
	if (state_ == STATE_IDLE || state_ == STATE_DISCONNECTED || state_ == STATE_ERROR) {
		return;
	}

	if (gate_socket_.is_valid()) {
		poll_gate();
	}
	if (nw_socket_.is_valid()) {
		poll_session();
	}

	tick_accum_ += delta;
	if (state_ == STATE_CONNECTED && tick_accum_ >= heartbeat_interval_s_) {
		tick_accum_ = 0.0;
		if (session_) {
			send_nw_datagram(session_->build_heartbeat());
		}
	}

	if (state_ == STATE_GATE_PROBING || state_ == STATE_SESSION_HELLO || state_ == STATE_SESSION_JOIN) {
		handshake_elapsed_ += delta;
		if (handshake_elapsed_ >= handshake_timeout_s_) {
			enter_state(STATE_ERROR,
			            String("handshake timeout in state ") + state_name(state_));
		}
	}
}

void NovaWorldClient::send_gate_probe() {
	if (!gate_socket_.is_valid()) return;

	auto probe = opennova::gate_probe_build(opennova::GATE_PROBE_TAG_JOINTOPS);
	// Wrap the NWU payload in the LSB-scatter CRC envelope the gate expects.
	// Retail does the same; the server strips it via napi_envelope_decode, and
	// without it the gate logs "bad envelope". (Same envelope the session
	// channel uses in libs/novaworld/client_session.)
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

void NovaWorldClient::poll_gate() {
	if (!gate_socket_.is_valid()) return;

	while (gate_socket_->get_available_packet_count() > 0) {
		auto packet = gate_socket_->get_packet();
		auto bytes = from_pba(packet);

		// The server wraps the response in the LSB-scatter CRC envelope; strip
		// it before decrypt+parse (same envelope the session channel uses).
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
		UtilityFunctions::print(String("[NovaWorldClient] gate response: udp_code1='")
		    + String(parsed.udp_code1.c_str()) + "' udp_code2='"
		    + String(parsed.udp_code2.c_str()) + "' (empty => live NW likely needs login)");
		emit_signal("server_info_received", info);

		// Parse "host:port" out of UDPNOVAWORLD.
		const auto colon = parsed.udp_novaworld.find(':');
		if (colon == std::string::npos) {
			enter_state(STATE_ERROR, String("UDPNOVAWORLD malformed"));
			return;
		}
		nw_udp_host_ = String(parsed.udp_novaworld.substr(0, colon).c_str());
		try {
			nw_udp_port_ = static_cast<uint16_t>(std::stoi(parsed.udp_novaworld.substr(colon + 1)));
		} catch (...) {
			enter_state(STATE_ERROR, String("UDPNOVAWORLD port parse failed"));
			return;
		}

		begin_session();
	}
}

// Deterministic [A-Z] string of `len` chars from a seed. Retail's NWPSSK/NWUSID
// are machine hardware fingerprints (CDKey_GenerateHardwareFingerprint @ 0x4a4a00
// / generate_hardware_fingerprint @ 0x4a4d00). The genuine .204 capture (frame
// 10166) shows the lobby verify is NOT gated on them — NWCDKIID/NWCDKIIDEXP1 are
// empty there and the server still returns Success=1 — so a stable plausible
// value is sufficient for parity. Deliberately not a real hardware fingerprint.
static std::string az_fingerprint(uint32_t seed, int len) {
	static const char kAlpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
	uint32_t x = seed ? seed : 0x12345678u;
	std::string s;
	s.reserve(static_cast<size_t>(len));
	for (int i = 0; i < len; ++i) {
		x = x * 1664525u + 1013904223u;
		s.push_back(kAlpha[(x >> 24) % 26u]);
	}
	return s;
}

// Create the session state machine and send its ClientHello. Called once the
// gate response yields the NW UDP host:port.
void NovaWorldClient::begin_session() {
	opennova::ClientSession::Config cfg;
	cfg.client_index = client_index_;
	cfg.client_key = client_key_;

	// 0x42 join CU set — byte-structure-matched to the genuine .204 capture
	// (frame 8977, decoded by tests/novaworld/nw204_lobby_decode_test): all 11
	// chunks, type=2, in retail's ConnectToNovaWorld @ 0x4d4640 order. The locale
	// + MetTag chunks are EMPTY on the join in retail (filled later in the verify
	// instead); UdpCode1/2 come straight from the gate (retail also got the
	// placeholders "abc"/"xyz" and still validated, so login is NOT a prereq —
	// NW-S5). type=2 matches retail (was type=1); HandleClientJoin accepts 1 or 2.
	auto gate_str = [&](const char *info_key) -> std::string {
		if (server_info_.has(info_key)) {
			String v = server_info_[info_key];
			return std::string(v.utf8().get_data());
		}
		return {};
	};
	cfg.cu_vars.push_back({"Application", "OpennovaGodotClient.exe", 2});
	cfg.cu_vars.push_back({"BuildDateAndTime", "Jul 21 2009 18:54:41", 2});
	cfg.cu_vars.push_back({"Debug", "0", 2});
	cfg.cu_vars.push_back({"CountryName", "", 2});
	cfg.cu_vars.push_back({"Language", "", 2});
	cfg.cu_vars.push_back({"TimeZoneBias", "", 2});
	cfg.cu_vars.push_back({"GateTag", cfg.na, 2});
	cfg.cu_vars.push_back({"MetTag", gate_str("met_label"), 2});
	cfg.cu_vars.push_back({"UdpCode1", gate_str("udp_code1"), 2});
	cfg.cu_vars.push_back({"UdpCode2", gate_str("udp_code2"), 2});
	cfg.cu_vars.push_back({"MaxPacketSize", "1300", 2});

	// Verify "Cookie" var-list (NW-S5) — the identity set the 892B
	// ClientRequestVerifyResult carries (capture frame 10166). NWUID is filled by
	// ClientSession from the ServerSessionInit. CD-key fields are empty (retail
	// sent them empty and still validated); the hardware fingerprints + NWHWI are
	// best-effort telemetry the lobby verify does not gate on. Locale is filled
	// here (unlike the join). Harmless against the permissive OpenNova server.
	std::string country = "United States", language = "English", tz_bias = "0";
	{
		Dictionary tz = Time::get_singleton()->get_time_zone_from_system();
		if (tz.has(String("bias"))) {
			int64_t bias = tz[String("bias")];
			tz_bias = std::to_string(bias);
		}
	}
	std::string nwhwi = std::string("OpenNova$0$2048$1920x1080$1920x1080");
	// Built once, reused for BOTH the UDP verify var-list and the HTTP login
	// cookies (the .204 capture shows the same identity set in both places).
	identity_vars_ = {
	    {"CountryName", country},
	    {"Language", language},
	    {"TimeZoneBias", tz_bias},
	    {"MyInstalledExpBits", "0"},
	    {"NWUID", ""},          // echoed from the ServerSessionInit at runtime
	    {"NWCDKIID", ""},       // empty in retail; verify is not CD-key-gated
	    {"NWCDKIIDEXP1", ""},
	    {"NWPSSK", az_fingerprint(client_index_ ^ 0x5053534Bu, 23)},
	    {"NWUSID", az_fingerprint(client_key_ ^ 0x55534944u, 16)},
	    {"NWHWI", nwhwi},
	};
	cfg.verify_cookie_vars = identity_vars_;

	UtilityFunctions::print(String("[NovaWorldClient] 0x42 join carries ")
	    + String::num_int64(static_cast<int64_t>(cfg.cu_vars.size()))
	    + " CU chunks; verify carries "
	    + String::num_int64(static_cast<int64_t>(cfg.verify_cookie_vars.size()))
	    + " Cookie vars");

	session_ = std::make_unique<opennova::ClientSession>(cfg);

	enter_state(STATE_SESSION_HELLO);
	handshake_elapsed_ = 0.0;
	send_nw_datagram(session_->start());
}

// Short name for a ClientSession::State int (for the handshake diagnostics).
static const char *cs_state_name(int s) {
	switch (s) {
	case 0: return "idle";    case 1: return "hello";    case 2: return "auth";
	case 3: return "verifying"; case 4: return "verified"; case 5: return "closed";
	case 6: return "error";   default: return "?";
	}
}

void NovaWorldClient::send_nw_datagram(const std::vector<uint8_t> &dg) {
	if (!nw_socket_.is_valid() || dg.empty()) return;
	nw_socket_->set_dest_address(nw_udp_host_, nw_udp_port_);
	nw_socket_->put_packet(to_pba(dg));

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
	UtilityFunctions::print(String("[NovaWorldClient] >> sent ")
	    + String::num_int64(static_cast<int64_t>(dg.size())) + "B op=" + String(op_buf)
	    + " to " + nw_udp_host_ + ":"
	    + String::num_int64(static_cast<int64_t>(nw_udp_port_)) + " local_port="
	    + String::num_int64(static_cast<int64_t>(nw_socket_->get_local_port()))
	    + " state=" + cs_state_name(session_ ? static_cast<int>(session_->state()) : -1));
}

void NovaWorldClient::poll_session() {
	if (!nw_socket_.is_valid() || !session_) return;

	while (nw_socket_->get_available_packet_count() > 0) {
		auto packet = nw_socket_->get_packet();
		auto bytes = from_pba(packet);
		const String src_ip = nw_socket_->get_packet_ip();
		const int src_port = nw_socket_->get_packet_port();
		const int state_before = static_cast<int>(session_->state());

		// Peek the plaintext session opcode (read-only; handle_datagram
		// re-decodes independently). 0x81=ServerHello, 0x82=ServerAuth,
		// 0x83=ServerProtocolMessage. Tells us exactly what the server sent.
		char op_buf[8] = "0x??";
		{
			std::vector<uint8_t> stripped(bytes.size());
			size_t ssz = 0;
			if (opennova::napi_envelope_decode(bytes.data(), bytes.size(),
			        stripped.data(), stripped.size(), &ssz) == 0 && ssz >= 1) {
				std::snprintf(op_buf, sizeof(op_buf), "0x%02x", stripped[0]);
			} else {
				std::snprintf(op_buf, sizeof(op_buf), "BADENV");
			}
		}

		// Hand the datagram to the protocol state machine; send whatever it
		// asks us to. All NWU/CRC/TLV/scrk handling lives in client_session.
		std::vector<std::vector<uint8_t>> replies;
		const bool ok = session_->handle_datagram(bytes.data(), bytes.size(), replies);
		const int state_after = static_cast<int>(session_->state());

		// Capture the real web host from the ServerSessionInit (0x82) — it carries
		// the NovaworldWebDomainNameAndPortNumber the gate's startupurl leaves as
		// "[domainname]". This is what makes the HTTP login/GSB/join target real NW.
		if (nw_web_domain_.is_empty() && !session_->server_web_domain().empty()) {
			nw_web_domain_ = String(session_->server_web_domain().c_str());
			UtilityFunctions::print(String("[NovaWorldClient] web host (SessionInit): ")
			    + nw_web_domain_);
		}

		// Diagnostics: a recv line for every inbound datagram. If state doesn't
		// advance (e.g. auth->auth) with ok=1 and replies=0, the datagram was
		// an unexpected opcode the session ignored; if no recv line appears
		// after the ClientAuth send, the server sent nothing (or it was lost).
		String msg = String("[NovaWorldClient] << recv ")
		    + String::num_int64(static_cast<int64_t>(bytes.size())) + "B op=" + String(op_buf)
		    + " from " + src_ip + ":"
		    + String::num_int64(static_cast<int64_t>(src_port)) + " state "
		    + cs_state_name(state_before) + "->" + cs_state_name(state_after)
		    + " ok=" + (ok ? "1" : "0") + " replies="
		    + String::num_int64(static_cast<int64_t>(replies.size()));
		if (!ok) msg += String(" err='") + String(session_->last_error().c_str()) + "'";
		// After the 0x82 ServerAuth, expose the parsed server SK + scrk: the SK
		// becomes the session_id on our outbound 0x43, and opennova-int's 0x43
		// handler DROPS the packet (no reply) unless that session_id == the SK
		// it issued. A zero/garbage SK here would explain the missing 0x83.
		{
			char sk_buf[40];
			std::snprintf(sk_buf, sizeof(sk_buf), " server_sk=0x%08x sscrk=%dB",
			              static_cast<unsigned>(session_->server_key()),
			              static_cast<int>(session_->server_scrk().size()));
			msg += String(sk_buf);
		}
		UtilityFunctions::print(msg);

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

// Map the libs-side session state onto our public State + signals. CONNECTED
// means the lobby verify handshake completed (ServerVerifyResult) — that's
// when the panel enables Host-a-Game.
void NovaWorldClient::sync_session_state() {
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
		if (state_ != STATE_CONNECTED) {
			enter_state(STATE_CONNECTED);
			handshake_elapsed_ = 0.0;
			tick_accum_ = 0.0;
			// Session is lobby-ready — fetch the server browser (Phase 2).
			request_server_list();
		}
		break;
	case S::Error:
		enter_state(STATE_ERROR, String(session_->last_error().c_str()));
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
	request_server_list();
}

// The GSB blob is served at <startup_url>/jop_2.gsb. The retail client GETs a
// server-provided browser URL (NW-G3 — the path is not a client literal); for
// OpenNova we derive it from the gate response's STARTUPURL so the same path
// works for both the OpenNova and real-NovaWorld targets.
String NovaWorldClient::gsb_url() const {
	// The GSB lives at <host>/jop_2.gsb. Derive the host from the gate's
	// STARTUPURL (which is the full /nwprepare.dll?... bootstrap URL, NOT a bare
	// host), the same way the login/join legs do — appending to the full
	// startup_url would yield ".../jop_2_start.htm/jop_2.gsb". This matches the
	// server's own GSB_SERVER template value (http_listener.cpp).
	const String base = http_base();
	if (base.is_empty()) {
		return String();
	}
	// ?a=1 matches the retail GSB fetch (capture frame 43624); a no-op query for
	// the OpenNova server (it routes on the path).
	return base + String("/jop_2.gsb?a=1");
}

void NovaWorldClient::request_server_list() {
	if (browser_http_ == nullptr) {
		return;
	}
	const String url = gsb_url();
	if (url.is_empty()) {
		return; // no startup_url yet — nothing to fetch
	}
	if (gsb_request_in_flight_) {
		browser_http_->cancel_request();
	}
	// Carry the login cookie jar: on real NW the GSB fetch is cookie-authenticated
	// (NW-S5/B3 — the engine attaches every subnet cookie to every request);
	// harmless to the permissive OpenNova server (empty jar -> no Cookie header).
	const Error err = browser_http_->request(url, request_headers(false));
	if (err != OK) {
		gsb_request_in_flight_ = false;
		UtilityFunctions::print(String("[NovaWorldClient] GSB request did not start: ") + url);
		return;
	}
	gsb_request_in_flight_ = true;
}

void NovaWorldClient::on_gsb_request_completed(int result, int response_code,
                                               const PackedStringArray &headers,
                                               const PackedByteArray &body) {
	(void)headers;
	gsb_request_in_flight_ = false;

	if (result != HTTPRequest::RESULT_SUCCESS || response_code != 200) {
		UtilityFunctions::print(String("[NovaWorldClient] GSB fetch failed result=")
			+ String::num_int64(result) + " code=" + String::num_int64(response_code));
		return;
	}

	opennova::GsbResponse parsed;
	if (!opennova::gsb_parse_response(reinterpret_cast<const uint8_t *>(body.ptr()),
	                                  static_cast<size_t>(body.size()), parsed)) {
		UtilityFunctions::print(String("[NovaWorldClient] GSB parse failed (")
			+ String::num_int64(body.size()) + " bytes)");
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
		char ip_buf[32];
		std::snprintf(ip_buf, sizeof(ip_buf), "%u.%u.%u.%u",
		              s.ip[0], s.ip[1], s.ip[2], s.ip[3]);
		row["ip"] = String(ip_buf);
		rows.push_back(row);
	}
	server_rows_ = rows;
	UtilityFunctions::print(String("[NovaWorldClient] server browser: ")
		+ String::num_int64(rows.size()) + " server(s)");
	emit_signal("server_list_updated", server_rows_);
}

// ---- HTTP helpers (shared by login + join) -----------------------------

// The HTTP base "scheme://host:port" the legacy NW*.dll routes live under,
// derived from the gate response's STARTUPURL (which points at /nwprepare.dll on
// that host). Falls back to the gate's POST ip:port.
String NovaWorldClient::http_base() const {
	// Real NW: the gate's startupurl carries a "[domainname]" placeholder; the
	// concrete web host arrives in the SessionInit (e.g. 207.178.209.204:80). Use
	// it ONLY in that templated case so the OpenNova path (concrete startupurl)
	// stays byte-identical — our own SessionInit also carries a web-domain CU.
	const bool templated = server_info_.has("startup_url") &&
		String(server_info_["startup_url"]).find("[domainname]") >= 0;
	if (templated && !nw_web_domain_.is_empty()) {
		String d = nw_web_domain_;
		if (!d.begins_with("http://") && !d.begins_with("https://")) {
			d = String("http://") + d;
		}
		return d;
	}
	if (server_info_.has("startup_url")) {
		String su = server_info_["startup_url"];
		const int scheme = su.find("://");
		if (scheme >= 0) {
			const int path = su.find("/", scheme + 3);
			return path >= 0 ? su.substr(0, path) : su;
		}
	}
	if (server_info_.has("post_ip") && server_info_.has("post_port")) {
		return String("http://") + String(server_info_["post_ip"]) + ":" +
		       String::num_int64(static_cast<int64_t>(static_cast<int>(server_info_["post_port"])));
	}
	return String();
}

// Headers for an HTTP request: the cookie jar (retail attaches every subnet
// cookie to every request — NW-S5/B3, so the GSB/join GETs carry NWHANDLE/PCID)
// plus, for the login POST, the form content type.
PackedStringArray NovaWorldClient::request_headers(bool form_content_type) const {
	PackedStringArray h;
	if (form_content_type) {
		h.push_back("Content-Type: application/x-www-form-urlencoded");
	}
	const std::string ch = cookie_jar_.cookie_header();
	if (!ch.empty()) {
		h.push_back(String("Cookie: ") + String(ch.c_str()));
	}
	return h;
}

// Feed every `Set-Cookie:` response header into the jar (case-insensitive prefix;
// HTTPRequest hands headers back as "Name: value" lines).
void NovaWorldClient::merge_response_cookies(const PackedStringArray &headers) {
	std::vector<std::string> values;
	for (int i = 0; i < headers.size(); ++i) {
		const String line = headers[i];
		if (line.to_lower().begins_with("set-cookie:")) {
			const String v = line.substr(11).strip_edges();  // strlen("set-cookie:") == 11
			values.push_back(std::string(v.utf8().get_data()));
		}
	}
	if (!values.empty()) {
		cookie_jar_.merge_set_cookie_values(values);
	}
}

// ---- Account login (EPASK) — ADR 0010 Phase 3 --------------------------

// Resolve the gate's startupurl. OpenNova serves a concrete URL (pass-through —
// keeps the proven local path byte-identical); real NW serves a template with
// [domainname]/[VER1]/[VER2]/[CC]/[GT] placeholders the client substitutes
// (witnessed in the .204 capture: ver1=3, ver2=2345, cc=us, gt=jop:cus2,
// domainname = the SessionInit web host).
String NovaWorldClient::resolve_startup_url() const {
	String su = server_info_.has("startup_url")
		? String(server_info_["startup_url"]) : String();
	if (su.is_empty() || su.find("[domainname]") < 0) {
		return su;  // concrete (OpenNova) — unchanged
	}
	su = su.replace("[domainname]", nw_web_domain_);  // "host:port" from SessionInit
	su = su.replace("[VER1]", "3").replace("[VER2]", "2345");
	String cc = "us";  // [CC] = ISO country from the OS locale (en_US -> us)
	const String locale = OS::get_singleton()->get_locale();
	const int us = locale.find("_");
	if (us >= 0) {
		const String region = locale.substr(us + 1).to_lower();
		if (!region.is_empty()) cc = region;
	}
	su = su.replace("[CC]", cc).replace("[GT]", "jop:cus2");
	return su;
}

// Seed the CD-key/hardware identity into the jar — the real-NW login POST carries
// these as cookies (capture frame 27663), the same set as the UDP verify var-list.
// NWUID comes from the SessionInit.
void NovaWorldClient::seed_identity_cookies() {
	for (const auto &kv : identity_vars_) {
		std::string value = kv.second;
		if (kv.first == "NWUID" && value.empty() && session_) {
			value = session_->server_nwuid();
		}
		cookie_jar_.set(kv.first, value);
	}
}

// Build the credential POST body and send it. Every field is EPASK-encrypted
// EXCEPT the echoed EPASK bundle (witnessed in the .204 capture, frame 27663 —
// our earlier plaintext hidden fields were the source of the server's benign
// "non-A-P decrypt" warnings). Identity cookies are already seeded into the jar.
void NovaWorldClient::send_login_post() {
	using opennova::LoginFormField;
	std::vector<LoginFormField> fields = {
		{"EPASK", opennova::epask_to_string(epask_), false},
		{"NAME", std::string(login_user_.utf8().get_data()), true},
		{"PASSWORD", std::string(login_pass_.utf8().get_data()), true},
		{"rememberlogindata", "", false},
		{"rememberlogin", "0", true},
		{"pfid", "28", true},
		{"needtoagree", "jop_2_needtoagree.htm", true},
		{"nodb", "jop_2_nodb.htm", true},
		{"relay", "jop_2_relay.htm", true},
		{"msgbase", "jop_2_msg.htm", true},
		{"enterkey", "jop_2_key.htm", true},
		{"failure", "jop_2_login.htm", true},
		{"success", "jop_2_main.htm", true},
	};
	const std::string post_body = opennova::build_login_post_body(epask_, fields);
	login_step_ = LOGIN_POST;
	const Error err = login_http_->request(http_base() + String("/NWLogin.dll"),
		request_headers(true), HTTPClient::METHOD_POST, String(post_body.c_str()));
	if (err != OK) {
		login_step_ = LOGIN_IDLE;
		emit_signal("login_failed", String("login POST failed to start"));
	}
}

void NovaWorldClient::login(const String &username, const String &password) {
	if (login_http_ == nullptr) {
		emit_signal("login_failed", String("client not started"));
		return;
	}
	if (login_step_ != LOGIN_IDLE) {
		return;  // a login is already in flight
	}
	const String prepare_url = resolve_startup_url();
	if (prepare_url.is_empty()) {
		emit_signal("login_failed", String("no gate startup_url yet — connect first"));
		return;
	}
	login_user_ = username;
	login_pass_ = password;
	login_step_ = LOGIN_PREPARE;
	login_poll_count_ = 0;
	// Prepare GET: sets the EPASK cookie (the bundle we encrypt credentials under).
	const Error err = login_http_->request(prepare_url, request_headers(false),
	                                        HTTPClient::METHOD_GET, String());
	if (err != OK) {
		login_step_ = LOGIN_IDLE;
		emit_signal("login_failed", String("prepare request failed"));
	}
}

// Build a /NWLogin.dll poll URL carrying the LOGINSESSIONTAG as ?tag= (an HTTP/1.0
// cookie-loss workaround the OpenNova server reads; the cookie rides too, and real
// NW reads the cookie). Returns the base URL when no tag is present.
static String nwlogin_poll_url(const String &base, const opennova::CookieJar &jar) {
	const std::string *tag = jar.find("LOGINSESSIONTAG");
	if (tag && !tag->empty()) return base + String("/NWLogin.dll?tag=") + String(tag->c_str());
	return base + String("/NWLogin.dll");
}

void NovaWorldClient::on_login_request_completed(int result, int response_code,
                                                 const PackedStringArray &headers,
                                                 const PackedByteArray &body) {
	(void)body;
	const LoginStep step = login_step_;
	if (result != HTTPRequest::RESULT_SUCCESS || response_code != 200) {
		login_step_ = LOGIN_IDLE;
		emit_signal("login_failed", String("login HTTP failed (code ")
			+ String::num_int64(response_code) + ")");
		return;
	}
	merge_response_cookies(headers);

	switch (step) {
	case LOGIN_PREPARE: {
		const std::string *epask = cookie_jar_.find("EPASK");
		if (epask == nullptr || epask->empty()) {
			login_step_ = LOGIN_IDLE;
			emit_signal("login_failed", String("server issued no EPASK cookie"));
			return;
		}
		try {
			epask_ = opennova::epask_from_string(*epask);
		} catch (const std::exception &e) {
			login_step_ = LOGIN_IDLE;
			emit_signal("login_failed", String("bad EPASK bundle: ") + String(e.what()));
			return;
		}
		// Seed the identity cookies the login POST carries (the browser form
		// fields OnNovaWorldConnected sets), NWUID from the SessionInit.
		seed_identity_cookies();
		// Real NW (templated startupurl) requires the NWStart.dll version/junction
		// gate before the login POST (capture frame 12178); the OpenNova server
		// answers the POST directly, so skip it there to keep the proven path.
		const bool templated = server_info_.has("startup_url") &&
			String(server_info_["startup_url"]).find("[domainname]") >= 0;
		if (templated) {
			login_step_ = LOGIN_NWSTART;
			const String nwstart_url = http_base() + String("/NWStart.dll?MSGBASE=jop_2_msg.htm"
				"&IN=jop_2_main.htm&OUT=jop_2_login.htm&verfile=jop_2.ver"
				"&newupdateavailable=jop_2_newupdateavailable.htm"
				"&newupdateavailablewithbypass=jop_2_newupdateavailable2.htm"
				"&junction=jop_2_junction.htm");
			const Error err = login_http_->request(nwstart_url, request_headers(false),
				HTTPClient::METHOD_GET, String());
			if (err != OK) {
				login_step_ = LOGIN_IDLE;
				emit_signal("login_failed", String("NWStart request failed to start"));
			}
		} else {
			send_login_post();
		}
		break;
	}
	case LOGIN_NWSTART:
		send_login_post();
		break;
	case LOGIN_POST: {
		// The server sets LOGINSESSIONTAG only when it accepted the credential
		// submit; its absence means bad credentials / a rendered failure page.
		const std::string *tag = cookie_jar_.find("LOGINSESSIONTAG");
		if (tag == nullptr || tag->empty()) {
			login_step_ = LOGIN_IDLE;
			emit_signal("login_failed", String("login rejected (no session tag)"));
			return;
		}
		login_step_ = LOGIN_POLL;
		login_poll_count_ = 0;
		const Error err = login_http_->request(nwlogin_poll_url(http_base(), cookie_jar_),
			request_headers(false), HTTPClient::METHOD_GET, String());
		if (err != OK) {
			login_step_ = LOGIN_IDLE;
			emit_signal("login_failed", String("login poll failed to start"));
		}
		break;
	}
	case LOGIN_POLL: {
		// Poll /NWLogin.dll until the auth completes and the identity cookies
		// (NWHANDLE/PCID) populate (capture frames 28856 -> 39723).
		const std::string *nh = cookie_jar_.find("NWHANDLE");
		const std::string *pc = cookie_jar_.find("PCID");
		if (nh && !nh->empty()) {
			login_step_ = LOGIN_IDLE;
			nwhandle_ = String(nh->c_str());
			pcid_ = (pc && !pc->empty()) ? String(pc->c_str()) : String();
			UtilityFunctions::print(String("[NovaWorldClient] logged in as ") + nwhandle_
				+ " (PCID " + pcid_ + ")");
			emit_signal("login_succeeded", nwhandle_);
			request_server_list();
			return;
		}
		static const int kMaxLoginPolls = 10;
		if (++login_poll_count_ >= kMaxLoginPolls) {
			login_step_ = LOGIN_IDLE;
			emit_signal("login_failed", String("login did not complete (no account handle)"));
			return;
		}
		const Error err = login_http_->request(nwlogin_poll_url(http_base(), cookie_jar_),
			request_headers(false), HTTPClient::METHOD_GET, String());
		if (err != OK) {
			login_step_ = LOGIN_IDLE;
			emit_signal("login_failed", String("login poll failed to start"));
		}
		break;
	}
	default:
		login_step_ = LOGIN_IDLE;
		break;
	}
}

// ---- Join a hosted game — ADR 0010 Phase 5 -----------------------------

void NovaWorldClient::join(int rid) {
	if (join_http_ == nullptr) {
		emit_signal("error_occurred", String("client not started"));
		return;
	}
	if (join_step_ != JOIN_IDLE) {
		return;  // a join is already in flight
	}
	if (http_base().is_empty()) {
		emit_signal("error_occurred", String("no server base URL — connect first"));
		return;
	}
	join_rid_ = rid;
	join_step_ = JOIN_FIRST;
	enter_state(STATE_JOINING);
	// First NWJoin call: stores a NWJOINSESSIONTAG and returns the relay page. Full
	// witnessed query (capture frame 59656); the OpenNova server ignores the extra
	// template params (only rid/success matter to it).
	const String url = http_base() + String("/NWJoin.dll?needexpkey=jop_2_key2err.htm"
		"&success=jop_2_join.joi&failure=jop_2_main.htm&relay=jop_2_relay.htm"
		"&msgbase=jop_2_msg.htm&nodb=jop_2_nodb.htm&pfid=28&mode=Login&rid=")
		+ String::num_int64(rid);
	const Error err = join_http_->request(url, request_headers(false),
		HTTPClient::METHOD_GET, String());
	if (err != OK) {
		join_step_ = JOIN_IDLE;
		emit_signal("error_occurred", String("join request failed to start"));
	}
}

void NovaWorldClient::on_join_request_completed(int result, int response_code,
                                                const PackedStringArray &headers,
                                                const PackedByteArray &body) {
	const JoinStep step = join_step_;
	if (result != HTTPRequest::RESULT_SUCCESS || response_code != 200) {
		join_step_ = JOIN_IDLE;
		emit_signal("error_occurred", String("join HTTP failed (code ")
			+ String::num_int64(response_code) + ")");
		return;
	}
	merge_response_cookies(headers);

	switch (step) {
	case JOIN_FIRST: {
		const std::string *tag = cookie_jar_.find("NWJOINSESSIONTAG");
		const String tagq = (tag && !tag->empty())
			? (String("&tag=") + String(tag->c_str())) : String();
		join_step_ = JOIN_SECOND;
		// Second NWJoin call: resolves the host by rid and returns the .joi with
		// the connection tokens.
		const String url = http_base() + String("/NWJoin.dll?rid=")
			+ String::num_int64(join_rid_) + tagq;
		const Error err = join_http_->request(url, request_headers(false),
			HTTPClient::METHOD_GET, String());
		if (err != OK) {
			join_step_ = JOIN_IDLE;
			emit_signal("error_occurred", String("join resolve failed to start"));
		}
		break;
	}
	case JOIN_SECOND: {
		join_step_ = JOIN_IDLE;
		std::string body_str;
		if (body.size() > 0) {
			body_str.assign(reinterpret_cast<const char *>(body.ptr()),
			                static_cast<size_t>(body.size()));
		}
		const opennova::JoiConnection conn = opennova::parse_joi_connection_string(body_str);
		if (!conn.ok) {
			emit_signal("error_occurred", String("join: no connection string in .joi"));
			// Fall back to CONNECTED (still in the lobby).
			enter_state(STATE_CONNECTED);
			return;
		}
		int port = std::atoi(conn.np.c_str());
		if (port <= 0 || port > 65535) {
			emit_signal("error_occurred", String("join: bad host port"));
			enter_state(STATE_CONNECTED);
			return;
		}
		UtilityFunctions::print(String("[NovaWorldClient] join resolved host ")
			+ String(conn.ni.c_str()) + ":" + String(conn.np.c_str()));
		send_jointops_hello(String(conn.ni.c_str()), static_cast<uint16_t>(port));
		break;
	}
	default:
		join_step_ = JOIN_IDLE;
		break;
	}
}

// Open a UDP session to the host and send the JointOperations ClientHello. This
// is the proto-switch boundary: the connection protocol flips from NOVAWORLDUDP
// (the lobby session on nw_socket_) to JointOperations (the game session on
// game_socket_). We send exactly one hello and stop — in-match gameplay is out
// of scope (ADR 0009 seam). On our own server the hello is rejected at the PN
// check and recorded by the unknown-tracker pn: channel, which is the proof.
void NovaWorldClient::send_jointops_hello(const String &host, uint16_t port) {
	game_socket_.instantiate();
	if (game_socket_->bind(0, "0.0.0.0") != OK) {
		enter_state(STATE_ERROR, String("game UDP bind failed"));
		return;
	}
	auto cfg = opennova::ClientSession::Config::jointoperations();
	cfg.client_index = pick_random_uint32();
	cfg.client_key = pick_random_uint32();
	game_session_ = std::make_unique<opennova::ClientSession>(cfg);

	const auto hello = game_session_->start();
	game_socket_->set_dest_address(host, port);
	game_socket_->put_packet(to_pba(hello));
	UtilityFunctions::print(String("[NovaWorldClient] >> JointOperations ClientHello ")
		+ String::num_int64(static_cast<int64_t>(hello.size())) + "B to " + host + ":"
		+ String::num_int64(static_cast<int64_t>(port))
		+ " — proto switched NOVAWORLDUDP -> JointOperations");

	enter_state(STATE_IN_GAME_HELLO);
	emit_signal("joined_game", host, static_cast<int>(port));
}

void NovaWorldClient::enter_state(State next, const String &reason) {
	if (state_ == next) return;
	const State previous = state_;
	state_ = next;
	UtilityFunctions::print(
		String("[NovaWorldClient] ") + state_name(previous) + " -> " + state_name(next)
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
