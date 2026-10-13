#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <net/novaworld/client_session.h>
#include <net/novaworld/gate_response.h>
#include <net/npwire/idatagram_socket.h>
#include <net/npwire/peer_addr.h>

namespace opennova {

// The NWU gate/session driver: the gate probe and its worker clocks, then the ClientSession
// handshake to a verified lobby session, pumped over two embedder-owned datagram sockets (the
// gate's and the session's). Retail runs one game session per process; the game shell and the
// opennova-nw-lister app both drive this one. Sockets, name resolution and randomness are the embedder's
// (Environment); the protocol is ClientSession's.
// [orig: CNapiGateManager_ProbeThreadProc @0x6339e0 (the gate worker);
//  CNapiGameSession_InitNPConnection @0x4d3be0; CNapiGameSession_ProcessPeriodicUpdate
//  @0x4d4400 (the per-tick pump order); CNapiGameSession_ConnectToNovaWorld @0x4d4640]
class NwuLobbySession {
public:
	// What the owner's per-datagram diagnostics need beyond session().
	struct RxInfo {
		std::vector<uint8_t> bytes;
		PeerAddr from;
		int state_before = 0; // ClientSession::State, as int
		int state_after = 0;
		bool ok = true;
		std::size_t replies = 0;
	};
	struct Hooks {
		// The parsed gate response, before the session handshake starts.
		std::function<void(const GateResponse &)> on_gate_response;
		// The current "Cookie" var-list, read for each statement that serializes it.
		std::function<std::vector<std::pair<std::string, std::string>>()> cookie_vars;
		// The session was created; its ClientHello ships right after.
		std::function<void(std::size_t cu_vars, std::size_t cookie_vars)> on_session_created;
		// An outbound datagram just shipped on the session socket to `to` (trace hook).
		std::function<void(const PeerAddr &to, const std::vector<uint8_t> &)> on_sent;
		// One inbound session datagram was handled (trace hook).
		std::function<void(const RxInfo &)> on_received;
		// The session advanced: the owner maps ClientSession::State onto its own states.
		std::function<void()> on_session_state;
		// Fatal: a gate response naming no UDPNOVAWORLD, a session error, or a give-up of the
		// first connect (novaworld_connect_error_tag's NWEC). The message is the retail NWEC tag
		// when one is witnessed for the failure.
		std::function<void(const std::string &)> on_fatal;
		// Recoverable decode noise on the gate socket (a bad envelope or response).
		std::function<void(const std::string &)> on_soft_error;
		// The gate named no NWU address and its POST address and port are both set: the connect
		// goes on with no NWU session (retail's NovaWorld menu then opens its web lobby, menu
		// state 9). No session is created; match_facts().in_use stays false.
		std::function<void()> on_sessionless;
	};
	struct Environment {
		// The client index/key draws, the reconnect's fresh CK and the host role's AppId and
		// cookie-key seeds. Unset: make_random_session_u32, the OS CSPRNG (base/os_random).
		std::function<uint32_t()> random_u32;
		// Resolve a gate host or a UDPNOVAWORLD entry's host ("a.b.c.d" or a name) into `out`
		// (its port left alone): the platform's inet_addr, then its name lookup; false when
		// neither reads it. Unset: dotted quads only.
		// [orig: Napi_ResolveAddress @0x62DF90 - inet_addr @0x62DFAD, gethostbyname
		//  @0x62DFCA]
		std::function<bool(const std::string &host, PeerAddr &out)> resolve_ipv4;
		// The configured game.cfg `mpmaxpacketsize` (0 -> 1300): the lobby template's field 13
		// and the 0x42's MaxPacketSize CU. [orig: InitNPConnection @0x4d3df4; ConnectToNovaWorld
		//  @0x4d4913..0x4d493e]
		int32_t max_packet_size = 0;
	};
	// The NovaWorld switches of the game's command line the gate response and the connect read
	// (LaunchFlags): the POST address (resolved with DNS on; one that does not resolve overrides
	// nothing) and port (0 overrides nothing) that replace the gate's POSTIPADDRESS / POSTIPPORT;
	// the /UDPNOVAWORLD entries, which mark the NWU session in use and are probed in place of the
	// gate's; and the USEJUNCTION / CLEARJUNCTION overrides (-1 overrides nothing).
	// [orig: CNapiGateManager_ProcessResponse @0x4CED20 - dword_B4C6B0 into dword_B5F490
	//  @0x4CF54B..0x4CF554, dword_B4C6B4 into dword_B5F494 @0x4CF559..0x4CF562, the command
	//  line's count dword_B4C71C @0x4CF52E, dword_829F88 / dword_829F8C @0x4CF6ED..0x4CF709;
	//  CNapiGameSession_ConnectToNovaWorld @0x4D4640 - the entries @0x4D4A78..0x4D4A9E]
	struct CommandLine {
		std::string post_ip_address;
		int32_t post_port_number = 0;
		std::vector<std::string> udp_novaworld;
		int32_t use_junction = -1;
		int32_t clear_junction = -1;
	};
	// What a match reads of this session: the gate or the command line named an NWU address
	// (retail's dword_B5FD2C), and the session's flags word, hosting/playing word and mission-exit
	// store (all 0 once the session is dropped). The first is the gate response's, so it outlives
	// a closed session as retail's global does.
	struct MatchFacts {
		bool in_use = false;
		uint32_t flags = 0;
		int32_t role = 0;
		int32_t exit_reason = 0;
	};

