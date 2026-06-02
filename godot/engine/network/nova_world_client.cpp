#include "nova_world_client.h"

#include <godot_cpp/classes/ip.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <napi/envelope.h>
#include <novacrypto/nwu.h>
#include <novaworld/gate_probe.h>
#include <novaworld/gate_response.h>
#include <novaworld/session_hello.h>
#include <novaworld/session_keys.h>

#include <cstring>
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

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "host"),     "set_host", "get_host");
	ADD_PROPERTY(PropertyInfo(Variant::INT,    "gate_port"), "set_gate_port", "get_gate_port");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "player_name"), "set_player_name", "get_player_name");

	ADD_SIGNAL(MethodInfo("server_info_received", PropertyInfo(Variant::DICTIONARY, "info")));
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
	client_index_ = pick_random_uint32();
	client_key_ = pick_random_uint32();
	server_host_key_ = 0;
	nw_udp_port_ = 0;
	nw_udp_host_ = String();
	handshake_elapsed_ = 0.0;

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
	if (state_ == STATE_CONNECTED || state_ == STATE_SESSION_HELLO || state_ == STATE_SESSION_JOIN) {
		send_session_goodbye();
	}
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
		send_session_heartbeat();
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
	gate_socket_->set_dest_address(host_, gate_port_);
	gate_socket_->put_packet(to_pba(probe));
}

