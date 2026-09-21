#include "network/nwu_lobby_session.h"
#include "network/random_id.h"
#include "util/data_format.h"

#include <net/napi/envelope.h>
#include <net/napi/session.h>
#include <net/novaworld/gate_probe.h>
#include <net/novaworld/lobby_vars.h> // parse_host_port + the CU/identity builders

namespace godot {

bool NwuLobbySession::open() {
	client_index_ = pick_random_uint32();
	client_key_ = pick_random_uint32();
	session_.reset();
	probe_sent_ = false;
	gate_probe_datagram_.clear();
	nw_udp_host_ = String();
	nw_udp_port_ = 0;
	gate_response_ = opennova::GateResponse{};
	clock_ms_ = 0;
	clock_accum_s_ = 0.0;
	gate_started_ms_ = 0;
	gate_last_send_ms_ = 0;
	connect_started_ms_ = 0;
	connect_last_send_ms_ = 0;
	connect_reported_ = false;

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

	auto probe_dg = opennova::gate_probe_build(opennova::GATE_PROBE_TAG_JOINTOPS);
	// Wrap the NWU payload in the LSB-scatter CRC envelope the gate expects.
	// Retail does the same at its socket layer (NapiSocket_SendTo with encrypt
	// set routes through NapiSocket_EncodePacket); the server strips it via
	// napi_envelope_decode. (Same envelope the session channel uses in
	// engine/net/novaworld/client_session.)
	std::vector<uint8_t> packet(probe_dg.size() + 4);
	size_t out_size = 0;
	if (opennova::napi_envelope_encode(probe_dg.data(), probe_dg.size(),
	                                   packet.data(), packet.size(), &out_size) != 0) {
		if (hooks_.on_fatal) hooks_.on_fatal(String("gate probe envelope encode failed"));
		return;
	}
	packet.resize(out_size);
	gate_probe_datagram_ = std::move(packet);
	probe_sent_ = true;
	gate_started_ms_ = clock_ms_;
	send_gate_probe();
}

void NwuLobbySession::send_gate_probe() {
	if (!gate_socket_.is_valid() || gate_probe_datagram_.empty()) return;
	gate_socket_->set_dest_address(gate_host_, gate_port_);
	gate_socket_->put_packet(to_packed_bytes(gate_probe_datagram_));
	gate_last_send_ms_ = clock_ms_;
}

void NwuLobbySession::close() {
	session_.reset();
	probe_sent_ = false;
	gate_probe_datagram_.clear();
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
	// The ms wall clock every retail deadline and interval runs on (GetTickCount).
	clock_accum_s_ += delta;
	clock_ms_ = static_cast<uint32_t>(clock_accum_s_ * 1000.0);
	if (session_) session_->set_clock_ms(clock_ms_);

	if (gate_socket_.is_valid()) {
		poll_gate();
	}
	if (nw_socket_.is_valid() && session_) {
		poll_session();
	}

	using S = opennova::ClientSession::State;
	// The gate worker: while no gate response has landed, re-send the probe every
	// SESSION_GATE_PROBE_RETRY_MS and give up at SESSION_GATE_PROBE_TIMEOUT_MS (the
	// engine's gate-error map, no response phase -> NWEC15).
	if (probe_sent_ && !session_ && !gate_probe_datagram_.empty()) {
		if (clock_ms_ - gate_started_ms_ > opennova::SESSION_GATE_PROBE_TIMEOUT_MS) {
			gate_probe_datagram_.clear(); // report once
			if (hooks_.on_fatal) {
				hooks_.on_fatal(String(opennova::novaworld_gate_error_tag(0, false).c_str()));
			}
			return;
		}
		if (clock_ms_ - gate_last_send_ms_ > opennova::SESSION_GATE_PROBE_RETRY_MS) {
			send_gate_probe();
		}
	}

	// The connect legs: the ClientHello / ClientAuth stage datagram is re-sent every
	// SESSION_CONNECT_RETRANSMIT_MS (ClientSession::retransmit_stage_datagram), and
	// the whole connect gives up at SESSION_CONNECT_TIMEOUT_MS.
	const bool connecting = session_ &&
			(session_->state() == S::Hello || session_->state() == S::Auth ||
			 session_->state() == S::Verifying);
	if (connecting && !connect_reported_) {
		if (clock_ms_ - connect_started_ms_ > opennova::SESSION_CONNECT_TIMEOUT_MS) {
			connect_reported_ = true; // report once
			if (hooks_.on_fatal) {
				hooks_.on_fatal(String("handshake timeout in state ") + phase_name());
			}
			return;
		}
		if (clock_ms_ - connect_last_send_ms_ > opennova::SESSION_CONNECT_RETRANSMIT_MS) {
			const std::vector<uint8_t> again = session_->retransmit_stage_datagram();
			connect_last_send_ms_ = clock_ms_;
			if (!again.empty()) send(again);
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

		// The gate answered: the worker stops re-probing.
		gate_probe_datagram_.clear();
		gate_response_ = parsed;
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
	// gate; empty against the permissive OpenNova gate. METLABEL -> MetTag,
	// UDPCODE1/2 -> UdpCode1/2.
	opennova::NovaWorldJoinCu cu;
	cu.gate_tag = cfg.na;
	cu.met_tag = gate_response_.met_label;
	cu.udp_code1 = gate_response_.udp_code1;
	cu.udp_code2 = gate_response_.udp_code2;
	cfg.cu_vars = opennova::make_novaworld_join_cu(cu);

	// The gate's GLSVSS trio arms the session's periodic GLSVSS leg.
	cfg.glsvss_request = gate_response_.glsvss_request;
	cfg.glsvss_rims_ms = gate_response_.glsvss_rims;
	cfg.glsvss_agrms_ms = gate_response_.glsvss_agrms;

	// The "Cookie" var-list (the login cookie jar + locale) every Cookie-bearing
	// statement re-serializes: the verify reply, the host request, the play
	// request and the GLSVSS request.
	if (hooks_.verify_cookie_vars) {
		cfg.verify_cookie_vars = hooks_.verify_cookie_vars();
	}

	session_ = std::make_unique<opennova::ClientSession>(cfg);
	session_->set_clock_ms(clock_ms_);
	if (hooks_.on_session_created) {
		hooks_.on_session_created(cfg.cu_vars.size(), cfg.verify_cookie_vars.size());
	}
	connect_started_ms_ = clock_ms_;
	connect_last_send_ms_ = clock_ms_;
	connect_reported_ = false;
	if (hooks_.on_session_state) hooks_.on_session_state();
	send(session_->start());
}

void NwuLobbySession::send(const std::vector<uint8_t> &dg) {
	if (!nw_socket_.is_valid() || dg.empty()) return;
	nw_socket_->set_dest_address(nw_udp_host_, nw_udp_port_);
	nw_socket_->put_packet(to_packed_bytes(dg));
	if (hooks_.on_sent) hooks_.on_sent(dg);
}

void NwuLobbySession::send_to(const String &host, int port, const std::vector<uint8_t> &dg) {
	if (!gate_socket_.is_valid() || dg.empty() || host.is_empty() || port <= 0) return;
	gate_socket_->set_dest_address(host, port);
	gate_socket_->put_packet(to_packed_bytes(dg));
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
	// periodic update after ServerSessionInit, never a synchronous 0x82 reply;
	// the same tick polls the GLSVSS deadline.
	// [orig: CNapiGameSession_ProcessPeriodicUpdate @ 0x4d4400, see docs/net/novaworld-net-re.md]
	std::vector<std::vector<uint8_t>> periodic;
	session_->process_periodic_update(periodic);
	// The connection's send-interval pump: the negotiated idle keepalive and the
	// receive-silence reap (both witnessed in ClientSession::pump_send_intervals).
	session_->pump_send_intervals(periodic);
	for (const auto &dg : periodic) {
		send(dg);
	}
	if (session_->state() == opennova::ClientSession::State::Closed &&
	    session_->disconnected_by_peer()) {
		if (hooks_.on_session_state) hooks_.on_session_state();
	}
}

} // namespace godot
