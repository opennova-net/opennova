#include <net/novaworld/nwu_lobby_session.h>

#include <net/napi/envelope.h>
#include <net/napi/session.h>
#include <net/novaworld/gate_metrics.h>
#include <net/novaworld/gate_probe.h>
#include <net/novaworld/lobby_identity.h> // read_exe_basename
#include <net/novaworld/lobby_vars.h> // parse_host_and_port
#include <net/npwire/nw_session_framing.h> // advance_connection_index / make_random_session_u32
#include <net/npwire/session_keys.h>      // the server-direction opcodes
#include <net/title/title_identity.h>

#include <base/io/le.h>
#include <base/io/strutil.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

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

// The gate context's state the response's processing leaves: 2 its success, -9 its failure (no
// field counted, no POST pair), which the menu's state 3 shows as NWEC25.
// [orig: CNapiGateManager_ProcessResponse @0x4CED20 - 2 @0x4CF8AE, -9 @0x4CF6AC;
//  UI_ProcessLANSessionStateMachine @0x558DE0 - NWEC25 @0x5590FD]
constexpr int kGateStateSucceeded = 2;
constexpr int kGateStateFailed = -9;

} // namespace

NwuLobbySession::NwuLobbySession(Hooks hooks, Environment env)
	: hooks_(std::move(hooks)), env_(std::move(env)) {}

uint32_t NwuLobbySession::random_u32() const {
	return env_.random_u32 ? env_.random_u32() : make_random_session_u32();
}

bool NwuLobbySession::connect_log_on() const {
	return hooks_.connect_log_on && hooks_.connect_log && hooks_.connect_log_on();
}

void NwuLobbySession::connect_log(const std::string &text) {
	if (connect_log_on()) hooks_.connect_log(text);
}

// Napi_FormatAddress over the gate socket's bind address, 0 (the gate manager's), and its port.
// [orig: Napi_FormatAddress @0x62DED0 - "%s:%ld" @0x62DF22; CNapiGateManager_WaitForConnect
//  @0x4CE6B0 - the bind address 0 @0x4CE7D2; CNapiGateManager_CreateAndBindSocket @0x633770 -
//  @0x6337FC; NapiUdpSocket_CreateAndBind @0x62D2A0 - the socket's address and port
//  @0x62D4C1 / @0x62D4C4]
std::string NwuLobbySession::gate_socket_text() const {
	const uint16_t port = env_.gate_local_port ? env_.gate_local_port() : uint16_t{0};
	return "0.0.0.0:" + std::to_string(port);
}

// OnNovaWorldConnected's line: the connection's server (its +0x30 / +0x34), the 0x82's
// NovaworldName and the address it saw (RIP / RPN, the connection's +0x38 / +0x3C), each
// address through Napi_FormatAddress ("%s:%ld"). [orig: CNapiGameSession_OnNovaWorldConnected
//  @0x4D1570 - Napi_FormatAddress @0x4D181A / @0x4D1831, "%s : NWU CONNECTED TO NOVAWORLD %s [%s]
//  [yourip=%s]" @0x4D1871]
void NwuLobbySession::log_connection_up() {
	if (!connect_log_on() || !session_) return;
	const PeerAddr seen{ session_->server_reflected_ip(), 0 };
	connect_log("NWU CONNECTED TO NOVAWORLD " + peer_addr_to_string(nw_peer_) + " [" +
			session_->server_novaworld_name() + "]  [yourip=" + peer_addr_ip_to_string(seen) + ":" +
			std::to_string(static_cast<int32_t>(session_->server_reflected_port())) + "]");
}

// OnDisconnect's line, ahead of its reset: the server, the 0x82's NovaworldName and the
// connection's disconnect record (its role, class, the two parameters, the text, the reason code
// and the tag; zeroed at the state-5 entry, so a teardown with none latched prints zeros). The
// callback runs after the teardown's 0x46 burst, each of whose sends writes its SENDING line
// first, so the line waits for the burst's last send (send_datagram).
// [orig: CNapiGameSession_OnDisconnect @0x4CFAA0 - Napi_FormatAddress @0x4CFAF0, the record
//  conn+0x658..0x664 / +0x668 / +0x6E8 / +0x6EC, "%s : NWU DISCONNECTING FROM NOVAWORLD %s [%s]
//  [%ld,%ld,%ld,%ld,\"%s\",%ld,\"%s\"]" @0x4CFB5C; CNapiNPConnection_TeardownActiveConnection
//  @0x6253C0 - the state-5 burst's CNapiNPConnection_SendDisconnectPacket calls
//  @0x6254B5..0x6254D3 (each writing its SENDING line, the +0x270 load @0x61F514), then the
//  conn+0xC4 callback (the call @0x6254E7)]
void NwuLobbySession::log_connection_down() {
	if (!connect_log_on() || !session_) return;
	write_pending_connection_down(); // an earlier teardown's whose burst never went out here
	const DisconnectEvent &record = session_->disconnect_event();
	const auto value = [](uint32_t v) { return std::to_string(static_cast<int32_t>(v)); };
	pending_down_line_ = "NWU DISCONNECTING FROM NOVAWORLD " + peer_addr_to_string(nw_peer_) +
			" [" + session_->server_novaworld_name() + "] [" + value(record.ds) + "," +
			value(record.dc) + "," + value(record.dp1) + "," + value(record.dp2) + ",\"" +
			record.dstr + "\"," + value(record.dpc) + ",\"" + record.ddstr + "\"]";
	pending_down_sends_ = session_->disconnect_burst_count();
}

void NwuLobbySession::write_pending_connection_down() {
	if (pending_down_line_.empty()) return;
	std::string line;
	line.swap(pending_down_line_);
	pending_down_sends_ = 0;
	connect_log(line);
}

// The record's lines over its sender and SN. [orig: Network_LogSessionDiscovered @0x4CF960 -
//  Napi_FormatAddress of the record's +0x18 / +0x1C @0x4CF9A5, "%s : NWU SESSION DISCOVERED AT %s
//  [%s]" over +0x1FC (the append @0x4CF9DD); Network_LogSessionRemoved @0x4CFA00 - "%s : NWU
//  SESSION AT %s [%s] HAS BEEN REMOVED." (the append @0x4CFA7D); both installed on the enumerator
//  by CNapiGameSession_InitPlayerConnection @0x4D4160 (+0x1C @0x4D420D, +0x20 @0x4D4221), the
//  discovery called by Nwu_HandleServerHello (@0x6278FE / @0x627BE2), the removal by
//  NapiNPTimer_Destroy @0x621E50 (the call @0x621E6F)]
void NwuLobbySession::note_server_hello(const PeerAddr &from) {
	const std::string &name = session_->server_hello_name();
	for (auto it = records_.begin(); it != records_.end(); ++it) {
		if (!(it->from == from)) continue;
		// The record kept for this sender: the same SN refreshes it, another one replaces it.
		// Retail's identity is the 0x81's HK, then its SN; ours compares the SN alone, which
		// differs only for a second 0x81 in one probe window, which never reaches the record
		// logic (the taken 0x81 stops the enumerator).
		// [orig: Nwu_HandleServerHello - NapiNPEnumerator_FindSessionByAddress (the call
		//  @0x627667), the HK and SN compares @0x627917 / @0x627928, NapiNPTimer_Destroy (the call
		//  @0x627941)]
		if (it->name == name) return;
		if (connect_log_on())
			connect_log("NWU SESSION AT " + peer_addr_to_string(it->from) + " [" + it->name +
					"] HAS BEEN REMOVED.");
		records_.erase(it);
		break;
	}
	records_.push_back(SessionRecord{ from, name });
	if (connect_log_on())
		connect_log("NWU SESSION DISCOVERED AT " + peer_addr_to_string(from) + " [" + name + "]");
}

