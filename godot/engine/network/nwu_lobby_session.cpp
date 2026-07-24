#include "nwu_lobby_session.h"

#include <napi/envelope.h>
#include <novaworld/gate_probe.h>
#include <novaworld/lobby_vars.h> // parse_host_port + the CU/identity builders

#include <cstring>
#include <random>

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

} // namespace

bool NwuLobbySession::open() {
	client_index_ = pick_random_uint32();
	client_key_ = pick_random_uint32();
	session_.reset();
	probe_sent_ = false;
	nw_udp_host_ = String();
	nw_udp_port_ = 0;
	gate_met_tag_.clear();
	gate_udp_code1_.clear();
	gate_udp_code2_.clear();
	handshake_elapsed_ = 0.0;

	gate_socket_.instantiate();
	nw_socket_.instantiate();
	if (gate_socket_->bind(0, "0.0.0.0") != OK) {
		if (hooks_.on_fatal) hooks_.on_fatal(String("gate UDP bind failed"));
		return false;
	}
	if (nw_socket_->bind(0, "0.0.0.0") != OK) {
		if (hooks_.on_fatal) hooks_.on_fatal(String("nw UDP bind failed"));
		return false;
	}
	return true;
}

void NwuLobbySession::probe(const String &gate_host, int gate_port) {
	if (!gate_socket_.is_valid()) return;
	gate_host_ = gate_host;
	gate_port_ = gate_port;
	probe_sent_ = true;

	auto probe_dg = opennova::gate_probe_build(opennova::GATE_PROBE_TAG_JOINTOPS);
	// Wrap the NWU payload in the LSB-scatter CRC envelope the gate expects.
	// Retail does the same; the server strips it via napi_envelope_decode, and
	// without it the gate logs "bad envelope". (Same envelope the session
	// channel uses in libs/novaworld/client_session.)
	std::vector<uint8_t> packet(probe_dg.size() + 4);
	size_t out_size = 0;
	if (opennova::napi_envelope_encode(probe_dg.data(), probe_dg.size(),
	                                   packet.data(), packet.size(), &out_size) != 0) {
		if (hooks_.on_fatal) hooks_.on_fatal(String("gate probe envelope encode failed"));
		return;
	}
	packet.resize(out_size);
	gate_socket_->set_dest_address(gate_host_, gate_port_);
	gate_socket_->put_packet(to_pba(packet));
}

void NwuLobbySession::close() {
	session_.reset();
	probe_sent_ = false;
	if (gate_socket_.is_valid()) {
		gate_socket_->close();
		gate_socket_.unref();
	}
	if (nw_socket_.is_valid()) {
		nw_socket_->close();
		nw_socket_.unref();
	}
}

void NwuLobbySession::process(double delta) {
	if (gate_socket_.is_valid()) {
		poll_gate();
	}
	if (nw_socket_.is_valid() && session_) {
		poll_session();
	}

	// The handshake timeout runs from the probe until the session verifies
	// (the owners' GATE_PROBING / SESSION_HELLO / SESSION_JOIN window).
	using S = opennova::ClientSession::State;
	const bool handshake_in_flight = probe_sent_ &&
			(!session_ || (session_->state() != S::Verified &&
			               session_->state() != S::Closed &&
			               session_->state() != S::Error));
	if (handshake_in_flight) {
		handshake_elapsed_ += delta;
		if (handshake_elapsed_ >= handshake_timeout_s_) {
			probe_sent_ = false; // report once
			if (hooks_.on_fatal) {
				hooks_.on_fatal(String("handshake timeout in state ") + phase_name());
			}
		}
	}
}

const char *NwuLobbySession::phase_name() const {
	if (!session_) return "gate_probing";
	switch (session_->state()) {
	case opennova::ClientSession::State::Hello: return "session_hello";
	case opennova::ClientSession::State::Auth:
	case opennova::ClientSession::State::Verifying: return "session_join";
	default: return "connected";
	}
}

