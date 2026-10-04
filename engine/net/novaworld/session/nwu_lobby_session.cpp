#include <net/novaworld/nwu_lobby_session.h>

#include <net/napi/envelope.h>
#include <net/napi/session.h>
#include <net/novaworld/gate_probe.h>
#include <net/novaworld/lobby_vars.h> // parse_host_port
#include <net/npwire/nw_session_framing.h> // make_random_session_u32

#include <cstdlib>

namespace opennova {

namespace {

// "a.b.c.d" into a PeerAddr's IP (octet 0 in the low byte); false for anything else.
bool parse_dotted_quad(const std::string &host, PeerAddr &out) {
	uint32_t ip = 0;
	int octets = 0;
	std::size_t at = 0;
	while (octets < 4) {
		std::size_t end = at;
		while (end < host.size() && host[end] >= '0' && host[end] <= '9') ++end;
		if (end == at || end - at > 3) return false;
		const unsigned long value = std::strtoul(host.substr(at, end - at).c_str(), nullptr, 10);
		if (value > 255) return false;
		ip |= static_cast<uint32_t>(value) << (8 * octets);
		++octets;
		if (octets == 4) {
			if (end != host.size()) return false;
			break;
		}
		if (end >= host.size() || host[end] != '.') return false;
		at = end + 1;
	}
	out.ip = ip;
	return true;
}

} // namespace

NwuLobbySession::NwuLobbySession(Hooks hooks, Environment env)
	: hooks_(std::move(hooks)), env_(std::move(env)) {}

uint32_t NwuLobbySession::random_u32() const {
	return env_.random_u32 ? env_.random_u32() : make_random_session_u32();
}

bool NwuLobbySession::resolve(const std::string &host, uint16_t port, PeerAddr &out) const {
	PeerAddr addr;
	const bool ok = env_.resolve_ipv4 ? env_.resolve_ipv4(host, addr) : parse_dotted_quad(host, addr);
	if (!ok) return false;
	out = addr;
	out.port = port;
	return true;
}

void NwuLobbySession::open(IDatagramSocket &gate, IDatagramSocket &session) {
	gate_socket_ = &gate;
	session_socket_ = &session;
	client_index_ = random_u32();
	client_key_ = random_u32();
	session_.reset();
	gate_resolved_ = false;
	probe_sent_ = false;
	gate_answered_ = false;
	gate_probe_datagram_.clear();
	nw_udp_host_.clear();
	nw_udp_port_ = 0;
	nw_peer_ = PeerAddr{};
	gate_response_ = GateResponse{};
	clock_ms_ = 0;
	gate_started_ms_ = 0;
	gate_last_send_ms_ = 0;
	connect_started_ms_ = 0;
	connect_last_send_ms_ = 0;
	connect_reported_ = false;
}

void NwuLobbySession::probe(const std::string &gate_host, uint16_t gate_port) {
	if (gate_socket_ == nullptr) return;
	const std::vector<uint8_t> probe_dg = gate_probe_build(GATE_PROBE_TAG_JOINTOPS);
	// The NWU payload rides the LSB-scatter CRC envelope the gate expects: retail's socket
	// layer applies it when NapiSocket_SendTo's encrypt flag is set; the service strips it.
	// [orig: CNapiGateManager_ProbeThreadProc @0x633ade -> NapiSocket_SendTo(..., 1)]
	std::vector<uint8_t> packet(probe_dg.size() + 4);
	std::size_t out_size = 0;
	if (napi_envelope_encode(probe_dg.data(), probe_dg.size(), packet.data(), packet.size(),
	                         &out_size) != 0) {
		if (hooks_.on_fatal) hooks_.on_fatal("gate probe envelope encode failed");
		return;
	}
	packet.resize(out_size);
	gate_probe_datagram_ = std::move(packet);
	// The worker resolves the gate host once (inet_addr, then gethostbyname); an unresolved
	// host sends nothing and the deadline reports it. [orig: ProbeThreadProc @0x633a12..0x633a2c]
	gate_resolved_ = resolve(gate_host, gate_port, gate_peer_);
	probe_sent_ = true;
	gate_started_ms_ = clock_ms_;
	send_gate_probe();
}

void NwuLobbySession::send_gate_probe() {
	gate_last_send_ms_ = clock_ms_;
	if (gate_socket_ == nullptr || !gate_resolved_ || gate_probe_datagram_.empty()) return;
	gate_socket_->send_to(gate_peer_, gate_probe_datagram_.data(), gate_probe_datagram_.size());
}

void NwuLobbySession::close() {
	session_.reset();
	probe_sent_ = false;
	gate_probe_datagram_.clear();
	gate_socket_ = nullptr;
	session_socket_ = nullptr;
}

NwuLobbySession::MatchFacts NwuLobbySession::match_facts() const {
	MatchFacts out;
	out.in_use = !gate_response_.udp_novaworld.empty();
	if (session_) {
		out.flags = session_->session_flags();
		out.role = session_->session_role();
		out.exit_reason = session_->mission_exit_reason();
	}
	return out;
}

void NwuLobbySession::tick(uint32_t now_ms) {
	clock_ms_ = now_ms;
	if (session_) session_->set_clock_ms(clock_ms_);

	if (gate_socket_ != nullptr && !gate_answered_) poll_gate();
	if (session_socket_ != nullptr && session_) poll_session();

	using S = ClientSession::State;
	// The gate worker: while no gate response has landed, re-send the probe every
	// SESSION_GATE_PROBE_RETRY_MS and give up past SESSION_GATE_PROBE_TIMEOUT_MS (the gate-error
	// map with no response phase -> NWEC15). [orig: CNapiGateManager_ProbeThreadProc
	//  @0x633a50..0x633a70]
	if (probe_sent_ && !session_ && !gate_probe_datagram_.empty()) {
		if (clock_ms_ - gate_started_ms_ > SESSION_GATE_PROBE_TIMEOUT_MS) {
			gate_probe_datagram_.clear(); // report once
			if (hooks_.on_fatal) hooks_.on_fatal(novaworld_gate_error_tag(0, false));
			return;
		}
		if (clock_ms_ - gate_last_send_ms_ > SESSION_GATE_PROBE_RETRY_MS) send_gate_probe();
	}

	// The first connect's legs: the ClientHello / ClientAuth stage datagram is re-sent every
	// SESSION_CONNECT_RETRANSMIT_MS, and the whole connect gives up past
	// SESSION_CONNECT_TIMEOUT_MS. Once verified they stand down for good: a reconnect's re-probe
	// and re-join are the session's own (ClientSession::reconnecting).
	// [orig: CNapiNPConnection_PumpEnumeratorAndSend @0x6290c0; PumpStateMachine case 3
	//  @0x629508; the ConnectOrHost poll @0x4d4f10 (0xEA60)]
	if (session_ && session_->state() == S::Verified) connect_reported_ = true;
	const bool connecting = session_ && !session_->reconnecting() &&
			(session_->state() == S::Hello || session_->state() == S::Auth ||
			 session_->state() == S::Verifying);
	if (connecting && !connect_reported_) {
		if (clock_ms_ - connect_started_ms_ > SESSION_CONNECT_TIMEOUT_MS) {
			connect_reported_ = true; // report once
			if (hooks_.on_fatal) hooks_.on_fatal(std::string("handshake timeout in state ") + phase_name());
			return;
		}
		if (clock_ms_ - connect_last_send_ms_ > SESSION_CONNECT_RETRANSMIT_MS) {
			const std::vector<uint8_t> again = session_->retransmit_stage_datagram();
			connect_last_send_ms_ = clock_ms_;
			if (!again.empty()) send(again);
		}
	}
}

const char *NwuLobbySession::phase_name() const {
	if (!session_) return "gate_probing";
	switch (session_->state()) {
	case ClientSession::State::Hello: return "session_hello";
	case ClientSession::State::Auth:
	case ClientSession::State::Verifying: return "session_join";
	default: return "connected";
	}
}

// The gate worker reads until the first reply its response buffer accepts, then closes the gate
// socket and ends: a late or duplicate reply is never read.
// [orig: CNapiGateManager_ProbeThreadProc @0x633b13..0x633bea (the receive, GATEAPI decrypt,
//  SetResponseBuffer), @0x633b65..0x633c3b (state 2: CloseSocket @0x633c36, state 3)]
void NwuLobbySession::poll_gate() {
	uint8_t buf[65536];
	for (;;) {
		PeerAddr from;
		const int n = gate_socket_->recv_from(buf, sizeof(buf), from);
		if (n <= 0) return;

		// The service wraps the response in the CRC envelope; strip it before decrypt+parse.
		std::vector<uint8_t> inner(static_cast<std::size_t>(n));
		std::size_t inner_size = 0;
		if (napi_envelope_decode(buf, static_cast<std::size_t>(n), inner.data(), inner.size(),
		                         &inner_size) != 0) {
			if (hooks_.on_soft_error) hooks_.on_soft_error("bad gate envelope");
			continue;
		}
		inner.resize(inner_size);
		GateResponse parsed;
		if (!gate_response_decrypt_and_parse(inner.data(), inner.size(), parsed)) {
			if (hooks_.on_soft_error) hooks_.on_soft_error("bad gate response");
			continue;
		}

		gate_answered_ = true;
		gate_probe_datagram_.clear();
		gate_response_ = parsed;
		if (hooks_.on_gate_response) hooks_.on_gate_response(parsed);

		std::string udp_host;
		uint16_t udp_port = 0;
		if (!parse_host_port(parsed.udp_novaworld, udp_host, udp_port) ||
		    !resolve(udp_host, udp_port, nw_peer_)) {
			if (hooks_.on_fatal) hooks_.on_fatal("UDPNOVAWORLD malformed");
			return;
		}
		nw_udp_host_ = udp_host;
		nw_udp_port_ = udp_port;
		begin_session();
		return;
	}
}

// The session state machine and its ClientHello, once the gate response named the NW UDP
// endpoint. The 0x42-join CU set is the one retail builds for any NovaWorld connection, browse
// or host: all 11 chunks, type 2, the locale chunks empty on the join; UdpCode1/2 and MetTag come
// from the gate. [orig: CNapiGameSession_ConnectToNovaWorld @0x4d4640]
void NwuLobbySession::begin_session() {
	ClientSession::Config cfg;
	cfg.client_index = client_index_;
	cfg.client_key = client_key_;
	cfg.max_packet_size = env_.max_packet_size;
	NovaWorldJoinCu cu;
	cu.gate_tag = cfg.na;
	cu.met_tag = gate_response_.met_label;
	cu.udp_code1 = gate_response_.udp_code1;
	cu.udp_code2 = gate_response_.udp_code2;
	if (env_.max_packet_size != 0) cu.max_packet_size = env_.max_packet_size;
	cfg.cu_vars = make_novaworld_join_cu(cu);
	// The gate's GLSVSS trio arms the session's periodic GLSVSS leg.
	cfg.glsvss_request = gate_response_.glsvss_request;
	cfg.glsvss_rims_ms = gate_response_.glsvss_rims;
	cfg.glsvss_agrms_ms = gate_response_.glsvss_agrms;
	// The "Cookie" var-list every Cookie-bearing statement re-serializes.
	cfg.cookie_vars = hooks_.cookie_vars;
	// A reconnect's re-join mints a fresh client key (the client index stays).
	cfg.next_client_key = [this]() { return random_u32(); };

	session_ = std::make_unique<ClientSession>(cfg);
	session_->set_clock_ms(clock_ms_);
	if (hooks_.on_session_created)
		hooks_.on_session_created(cfg.cu_vars.size(), cfg.cookie_vars ? cfg.cookie_vars().size() : 0);
	connect_started_ms_ = clock_ms_;
	connect_last_send_ms_ = clock_ms_;
	connect_reported_ = false;
	if (hooks_.on_session_state) hooks_.on_session_state();
	send(session_->start());
}

void NwuLobbySession::send(const std::vector<uint8_t> &dg) {
	if (session_socket_ == nullptr || dg.empty()) return;
	session_socket_->send_to(nw_peer_, dg.data(), dg.size());
	if (hooks_.on_sent) hooks_.on_sent(dg);
}

void NwuLobbySession::send_to(const PeerAddr &to, const std::vector<uint8_t> &dg) {
	if (session_socket_ == nullptr || dg.empty() || to.port == 0) return;
	session_socket_->send_to(to, dg.data(), dg.size());
}

void NwuLobbySession::flush() {
	if (session_socket_ == nullptr || !session_) return;
	session_->set_clock_ms(clock_ms_);
	std::vector<std::vector<uint8_t>> pumped;
	session_->pump(pumped);
	for (const auto &dg : pumped) send(dg);
}

// The receive batch, then the rest of retail's periodic update in its order: the receive pump's
// missing-sequence tail, the connection's send pump (the queued statements and a pending ACK, the
// reap, the send-interval legs), then the statements the update queues for the next pump (the
// one-shot ClientConnected after ServerSessionInit, the GLSVSS poll).
// [orig: CNapiGameSession_ProcessPeriodicUpdate @0x4d4400 — NapiNPProtocol_Pump(proto, -1, 250)
//  @0x4d442e, then @0x4d444e..0x4d4532]
void NwuLobbySession::poll_session() {
	uint8_t buf[65536];
	for (;;) {
		PeerAddr from;
		const int n = session_socket_->recv_from(buf, sizeof(buf), from);
		if (n <= 0) break;
		RxInfo rx;
		rx.bytes.assign(buf, buf + n);
		rx.from = from;
		rx.state_before = static_cast<int>(session_->state());
		std::vector<std::vector<uint8_t>> replies;
		rx.ok = session_->handle_datagram(rx.bytes.data(), rx.bytes.size(), replies);
		rx.state_after = static_cast<int>(session_->state());
		rx.replies = replies.size();
		if (hooks_.on_received) hooks_.on_received(rx);
		for (const auto &dg : replies) send(dg);
		if (!rx.ok) {
			if (hooks_.on_fatal) hooks_.on_fatal(session_->last_error());
			return;
		}
		if (hooks_.on_session_state) hooks_.on_session_state();
		if (!session_) return; // a hook closed the driver
	}
	if (!session_) return;
	std::vector<std::vector<uint8_t>> pumped;
	session_->finish_receive_batch(pumped);
	session_->pump(pumped);
	session_->process_periodic_update();
	for (const auto &dg : pumped) send(dg);
	if (session_->state() == ClientSession::State::Closed && session_->disconnected_by_peer()) {
		if (hooks_.on_session_state) hooks_.on_session_state();
	}
}

} // namespace opennova