void NwuLobbySession::note_session_drains() {
	if (!session_ || session_->enumerator_drains() == drains_seen_) return;
	drains_seen_ = session_->enumerator_drains();
	drain_records();
}

void NwuLobbySession::drain_records() {
	for (const SessionRecord &record : records_) {
		if (connect_log_on())
			connect_log("NWU SESSION AT " + peer_addr_to_string(record.from) + " [" + record.name +
					"] HAS BEEN REMOVED.");
	}
	records_.clear();
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
	GateMemory &kept = memory();
	if (kept.response_kept && kept.use_junction != 0 && kept.clear_junction != 0)
		kept.junction_code.clear();
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
	clock_ms_ = 0;
	gate_started_ms_ = 0;
	gate_last_send_ms_ = 0;
	dedicated_wait_over_ = false;
	gate_menu_ = GateMenu::Off;
	gate_result_ = 0;
	first_announce_.clear();
	connect_poll_ = ConnectPoll::Off;
	connect_stamp_ms_ = 0;
	connect_timed_out_ = false;
	connect_escaped_ = false;
	escape_requested_ = false;
	give_up_ = 0;
	connect_last_send_ms_ = 0;
}

// [orig: CNapiGateManager_WaitForConnect @0x4CE6B0 - "GATE CONFIGURING GATE MANAGER." the
//  append @0x4CE84D, "GATE STARTING GATE SERVICE." @0x4CE88C, the stamp @0x4CE896]
void NwuLobbySession::log_gate_service_start() {
	if (connect_log_on()) {
		connect_log("GATE CONFIGURING GATE MANAGER.");
		if (connect_log_on()) {
			connect_log("GATE STARTING GATE SERVICE.");
			gate_log_stamp_ms_ = clock_ms_;
		}
	}
}

std::string NwuLobbySession::gate_start_failed() {
	// The context's state at the failure: the start's 2, its SetState(-6) refused; then the wait's
	// default leg's -1. [orig: CNapiGateManager_ProcessConnectStateMachine @0x633D10 - @0x633E3C,
	//  @0x633E64; CNapiGateManager_SetState @0x633880 - @0x633886..0x63388B;
	//  CNapiGateManager_WaitForConnect @0x4CE6B0 - @0x4CEA18, @0x4CEA41]
	constexpr int kGateContextStateAtFailure = 2;
	constexpr int kGateWaitResult = -1;
	log_gate_service_start();
	if (connect_log_on())
		connect_log("GATE GATE SERVICE START FAILED [" +
				std::to_string(kGateContextStateAtFailure) + "].");
	return novaworld_gate_error_tag(kGateWaitResult, /*response_phase=*/false);
}

void NwuLobbySession::probe(const std::string &gate_host, uint16_t gate_port) {
	if (gate_socket_ == nullptr) return;
	if (title_ == nullptr) {
		if (hooks_.on_fatal) hooks_.on_fatal("no title identity to probe the gate with");
		return;
	}
	// While the last response that went through is kept, the wait starts no probe and the menu
	// goes on to the connect over it (the frames of states 2..4 after this one), its processing
	// not run again; a probe starts over with the response emptied.
	// [orig: CNapiGateManager_WaitForConnect @0x4CE6B0 - dword_B5FF48 @0x4CE6CF, the gate state 2
	//  @0x4CE6F3, 0 returned @0x4CE6FA; else CMissionInfo_Init(byte_B5F450) @0x4CE719;
	//  UI_ProcessLANSessionStateMachine @0x558DE0 - 2 set @0x55904B, state 2 @0x55906B..0x559094,
	//  the connect @0x559156]
	GateMemory &kept = memory();
	if (kept.response_kept && kept.gate_host == gate_host && kept.gate_port == gate_port) {
		gate_answered_ = true;
		if (hooks_.on_gate_response)
			hooks_.on_gate_response(kept.response);
		gate_processed(kGateStateSucceeded);
		return;
	}
	kept.response_kept = false;
	kept.response = GateResponse{};
	kept.gate_address = 0; // [orig: CMissionInfo_Init @0x4CE5D0 - +0x8C @0x4CE5F7]
	kept.gate_host = gate_host;
	kept.gate_port = gate_port;
	log_gate_service_start();
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
	// The service started: its tag and its socket, then the stamp.
	// [orig: CNapiGateManager_WaitForConnect @0x4CE6B0 - the socket @0x4CEA78..0x4CEA91, the
	//  line "%s : GATE GATE SERVICE STARTED \"%s\"... [%s]" (the tag this+76) @0x4CEAE3, the
	//  stamp @0x4CEAED]
	if (connect_log_on()) {
		connect_log("GATE GATE SERVICE STARTED \"" + gate_tag_ + "\"... [" + gate_socket_text() +
				"]");
		gate_log_stamp_ms_ = clock_ms_;
	}
}

// The command line's POST pair over the gate's, each only when set (an address that resolves,
// a nonzero port), then its GLSVSS pair, each when its switch was given, as the gate response
// stores them.
// [orig: CNapiGateManager_ProcessResponse @0x4CED20 - the POST pair @0x4CF54B..0x4CF562, the
//  GLSVSS pair under its flags @0x4CF567..0x4CF589; Game_ParseCommandLineAndInit @0x4A7310 -
//  Napi_ResolveAddress(token, 1) @0x4A75C7]
void NwuLobbySession::apply_command_line_overrides() {
	GateResponse &response = memory().response;
	if (!command_line_.post_ip_address.empty()) {
		const uint32_t ip = resolve_address(command_line_.post_ip_address);
		if (ip != 0) {
			for (std::size_t i = 0; i < 4; ++i)
				response.post_ip[i] = static_cast<uint8_t>(ip >> (8 * i));
		}
	}
	if (command_line_.post_port_number != 0)
		response.post_port = static_cast<uint32_t>(command_line_.post_port_number);
	if (command_line_.glsvss_rims)
		response.glsvss_rims = *command_line_.glsvss_rims;
	if (command_line_.glsvss_agrms)
		response.glsvss_agrms = *command_line_.glsvss_agrms;
}