	NwuLobbySession(Hooks hooks, Environment env);

	// The command line's switches (unset: none), read at each gate response.
	void set_command_line(CommandLine command_line) { command_line_ = std::move(command_line); }
	// The NovaWorld menu's JUNCTIONCODE form field: when it is nonempty at the connect, its one
	// address is the connect's only probe target, ahead of the command line's and the gate's
	// entries. A gate response whose USEJUNCTION (or the command line's) is set empties it, and
	// so does the menu's entry (open()) when the last response's USEJUNCTION and CLEARJUNCTION
	// were both set. It outlives open() and close(), as the menu's form does. Only the web
	// lobby's form writes it in retail, which no shell here has (unwitnessed).
	// [orig: CNapiGameSession_ConnectToNovaWorld @0x4D4640 - the read @0x4D4A31..0x4D4A4A, its
	//  one target @0x4D4A4F..0x4D4A71; CNapiGateManager_ProcessResponse @0x4CED20 - the clear
	//  @0x4CF71B..0x4CF76E; UI_ProcessLANSessionStateMachine @0x558DE0 - @0x558E2D..0x558E64]
	void set_junction_code(std::string code) { junction_code_ = std::move(code); }
	const std::string &junction_code() const { return junction_code_; }

	// Start over on the embedder's two bound sockets (non-owning; they outlive close()) and mint
	// the ci/ck pair. The clock restarts at 0 with the first tick().
	void open(IDatagramSocket &gate, IDatagramSocket &session);
	// Send the enveloped gate probe toward `gate_host:gate_port` and start the gate worker's
	// retry/deadline clocks. An unresolvable host sends nothing, so the deadline reports it.
	void probe(const std::string &gate_host, uint16_t gate_port);
	// Drop the session and the sockets (the embedder closes them). Owners send their goodbye
	// datagrams BEFORE closing (send()/session() are still live).
	void close();
	// One tick at the owner's wall clock: the gate socket until its reply landed, the session
	// socket (each datagram through the session, its immediate sends out), the receive batch's
	// missing-sequence tail, the connection's send pump, the periodic update's statements and its
	// first-connect give-up, then the gate worker's retransmits and deadline, the first connect's
	// poll (its give-ups and their NWEC reports) and its stage retransmits.
	void tick(uint32_t now_ms);
	// Send one datagram (fires on_sent per send): the announce (a 0x41) to every probe target,
	// each target its own announce carrying that target's address and port as EIP / EPN
	// (ClientSession::build_announce), anything else to the connection's server.
	void send(const std::vector<uint8_t> &dg);
	// One connection send pump now, outside the tick: what the session queued (a stop statement
	// before the goodbye) goes out at once.
	void flush();
	// Send one raw datagram to `to` over the session socket: the host's status heartbeat to the
	// gate's POSTIPADDRESS:POSTIPPORT, retail's plain sendto on the NP manager's socket outside
	// the NP session. [orig: CNapiNetwork_SendUDPPacket -> CNapiNPManager_SendTo @0x61ec20]
	void send_to(const PeerAddr &to, const std::vector<uint8_t> &dg);

	// Whether this session's protocol takes `data` from `from` when the session socket is shared
	// with the game's protocol (DatagramDemux's claim): a server-direction opcode (0x81..0x87, the
	// client-side handlers of the opcode table) from the lobby server's endpoint, which is where
	// its connection lives, or a 0x81 its connection takes from any sender (the reply finds the
	// connection by its CI: ClientSession::takes_server_hello). Everything else is the game's (a
	// joiner's 0x41..0x47, a LAN probe).
	// [orig: NapiNPManager_HandlePacket @0x622f10 offers the envelope-stripped datagram to each
	//  protocol; NapiNPProtocol_DispatchOpcode @0x622b40 over g_NPOpcodeHandlers @0x849d90 (0x81
	//  Nwu_HandleServerHello .. 0x87 Nwu_HandleServerProbe); Nwu_HandleServerSession @0x627f90 ->
	//  NapiNPProtocol_HandleSessionPacket(conn_type 2) @0x626a00, FindConnection(proto, 2, addr,
	//  port) @0x626ac5]
	bool claims(const PeerAddr &from, const uint8_t *data, std::size_t len) const;