void NovaWorldClient::poll_gate() {
	if (!gate_socket_.is_valid()) return;

	while (gate_socket_->get_available_packet_count() > 0) {
		auto packet = gate_socket_->get_packet();
		auto bytes = from_pba(packet);

		opennova::GateResponse parsed;
		if (!opennova::gate_response_decrypt_and_parse(bytes.data(), bytes.size(), parsed)) {
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
		server_info_ = info;
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

		enter_state(STATE_SESSION_HELLO);
		handshake_elapsed_ = 0.0;
		send_session_hello();
	}
}

namespace {

// Encode an outbound NW-UDP datagram. Same flow as the standalone server's
// nw_udp_listener.cpp encode_outbound() helper but client-side.
std::vector<uint8_t> encode_session_outbound(uint8_t opcode, std::vector<uint8_t> body) {
	if (!body.empty()) {
		// Client-side encrypt is our nwu_decrypt (names swapped vs onnet).
		opennova::nwu_decrypt(body.data(), body.size(), opennova::SESSION_NWU_KEY);
	}
	std::vector<uint8_t> with_opcode;
	with_opcode.reserve(1 + body.size());
	with_opcode.push_back(opcode);
	with_opcode.insert(with_opcode.end(), body.begin(), body.end());
	std::vector<uint8_t> packet(with_opcode.size() + 4);
	size_t out_size = 0;
	if (opennova::napi_envelope_encode(with_opcode.data(), with_opcode.size(),
	                                   packet.data(), packet.size(), &out_size) != 0) {
		return {};
	}
	packet.resize(out_size);
	return packet;
}

bool decode_session_inbound(const uint8_t *raw, size_t raw_len,
                            uint8_t &opcode_out, std::vector<uint8_t> &body_out) {
	std::vector<uint8_t> stripped(raw_len);
	size_t out_size = 0;
	if (opennova::napi_envelope_decode(raw, raw_len, stripped.data(), stripped.size(),
	                                   &out_size) != 0) {
		return false;
	}
	stripped.resize(out_size);
	if (stripped.empty()) return false;
	opcode_out = stripped[0];
	body_out.assign(stripped.begin() + 1, stripped.end());
	if (!body_out.empty()) {
		// Client-side decrypt is our nwu_encrypt (names swapped vs onnet).
		opennova::nwu_encrypt(body_out.data(), body_out.size(), opennova::SESSION_NWU_KEY);
	}
	return true;
}

} // namespace

void NovaWorldClient::send_session_hello() {
	if (!nw_socket_.is_valid()) return;

	opennova::ClientHello hello;
	hello.nvs  = "OpenNova Godot Client 0.1";
	hello.co   = "OpenNova";
	hello.ap   = "OpennovaGodotClient.exe";
	hello.bdat = "Apr 27 2026 00:00:00";
	hello.pn   = "NOVAWORLDUDP";
	hello.pv1  = "0.0.0 2/10/2004 EM";
	hello.pv2  = "1";
	hello.ci   = client_index_;
	hello.eip  = 0;
	hello.epn  = 0;

	auto body = opennova::client_hello_to_bytes(hello);
	auto packet = encode_session_outbound(opennova::SESSION_OPCODE_CLIENT_HELLO,
	                                      std::move(body));
	nw_socket_->set_dest_address(nw_udp_host_, nw_udp_port_);
	nw_socket_->put_packet(to_pba(packet));
}

void NovaWorldClient::send_session_join(uint32_t server_hk) {
	if (!nw_socket_.is_valid()) return;

	opennova::ClientAuth auth;
	auth.ci   = client_index_;
	auth.hk   = server_hk;
	auth.ck   = client_key_;
	auth.na   = "jop:cus2"; // gate tag echo (per memory reference_gate_tags)
	auth.sip  = 0;
	auth.spn  = 0;
	// Client SCRK: 62 ASCII chars deterministic from ck. Real retail uses a
	// random per-session value; this is fine for dev.
	auth.scrk.reserve(62);
	for (int i = 0; i < 62; ++i) {
		auth.scrk.push_back(static_cast<char>('a' + (((auth.ck >> (i % 28)) ^ i) % 26)));
	}

	auto body = opennova::client_auth_to_bytes(auth);
	auto packet = encode_session_outbound(opennova::SESSION_OPCODE_CLIENT_AUTH,
	                                      std::move(body));
	nw_socket_->set_dest_address(nw_udp_host_, nw_udp_port_);
	nw_socket_->put_packet(to_pba(packet));
}

void NovaWorldClient::send_session_heartbeat() {
	if (!nw_socket_.is_valid()) return;
	std::vector<uint8_t> body;
	auto packet = encode_session_outbound(opennova::SESSION_OPCODE_PROTOCOL_MESSAGE,
	                                      std::move(body));
	nw_socket_->set_dest_address(nw_udp_host_, nw_udp_port_);
	nw_socket_->put_packet(to_pba(packet));
}

void NovaWorldClient::send_session_goodbye() {
	if (!nw_socket_.is_valid()) return;
	std::vector<uint8_t> body(4);
	std::memcpy(body.data(), &client_index_, 4);
	auto packet = encode_session_outbound(opennova::SESSION_OPCODE_CLIENT_GOODBYE,
	                                      std::move(body));
	nw_socket_->set_dest_address(nw_udp_host_, nw_udp_port_);
	nw_socket_->put_packet(to_pba(packet));
}

void NovaWorldClient::poll_session() {
	if (!nw_socket_.is_valid()) return;

	while (nw_socket_->get_available_packet_count() > 0) {
		auto packet = nw_socket_->get_packet();
		auto bytes = from_pba(packet);

		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		if (!decode_session_inbound(bytes.data(), bytes.size(), opcode, body)) {
			emit_signal("error_occurred", String("bad NW UDP envelope"));
			continue;
		}

		switch (opcode) {
		case opennova::SESSION_OPCODE_SERVER_HELLO:
			if (state_ == STATE_SESSION_HELLO) {
				enter_state(STATE_SESSION_JOIN);
				handshake_elapsed_ = 0.0;
				send_session_join(/*server_hk=*/0);
			}
			break;
		case opennova::SESSION_OPCODE_SERVER_AUTH:
			if (state_ == STATE_SESSION_JOIN) {
				enter_state(STATE_CONNECTED);
				handshake_elapsed_ = 0.0;
				tick_accum_ = 0.0;
			}
			break;
		case opennova::SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE:
			// In-session traffic. Layer-4 dispatch lands here once we wire
			// it up. For the first pass we just note the keep-alive.
			break;
		default:
			break;
		}
	}
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