void NwuLobbySession::send_gate_probe() {
	gate_last_send_ms_ = clock_ms_;
	if (gate_socket_ == nullptr || !gate_resolved_ || gate_probe_datagram_.empty()) return;
	gate_socket_->send_to(gate_peer_, gate_probe_datagram_.data(), gate_probe_datagram_.size());
}

// The connection's destroy drains the enumerator's records. [orig: CNapiNPConnection_Destroy
//  @0x62A4B0 - NapiNPEnumerator_DrainTimerList (the call @0x62A554)]
void NwuLobbySession::close() {
	write_pending_connection_down();
	drain_records();
	session_.reset();
	probe_sent_ = false;
	gate_probe_datagram_.clear();
	gate_menu_ = GateMenu::Off;
	first_announce_.clear();
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
	out.in_use = memory().in_use;
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
constexpr int kConnectEscaped = -16;        // NWEC41 [orig: @0x4D1041]
constexpr int kConnectVerifyEscaped = -22;  // NWEC47 [orig: @0x4D1324]
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

	if (gate_socket_ != nullptr && !gate_answered_)
		poll_gate();
	else if (gate_menu_ != GateMenu::Off && step_gate_menu())
		return;
	// The menu's connect sends nothing: its first announce goes at the next frame's pump, the
	// enumerator's send stamp 0 until then, the re-announce interval counted from that send.
	// [orig: CNapiGameSession_InitPlayerConnection @0x4D4160 - NapiTimer_Start @0x4D43C2;
	//  UI_ProcessLANSessionStateMachine @0x558DE0 - the pump @0x558DFA;
	//  CNapiNPConnection_PumpEnumeratorAndSend @0x6290C0 - the stamp test @0x629108..0x629119,
	//  the stamp @0x62911F, the target walk @0x629183..0x6291B4]
	if (session_socket_ != nullptr && session_ && !first_announce_.empty()) {
		const std::vector<uint8_t> first = std::move(first_announce_);
		first_announce_.clear();
		connect_last_send_ms_ = clock_ms_;
		send(first);
	}
	if (session_socket_ != nullptr && session_) poll_session();
	// The frame's head pump, after the receive and ahead of the periodic update's give-up tests
	// and the menu state's work: the first connect's 0x41 re-announce every
	// SESSION_CONNECT_RETRANSMIT_MS while its poll runs (here) and the 0x42 leg of the state
	// machine (the session's pump, inside poll_session), so a send that falls due in a give-up's
	// or an ESC's frame still goes. Once the poll ends the re-announce stands down for good: a
	// reconnect's re-probe is the session's own (ClientSession::reconnecting).
	// [orig: UI_ProcessLANSessionStateMachine @0x558DE0 - the pump @0x558DFA ahead of the
	//  state's case; CNapiGameSession_ProcessPeriodicUpdate @0x4D4400 - NapiNPManager_Pump
	//  @0x4D441F, NapiNPProtocol_Pump @0x4D442E, the give-up tests @0x4D4436..0x4D44B1;
	//  NapiNPProtocol_Pump @0x62A650 - CNapiNPConnection_PumpFlags (the call @0x62A6F8);
	//  CNapiNPConnection_PumpFlags @0x629780 - PumpEnumeratorAndSend @0x6297B7,
	//  PumpStateMachine @0x6297C3; CNapiNPConnection_PumpEnumeratorAndSend @0x6290c0]
	if (session_socket_ != nullptr && session_ && connect_polling() && !session_->reconnecting() &&
			session_->state() == ClientSession::State::Hello &&
			clock_ms_ - connect_last_send_ms_ > SESSION_CONNECT_RETRANSMIT_MS) {
		const std::vector<uint8_t> again = session_->reannounce_datagram();
		connect_last_send_ms_ = clock_ms_;
		if (!again.empty())
			send(again);
	}
	if (session_socket_ != nullptr && session_)
		give_up_stalled_connect();

	// The gate worker: while no gate response has landed, re-send the probe once more than
	// SESSION_GATE_PROBE_RETRY_MS (2000) passed since the last and give up once more than
	// SESSION_GATE_PROBE_TIMEOUT_MS (10000) passed since the first, the cadence every fresh probe
	// sets (session.h). Retail's worker then exits with no report and its menu waits on (the
	// stuck state D-NET-NEW-L9-3 keeps out); ours reports the gate-error map with no response
	// phase, NWEC15. [orig: CNapiGateManager_ProbeThreadProc @0x6339E0 - the loop's clock read
	//  @0x633A50, the deadline test @0x633A58, the interval test @0x633A6A;
	//  CNapiGateManager_WaitForConnect @0x4CE6B0 - the interval @0x4CE7B4, the deadline @0x4CE7C6]
	if (probe_sent_ && !session_ && !gate_probe_datagram_.empty()) {
		if (clock_ms_ - gate_started_ms_ > SESSION_GATE_PROBE_TIMEOUT_MS) {
			gate_probe_datagram_.clear(); // the worker is gone: report once, read no more
			if (!env_.command_line_host) {
				if (hooks_.on_fatal) hooks_.on_fatal(novaworld_gate_error_tag(0, false));
				return;
			}
		} else if (clock_ms_ - gate_last_send_ms_ > SESSION_GATE_PROBE_RETRY_MS) {
			send_gate_probe();
		}
	}
	// The dedicated host's wait outlives the worker: past SESSION_DEDICATED_GATE_WAIT_MS with no
	// response the gate service stops, its line naming the worker's socket, still open after a
	// worker that left at its deadline, and the hosting ends. [orig:
	//  CNapiGateManager_WaitForGateResponse @0x4D1510 - the compare @0x4D1541 (jbe @0x4D1546), the
	//  call @0x4D154A; CNapiGateManager_StopGateService @0x4CEC10 - the socket +0x30 @0x4CEC3A,
	//  Napi_FormatAddress @0x4CEC53 ("n/a" @0x4CEC63), "%s : GATE STOPPING GATE SERVICE... [%s]"
	//  @0x4CECA4, the stamp @0x4CECB2, the context freed @0x4CECC7, the gate state 1 -> 0
	//  @0x4CECEC]
	if (env_.command_line_host && probe_sent_ && !session_ && !gate_answered_ &&
			!dedicated_wait_over_ && clock_ms_ - gate_started_ms_ > SESSION_DEDICATED_GATE_WAIT_MS) {
		dedicated_wait_over_ = true;
		if (connect_log_on()) {
			connect_log("GATE STOPPING GATE SERVICE... [" + gate_socket_text() + "]");
			gate_log_stamp_ms_ = clock_ms_;
		}
		if (hooks_.on_fatal) hooks_.on_fatal(novaworld_gate_error_tag(0, false));
		return;
	}

	if (session_socket_ == nullptr || !session_)
		return;
	poll_connect();
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
	note_session_drains();
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
	connect_escaped_ = false;
	escape_requested_ = false;
	give_up_ = 0;
	std::vector<std::vector<uint8_t>> out;
	session_->request_disconnect(out);
	for (const auto &dg : out)
		send(dg);
	drain_records(); // the destroy's [orig: CNapiNPConnection_Destroy - the call @0x62A554]
	if (hooks_.on_fatal)
		hooks_.on_fatal(novaworld_connect_error_tag(code));
	session_.reset();
	targets_.clear();
	nw_peer_ = PeerAddr{};
}

