#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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
// gate's and the session's). Retail runs one game session per process; the game shell and
// opennova-serve's listing both drive this one. Sockets, name resolution and randomness are the
// embedder's (Environment); the protocol is ClientSession's. [orig:
// CNapiGateManager_ProbeThreadProc @0x6339e0 (the gate worker);
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
		// The parsed gate response, before the session handshake starts; on an entry over a kept
		// response (no probe, GateMemory), the kept one.
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
		// Fatal: a gate response failing the gate (NWEC25), a session error once the first
		// connect's poll has ended, or a give-up or failure of that poll
		// (novaworld_connect_error_tag's NWEC), which reports any session error while it runs.
		// After a poll failure the driver has dropped the connection (session() is null once the
		// hook returns). The message is the retail NWEC tag when one is witnessed for the
		// failure.
		std::function<void(const std::string &)> on_fatal;
		// Recoverable decode noise on the gate socket (a bad envelope or response).
		std::function<void(const std::string &)> on_soft_error;
		// The first connect's poll ended connected and validated: the menu's state 9, the frame
		// after the connection wait's success, opens the web lobby. The command-line host runs no
		// menu and leaves it unset.
		// [orig: UI_ProcessLANSessionStateMachine @0x558DE0 - state 8's success @0x559242, state 9
		//  @0x559262..0x5592F2]
		std::function<void()> on_connect_validated;
		// The gate named no NWU address and its POST address and port are both set: the connect
		// goes on with no NWU session (retail's NovaWorld menu then opens its web lobby, menu
		// state 9). No session is created; match_facts().in_use stays false.
		std::function<void()> on_sessionless;
		// The process's _connectlog.txt (`/CONNECTLOG`; the embedder's inmatch::ConnectLog):
		// whether it is on now, and one line's text, which the device stamps with the time and
		// ends. Unset: off. The gate and connect lines are the menu's
		// [orig: g_ConnectLogEnabled @0xB4C720; NapiFile_AppendLine("_connectlog.txt", ..)].
		std::function<bool()> connect_log_on;
		std::function<void(const std::string &)> connect_log;
	};
	struct Environment {
		// The host role's AppId and cookie-key seeds. Unset: make_random_session_u32, the OS
		// CSPRNG (base/os_random).
		std::function<uint32_t()> random_u32;
		// The connection's CK and SCRK at each 0x81 (ClientSession::Config::connection_keys).
		// Unset: the process's one manager LCG, as retail's; a test injects its own.
		std::function<ConnectionKeys()> connection_keys;
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
		// The gate socket's bound port, which the connect log's gate lines name. Unset: 0.
		std::function<uint16_t()> gate_local_port;
		// Retail's command-line host (opennova-serve), which runs no NovaWorld menu: its gate wait
		// outlives the worker (the worker's deadline reports nothing and stops the gate's
		// receive; SESSION_DEDICATED_GATE_WAIT_MS after the probe the gate service's stop line,
		// then the gate-error map's NWEC15 to on_fatal), and its host request waits for the
		// result with no timeout and writes none of ConnectOrHost's lines (NwuHostRole). False:
		// the menu's legs. [orig: Game_HostMultiplayerSession @0x4A65A0 -
		//  CNapiGameSession_WaitForStateChange @0x4D1510 (the call @0x4A66B6), the hosting wait
		//  while state 5 @0x4A6725..0x4A6735]
		bool command_line_host = false;
	};
	// The NovaWorld switches of the game's command line the gate response and the connect read
	// (LaunchFlags): the POST address (resolved with DNS on; one that does not resolve overrides
	// nothing) and port (0 overrides nothing) that replace the gate's POSTIPADDRESS / POSTIPPORT;
	// the /UDPNOVAWORLD entries, which mark the NWU session in use and are probed in place of the
	// gate's; the GLSVSSRIMS / GLSVSSAGRMS a switch gave (unset overrides nothing, any value
	// set does); and the USEJUNCTION / CLEARJUNCTION overrides (-1 overrides nothing).
	// [orig: CNapiGateManager_ProcessResponse @0x4CED20 - dword_B4C6B0 into dword_B5F490
	//  @0x4CF54B..0x4CF554, dword_B4C6B4 into dword_B5F494 @0x4CF559..0x4CF562, under
	//  dword_B4C69C dword_B4C6A0 into dword_B5FF40 @0x4CF567..0x4CF575, under dword_B4C6A4
	//  dword_B4C6A8 into dword_B5FF44 @0x4CF57A..0x4CF589, the command line's count
	//  dword_B4C71C @0x4CF52E, dword_829F88 / dword_829F8C @0x4CF6ED..0x4CF709;
	//  CNapiGameSession_ConnectToNovaWorld @0x4D4640 - the entries @0x4D4A78..0x4D4A9E]
	struct CommandLine {
		std::string post_ip_address;
		int32_t post_port_number = 0;
		std::vector<std::string> udp_novaworld;
		std::optional<int32_t> glsvss_rims;
		std::optional<int32_t> glsvss_agrms;
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

	// What a gate response that went through leaves for the NovaWorld menu's next entry: retail's
	// gate manager globals, which outlive the session and every exit from the menu until the menu
	// itself is destroyed (forget()). While `response_kept` is set an entry starts no probe and
	// connects from the kept response, which is not processed again (probe()). The embedder whose
	// menu entries share them hands each driver the same one (set_gate_memory: the game shell's
	// process_gate_memory()); a driver without one keeps its own.
	// [orig: CNapiGateManager_ProcessResponse @0x4CED20 - the in-use word dword_B5FD2C
	//  @0x4CF525..0x4CF541, dword_B5FD38 / dword_B5FD3C @0x4CF6ED..0x4CF709, dword_B5FF48 = 1
	//  @0x4CF81D; CNapiGateManager_WaitForConnect @0x4CE6B0 - the kept test @0x4CE6CF..0x4CE6F3,
	//  the fresh probe's CMissionInfo_Init(byte_B5F450) @0x4CE719]
	struct GateMemory {
		bool response_kept = false; // dword_B5FF48
		// The response the connect reads (byte_B5F450 and the words its processing wrote, the
		// command line's overrides applied) and the gate it came from.
		GateResponse response;
		std::string gate_host;
		uint16_t gate_port = 0;
		bool in_use = false;        // dword_B5FD2C
		int32_t use_junction = 0;   // dword_B5FD38
		int32_t clear_junction = 0; // dword_B5FD3C
		// The menu form's JUNCTIONCODE (set_junction_code).
		std::string junction_code;
		// The menu's teardown: the response no longer kept and the form gone with the menu; the
		// in-use word stays until the next fresh probe, whose CMissionInfo_Init zeroes it with the
		// other gate fields (CNapiGateManager_WaitForConnect @0x4CE6B0, the call @0x4CE719;
		// CMissionInfo_Init @0x4CE5D0 - @0x4CE62F..0x4CE659) ahead of the reply that sets it.
		// The shell's expansion reload runs it (menu_shell.gd, NovaWorldClient.forget_gate_response),
		// as retail's reload runs the teardown with the flag set.
		// [orig: Menu_TeardownShellAndCloseBinkVideos @0x54E430 - under dword_2551104 (its test
		//  @0x54E491..0x54E4A3),
		//  g_GameMenu destroyed @0x54E4BA..0x54E4C2, dword_B5FF48 = 0 @0x54E4C8;
		//  Game_ReloadExpansionAndMods @0x552710 - the flag @0x552756, the call @0x55275C]
		void forget() {
			response_kept = false;
			junction_code.clear();
		}
	};

	NwuLobbySession(Hooks hooks, Environment env);

	// The gate memory this driver reads and writes (null: its own).
	void set_gate_memory(GateMemory *memory) { shared_memory_ = memory; }

	// The title the session speaks (complete: resolve_title_identity; it outlives the session)
	// and the tag its gate manager holds (title_gate_tag: the title's, or the mount's gt.ssc's):
	// the gate probe's payload, the connection's NA and the 0x42's GateTag CU. Set before
	// probe(); a probe with no title sends nothing and reports on_fatal.
	// [orig: CNapiGateManager_InitDefaults @0x4d1460 (the tag at +76 @0x4d14b6); Game_InitSubsystems
	//  -> Mission_LoadEncryptedConfig @0x4cdcd0; CNapiGateManager_ProbeThreadProc @0x6339e0 (the
	//  tag's bytes @0x633aa1); CNapiGameSession_ConnectToNovaWorld @0x4d48d8 / @0x4d494f]
	void set_title(const TitleIdentity *title, std::string gate_tag) {
		title_ = title;
		gate_tag_ = std::move(gate_tag);
	}
	const TitleIdentity *title() const { return title_; }
	const std::string &gate_tag() const { return gate_tag_; }

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
	void set_junction_code(std::string code) { memory().junction_code = std::move(code); }
	const std::string &junction_code() const { return memory().junction_code; }

	// Start over on the embedder's two bound sockets (non-owning; they outlive close()) and mint
	// the ci/ck pair. The clock restarts at 0 with the first tick().
	void open(IDatagramSocket &gate, IDatagramSocket &session);
	// Send the enveloped gate probe toward `gate_host:gate_port` and start the gate worker's
	// retry/deadline clocks. An unresolvable host sends nothing, so the deadline reports it.
	// While the gate memory keeps a response from this gate, no probe: the kept response goes to
	// on_gate_response and the connect runs from it at once. The process's gate is its init's in
	// retail, so a response is kept for the gate it came from alone (the shell's NovaWorld target
	// can name another gate within one process).
	// [orig: CNapiGateManager_WaitForConnect @0x4CE6B0 - dword_B5FF48 @0x4CE6CF, the gate state 2
	//  @0x4CE6F3, 0 returned @0x4CE6FA; UI_ProcessLANSessionStateMachine @0x558DE0 - state 2
	//  passing over the response's processing for a gate state other than 1 @0x559072..0x559094,
	//  then ConnectToNovaWorld @0x559156; CNapiGameSession_ConnectToNovaWorld @0x4D4640 - its
	//  kept-response test @0x4D46D8; CNapiGateManager_InitDefaults @0x4d1460 - the host
	//  @0x4d149f, the port @0x4d14af]
	void probe(const std::string &gate_host, uint16_t gate_port);
	// The gate service's start where no port of the gate quad bound (the embedder's bind before
	// open): the manager's two lines, then the start failure over the gate context's state, which
	// the start left at 2 (its SetState(-6) is refused), and no probe; the wait's default leg then
	// returns -1, which the menu shows as NWEC15.
	// [orig: CNapiGateManager_WaitForConnect @0x4CE6B0 - @0x4CE84D, @0x4CE88C,
	//  CNapiGateManager_ProcessConnectStateMachine (the call @0x4CE89D), "%s : GATE GATE SERVICE
	//  START FAILED [%ld]." over the context's +0x20 (@0x4CE8C2, the append @0x4CE8E7), the
	//  switch's default -1 (@0x4CEA18, stored @0x4CEA35, returned @0x4CEA41);
	//  CNapiGateManager_ProcessConnectStateMachine @0x633D10 - +0x20 = 2 @0x633E3C,
	//  CNapiGateManager_CreateAndBindSocket (the call @0x633E43), SetState(-6) @0x633E64;
	//  CNapiGateManager_SetState @0x633880 - below -5 refused @0x633886..0x63388B;
	//  UI_ProcessLANSessionStateMachine @0x558DE0 - the result @0x558EAD, NWEC15 @0x559016]
	std::string gate_start_failed();
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
	const GateResponse &gate_response() const { return memory().response; }
	MatchFacts match_facts() const;
	// The connection's CI (1 once a session is made, 0 before) and the CK its last 0x81 drew.
	uint32_t client_index() const { return client_index_; }
	uint32_t client_key() const { return session_ ? session_->client_key() : 0u; }
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
	bool command_line_host() const { return env_.command_line_host; }
	// The player's ESC during the first connect's poll (the NovaWorld menu's states 5..8): the
	// next poll frame that reads the keyboard (the connect poll in session state 1, the verify
	// poll in states 2 and 3) writes its line, gives the connect up and reports NWEC41 / NWEC47
	// through the waits the frame after; the validation wait's pass to the verify poll drops it,
	// and the connection wait reads none. False (nothing taken) when no poll runs. [orig:
	//  CNapiGameSession_PollConnectPhase1 @0x4D4C30 - @0x4D4CDE..0x4D4D31;
	//  CNapiGameSession_PollConnectPhase2 @0x4D4D70 - @0x4D4E25..0x4D4E7D;
	//  CGameSession_WaitForNWUValidation @0x4D0F20 - Input_ResetKeyQueue (the call @0x4D11F7)]
	bool escape();
	// The connect log (Hooks::connect_log_on / connect_log): on now, and one line when on.
	bool connect_log_on() const;
	void connect_log(const std::string &text);

private:
	// The gate manager's configure and start lines ahead of its socket's bind.
	void log_gate_service_start();
	// The session, its stamp and its first datagram (the caller sends it).
	std::vector<uint8_t> begin_session();
	// The connect over the gate memory's response: ConnectToNovaWorld's sessionless return, or
	// its probe targets and the session.
	void connect_from_response();
	GateMemory &memory() { return shared_memory_ != nullptr ? *shared_memory_ : own_memory_; }
	const GateMemory &memory() const {
		return shared_memory_ != nullptr ? *shared_memory_ : own_memory_;
	}
	// One address string into a probe target, as retail's send-to-address does; false (no
	// target) when it names no address.
	bool add_target(const std::string &address);
	// The address of a host as Napi_ResolveAddress reads it (DNS on), 0 when it does not resolve.
	uint32_t resolve_address(const std::string &host) const;
	void send_one(const PeerAddr &to, const std::vector<uint8_t> &dg);
	void poll_gate();
	// The command line's POST address and port over the gate response's.
	void apply_command_line_overrides();
	void poll_session();
	void send_gate_probe();
	bool resolve(const std::string &host, uint16_t port, PeerAddr &out) const;
	// The periodic update's give-up legs, then the first connect's poll.
	void give_up_stalled_connect();
	bool poll_connect();
	// A give-up: the session's abort, its sends out.
	void abort_connect();
	// A connect wait's failure: the session's reset (a connection still up tears down), the
	// code's NWEC to on_fatal, then the connection dropped (session() null from here).
	void fail_connect(int code);
	// The connection's state-5 callbacks' lines (ClientSession::Config::on_connection_up / _down).
	// The down line is composed at the teardown (ahead of the session's reset) and written once
	// the teardown's 0x46 burst has gone out through send() (its SENDING lines first), or at the
	// next close() / connect when the burst never went through this driver.
	void log_connection_up();
	void log_connection_down();
	void write_pending_connection_down();
	// The enumerator's session records: a taken 0x81's record (its sender and SN) with its
	// DISCOVERED line, one already kept for that sender with another SN replaced (its REMOVED line
	// first); the session's drains (ClientSession::enumerator_drains) and the connection's
	// destroy remove each kept record with its line.
	void note_server_hello(const PeerAddr &from);
	// One datagram to the socket, its SENDING line first (send_one's body).
	void send_datagram(const PeerAddr &to, const std::vector<uint8_t> &dg);
	void note_session_drains();
	void drain_records();
	// The gate socket as the gate lines name it: its bind address and port.
	std::string gate_socket_text() const;

	Hooks hooks_;
	Environment env_;
	const TitleIdentity *title_ = nullptr; // set_title
	std::string gate_tag_;
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
	std::vector<Target> targets_; // the enumerator's target list
	PeerAddr nw_peer_;
	// The enumerator's session records and the session drains already read.
	struct SessionRecord {
		PeerAddr from;
		std::string name;
	};
	std::vector<SessionRecord> records_;
	uint32_t drains_seen_ = 0;
	// OnDisconnect's line while its teardown's burst is still to go out, and the burst's sends left.
	std::string pending_down_line_;
	std::size_t pending_down_sends_ = 0;
	// The kept gate state, none of it reset by open() (GateMemory).
	GateMemory own_memory_;
	GateMemory *shared_memory_ = nullptr;
	uint32_t clock_ms_ = 0;
	// The gate worker: the probe re-sent every retry interval until the deadline.
	uint32_t gate_started_ms_ = 0;
	uint32_t gate_last_send_ms_ = 0;
	// The dedicated host's gate wait ended (Environment::command_line_host).
	bool dedicated_wait_over_ = false;
	// The connect log's gate stamp, written only while the log is on (dword_B5F440).
	uint32_t gate_log_stamp_ms_ = 0;
	// The first connect's poll, retail's NovaWorld menu running one state a frame: PollConnectPhase1
	// (5) until it returns 0, the validation wait (6), PollConnectPhase2 (7) until it returns 0, the
	// connection wait (8), the web lobby's open (9); the command-line host's blocking sequence runs
	// each wait at once after its poll's loop and opens no web lobby. Its stamp (dword_B5F444), its
	// timed-out flag (dword_B5F448) and the give-up word the periodic update sets (session+0x12C:
	// 1 the state-1 give-up). The stage datagram is re-sent every retransmit interval while it
	// runs (states 5..8).
	// [orig: UI_ProcessLANSessionStateMachine @0x558DE0 - states 5..9 @0x55919E..0x5592F2;
	//  CNapiGameSession_ConnectAndWaitForValidation @0x4D4EC0 - the loops @0x4D4ED0..0x4D4ED9 /
	//  @0x4D4EE6..0x4D4EEF, the waits @0x4D4EDD / @0x4D4EF3]
	enum class ConnectPoll { Off, Connect, Validation, Verify, Connection, WebLobby, Done };
	// The poll's states 5..8 (the menu's connect states, ahead of its web lobby).
	bool connect_polling() const {
		return connect_poll_ == ConnectPoll::Connect || connect_poll_ == ConnectPoll::Validation ||
				connect_poll_ == ConnectPoll::Verify || connect_poll_ == ConnectPoll::Connection;
	}
	ConnectPoll connect_poll_ = ConnectPoll::Off;
	uint32_t connect_stamp_ms_ = 0;
	bool connect_timed_out_ = false;
	// The connect poll's escape flag (dword_B5F44C) and the player's ESC not yet read by a poll
	// frame (escape()).
	bool connect_escaped_ = false;
	bool escape_requested_ = false;
	int give_up_ = 0;
	uint32_t connect_last_send_ms_ = 0;
};

// The process's one gate memory, for an embedder whose NovaWorld menu entries all share it as
// retail's globals are shared (the game shell's NovaWorldClient; its menu's teardown forgets it).
NwuLobbySession::GateMemory &process_gate_memory();

} // namespace opennova
