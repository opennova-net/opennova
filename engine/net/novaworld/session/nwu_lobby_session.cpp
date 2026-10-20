#include <net/novaworld/nwu_lobby_session.h>

#include <net/napi/envelope.h>
#include <net/napi/session.h>
#include <net/novaworld/gate_probe.h>
#include <net/novaworld/lobby_identity.h> // read_exe_basename
#include <net/novaworld/lobby_vars.h> // parse_host_and_port
#include <net/npwire/nw_session_framing.h> // advance_connection_index / make_random_session_u32
#include <net/npwire/session_keys.h>      // the server-direction opcodes
#include <net/title/title_identity.h>

#include <base/io/strutil.h>

#include <array>
#include <cstdlib>

namespace opennova {

namespace {

// The port a probe target takes when its address string gives none (or a zero one).
// [orig: CNapiGameSession_SendToAddress @0x4D0970 - mov eax, 0FACEh @0x4D09C3]
constexpr uint32_t kNwuTargetDefaultPort = 0xFACEu; // 64206

// Byte 0 of an enveloped NWU datagram (the opcode, in the clear); false when the envelope
// rejects it. [orig: NapiNPManager_PumpReceive @0x623010 decodes into recv_buffer before
//  HandlePacket]
bool stripped_opcode(const uint8_t *data, std::size_t len, uint8_t &opcode) {
	if (data == nullptr || len == 0) return false;
	std::vector<uint8_t> stripped(len);
	std::size_t out_size = 0;
	if (napi_envelope_decode(data, len, stripped.data(), stripped.size(), &out_size) != 0 ||
	    out_size == 0)
		return false;
	opcode = stripped[0];
	return true;
}

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

// "255.255.255.255" reads as itself (inet_addr answers it INADDR_NONE), anything else through the
// environment's inet_addr and name lookup, and 0 when neither reads it.
// [orig: Napi_ResolveAddress @0x62DF90 - Napi_StrCaseEqual(host, "255.255.255.255")
//  @0x62DF9B..0x62DFA7, inet_addr @0x62DFAD, gethostbyname with DNS on @0x62DFCA, else 0
//  @0x62DFD8]
uint32_t NwuLobbySession::resolve_address(const std::string &host) const {
	if (strutil::iequals(host, "255.255.255.255")) return 0xFFFFFFFFu;
	PeerAddr addr;
	return resolve(host, 0, addr) ? addr.ip : 0u;
}

// One address string into a probe target: split and resolved with DNS on, refused when the
// address is 0, a zero port taken as 64206, and kept once per address and port in the order made.
// [orig: CNapiGameSession_SendToAddress @0x4D0970 - Network_ParseHostAndPort @0x62E200 (the call
//  @0x4D09A0, every caller pushing DNS on: @0x4D4A62, @0x4D4A88, @0x4D4AB0), the zero-address
//  fail @0x4D09B0..0x4D09B2, port 0 @0x4D09C1..0x4D09C8; NapiNPEnumerator_CreateTarget @0x624C30
//  (the call @0x4D09CF): an existing pair returned @0x624C49..0x624C53, else appended to the
//  list's tail @0x624CFF..0x624D30]
bool NwuLobbySession::add_target(const std::string &address) {
	std::string host;
	uint32_t port = 0;
	if (!parse_host_and_port(address, host, port)) return false;
	const uint32_t ip = resolve_address(host);
	if (ip == 0) return false;
	if (port == 0) port = kNwuTargetDefaultPort;
	for (const Target &target : targets_) {
		if (target.ip == ip && target.port == port) return true;
	}
	targets_.push_back(Target{ip, port});
	return true;
}

void NwuLobbySession::open(IDatagramSocket &gate, IDatagramSocket &session) {
	// The NovaWorld menu's entry empties JUNCTIONCODE when the last gate response went through
	// with USEJUNCTION and CLEARJUNCTION both set.
	// [orig: UI_ProcessLANSessionStateMachine @0x558DE0 - dword_B5FF48 @0x558E2D, dword_B5FD38
	//  @0x558E3B, dword_B5FD3C @0x558E43, the form write @0x558E64]
	if (gate_ok_ && use_junction_ != 0 && clear_junction_ != 0)
		junction_code_.clear();
	gate_socket_ = &gate;
	session_socket_ = &session;
	client_index_ = 0;
	session_.reset();
	gate_resolved_ = false;
	probe_sent_ = false;
	gate_answered_ = false;
	gate_probe_datagram_.clear();
	targets_.clear();
	nw_peer_ = PeerAddr{};
	gate_response_ = GateResponse{};
	in_use_ = false;
	clock_ms_ = 0;
	gate_started_ms_ = 0;
	gate_last_send_ms_ = 0;
	connect_poll_ = ConnectPoll::Off;
	connect_stamp_ms_ = 0;
	connect_timed_out_ = false;
	give_up_ = 0;
	connect_last_send_ms_ = 0;
}

void NwuLobbySession::probe(const std::string &gate_host, uint16_t gate_port) {
	if (gate_socket_ == nullptr) return;
	if (title_ == nullptr) {
		if (hooks_.on_fatal) hooks_.on_fatal("no title identity to probe the gate with");
		return;
	}
	// The gate manager's tag: the title's, or the boot's gt.ssc (set_title).
	// [orig: CNapiGateManager_ProbeThreadProc @0x6339e0 - the tag's bytes @0x633aa1]
	const std::vector<uint8_t> probe_dg = gate_probe_build(gate_tag_);
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

// The command line's POST pair over the gate's, each only when set (an address that resolves,
// a nonzero port), then its GLSVSS pair, each when its switch was given, as the gate response
// stores them.
// [orig: CNapiGateManager_ProcessResponse @0x4CED20 - the POST pair @0x4CF54B..0x4CF562, the
//  GLSVSS pair under its flags @0x4CF567..0x4CF589; Game_ParseCommandLineAndInit @0x4A7310 -
//  Napi_ResolveAddress(token, 1) @0x4A75C7]
void NwuLobbySession::apply_command_line_overrides() {
	if (!command_line_.post_ip_address.empty()) {
		const uint32_t ip = resolve_address(command_line_.post_ip_address);
		if (ip != 0) {
			for (std::size_t i = 0; i < 4; ++i)
				gate_response_.post_ip[i] = static_cast<uint8_t>(ip >> (8 * i));
		}
	}
	if (command_line_.post_port_number != 0)
		gate_response_.post_port = static_cast<uint32_t>(command_line_.post_port_number);
	if (command_line_.glsvss_rims)
		gate_response_.glsvss_rims = *command_line_.glsvss_rims;
	if (command_line_.glsvss_agrms)
		gate_response_.glsvss_agrms = *command_line_.glsvss_agrms;
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

bool NwuLobbySession::claims(const PeerAddr &from, const uint8_t *data, std::size_t len) const {
	// The manager hands the handlers the envelope-stripped packet; byte 0 is the opcode.
	uint8_t opcode = 0;
	if (!session_ || !stripped_opcode(data, len, opcode)) return false;
	if (opcode < SESSION_OPCODE_SERVER_HELLO || opcode > kNwuServerOpcodeLast) return false;
	// A 0x81 finds the connection by its CI from any sender (one probe target among several,
	// say), and one the connection does not take goes on to the next protocol.
	// [orig: Nwu_HandleServerHello @0x626D20 - the misses return 0 @0x626DCB]
	if (opcode == SESSION_OPCODE_SERVER_HELLO) return session_->takes_server_hello(data, len);
	return from == nw_peer_;
}

NwuLobbySession::MatchFacts NwuLobbySession::match_facts() const {
	MatchFacts out;
	out.in_use = in_use_;
	if (session_) {
		out.flags = session_->session_flags();
		out.role = session_->session_role();
		out.exit_reason = session_->mission_exit_reason();
	}
	return out;
}

namespace {

// The connect waits' failure codes (the menu's NWEC map, novaworld_connect_error_tag).
// [orig: CGameSession_WaitForNWUValidation @0x4D0F20 - the poll's timeout -15 @0x4D0FD6, the
//  give-up word 1 -> -17 @0x4D111C, 2 -> -18 @0x4D10FF, else -19 @0x4D10E2, a session not in
//  states 2..4 -20 @0x4D1196; CGameSession_WaitForNWUConnection @0x4D1230 - the poll's timeout
//  -21 @0x4D1296, a session whose flags lack bit 2 (states 2..8) -23 @0x4D1381]
constexpr int kConnectPollTimedOut = -15;
constexpr int kConnectGaveUpInStateOne = -17;
constexpr int kConnectGaveUpConnectionDown = -18;
constexpr int kConnectGaveUpOther = -19;
constexpr int kConnectNotVerifying = -20;
constexpr int kConnectVerifyTimedOut = -21;
constexpr int kConnectNotConnected = -23;

// The give-up word's values (session+0x12C) the periodic update sets: 1 the state-1 give-up, 2
// a connection down. [orig: CNapiGameSession_ProcessPeriodicUpdate @0x4D4400 - 1 @0x4D44B1, 2
//  @0x4D446D]
constexpr int kGiveUpStateOne = 1;
constexpr int kGiveUpConnectionDown = 2;

} // namespace

void NwuLobbySession::tick(uint32_t now_ms) {
	clock_ms_ = now_ms;
	if (session_) session_->set_clock_ms(clock_ms_);

	if (gate_socket_ != nullptr && !gate_answered_) poll_gate();
	if (session_socket_ != nullptr && session_) poll_session();
	if (session_socket_ != nullptr && session_)
		give_up_stalled_connect();

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

	if (session_socket_ == nullptr || !session_)
		return;
	if (poll_connect())
		return; // a give-up was reported
	// The first connect's stage datagram (the 0x41 while the probe runs, the 0x42 while it
	// awaits its 0x82) is re-sent every SESSION_CONNECT_RETRANSMIT_MS while its poll runs. Once
	// the poll ends it stands down for good: a reconnect's re-probe and re-join are the
	// session's own (ClientSession::reconnecting).
	// [orig: CNapiNPConnection_PumpEnumeratorAndSend @0x6290c0; PumpStateMachine case 3
	//  @0x629508]
	const bool polling =
			connect_poll_ == ConnectPoll::Connect || connect_poll_ == ConnectPoll::Verify;
	if (polling && !session_->reconnecting() &&
			clock_ms_ - connect_last_send_ms_ > SESSION_CONNECT_RETRANSMIT_MS) {
		const std::vector<uint8_t> again = session_->retransmit_stage_datagram();
		connect_last_send_ms_ = clock_ms_;
		if (!again.empty())
			send(again);
	}
}

// The periodic update's give-ups of a first connect (the hosting/playing word 0): its connection
// down (a rejecting 0x82) aborts it with the word set to 2; still in state 1
// SESSION_PERIODIC_UPDATE_TIMEOUT_MS after the state's entry aborts it with the word set to 1.
// [orig: CNapiGameSession_ProcessPeriodicUpdate @0x4D4400 - the connection's state 4
//  @0x4D4436..0x4D4441 and the word @0x4D4462, +0x12C = 2 @0x4D446D, CNapiNPConnection_SetState(2)
//  @0x4D4477, CNapiGameSession_ProcessNetwork @0x4D447E; the word test @0x4D4483, the state-1
//  test @0x4D4493, the 0x4E20 compare @0x4D44A3, CNapiGameSession_ProcessNetwork @0x4D44AC,
//  +0x12C = 1 @0x4D44B1]
void NwuLobbySession::give_up_stalled_connect() {
	if (session_->session_role() != ClientSession::kSessionRoleNone)
		return;
	if (session_->connection_down()) {
		abort_connect();
		give_up_ = kGiveUpConnectionDown;
	}
	if (session_->lobby_state() != 1)
		return;
	if (clock_ms_ - session_->lobby_state_entered_ms() <= SESSION_PERIODIC_UPDATE_TIMEOUT_MS)
		return;
	abort_connect();
	give_up_ = kGiveUpStateOne;
}

void NwuLobbySession::abort_connect() {
	std::vector<std::vector<uint8_t>> out;
	session_->abort_connect(out);
	for (const auto &dg : out)
		send(dg);
}

// The reset the connect waits run on a failure: a connection still up tears down (its burst out),
// the poll's flag and the give-up word clear, the menu shows the code's NWEC, and the connection,
// its enumerator and its protocol are destroyed with the session marked inactive, so a late
// datagram (a 0x82 answering a 0x42 the give-up left outstanding) finds no connection to advance.
// The session goes after the hook returns (one that closed the driver already dropped it).
// [orig: CNapiGameSession_ResetToDisconnected @0x4D0890 - CNapiNPConnection_RequestDisconnect
//  @0x4D08A1, the connection's destroy 0x62A4B0 @0x4D08A7, the protocol's 0x62B500 @0x4D08B8,
//  +0x12C..+0x138 = 0 @0x4D08E4..0x4D08F6, the session inactive @0x4D092E;
//  UI_ProcessLANSessionStateMachine @0x558DE0 - the error leg @0x55915F..0x559160 ->
//  UI_ShowNetworkErrorByCode @0x5584A0]
void NwuLobbySession::fail_connect(int code) {
	connect_poll_ = ConnectPoll::Done;
	connect_timed_out_ = false;
	give_up_ = 0;
	std::vector<std::vector<uint8_t>> out;
	session_->request_disconnect(out);
	for (const auto &dg : out)
		send(dg);
	if (hooks_.on_fatal)
		hooks_.on_fatal(novaworld_connect_error_tag(code));
	session_.reset();
	targets_.clear();
	nw_peer_ = PeerAddr{};
}

// The first connect's poll, one wait per tick as the menu runs one state per frame: the connect
// poll while the session is in state 1 (giving up SESSION_CONNECT_POLL_TIMEOUT_MS after the
// connect's stamp), the validation wait (its failures, else the re-stamp), the verify poll while
// the session is in state 2 or 3 (giving up SESSION_CONNECT_VERIFY_TIMEOUT_MS after the
// re-stamp), then the connection wait (verified, or its failure). True when it reported a
// failure.
// [orig: UI_ProcessLANSessionStateMachine @0x558DE0 - state 5 @0x55919E, 6 @0x5591D0, 7
//  @0x5591FE, 8 @0x559230; CNapiGameSession_PollConnectPhase1 @0x4D4C30 - the state-1 test
//  @0x4D4C50, the compare @0x4D4C6A, the give-up @0x4D4CB2, the flag @0x4D4CB7;
//  CGameSession_WaitForNWUValidation @0x4D0F20 - the flag @0x4D0F86, the word @0x4D107E, the
//  states @0x4D1137..0x4D114A, the re-stamp @0x4D120C;
//  CNapiGameSession_PollConnectPhase2 @0x4D4D70 - the states @0x4D4D90..0x4D4D9E, the compare
//  @0x4D4DB1, the flag @0x4D4DBF, the give-up @0x4D4E03; CGameSession_WaitForNWUConnection
//  @0x4D1230 - the flag @0x4D1244, bit 2 @0x4D1332]
bool NwuLobbySession::poll_connect() {
	switch (connect_poll_) {
		case ConnectPoll::Connect: {
			if (session_->lobby_state() == 1) {
				if (clock_ms_ - connect_stamp_ms_ <= SESSION_CONNECT_POLL_TIMEOUT_MS)
					return false;
				abort_connect();
				connect_timed_out_ = true;
			}
			if (connect_timed_out_) {
				fail_connect(kConnectPollTimedOut);
				return true;
			}
			if (give_up_ != 0) {
				int code = kConnectGaveUpOther;
				if (give_up_ == kGiveUpStateOne)
					code = kConnectGaveUpInStateOne;
				else if (give_up_ == kGiveUpConnectionDown)
					code = kConnectGaveUpConnectionDown;
				fail_connect(code);
				return true;
			}
			const int state = session_->lobby_state();
			if (state != 2 && state != 3 && state != 4) {
				fail_connect(kConnectNotVerifying);
				return true;
			}
			connect_stamp_ms_ = clock_ms_;
			connect_poll_ = ConnectPoll::Verify;
			return false;
		}
		case ConnectPoll::Verify: {
			const int state = session_->lobby_state();
			if (state == 2 || state == 3) {
				if (clock_ms_ - connect_stamp_ms_ <= SESSION_CONNECT_VERIFY_TIMEOUT_MS)
					return false;
				connect_timed_out_ = true;
				abort_connect();
			}
			if (connect_timed_out_) {
				fail_connect(kConnectVerifyTimedOut);
				return true;
			}
			if ((session_->session_flags() & 0x2u) == 0) {
				fail_connect(kConnectNotConnected);
				return true;
			}
			connect_poll_ = ConnectPoll::Done;
			return false;
		}
		default:
			return false;
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
		const bool counted = gate_response_decrypt_and_parse(inner.data(), inner.size(), parsed);

		gate_answered_ = true;
		gate_probe_datagram_.clear();
		gate_response_ = parsed;
		// The reply the worker took is the gate's answer: one whose parse counted no field (no
		// VAR line of a key the parse knows, or no line at all) fails the gate as state -9, the
		// menu's NWEC25, as a reply with no POST pair does below.
		// [orig: CNapiGateManager_ProcessResponse @0x4CED20 - the first line's read failing
		//  @0x4CF0C0, the count test @0x4CF4ED..0x4CF4EF, the state's switch @0x4CF51E, -9
		//  stored @0x4CF6AC]
		if (!counted) {
			if (hooks_.on_fatal)
				hooks_.on_fatal(novaworld_gate_error_tag(-9, true));
			return;
		}
		apply_command_line_overrides();
		if (hooks_.on_gate_response)
			hooks_.on_gate_response(gate_response_);

		// The NWU session is in use when the gate or the command line named an address (the
		// command line's count @0x4CF52E). Without one the response
		// must hold the POST address and port ("NO NW POST IP" / "NO NW POST PORT"): the gate
		// state -9, which the menu shows as NWEC25; with both the connect goes on with no
		// session (ConnectToNovaWorld returns at once, each connect poll too, and the menu opens
		// its web lobby).
		// [orig: CNapiGateManager_ProcessResponse @0x4CED20 - the in-use word dword_B5FD2C
		//  @0x4CF525..0x4CF541, its test @0x4CF599..0x4CF5A0, the POST address @0x4CF5A6, the
		//  port @0x4CF627, the -9 store @0x4CF6AC; UI_ProcessLANSessionStateMachine @0x558DE0 -
		//  state 3's -9 -> NWEC25 @0x5590FD..0x559102, state 9 @0x559262;
		//  CNapiGameSession_ConnectToNovaWorld @0x4D4640 - @0x4D46EA..0x4D46F0]
		in_use_ = !parsed.udp_novaworld.empty() || !command_line_.udp_novaworld.empty();
		if (!in_use_) {
			const std::array<uint8_t, 4> none{ 0, 0, 0, 0 };
			if (gate_response_.post_ip == none || gate_response_.post_port == 0) {
				if (hooks_.on_fatal)
					hooks_.on_fatal(novaworld_gate_error_tag(-9, true));
				return;
			}
		}
		// The response's success tail: its USEJUNCTION / CLEARJUNCTION take the command line's
		// overrides, a set USEJUNCTION empties the menu's JUNCTIONCODE (and sets the web lobby
		// form's USEJUNCTION, which nothing here reads), and the gate's info stands.
		// [orig: CNapiGateManager_ProcessResponse @0x4CED20 - the overrides @0x4CF6ED..0x4CF709,
		//  the form writes @0x4CF71B..0x4CF76E, dword_B5FF48 = 1 @0x4CF81D]
		use_junction_ = command_line_.use_junction != -1
				? command_line_.use_junction
				: static_cast<int32_t>(parsed.use_junction);
		clear_junction_ = command_line_.clear_junction != -1
				? command_line_.clear_junction
				: static_cast<int32_t>(parsed.clear_junction);
		if (use_junction_ != 0)
			junction_code_.clear();
		gate_ok_ = true;
		if (!in_use_) {
			if (hooks_.on_sessionless)
				hooks_.on_sessionless();
			return;
		}
		// The old targets go, then the connect's one source is made probe targets: the menu's
		// JUNCTIONCODE when it is nonempty, else every /UDPNOVAWORLD command-line entry, else
		// every gate entry. An address that names none makes no target and is passed over, so
		// with none the connect probes nothing until it gives up.
		// [orig: CNapiGameSession_ConnectToNovaWorld @0x4D4640 - the old targets
		//  freed @0x4D4A22 (NapiNPEnumerator_DestroyAllTimers @0x6220C0 over the list
		//  CreateTarget fills); JUNCTIONCODE @0x4D4A4A, its one target @0x4D4A71; the command
		//  line's entries @0x4D4A78..0x4D4A9E; the gate variables @0x4D4AA2..0x4D4ACE
		//  (CMissionInfo_GetVariable @0x4D4AB8, CNapiGameSession_SendToAddress @0x4D4AC0, its
		//  result unread), then CNapiGameSession_InitPlayerConnection @0x4D4B16]
		targets_.clear();
		if (!junction_code_.empty()) {
			add_target(junction_code_);
		} else if (!command_line_.udp_novaworld.empty()) {
			for (const std::string &address : command_line_.udp_novaworld)
				add_target(address);
		} else {
			for (const std::string &address : parsed.udp_novaworld)
				add_target(address);
		}
		begin_session();
		return;
	}
}

// The session state machine and its ClientHello, once the gate response named the NW UDP
// endpoint. The 0x42-join CU set is the one retail builds for any NovaWorld connection, browse
// or host: all 11 chunks, type 2, the locale chunks empty on the join; UdpCode1/2 and MetTag come
// from the gate. [orig: CNapiGameSession_ConnectToNovaWorld @0x4d4640]
void NwuLobbySession::begin_session() {
	// The connect makes a fresh lobby protocol, its connection counter 0, and one connection on
	// it, whose index is the CI every 0x41 and the 0x42 carry: 1 at every connect. Its CK and
	// SCRK are drawn at each 0x81 it takes, from the process's one manager LCG
	// (ClientSession::Config::connection_keys; Environment::connection_keys in a test).
	// [orig: CNapiGameSession_InitNPConnection @0x4D3BE0 - NapiNPProtocol_Create @0x4D3C89 (the
	//  counter +0xECC zeroed @0x625A81), CNapiNPConnection_Create @0x4D3F70 (the step and the
	//  index @0x62B282..0x62B296)]
	uint32_t protocol_connections = 0;
	client_index_ = advance_connection_index(protocol_connections);
	ClientSession::Config cfg = ClientSession::Config::lobby(*title_, gate_tag_);
	cfg.client_index = client_index_;
	cfg.connection_keys = env_.connection_keys;
	cfg.max_packet_size = env_.max_packet_size;
	NovaWorldJoinCu cu;
	cu.application = read_exe_basename().substr(0, kLobbyApplicationChars);
	cu.build_date_and_time = title_->lobby_bdat;
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

	session_ = std::make_unique<ClientSession>(cfg);
	session_->set_clock_ms(clock_ms_);
	if (hooks_.on_session_created)
		hooks_.on_session_created(cfg.cu_vars.size(), cfg.cookie_vars ? cfg.cookie_vars().size() : 0);
	// The connect's stamp, its poll from here. [orig: CNapiGameSession_ConnectToNovaWorld
	//  @0x4D4640 - dword_B5F444 @0x4D4BD5, the flags cleared @0x4D4BDA..0x4D4BE0]
	connect_poll_ = ConnectPoll::Connect;
	connect_stamp_ms_ = clock_ms_;
	connect_timed_out_ = false;
	give_up_ = 0;
	connect_last_send_ms_ = clock_ms_;
	if (hooks_.on_session_state) hooks_.on_session_state();
	send(session_->start());
}

// The announce (the 0x41) is the enumerator's and goes to every target in list order, one
// datagram each, at the target's port narrowed to 16 bits, each its own announce carrying that
// target's address and port as EIP / EPN; everything else is the connection's.
// [orig: CNapiNPConnection_PumpEnumeratorAndSend @0x6290C0 - the target walk @0x629183..0x6291B4
//  (the NWU enumerator's +0x26 set @0x4D4261), NapiNPSession_SendAnnouncePacket @0x61FA00 per
//  target (the call @0x62919D, its address, port and +0x34 pushed @0x629190..0x62919C) ->
//  CNapiNPManager_SendTo @0x61FDE4; NapiSocket_SendTo's htons @0x62D040]
void NwuLobbySession::send(const std::vector<uint8_t> &dg) {
	if (session_socket_ == nullptr || dg.empty()) return;
	uint8_t opcode = 0;
	if (stripped_opcode(dg.data(), dg.size(), opcode) && opcode == SESSION_OPCODE_CLIENT_HELLO) {
		for (const Target &target : targets_) {
			const PeerAddr to{ target.ip, static_cast<uint16_t>(target.port) };
			send_one(to, session_ ? session_->build_announce(target.ip, target.port) : dg);
		}
		return;
	}
	send_one(nw_peer_, dg);
}

// A connection with no server yet (no 0x81 taken) has address 0, which never reaches the socket:
// retail queues such a send to the manager's loopback FIFO instead, which offers it to the local
// protocols and tosses it when none takes it (a _connectlog line); ours drops it here.
// [orig: CNapiNPManager_SendTo @0x61EC20 - the address test @0x61EC59, the FIFO write (+0xBC)
//  @0x61EC9F..0x61ECC6]
void NwuLobbySession::send_one(const PeerAddr &to, const std::vector<uint8_t> &dg) {
	if (to.ip == 0) return;
	session_socket_->send_to(to, dg.data(), dg.size());
	if (hooks_.on_sent) hooks_.on_sent(to, dg);
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
		const uint32_t hellos_taken = session_->server_hellos_taken();
		rx.ok = session_->handle_datagram(rx.bytes.data(), rx.bytes.size(), replies);
		// A 0x81 the connection took points it at the reply's sender, where its 0x42 goes.
		// [orig: CNapiNPConnection_InitFromSession @0x626320 - the session record's address
		//  and port into the connection @0x6263B4 / @0x6263B7]
		if (session_->server_hellos_taken() != hellos_taken) nw_peer_ = from;
		rx.state_after = static_cast<int>(session_->state());
		rx.replies = replies.size();
		if (hooks_.on_received) hooks_.on_received(rx);
		for (const auto &dg : replies) send(dg);
		if (!rx.ok) {
			// While the first connect's poll runs, the poll reports the failure with the menu's
			// NWEC (a rejected verify leaves the session down: the connection wait's NWEC48),
			// as retail's menu shows the connect's errors only through its waits.
			// [orig: UI_ProcessLANSessionStateMachine @0x558DE0 - the error leg
			//  @0x55915F..0x559160; CGameSession_WaitForNWUConnection @0x4D1230 - bit 2
			//  @0x4D1332, -23 @0x4D1381]
			const bool polling =
					connect_poll_ == ConnectPoll::Connect || connect_poll_ == ConnectPoll::Verify;
			if (!polling && hooks_.on_fatal)
				hooks_.on_fatal(session_->last_error());
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