// The first connect's poll, one menu state a tick as the NovaWorld menu runs one state a frame
// (each frame pumping the session first): the connect poll while the session is in state 1
// (giving up SESSION_CONNECT_POLL_TIMEOUT_MS after the connect's stamp); the tick after it ends,
// the validation wait (its failures, else the re-stamp); the verify poll while the session is in
// state 2 or 3 (giving up SESSION_CONNECT_VERIFY_TIMEOUT_MS after the re-stamp); the tick after
// it ends, the connection wait (verified, or its failure); the tick after that, the web lobby's
// open (the validated hook). The command-line host's blocking sequence runs each wait at once
// after its poll's loop and opens no web lobby. True when it reported a failure.
// [orig: UI_ProcessLANSessionStateMachine @0x558DE0 - the pump @0x558DFA; state 5 (@0x55919E):
//  PollConnectPhase1 (the call @0x5591A3), state 6 set @0x5591B0; state 6 (@0x5591D0): the
//  validation wait (@0x5591D5), 7 set @0x5591DE; state 7 (@0x5591FE): PollConnectPhase2
//  (@0x559203), 8 set @0x559210; state 8 (@0x559230): the connection wait (@0x559235), 9 set
//  @0x559242; state 9 @0x559262..0x5592F2;
//  CNapiGameSession_ConnectAndWaitForValidation @0x4D4EC0 - the loops @0x4D4ED0..0x4D4ED9 /
//  @0x4D4EE6..0x4D4EEF, the waits @0x4D4EDD / @0x4D4EF3;
//  CNapiGameSession_PollConnectPhase1 @0x4D4C30 - the state-1 test @0x4D4C50, the compare
//  @0x4D4C6A, the give-up @0x4D4CB2, the flag @0x4D4CB7;
//  CGameSession_WaitForNWUValidation @0x4D0F20 - the flag @0x4D0F86, the word @0x4D107E, the
//  states @0x4D1137..0x4D114A, the re-stamp @0x4D120C;
//  CNapiGameSession_PollConnectPhase2 @0x4D4D70 - the states @0x4D4D90..0x4D4D9E, the compare
//  @0x4D4DB1, the flag @0x4D4DBF, the give-up @0x4D4E03; CGameSession_WaitForNWUConnection
//  @0x4D1230 - the flag @0x4D1244, bit 2 @0x4D1332]
bool NwuLobbySession::escape() {
	if (!connect_polling()) return false;
	escape_requested_ = true;
	return true;
}

bool NwuLobbySession::poll_connect() {
	switch (connect_poll_) {
		case ConnectPoll::Connect: {
			if (session_->lobby_state() == 1) {
				if (clock_ms_ - connect_stamp_ms_ > SESSION_CONNECT_POLL_TIMEOUT_MS) {
					// [orig: CNapiGameSession_PollConnectPhase1 @0x4D4C30 - "%s : NWU 30 secs are
					//  up! (a)" @0x4D4CA8, ahead of the give-up @0x4D4CB2]
					connect_log("NWU 30 secs are up! (a)");
					abort_connect();
					connect_timed_out_ = true;
				} else if (escape_requested_) {
					// The frame's key read past the timeout test: ESC gives the connect up.
					// [orig: CNapiGameSession_PollConnectPhase1 @0x4D4C30 - Input_DequeueKeyEvent
					//  @0x4D4CDE, 27 @0x4D4CE9, "%s : NWU USER ESCAPED1" @0x4D4D22, the give-up
					//  @0x4D4D2C, the flag dword_B5F44C @0x4D4D31]
					escape_requested_ = false;
					connect_log("NWU USER ESCAPED1");
					abort_connect();
					connect_escaped_ = true;
				} else {
					return false;
				}
			}
			// The poll returned 0: the validation wait is the menu's next frame.
			connect_poll_ = ConnectPoll::Validation;
			if (!env_.command_line_host)
				return false;
			[[fallthrough]];
		}
		case ConnectPoll::Validation: {
			// The validation wait's lines, each ahead of its reset.
			// [orig: CGameSession_WaitForNWUValidation @0x4D0F20 - "%s : NWU LEAVING JOIN
			//  LOOP..." @0x4D0F79; "%s : NWU TIMEOUT!!!" @0x4D0FC0; "%s : NWU FAILURE [%ld]!!!"
			//  (the give-up word) @0x4D10BF; "%s : NWU FAILURE2 [%ld]!!!" (the session state
			//  +0x11C) @0x4D1185; "%s : NWU WAITING FOR VALIDATION..." @0x4D11E3]
			connect_log("NWU LEAVING JOIN LOOP...");
			if (connect_timed_out_) {
				connect_log("NWU TIMEOUT!!!");
				fail_connect(kConnectPollTimedOut);
				return true;
			}
			// [orig: CGameSession_WaitForNWUValidation @0x4D0F20 - the flag dword_B5F44C, "%s : NWU
			//  USER HIT ESCAPE!!!" @0x4D102B, the reset @0x4D103B, -16 @0x4D1041]
			if (connect_escaped_) {
				connect_log("NWU USER HIT ESCAPE!!!");
				fail_connect(kConnectEscaped);
				return true;
			}
			if (give_up_ != 0) {
				connect_log("NWU FAILURE [" + std::to_string(give_up_) + "]!!!");
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
				connect_log("NWU FAILURE2 [" + std::to_string(state) + "]!!!");
				fail_connect(kConnectNotVerifying);
				return true;
			}
			connect_log("NWU WAITING FOR VALIDATION...");
			// The pass to the verify poll clears both flags and empties the key queue before the
			// re-stamp: an ESC the connect poll did not read is dropped.
			// [orig: CGameSession_WaitForNWUValidation @0x4D0F20 - the flags @0x4D11EB / @0x4D11F1,
			//  Input_ResetKeyQueue (the call @0x4D11F7)]
			escape_requested_ = false;
			connect_stamp_ms_ = clock_ms_;
			connect_poll_ = ConnectPoll::Verify;
			return false;
		}
		case ConnectPoll::Verify: {
			const int state = session_->lobby_state();
			if (state == 2 || state == 3) {
				if (clock_ms_ - connect_stamp_ms_ > SESSION_CONNECT_VERIFY_TIMEOUT_MS) {
					connect_timed_out_ = true;
					// [orig: CNapiGameSession_PollConnectPhase2 @0x4D4D70 - the flag @0x4D4DBF, "%s :
					//  NWU 20 secs are up! (b)" @0x4D4DF9, the give-up @0x4D4E03]
					connect_log("NWU 20 secs are up! (b)");
					abort_connect();
				} else if (escape_requested_) {
					// [orig: CNapiGameSession_PollConnectPhase2 @0x4D4D70 - Input_DequeueKeyEvent
					//  @0x4D4E25, 27 @0x4D4E30, the flag @0x4D4E39, "%s : NWU USER ESCAPED2"
					//  @0x4D4E73, the give-up @0x4D4E7D]
					escape_requested_ = false;
					connect_escaped_ = true;
					connect_log("NWU USER ESCAPED2");
					abort_connect();
				} else {
					return false;
				}
			}
			// The poll returned 0: the connection wait is the menu's next frame.
			connect_poll_ = ConnectPoll::Connection;
			if (!env_.command_line_host)
				return false;
			[[fallthrough]];
		}
		case ConnectPoll::Connection: {
			// [orig: CGameSession_WaitForNWUConnection @0x4D1230 - "%s : NWU TIMEOUT2!!!"
			//  @0x4D1287; "%s : NWU NOT CONNECTED FOR SOME REASON..." @0x4D1372; "%s : NWU
			//  YIPPEE WE ARE CONNECTED AND VALIDATED." @0x4D13D3]
			if (connect_timed_out_) {
				connect_log("NWU TIMEOUT2!!!");
				fail_connect(kConnectVerifyTimedOut);
				return true;
			}
			// [orig: CGameSession_WaitForNWUConnection @0x4D1230 - the flag dword_B5F44C, "%s : NWU
			//  USER HIT ESCAPE2!!!" @0x4D12F1, the reset @0x4D1305, -22 @0x4D1324]
			if (connect_escaped_) {
				connect_log("NWU USER HIT ESCAPE2!!!");
				fail_connect(kConnectVerifyEscaped);
				return true;
			}
			if ((session_->session_flags() & 0x2u) == 0) {
				connect_log("NWU NOT CONNECTED FOR SOME REASON...");
				fail_connect(kConnectNotConnected);
				return true;
			}
			connect_log("NWU YIPPEE WE ARE CONNECTED AND VALIDATED.");
			connect_poll_ = env_.command_line_host ? ConnectPoll::Done : ConnectPoll::WebLobby;
			return false;
		}
		case ConnectPoll::WebLobby:
			// The menu's state 9 opens the web lobby (its HTTP Getting line is the hook's).
			// [orig: UI_ProcessLANSessionStateMachine @0x558DE0 - NW_MULTI_PLAYER @0x55926F,
			//  "%s : HTTP Getting \"%s\"" @0x5592D8, UIScene_OpenUrl @0x5592ED, state 0 @0x5592F2]
			connect_poll_ = ConnectPoll::Done;
			if (hooks_.on_connect_validated) hooks_.on_connect_validated();
			return false;
		default:
			return false;
	}
}