	bool is_open() const { return session_socket_ != nullptr; }
	ClientSession *session() { return session_.get(); }
	const ClientSession *session() const { return session_.get(); }
	bool session_verified() const { return session_ && session_->is_verified(); }
	const GateResponse &gate_response() const { return gate_response_; }
	MatchFacts match_facts() const;
	uint32_t client_index() const { return client_index_; }
	uint32_t client_key() const { return client_key_; }
	// One probe target: an address and the port as parsed (the send narrows it to 16 bits).
	struct Target {
		uint32_t ip = 0; // PeerAddr's packing
		uint32_t port = 0;
	};
	// The connect's probe targets, one per gate UDPNOVAWORLD entry that resolved, in order and
	// without repeats.
	const std::vector<Target> &targets() const { return targets_; }
	// The connection's server: the sender of the 0x81 it last took (empty until then).
	const PeerAddr &nw_peer() const { return nw_peer_; }
	// The driver's wall clock in ms (the session's GetTickCount).
	uint32_t clock_ms() const { return clock_ms_; }
	uint32_t random_u32() const;

private:
	void begin_session();
	// One address string into a probe target, as retail's send-to-address does; false (no
	// target) when it names no address.
	bool add_target(const std::string &address);
	// The address of a host as Napi_ResolveAddress reads it (DNS on), 0 when it does not resolve.
	uint32_t resolve_address(const std::string &host) const;
	void send_one(const PeerAddr &to, const std::vector<uint8_t> &dg);
	void poll_gate();
	// The command line's POST address and port over the gate response's.
	void apply_command_line_post();
	void poll_session();
	void send_gate_probe();
	bool resolve(const std::string &host, uint16_t port, PeerAddr &out) const;
	// The periodic update's give-up legs, then the first connect's poll.
	void give_up_stalled_connect();
	bool poll_connect();
	// A give-up: the session's abort, its sends out.
	void abort_connect();
	// A connect wait's failure: the session's reset (a connection still up tears down) and the
	// code's NWEC to on_fatal.
	void fail_connect(int code);

	Hooks hooks_;
	Environment env_;
	CommandLine command_line_;
	IDatagramSocket *gate_socket_ = nullptr;
	IDatagramSocket *session_socket_ = nullptr;
	PeerAddr gate_peer_;
	bool gate_resolved_ = false;
	bool probe_sent_ = false;
	bool gate_answered_ = false;
	std::vector<uint8_t> gate_probe_datagram_;
	std::unique_ptr<ClientSession> session_;
	uint32_t client_index_ = 0;
	uint32_t client_key_ = 0;
	std::vector<Target> targets_; // the enumerator's target list
	PeerAddr nw_peer_;
	GateResponse gate_response_;
	bool in_use_ = false; // dword_B5FD2C
	// The menu form's JUNCTIONCODE, and the last gate response's word set (dword_B5FF48) with its
	// USEJUNCTION / CLEARJUNCTION after the command line's (dword_B5FD38 / dword_B5FD3C), which
	// the menu's entry reads; none reset by open().
	std::string junction_code_;
	bool gate_ok_ = false;
	int32_t use_junction_ = 0;
	int32_t clear_junction_ = 0;
	uint32_t clock_ms_ = 0;
	// The gate worker: the probe re-sent every retry interval until the deadline.
	uint32_t gate_started_ms_ = 0;
	uint32_t gate_last_send_ms_ = 0;
	// The first connect's poll, retail's NovaWorld menu running PollConnectPhase1, the validation
	// wait, PollConnectPhase2 and the connection wait in turn: its stamp (dword_B5F444), its
	// timed-out flag (dword_B5F448) and the give-up word the periodic update sets (session+0x12C:
	// 1 the state-1 give-up). The stage datagram is re-sent every retransmit interval while it
	// runs.
	// [orig: UI_ProcessLANSessionStateMachine @0x558DE0 - states 5..8 @0x55919E..0x55923C]
	enum class ConnectPoll { Off, Connect, Verify, Done };
	ConnectPoll connect_poll_ = ConnectPoll::Off;
	uint32_t connect_stamp_ms_ = 0;
	bool connect_timed_out_ = false;
	int give_up_ = 0;
	uint32_t connect_last_send_ms_ = 0;
};

} // namespace opennova