void NwuLobbySession::poll_gate() {
	while (gate_socket_->get_available_packet_count() > 0) {
		auto bytes = from_pba(gate_socket_->get_packet());

		// The server wraps the response in the LSB-scatter CRC envelope; strip
		// it before decrypt+parse (same envelope the session channel uses).
		std::vector<uint8_t> inner(bytes.size());
		size_t inner_size = 0;
		if (opennova::napi_envelope_decode(bytes.data(), bytes.size(),
		                                   inner.data(), inner.size(), &inner_size) != 0) {
			if (hooks_.on_soft_error) hooks_.on_soft_error(String("bad gate envelope"));
			continue;
		}
		inner.resize(inner_size);

		opennova::GateResponse parsed;
		if (!opennova::gate_response_decrypt_and_parse(inner.data(), inner.size(), parsed)) {
			if (hooks_.on_soft_error) hooks_.on_soft_error(String("bad gate response"));
			continue;
		}

		// Gate-issued session-auth codes the 0x42 join carries as CU chunks
		// (NW-S3): METLABEL -> MetTag, UDPCODE1/2 -> UdpCode1/2.
		gate_met_tag_ = parsed.met_label;
		gate_udp_code1_ = parsed.udp_code1;
		gate_udp_code2_ = parsed.udp_code2;
		if (hooks_.on_gate_response) hooks_.on_gate_response(parsed);

		// Parse "host:port" out of UDPNOVAWORLD.
		std::string udp_host;
		uint16_t udp_port = 0;
		if (!opennova::parse_host_port(parsed.udp_novaworld, udp_host, udp_port)) {
			if (hooks_.on_fatal) hooks_.on_fatal(String("UDPNOVAWORLD malformed"));
			return;
		}
		nw_udp_host_ = String(udp_host.c_str());
		nw_udp_port_ = udp_port;

		begin_session();
	}
}

// Create the session state machine and send its ClientHello. Called once the
// gate response yields the NW UDP host:port.
void NwuLobbySession::begin_session() {
	opennova::ClientSession::Config cfg;
	cfg.client_index = client_index_;
	cfg.client_key = client_key_;

	// 0x42-join CU set (NW-S3/NW-S5) — the set retail builds in
	// ConnectToNovaWorld @ 0x4d4640 for ANY NovaWorld connection, browse or
	// host: all 11 chunks, type=2, with the locale + MetTag chunks empty on the
	// join (filled in the verify instead). UdpCode1/2 come straight from the
	// gate; empty against the permissive OpenNova gate.
	opennova::NovaWorldJoinCu cu;
	cu.gate_tag = cfg.na;
	cu.met_tag = gate_met_tag_;
	cu.udp_code1 = gate_udp_code1_;
	cu.udp_code2 = gate_udp_code2_;
	cfg.cu_vars = opennova::make_novaworld_join_cu(cu);

	// The verify "Cookie" var-list is the one role-specific piece: the lobby
	// client ships the full identity set (NW-S5), the host only the NWUID echo.
	if (hooks_.verify_cookie_vars) {
		cfg.verify_cookie_vars = hooks_.verify_cookie_vars();
	}

	session_ = std::make_unique<opennova::ClientSession>(cfg);
	if (hooks_.on_session_created) {
		hooks_.on_session_created(cfg.cu_vars.size(), cfg.verify_cookie_vars.size());
	}
	handshake_elapsed_ = 0.0;
	if (hooks_.on_session_state) hooks_.on_session_state();
	send(session_->start());
}

void NwuLobbySession::send(const std::vector<uint8_t> &dg) {
	if (!nw_socket_.is_valid() || dg.empty()) return;
	nw_socket_->set_dest_address(nw_udp_host_, nw_udp_port_);
	nw_socket_->put_packet(to_pba(dg));
	if (hooks_.on_sent) hooks_.on_sent(dg);
}

void NwuLobbySession::poll_session() {
	while (nw_socket_->get_available_packet_count() > 0) {
		RxInfo rx;
		rx.bytes = from_pba(nw_socket_->get_packet());
		rx.src_ip = nw_socket_->get_packet_ip();
		rx.src_port = nw_socket_->get_packet_port();
		rx.state_before = static_cast<int>(session_->state());

		// Hand the datagram to the protocol state machine; send whatever it
		// asks us to. All NWU/CRC/TLV/scrk handling lives in client_session.
		std::vector<std::vector<uint8_t>> replies;
		rx.ok = session_->handle_datagram(rx.bytes.data(), rx.bytes.size(), replies);
		rx.state_after = static_cast<int>(session_->state());
		rx.replies = static_cast<int>(replies.size());
		if (rx.state_after != rx.state_before) {
			handshake_elapsed_ = 0.0;
		}
		if (hooks_.on_received) hooks_.on_received(rx);

		for (const auto &dg : replies) {
			send(dg);
		}
		if (!rx.ok) {
			if (hooks_.on_fatal) hooks_.on_fatal(String(session_->last_error().c_str()));
			return;
		}
		if (hooks_.on_session_state) hooks_.on_session_state();
	}

	// Match the retail session boundary: ClientConnected is a one-shot from the
	// periodic update after ServerSessionInit, never a synchronous 0x82 reply.
	// [orig: CNapiGameSession_ProcessPeriodicUpdate @ 0x4d4400]
	std::vector<std::vector<uint8_t>> periodic;
	session_->process_periodic_update(periodic);
	for (const auto &dg : periodic) {
		send(dg);
	}
}

} // namespace godot