// The gate worker reads until the first reply its response buffer accepts, then closes the gate
// socket and ends: a late or duplicate reply is never read.
// [orig: CNapiGateManager_ProbeThreadProc @0x633b13..0x633bea (the receive, GATEAPI decrypt,
//  SetResponseBuffer), @0x633b65..0x633c3b (state 2: CloseSocket @0x633c36, state 3)]
void NwuLobbySession::poll_gate() {
	// Each frame of the wait, while the service runs: the waiting line past 500 ms from the stamp.
	// [orig: CNapiGateManager_ProcessResponse @0x4CED20 - the test @0x4CED49..0x4CED67, the
	//  socket @0x4CED84..0x4CEDB0, "%s : GATE WAITING FOR REPLY... [%s]" @0x4CEDF4, the stamp
	//  @0x4CEDFE]
	if (dedicated_wait_over_) return; // the dedicated wait stopped the gate service
	if (probe_sent_ && connect_log_on() && clock_ms_ - gate_log_stamp_ms_ > 500u) {
		connect_log("GATE WAITING FOR REPLY... [" + gate_socket_text() + "]");
		gate_log_stamp_ms_ = clock_ms_;
	}
	// A worker gone at its deadline receives nothing more: its status stays 2 and the response's
	// processing returns at its test every frame (D-NET-NEW-L9-3). [orig:
	//  CNapiGateManager_ProbeThreadProc @0x6339E0 - the exit @0x633A5E;
	//  CNapiGateManager_ProcessResponse @0x4CED20 - @0x4CEE84]
	if (probe_sent_ && gate_probe_datagram_.empty()) return;
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
		const std::string plain = gate_response_decrypt(inner.data(), inner.size());
		GateResponse parsed;
		const bool counted = gate_response_parse(plain, parsed);
		// The worker closed its socket with the reply it took, so the done line names none; the
		// reply's processing then names its size and each line it reads.
		// [orig: CNapiGateManager_ProbeThreadProc @0x6339E0 - CloseSocket @0x633C36, the status
		//  3 @0x633C3B; CNapiGateManager_ProcessResponse @0x4CED20 - "%s : GATE DONE WITH GATE
		//  LOOP [%s]" ("n/a" @0x4CEEBE) @0x4CEF02, its stamp @0x4CEF12; "%s : GATE GATE SERVICE
		//  SUCCEEDED.  EXTRACTING INFO." @0x4CF037; "%s : PROCESSING GATE DATA...(%ld bytes)"
		//  (the length +0x38) @0x4CF083; "%s : GATE LINE #%2.2ld [%s]" @0x4CF10C, from line 1
		//  @0x4CF0B4 (the next number @0x4CF4BE), each read @0x4CF4E3]
		if (connect_log_on()) {
			connect_log("GATE DONE WITH GATE LOOP [n/a]");
			gate_log_stamp_ms_ = clock_ms_;
			if (connect_log_on()) {
				connect_log("GATE GATE SERVICE SUCCEEDED.  EXTRACTING INFO.");
				if (connect_log_on()) {
					connect_log("PROCESSING GATE DATA...(" + std::to_string(plain.size()) +
							" bytes)");
				}
			}
		}
		if (connect_log_on()) {
			int32_t number = 1;
			for (const std::string &line : gate_response_lines(plain)) {
				char tag[32];
				std::snprintf(tag, sizeof(tag), "GATE LINE #%2.2ld [", static_cast<long>(number++));
				connect_log(tag + line + "]");
			}
		}

		gate_answered_ = true;
		gate_probe_datagram_.clear();
		GateMemory &kept = memory();
		kept.response = parsed;
		// The reply the worker took is the gate's answer: one whose parse counted no field (no
		// VAR line of a key the parse knows, or no line at all) fails the gate as state -9, the
		// menu's NWEC25, as a reply with no POST pair does below.
		// [orig: CNapiGateManager_ProcessResponse @0x4CED20 - the first line's read failing
		//  @0x4CF0C0, the count test @0x4CF4ED..0x4CF4EF, the state's switch @0x4CF51E, -9
		//  stored @0x4CF6AC]
		if (!counted) {
			gate_processed(kGateStateFailed);
			return;
		}
		apply_command_line_overrides();
		if (hooks_.on_gate_response)
			hooks_.on_gate_response(kept.response);

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
		kept.in_use = !parsed.udp_novaworld.empty() || !command_line_.udp_novaworld.empty();
		if (!kept.in_use) {
			const std::array<uint8_t, 4> none{ 0, 0, 0, 0 };
			// [orig: CNapiGateManager_ProcessResponse @0x4CED20 - "%s : GATE FAILING GATE
			//  SERVICE...NO NW POST IP" @0x4CF5EC, "...NO NW POST PORT" @0x4CF671]
			if (kept.response.post_ip == none) {
				connect_log("GATE FAILING GATE SERVICE...NO NW POST IP");
				gate_processed(kGateStateFailed);
				return;
			}
			if (kept.response.post_port == 0) {
				connect_log("GATE FAILING GATE SERVICE...NO NW POST PORT");
				gate_processed(kGateStateFailed);
				return;
			}
		}
		// The success tail resolves the metrics gate first: the METIPADDRESS string, with DNS on,
		// when the address, METIPPORT and METLABEL are all set (GateMemory::gate_address).
		// [orig: CNapiGateManager_ProcessResponse @0x4CED20 - @0x4CF6B8..0x4CF6E2]
		if (!kept.response.met_ip.empty() && kept.response.met_port != 0 &&
				!kept.response.met_label.empty())
			kept.gate_address = resolve_address(kept.response.met_ip);
		// The response's success tail: its USEJUNCTION / CLEARJUNCTION take the command line's
		// overrides, a set USEJUNCTION empties the menu's JUNCTIONCODE (and sets the web lobby
		// form's USEJUNCTION, which nothing here reads), and the gate's info stands.
		// [orig: CNapiGateManager_ProcessResponse @0x4CED20 - the overrides @0x4CF6ED..0x4CF709,
		//  the form writes @0x4CF71B..0x4CF76E, dword_B5FF48 = 1 @0x4CF81D]
		kept.use_junction = command_line_.use_junction != -1
				? command_line_.use_junction
				: static_cast<int32_t>(parsed.use_junction);
		kept.clear_junction = command_line_.clear_junction != -1
				? command_line_.clear_junction
				: static_cast<int32_t>(parsed.clear_junction);
		if (kept.use_junction != 0)
			kept.junction_code.clear();
		kept.response_kept = true;
		// [orig: CNapiGateManager_ProcessResponse @0x4CED20 - Napi_FormatAddress over the
		//  REFLECTEDIPADDRESS / REFLECTEDPORTNUMBER words (g_NovaWorldServerAddr @0x4CF3A9,
		//  g_NovaWorldServerPort @0x4CF3EA) @0x4CF850, "%s : GATE GATE SERVICE DONE AND
		//  SUCCESSFUL [yourip=%s]." @0x4CF884]
		if (connect_log_on()) {
			const std::array<uint8_t, 4> &ip = kept.response.reflected_ip;
			connect_log("GATE GATE SERVICE DONE AND SUCCESSFUL [yourip=" + std::to_string(ip[0]) +
					"." + std::to_string(ip[1]) + "." + std::to_string(ip[2]) + "." +
					std::to_string(ip[3]) + ":" +
					std::to_string(static_cast<int32_t>(kept.response.reflected_port)) + "].");
		}
		gate_processed(kGateStateSucceeded);
		return;
	}
}

