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
		// An outbound datagram just shipped on the session socket (trace hook).
		std::function<void(const std::vector<uint8_t> &)> on_sent;
		// One inbound session datagram was handled (trace hook).
		std::function<void(const RxInfo &)> on_received;
		// The session advanced: the owner maps ClientSession::State onto its own states.
		std::function<void()> on_session_state;
		// Fatal: a malformed gate endpoint, a session error, or a connect deadline. The message is
		// the retail NWEC tag when one is witnessed for the failure.
		std::function<void(const std::string &)> on_fatal;
		// Recoverable decode noise on the gate socket (a bad envelope or response).
		std::function<void(const std::string &)> on_soft_error;
	};
	struct Environment {
		// The client index/key draws, the reconnect's fresh CK and the host role's AppId and
		// cookie-key seeds. Unset: make_random_session_u32, the OS CSPRNG (base/os_random).
		std::function<uint32_t()> random_u32;
		// Resolve a gate or UDPNOVAWORLD host ("a.b.c.d" or a name) into `out` (its port left
		// alone); false when it does not resolve. Unset: dotted quads only.
		std::function<bool(const std::string &host, PeerAddr &out)> resolve_ipv4;
		// The configured game.cfg `mpmaxpacketsize` (0 -> 1300): the lobby template's field 13
		// and the 0x42's MaxPacketSize CU. [orig: InitNPConnection @0x4d3df4; ConnectToNovaWorld
		//  @0x4d4913..0x4d493e]
		int32_t max_packet_size = 0;
	};
	// What a match reads of this session: the gate named an NWU address (retail's dword_B5FD2C),
	// and the session's flags word, hosting/playing word and mission-exit store (all 0 once the
	// session is dropped). The first is the gate response's, so it outlives a closed session as
	// retail's global does.
	struct MatchFacts {
		bool in_use = false;
		uint32_t flags = 0;
		int32_t role = 0;
		int32_t exit_reason = 0;
	};

	NwuLobbySession(Hooks hooks, Environment env);

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
	// missing-sequence tail, the connection's send pump, the periodic update's statements, then
	// the gate worker's and the first connect's retransmits and deadlines.
	void tick(uint32_t now_ms);
	// Send one datagram to the NW session endpoint (fires on_sent).
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
	// its connection lives. Everything else is the game's (a joiner's 0x41..0x47, a LAN probe).
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
	const std::string &nw_udp_host() const { return nw_udp_host_; }
	uint16_t nw_udp_port() const { return nw_udp_port_; }
	// The driver's wall clock in ms (the session's GetTickCount).
	uint32_t clock_ms() const { return clock_ms_; }
	uint32_t random_u32() const;

private:
	void begin_session();
	void poll_gate();
	void poll_session();
	void send_gate_probe();
	bool resolve(const std::string &host, uint16_t port, PeerAddr &out) const;
	// The handshake phase for the timeout diagnostic (gate_probing / session_hello /
	// session_join).
	const char *phase_name() const;

	Hooks hooks_;
	Environment env_;
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
	std::string nw_udp_host_; // from the gate's UDPNOVAWORLD
	uint16_t nw_udp_port_ = 0;
	PeerAddr nw_peer_;
	GateResponse gate_response_;
	uint32_t clock_ms_ = 0;
	// The gate worker: the probe re-sent every retry interval until the deadline.
	uint32_t gate_started_ms_ = 0;
	uint32_t gate_last_send_ms_ = 0;
	// The first connect's legs: the stage datagram re-sent every retransmit interval until the
	// session's connect deadline.
	uint32_t connect_started_ms_ = 0;
	uint32_t connect_last_send_ms_ = 0;
	bool connect_reported_ = false;
};

} // namespace opennova
