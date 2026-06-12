#include "nova_world_client.h"

#include <godot_cpp/classes/http_request.hpp>
#include <godot_cpp/classes/ip.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <napi/envelope.h>
#include <novaworld/client_session.h>
#include <novaworld/gate_probe.h>
#include <novaworld/gate_response.h>
#include <novaworld/gsb.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <string>
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
	// Bound so the HTTPRequest.request_completed signal can target it.
	ClassDB::bind_method(
		D_METHOD("on_gsb_request_completed", "result", "response_code", "headers", "body"),
		&NovaWorldClient::on_gsb_request_completed);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "host"),     "set_host", "get_host");
	ADD_PROPERTY(PropertyInfo(Variant::INT,    "gate_port"), "set_gate_port", "get_gate_port");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "player_name"), "set_player_name", "get_player_name");

	ADD_SIGNAL(MethodInfo("server_info_received", PropertyInfo(Variant::DICTIONARY, "info")));
	ADD_SIGNAL(MethodInfo("server_list_updated", PropertyInfo(Variant::ARRAY, "rows")));
	ADD_SIGNAL(MethodInfo("connected"));
	ADD_SIGNAL(MethodInfo("disconnected", PropertyInfo(Variant::STRING, "reason")));
	ADD_SIGNAL(MethodInfo("error_occurred", PropertyInfo(Variant::STRING, "message")));
	ADD_SIGNAL(MethodInfo("state_changed", PropertyInfo(Variant::INT, "state")));

	BIND_ENUM_CONSTANT(STATE_IDLE);
	BIND_ENUM_CONSTANT(STATE_GATE_PROBING);
	BIND_ENUM_CONSTANT(STATE_SESSION_HELLO);
	BIND_ENUM_CONSTANT(STATE_SESSION_JOIN);
	BIND_ENUM_CONSTANT(STATE_CONNECTED);
	BIND_ENUM_CONSTANT(STATE_DISCONNECTED);
	BIND_ENUM_CONSTANT(STATE_ERROR);
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
	nw_udp_port_ = 0;
	nw_udp_host_ = String();
	handshake_elapsed_ = 0.0;

	// The server-browser HTTP fetcher is a child node (Phase 2). Created once
	// and reused; its request_completed signal drives on_gsb_request_completed.
	if (browser_http_ == nullptr) {
		browser_http_ = memnew(HTTPRequest);
		add_child(browser_http_);
		browser_http_->connect("request_completed",
		                       Callable(this, "on_gsb_request_completed"));
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
	if (browser_http_ != nullptr) {
		browser_http_->cancel_request();
	}
	gsb_request_in_flight_ = false;
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

// Create the session state machine and send its ClientHello. Called once the
// gate response yields the NW UDP host:port.
void NovaWorldClient::begin_session() {
	opennova::ClientSession::Config cfg;
	cfg.client_index = client_index_;
	cfg.client_key = client_key_;

	// NW-S3: carry the retail CU-chunk set in the 0x42 join. UdpCode1/UdpCode2
	// are the gate-issued session-auth codes live NW's join callbacks validate;
	// the rest is client env. Harmless to the OpenNova server (permissive
	// callbacks). Best-effort values — the locale/MetTag set is provisional
	// pending a live /connectlog capture; the auth-critical UdpCode1/2 come
	// straight from the gate response.
	auto cu_from_info = [&](const char *cu_name, const char *info_key) {
		if (server_info_.has(info_key)) {
			String v = server_info_[info_key];
			std::string val(v.utf8().get_data());
			if (!val.empty()) cfg.cu_vars.push_back({cu_name, val, 1});
		}
	};
	cfg.cu_vars.push_back({"Application", "OpennovaGodotClient.exe", 1});
	cfg.cu_vars.push_back({"BuildDateAndTime", "Jul 21 2009 18:54:41", 1});
	cfg.cu_vars.push_back({"Debug", "0", 1});
	cfg.cu_vars.push_back({"GateTag", cfg.na, 1});
	cu_from_info("MetTag", "met_label");
	cu_from_info("UdpCode1", "udp_code1");
	cu_from_info("UdpCode2", "udp_code2");
	cfg.cu_vars.push_back({"MaxPacketSize", "1300", 1});
	UtilityFunctions::print(String("[NovaWorldClient] 0x42 join carries ")
	    + String::num_int64(static_cast<int64_t>(cfg.cu_vars.size())) + " CU chunks");

	session_ = std::make_unique<opennova::ClientSession>(cfg);

	enter_state(STATE_SESSION_HELLO);
	handshake_elapsed_ = 0.0;
	send_nw_datagram(session_->start());
}

void NovaWorldClient::send_nw_datagram(const std::vector<uint8_t> &dg) {
	if (!nw_socket_.is_valid() || dg.empty()) return;
	nw_socket_->set_dest_address(nw_udp_host_, nw_udp_port_);
	nw_socket_->put_packet(to_pba(dg));
}

void NovaWorldClient::poll_session() {
	if (!nw_socket_.is_valid() || !session_) return;

	while (nw_socket_->get_available_packet_count() > 0) {
		auto packet = nw_socket_->get_packet();
		auto bytes = from_pba(packet);

		// Hand the datagram to the protocol state machine; send whatever it
		// asks us to. All NWU/CRC/TLV/scrk handling lives in client_session.
		std::vector<std::vector<uint8_t>> replies;
		const bool ok = session_->handle_datagram(bytes.data(), bytes.size(), replies);
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
	if (!server_info_.has("startup_url")) {
		return String();
	}
	String base = server_info_["startup_url"];
	if (base.is_empty()) {
		return String();
	}
	if (base.ends_with("/")) {
		base = base.substr(0, base.length() - 1);
	}
	return base + "/jop_2.gsb";
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
	const Error err = browser_http_->request(url);
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