// The command-line host's gate wait returns once the gate state leaves 1 and the host goes on
// to the connect at once (a failure ends the hosting); the menu runs its next states one a frame.
// [orig: Game_HostMultiplayerSession @0x4A65A0 - CNapiGateManager_WaitForGateResponse (the call
//  @0x4A66B6), CNapiGameSession_ConnectAndWaitForValidation (@0x4A66E2) -> ConnectToNovaWorld
//  (@0x4D4EC3); UI_ProcessLANSessionStateMachine @0x558DE0 - state 2 @0x55906B..0x559094]
void NwuLobbySession::gate_processed(int gate_state) {
	if (env_.command_line_host) {
		if (gate_state < 0) {
			if (hooks_.on_fatal)
				hooks_.on_fatal(novaworld_gate_error_tag(gate_state, true));
			return;
		}
		connect_from_response();
		return;
	}
	gate_result_ = gate_state;
	gate_menu_ = GateMenu::Answered;
}

// [orig: UI_ProcessLANSessionStateMachine @0x558DE0 - state 2: the gate state 1 test
//  @0x55906B..0x559072 (CNapiGateManager_ProcessResponse, the call @0x559079, while it holds), 3
//  set @0x559094; state 3: the test @0x5590B4..0x5590BB, a negative state's NWEC (-9: NWEC25
//  @0x5590FD, UI_ShowNovaWorldErrorMessage the call @0x559109, state 0 @0x559111), else 4 set
//  @0x559131; state 4: CNapiGameSession_ConnectToNovaWorld (the call @0x559156), 5 set
//  @0x55917E]
bool NwuLobbySession::step_gate_menu() {
	switch (gate_menu_) {
		case GateMenu::Answered:
			gate_menu_ = GateMenu::Check;
			return false;
		case GateMenu::Check:
			if (gate_result_ < 0) {
				gate_menu_ = GateMenu::Off;
				if (hooks_.on_fatal)
					hooks_.on_fatal(novaworld_gate_error_tag(gate_result_, true));
				return true;
			}
			gate_menu_ = GateMenu::Connect;
			return false;
		case GateMenu::Connect:
			gate_menu_ = GateMenu::Off;
			connect_from_response();
			return true;
		default:
			return false;
	}
}

// ConnectToNovaWorld over the gate memory's response: with the session not in use it returns at
// once (each connect poll too, and the menu opens its web lobby); else the old targets go, then
// the connect's one source is made probe targets: the menu's JUNCTIONCODE when it is nonempty,
// else every /UDPNOVAWORLD command-line entry, else every gate entry. An address that names none
// makes no target and is passed over, so with none the connect probes nothing until it gives up.
// [orig: CNapiGameSession_ConnectToNovaWorld @0x4D4640 - @0x4D46EA..0x4D46F0; the old targets
//  freed @0x4D4A22 (NapiNPEnumerator_DestroyAllTimers @0x6220C0 over the list CreateTarget
//  fills); JUNCTIONCODE @0x4D4A4A, its one target @0x4D4A71; the command line's entries
//  @0x4D4A78..0x4D4A9E; the gate variables @0x4D4AA2..0x4D4ACE (CMissionInfo_GetVariable
//  @0x4D4AB8, CNapiGameSession_SendToAddress @0x4D4AC0, its result unread), then
//  CNapiGameSession_InitPlayerConnection @0x4D4B16]
void NwuLobbySession::connect_from_response() {
	const GateMemory &kept = memory();
	// [orig: CNapiGameSession_ConnectToNovaWorld @0x4D4640 - the in-use test @0x4D467D, "%s : NWU
	//  DO ALL KINDS OF STUFF." @0x4D46C3; "%s : NWU INIT STUFF." @0x4D4751]
	if (kept.in_use)
		connect_log("NWU DO ALL KINDS OF STUFF.");
	if (!kept.in_use) {
		if (hooks_.on_sessionless)
			hooks_.on_sessionless();
		return;
	}
	connect_log("NWU INIT STUFF.");
	targets_.clear();
	if (!kept.junction_code.empty()) {
		add_target(kept.junction_code);
	} else if (!command_line_.udp_novaworld.empty()) {
		for (const std::string &address : command_line_.udp_novaworld)
			add_target(address);
	} else {
		for (const std::string &address : kept.response.udp_novaworld)
			add_target(address);
	}
	// [orig: CNapiGameSession_ConnectToNovaWorld @0x4D4640 - "%s : NWU ATTEMPTING TO CONNECT TO
	//  NOVAWORLD..." @0x4D4B0C, InitPlayerConnection @0x4D4B16, "%s : NWU GOING INTO JOIN
	//  LOOP..." @0x4D4BC2]
	connect_log("NWU ATTEMPTING TO CONNECT TO NOVAWORLD...");
	std::vector<uint8_t> first = begin_session();
	connect_log("NWU GOING INTO JOIN LOOP...");
	// The connection's first announce goes once the join loop runs: InitPlayerConnection starts the
	// enumerator's timer and sends nothing; the loop's first network pump sends it, at once in the
	// command-line host's blocking loop, at the menu's next frame otherwise (tick).
	// [orig: CNapiGameSession_InitPlayerConnection @0x4D4160 - NapiTimer_Start @0x4D43C2, no send
	//  among its callees; CNapiNPConnection_PumpEnumeratorAndSend @0x6290C0;
	//  CNapiGameSession_ConnectAndWaitForValidation @0x4D4EC0 - the first poll @0x4D4ED2]
	if (env_.command_line_host)
		send(first);
	else
		first_announce_ = std::move(first);
}

// The session state machine and its ClientHello, once the gate response named the NW UDP
// endpoint. The 0x42-join CU set is the one retail builds for any NovaWorld connection, browse
// or host: all 11 chunks, type 2, the locale chunks empty on the join; UdpCode1/2 and MetTag come
// from the gate. [orig: CNapiGameSession_ConnectToNovaWorld @0x4d4640]
std::vector<uint8_t> NwuLobbySession::begin_session() {
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
	const GateResponse &gate = memory().response;
	cu.met_tag = gate.met_label;
	cu.udp_code1 = gate.udp_code1;
	cu.udp_code2 = gate.udp_code2;
	if (env_.max_packet_size != 0) cu.max_packet_size = env_.max_packet_size;
	cfg.cu_vars = make_novaworld_join_cu(cu);
	// The gate's GLSVSS trio arms the session's periodic GLSVSS leg.
	cfg.glsvss_request = gate.glsvss_request;
	cfg.glsvss_rims_ms = gate.glsvss_rims;
	cfg.glsvss_agrms_ms = gate.glsvss_agrms;
	// The "Cookie" var-list every Cookie-bearing statement re-serializes.
	cfg.cookie_vars = hooks_.cookie_vars;
	// The connection's state-5 callbacks' connect-log lines.
	cfg.on_connection_up = [this]() { log_connection_up(); };
	cfg.on_connection_down = [this]() { log_connection_down(); };
	// [orig: Network_LogNWUProcessPacket @0x4CFC80 - Napi_FormatAddress of the connection's
	//  +0x30 / +0x34 @0x4CFCC4, "%s : NWU PROCESSING %ld BYTES FROM %s" @0x4CFCFD, installed as
	//  the protocol's +0x274 @0x4D42C2]
	cfg.on_packet_processed = [this](std::size_t bytes) {
		if (connect_log_on())
			connect_log("NWU PROCESSING " + std::to_string(bytes) + " BYTES FROM " +
					peer_addr_to_string(nw_peer_));
	};
	// [orig: Network_LogUnknownNWUMessage @0x4CDF90 - "%s : NWU UNKNOWN MESSAGE!! [%ld, %ld bytes]"
	//  over the tag and the length (the append @0x4CDFEB), installed as the protocol's +0x27C
	//  @0x4D42CF]
	cfg.on_unknown_message = [this](uint8_t tag, std::size_t bytes) {
		if (connect_log_on())
			connect_log("NWU UNKNOWN MESSAGE!! [" + std::to_string(tag) + ", " +
					std::to_string(bytes) + " bytes]");
	};

	write_pending_connection_down();
	session_ = std::make_unique<ClientSession>(cfg);
	drains_seen_ = 0; // the new connection's enumerator has drained nothing
	session_->set_clock_ms(clock_ms_);
	if (hooks_.on_session_created)
		hooks_.on_session_created(cfg.cu_vars.size(), cfg.cookie_vars ? cfg.cookie_vars().size() : 0);
	// The connect's stamp, its poll from here. [orig: CNapiGameSession_ConnectToNovaWorld
	//  @0x4D4640 - dword_B5F444 @0x4D4BD5, the flags cleared @0x4D4BDA..0x4D4BE0]
	connect_poll_ = ConnectPoll::Connect;
	connect_stamp_ms_ = clock_ms_;
	connect_timed_out_ = false;
	connect_escaped_ = false;
	escape_requested_ = false;
	give_up_ = 0;
	connect_last_send_ms_ = clock_ms_;
	if (hooks_.on_session_state) hooks_.on_session_state();
	return session_->start();
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
	send_datagram(to, dg);
	// A missing-sequence list naming a sequence, once sent. [orig: NWU_LogIncomingPacketError
	//  @0x4CE010 - "%s : NWU INCOMING PACKET ERROR!!" (the append @0x4CE05A), installed as the
	//  protocol's +0x280 @0x4D42DC; CNapiNPConnection_SendMissingSeqList @0x623560 - a nonzero
	//  entry @0x62368A..0x623690, after CNapiNPManager_SendTo (the call @0x623766) the client's
	//  callback @0x6237AA..0x6237BD]
	if (connect_log_on()) {
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		if (nw_decode_inbound(dg.data(), dg.size(), opcode, body) &&
				opcode == SESSION_OPCODE_CLIENT_RESEND_LIST) {
			for (std::size_t at = 4; at + 4 <= body.size(); at += 4) {
				if (io::read_u32_le(body.data() + at) != 0) {
					connect_log("NWU INCOMING PACKET ERROR!!");
					break;
				}
			}
		}
	}
}

void NwuLobbySession::send_datagram(const PeerAddr &to, const std::vector<uint8_t> &dg) {
	// Each NWU datagram the connection sends, ahead of the send: its length past the envelope
	// (opcode included), its address, its opcode and, for a session packet, its packet sequence
	// (0 for the others). [orig: Network_LogNWUSendPacket @0x4CDED0 - "%s : NWU SENDING %ld
	//  BYTES TO %s [0x%2.2lx, p#%ld]" @0x4CDF5C, installed as the protocol's +0x270 @0x4D42B5;
	//  its calls ahead of CNapiNPManager_SendTo: CNapiNPConnection_SendSessionPacket @0x61EDD0
	//  (the +0x270 load @0x61EFE0; the opcode, packet_seq and total_len @0x61EFEA..0x61F00D),
	//  CNapiNPConnection_SendPing @0x61F080 (the +0x270 load @0x61F21A),
	//  CNapiNPConnection_SendDisconnectPacket @0x61F2A0 (the +0x270 load @0x61F514),
	//  CNapiNPConnection_SendEncryptedPayload @0x61F5A0 (the +0x270 load @0x61F759),
	//  NapiNPSession_SendAnnouncePacket @0x61FA00 (the +0x270 load @0x61FDAC; 0x41, the target's
	//  address, p# 0, the length @0x61FD77..0x61FD7B), CNapiNPConnection_SendClientJoin @0x61FE20
	//  (the +0x270 load @0x6203C3; 0x42), CNapiNPConnection_SendMissingSeqList @0x623560 (the
	//  +0x270 load @0x623720), each p# 0]
	if (connect_log_on()) {
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		if (nw_decode_inbound(dg.data(), dg.size(), opcode, body)) {
			const uint32_t packet_seq = opcode == SESSION_OPCODE_PROTOCOL_MESSAGE && body.size() >= 8
					? io::read_u32_le(body.data() + 4)
					: 0u;
			char op[8];
			std::snprintf(op, sizeof(op), "0x%2.2x", static_cast<unsigned>(opcode));
			connect_log("NWU SENDING " + std::to_string(body.size() + 1) + " BYTES TO " +
					peer_addr_to_string(to) + " [" + op + ", p#" +
					std::to_string(static_cast<int32_t>(packet_seq)) + "]");
		}
	}
	session_socket_->send_to(to, dg.data(), dg.size());
	if (hooks_.on_sent) hooks_.on_sent(to, dg);
	// The teardown's burst out: OnDisconnect's line follows its last send.
	if (pending_down_sends_ != 0) {
		uint8_t opcode = 0;
		if (stripped_opcode(dg.data(), dg.size(), opcode) && opcode == SESSION_OPCODE_CLIENT_GOODBYE &&
				--pending_down_sends_ == 0)
			write_pending_connection_down();
	}
}

void NwuLobbySession::send_to(const PeerAddr &to, const std::vector<uint8_t> &dg) {
	if (session_socket_ == nullptr || dg.empty() || to.port == 0) return;
	session_socket_->send_to(to, dg.data(), dg.size());
}

// [orig: Server_SendMetricsToGate @0x4FB0A0 - g_GateAddress @0x4FB0D4, the send @0x4FB37E;
//  NapiSocket_SendTo's htons over g_GatePort]
void NwuLobbySession::send_gate_metrics(std::string_view text) {
	const GateMemory &kept = memory();
	if (kept.gate_address == 0) return;
	PeerAddr to;
	to.ip = kept.gate_address;
	to.port = static_cast<uint16_t>(kept.response.met_port);
	send_to(to, gate_metrics_datagram(text));
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
		const uint32_t resend_lists = session_->resend_lists_named();
		rx.ok = session_->handle_datagram(rx.bytes.data(), rx.bytes.size(), replies);
		// A 0x81 the connection took makes its record and points the connection at the reply's
		// sender, where its 0x42 goes.
		// [orig: CNapiNPConnection_InitFromSession @0x626320 - the session record's address
		//  and port into the connection @0x6263B4 / @0x6263B7]
		if (session_->server_hellos_taken() != hellos_taken) {
			note_server_hello(from);
			nw_peer_ = from;
		}
		rx.state_after = static_cast<int>(session_->state());
		rx.replies = replies.size();
		if (hooks_.on_received) hooks_.on_received(rx);
		for (const auto &dg : replies) send(dg);
		// A resend list that named a sequence, once its resends are out.
		// [orig: NWU_LogOutgoingPacketError @0x4CE080 - "%s : NWU OUTGOING PACKET ERROR!!" (the
		//  append @0x4CE0CA), installed as the protocol's +0x284 @0x4D42E9; NapiNP_HandleResendList
		//  @0x623800 - the call @0x623A37]
		if (session_->resend_lists_named() != resend_lists)
			connect_log("NWU OUTGOING PACKET ERROR!!");
		note_session_drains();
		if (!rx.ok) {
			// While the first connect's poll runs, the poll reports the failure with the menu's
			// NWEC (a rejected verify leaves the session down: the connection wait's NWEC48),
			// as retail's menu shows the connect's errors only through its waits.
			// [orig: UI_ProcessLANSessionStateMachine @0x558DE0 - the error leg
			//  @0x55915F..0x559160; CGameSession_WaitForNWUConnection @0x4D1230 - bit 2
			//  @0x4D1332, -23 @0x4D1381]
			if (!connect_polling() && hooks_.on_fatal)
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
	note_session_drains();
	if (session_->state() == ClientSession::State::Closed && session_->disconnected_by_peer()) {
		if (hooks_.on_session_state) hooks_.on_session_state();
	}
}

NwuLobbySession::GateMemory &process_gate_memory() {
	static NwuLobbySession::GateMemory memory;
	return memory;
}

// [orig: MultiPlayer_JoinSessionStateMachine @0x56A320 - @0x56A4DE..0x56A50B;
//  Server_SendPingMetricsToGate @0x511BF0 - LABEL g_ServerLabel @0x511CDD, GV g_VersionText
//  @0x511D50, g_IsDedicatedServer (METEXT) @0x511D58, the send @0x5120CE]
void send_join_gate_ping_report(IDatagramSocket &socket, uint32_t now_ms,
                                const NwuLobbySession::GateMemory &memory, GatePingManager &pings,
                                GateMetricsHost host, std::string_view cc_bin) {
	const GateResponse &gate = memory.response;
	if (!gate_ping_report_due(pings.report_tick, gate.met_ping, now_ms)) return;
	host.label = gate.met_label;
	host.met_ext = gate.met_ext != 0;
	const std::string text =
			gate_ping_report_text(pings, memory.gate_address, gate.met_ping, host, cc_bin);
	if (text.empty()) return;
	const std::vector<uint8_t> dg = gate_metrics_datagram(text);
	if (dg.empty()) return;
	PeerAddr to;
	to.ip = memory.gate_address;
	to.port = static_cast<uint16_t>(gate.met_port);
	socket.send_to(to, dg.data(), dg.size());
}

} // namespace opennova
