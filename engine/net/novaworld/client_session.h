#pragma once

#include <net/napi/session.h>       // ClientVar / ServerCommand / the NWEC maps
#include <net/napi/tlv.h>           // NapiMessage (lobby container shape)
#include <net/novaworld/lobby_vars.h> // HostRegistration / HostPlayerSlot
#include <net/npwire/cs_config.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_hello.h>

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace opennova {

// Godot-free client mirror of LobbySession: the bytes-in / bytes-out state
// machine that drives the NovaWorld *session* UDP channel from the client
// side. The gate-probe leg (which yields the session host:port) lives in the
// caller; ClientSession starts at ClientHello and runs the handshake through
// the lobby verify exchange, then the host-registration and start-playing
// legs and the server notifications:
//
//   start()                       -> ClientHello (0x41)
//   <- ServerHello (0x81)            : learn host key `hk`
//   ClientAuth (0x42)                : hk echoed, ck = client key, client scrk
//   <- ServerAuth (0x82)             : learn server key `sk` + server scrk (cr==1) + the CS block
//   process_periodic_update() queues ClientConnected: begin lobby verification
//   <- ServerStartVerify (0x83)
//   ClientRequestVerifyResult (queued)
//   <- ServerVerifyResult (0x83)     : Success=1 -> Verified (lobby-ready, retail state 4)
//   request_hosting() / request_playing() -> the host (5->6) / play (7->8) legs
//   <- ServerHostResult / ServerPlayResult / ServerPlayerEnterResult / ServerGLSVSSResults
//   <- ServerStopHosting / ServerStopPlaying / ServerLeaveNovaWorld / ServerCommand (notices)
//   <- ServerGoodBye (0x86) / the H:0x03 description record : the peer's disconnect
//
// The lobby connection is an ordinary type-2 NAPI connection: every statement is queued on it
// (QueueMessage, retained until the peer ACKs its packet), and the send pump builds the queue
// into packets no larger than CS field 13, splitting a long statement into FRAG records; the
// receive side admits only the next in-order packet, holds later ones and asks for the gap with
// a 0x44, and answers the peer's 0x84 from the retained records.
// [orig: every CNapiGameSession_Send* -> CNapiNPConnection_QueueMessage(conn, 0, 0, 0, 0, 0,
//  payload, len, 1300) @0x628640 (e.g. SendHostRequest @0x4d3834, SendHostUpdate @0x4d3902) ->
//  NapiNPMessage_Create @0x627fc0; CNapiGameSession_ProcessPeriodicUpdate @0x4d4400 ->
//  NapiNPProtocol_Pump(proto, -1, 250) @0x4d442e; CNapiGameSession_InitNPConnection @0x4d3be0]
//
// No socket I/O lives here (engine convention: portable C++ in engine/, Godot
// wrapper in godot/). The caller owns the UDP socket and pumps bytes, once per tick in
// retail's order: feed every received datagram into handle_datagram() (sending what it returns
// at once), then finish_receive_batch(), then pump(), then process_periodic_update(); set the
// wall clock (set_clock_ms) first — the negotiated intervals and the receive-silence reap are
// millisecond windows. Each leg is unit-testable via
// in-process loopback against apps/novaworld_server's own parsers/builders —
// see docs/adr/0010-novaworld-client-completion.md.
//
// [mirror: apps/novaworld_server/nw_udp_listener.cpp — the server direction
//  of the same wire protocol; this class inverts request <-> response]
class ClientSession {
public:
	enum class State {
		Idle,       // nothing sent yet
		Hello,      // ClientHello sent, awaiting ServerHello
		Auth,       // ClientAuth sent, awaiting ServerAuth
		Verifying,  // ServerAuth accepted; lobby verify handshake in flight
		Verified,   // ServerVerifyResult(Success=1) — lobby-ready
		Closed,     // GoodBye sent, or the peer closed / punted / reaped us (see disconnect_event)
		Error,      // protocol error or server rejection
	};

	// The host-registration leg [orig: CGameSession_SetState @0x4ce140 states 4/5/6].
	enum class HostState {
		Idle,        // state 4: verified, not hosting
		Requested,   // state 5: ClientHostRequest sent, awaiting ServerHostResult
		Established, // state 6: registered (HostCommands consumed)
		Failed,      // back to state 4 with a MsgCode (host_result())
	};
	// The start-playing leg [orig: states 4/7/8].
	enum class PlayState {
		Idle,
		Requested,   // state 7: ClientPlayRequest sent, awaiting ServerPlayResult
		Playing,     // state 8
		Failed,      // back to state 4 with a MsgCode (play_result())
	};

	struct Config {
		uint32_t client_index = 1;   // ci — caller-chosen (binding: random)
		uint32_t client_key = 1;     // ck — caller-chosen (binding: random)
		std::string na = "jop:cus2"; // gate/game tag echoed in ClientAuth NA

		// ClientHello identity. The real NovaWorld server VALIDATES four fields
		// in HandleClientHello @ 0x6213B0 and silently drops the ClientHello
		// (no ServerHello -> client times out) unless they match exactly:
		//   NVS  == the Milota NAPI-protocol version string (it is the protocol
		//           version, NOT client identity),
		//   PN   == the server game id "NOVAWORLDUDP",
		//   PV1  == "0.0.0 2/10/2004 EM",
		//   PG   == the 16-byte protocol GUID (added in build_client_hello).
		// CO/AP/BDAT are read but never validated, so they stay our identity.
		// All four constants are from CNapiGameSession_InitNPConnection @ 0x4d3be0.
		std::string nvs  = "NAPI NP Version 0.0.1 1/12/2004 - 2/20/2004 Milota Copyright 2004 NovaLogic";
		std::string co   = "OpenNova";
		std::string ap   = "OpennovaGodotClient.exe";
		std::string bdat = "Jul 21 2009 18:54:41";  // retail BDAT (free field)
		std::string pn   = "NOVAWORLDUDP";           // validated == server game id
		std::string pv1  = "0.0.0 2/10/2004 EM";     // validated == server PV1
		std::string pv2  = "1";

		// The 16-byte protocol GUID emitted as PG in the hello and the auth. By
		// default (use_default_pg) the builder picks the GUID for `pn`:
		// NOVAWORLDUDP -> the lobby GUID, JointOperations -> the game GUID. Set
		// `pg` + use_default_pg=false to override with explicit bytes.
		std::array<uint8_t, 16> pg{};
		bool use_default_pg = true;

		// CU chunks to emit in the ClientAuth (NW-S3). The retail client sends
		// a named var set (Application/BuildDateAndTime/Debug/CountryName/
		// Language/TimeZoneBias/GateTag/MetTag/UdpCode1/UdpCode2/MaxPacketSize)
		// from CNapiGameSession_ConnectToNovaWorld @ 0x4d4640; UdpCode1/UdpCode2
		// are the gate-issued session-auth codes (gate VARs UDPCODE1/UDPCODE2)
		// that live NW's join callbacks validate. Empty by default (the OpenNova
		// server doesn't require them); the binding populates it from the gate
		// response + client env when targeting live NW. Each is emitted as a
		// CU flat-TLV whose value is make_client_cu_chunk(type, name, value).
		struct CuVar {
			std::string name;
			std::string value;
			uint8_t type = 1;  // 1 or 2 (HandleClientJoin's CU loop accepts both)
		};
		std::vector<CuVar> cu_vars;

		// The "Cookie" var-list (NW-S5, witnessed byte-for-byte in the genuine
		// .204 capture, fixtures/novaworld/nw204_lobby.hexcap frame 10166): the
		// browser's cookie jar for the NovaWorld host plus the locale trio, which
		// CNapiSession_ReadLocaleInfo @ 0x4ce390 rebuilds into session+388 before
		// EVERY statement that serializes it — the verify reply, the host
		// request, the play request and the GLSVSS request. The gate response
		// seeds locale/expansion fields; OnNovaWorldConnected @ 0x4d1570 adds NWUID
		// and the machine fields. Each (name, value) becomes a ClientVar{VarFNum="0",
		// VarName,VarValue} child of a ClientVarList(VarList="Cookie"). The retail
		// set (in order) is CountryName, Language, TimeZoneBias, MyInstalledExpBits,
		// NWUID, NWCDKIID, NWCDKIIDEXP1, NWPSSK, NWUSID, NWHWI — and on the wire
		// NWCDKIID/NWCDKIIDEXP1 are EMPTY yet the live server still returns
		// Success=1, so the lobby verify is NOT credential-gated. Empty here still
		// emits an empty Cookie parent. The entry named "NWUID" with an empty
		// value is filled at runtime from the ServerSessionInit's NWUID (echo).
		// Called at serialization time so HTTP login/NWJoin updates are included.
		std::function<std::vector<std::pair<std::string, std::string>>()> cookie_vars;

		// The gate's GLSVSS trio: the request string (GLSVSSREQUEST; empty = the leg is
		// off), the repeat interval (GLSVSSRIMS, ms) and the after-game interval
		// (GLSVSSAGRMS, ms) that arms the deadline when the session returns to state 4
		// from Playing. [orig: CGameSession_SetState @0x4ce140 case 4 (byte_B5FD40 /
		//  dword_B5FF40 / dword_B5FF44); ProcessPeriodicUpdate @0x4d4400]
		std::string glsvss_request;
		int32_t glsvss_rims_ms = 0;
		int32_t glsvss_agrms_ms = 0;

		// The configured game.cfg `mpmaxpacketsize` (0 -> 1300): the lobby template's CS field 13,
		// the ceiling every built packet fits, until the peer's 0x82 overlays it. The 0x42's
		// MaxPacketSize CU carries the same configured value (NovaWorldJoinCu::max_packet_size).
		// [orig: CNapiGameSession_InitNPConnection @0x4d3df4..0x4d3eb8]
		int32_t max_packet_size = 0;

		// The fresh CK and SCRK a reconnect's re-join mints. Unset, the engine's own random
		// draws (make_random_session_u32 / make_dev_scrk) stand; the owner injects its
		// generator, a test a deterministic one.
		// [orig: CNapiNPConnection_InitFromSession @0x626320 — NapiNP_GenerateSessionKey
		//  @0x6263fb (CK), CNapiNPConnection_GenerateTxKey @0x626401 (SCRK)]
		std::function<uint32_t()> next_client_key;
		std::function<std::string()> next_scrk;

		// Preset for the in-match game session: the ClientHello the client sends
		// to a host after NWJoin, which flips the connection protocol from the
		// lobby (NOVAWORLDUDP) to the game (JOINTOPERATIONS). The complete
		// CO/AP/BDAT/PN/PG/PV1/PV2 identity is the retail CNapiNetwork_Init
		// @ 0x4ca4a0 block, independently witnessed in the LAN ClientAuth capture.
		static Config jointoperations();
	};

	// A server notification the owner acts on (state changes are already applied).
	struct PlayerEnterResult {
		uint32_t connection_id = 0;
		int success = 0;
		int msg_code = 0;
		std::string player_ticket;    // 128-char cap
		std::string access_code_list; // 8192-char cap
	};
	struct Notice {
		enum class Kind {
			HostResult,        // ServerHostResult landed (host_state() / host_result())
			PlayResult,        // ServerPlayResult landed (play_state() / play_result())
			StopHosting,       // ServerStopHosting: fields + msg_key, host leg back to Idle
			StopPlaying,       // ServerStopPlaying: fields, play leg back to Idle, exit reason 12
			LeaveNovaWorld,    // ServerLeaveNovaWorld: the punt; the session is Closed
			Command,           // ServerCommand: `command`
			GlsvssResults,     // ServerGLSVSSResults: `glsvss_results`
			PlayerEnterResult, // ServerPlayerEnterResult: `player_enter`
			// A reconnect's re-verify found the word hosting / playing and re-sent the
			// request itself (ClientHostRequest CurrentlyHosting=1 with ReconnectCounter
			// counted up / ClientPlayRequest CurrentlyPlaying=1); the reply lands as a
			// HostResult / PlayResult. [orig: HandleConnectVerifyResponse @0x4d5961..0x4d599e]
			Rehost,
			Replay,
		};
		Kind kind = Kind::HostResult;
		ServerResultFields fields;
		std::string msg_key;          // StopHosting: novaworld_server_msg_code_key(MsgCode)
		ServerCommand command;
		std::string glsvss_results;
		PlayerEnterResult player_enter;
	};

	// Two constructors rather than a `Config config = {}` default argument:
	// GCC/clang reject `= {}` for this aggregate-with-NSDMIs (MSVC accepts it),
	// which broke the Linux/macOS CI builds.
	ClientSession();
	explicit ClientSession(Config config);

	// The owner's wall clock in milliseconds (GetTickCount in retail). Set it before
	// pump()/process_periodic_update(); every framed send and every admitted datagram is
	// stamped with the latest value.
	void set_clock_ms(uint32_t now_ms) { clock_ms_ = now_ms; }

	// Begin the handshake. Returns the ClientHello datagram to send (envelope
	// + NWU already applied — ready for the wire). Transitions Idle -> Hello.
	std::vector<uint8_t> start();

	// The datagram the current connect stage re-sends on the retransmit cadence
	// (SESSION_CONNECT_RETRANSMIT_MS): the ClientHello while awaiting the ServerHello,
	// the ClientAuth while awaiting the ServerAuth; empty in every other state.
	// [orig: CNapiNPConnection_PumpEnumeratorAndSend @0x6290c0 (the 0x41 re-announce);
	//  CNapiNPConnection_PumpStateMachine @0x6292e0 case 3 (the 0x42 re-send)]
	std::vector<uint8_t> retransmit_stage_datagram();

	// Feed one inbound datagram (received off the UDP socket, CRC envelope intact). Appends the
	// datagrams retail sends from inside its receive pump to `out` — the 0x42 answering a
	// ServerHello, a teardown's 0x46 burst, the packets a 0x84 asks for — to send at once;
	// statements a dispatched container answers with are queued for the next pump(). Returns
	// false on a protocol error or a server rejection (state becomes Error; see last_error()).
	// A datagram that fails the envelope/NWU decode is TOSSED (counted, logged) without
	// touching the session, as retail's receive pump does; one that isn't expected in the
	// current state is ignored.
	bool handle_datagram(const uint8_t *data, size_t len,
	                     std::vector<std::vector<uint8_t>> &out);

	// The receive pump's tail, after the tick's datagrams went through handle_datagram(): when
	// a packet arrived ahead of a gap and one is still held, the missing-sequence list goes out
	// at once (a 0x44 carrying the server's key) and the latch clears.
	// [orig: NapiNPProtocol_PumpRecvQueues @0x6266a0 — the latch test @0x6269bb, the queue
	//  test @0x6269c8, CNapiNPConnection_SendMissingSeqList(conn, 0) @0x6269ce, the clear
	//  @0x6269d6]
	void finish_receive_batch(std::vector<std::vector<uint8_t>> &out);

	// The connection's send pump over the negotiated CS values, once per tick after the receive
	// batch: once the NP connection is up (Verifying/Verified) the queue — and a pending ACK —
	// builds into packets (split at CS field 13, at most field 14 of them); the reap fires after
	// timeout_ms of receive silence (a latched CLNTTMOUT record, the 0x46 burst, state Closed);
	// then the send-interval legs: with records still retained, a header-only packet once
	// active_send_interval_ms passed since the last send, the timed missing-sequence list when
	// packet_queue_interval_ms is set, and the empty keepalive once idle_send_interval_ms passed
	// with nothing queued, retained or held. Once torn down with the hosting/playing word set,
	// the same pump runs the reconnect: the 0x41 re-probe windows and the re-join's 0x42 leg
	// (see reconnecting()).
	// [orig: CNapiNPConnection_PumpFlags @0x629780 -> PumpEnumeratorAndSend @0x6290c0 (the send
	//  leg @0x629279..0x6292bb -> BuildOutgoingPackets @0x628430), then PumpStateMachine
	//  @0x6292e0 (case 3 @0x629508, case 5 @0x6295a2..0x62961c, the reconnect tail
	//  @0x629487..0x6296cb) -> PumpSendIntervals @0x628fd0; the flush counter @0x6297d5]
	void pump(std::vector<std::vector<uint8_t>> &out);

	// Run one session-periodic update after the protocol pump. Once ServerSessionInit has
	// established the NP connection and moved the session to Verifying, the first update
	// queues ClientConnected; later updates do not repeat it. Once Verified, the GLSVSS deadline
	// is polled here at most once per SESSION_GLSVSS_POLL_MS. What it queues builds on the next
	// pump(). [orig: CNapiGameSession_ProcessPeriodicUpdate @0x4d4400 — the protocol pump
	//  @0x4d442e first, then the ClientConnected gate @0x4d444e..0x4d445b and the GLSVSS poll
	//  @0x4d44cb..0x4d4532]
	void process_periodic_update();

	// Build a ClientGoodBye (0x46) datagram and mark the session Closed. Carries the
	// latched disconnect record when the peer/reap set one, else the zero record.
	std::vector<uint8_t> build_goodbye();

	// Queue one lobby container (e.g. a ClientHostUpdate built via napi/session.h) on the
	// connection; it rides the next pump(). False unless Verified.
	// [orig: CNapiNPConnection_QueueMessage @0x628640 — connection state 1 or 5, else -1]
	bool queue_statement(const NapiMessage &container);
	// Statements queued and not yet built, and records sent and not yet ACKed.
	size_t queued_statement_count() const { return send_queue_.size(); }
	size_t retained_record_count() const { return seq_.retained_outbound_message_count; }

	// ---- the host leg -------------------------------------------------------
	// ClientHostRequest (state 4 -> 5); the Cookie is the verify set with NWUID filled. False
	// when the session is not in state 4 or the request could not be queued (state back to 4).
	// [orig: CNapiGameSession_StartHostingSession @0x4d4540 -> SendHostRequest @0x4d3700]
	bool request_hosting(const HostRegistration &cfg, int currently_hosting);
	// ClientHostUpdate with the given Host and PlayerList vars (full lists or dirty deltas).
	bool send_host_update(const std::vector<ClientVar> &host,
	                      const std::vector<ClientVar> &player_list);
	// ClientHostPlayerAdded / ClientHostPlayerRemoved — queued only while Established (state 6).
	// [orig: the state-6 wrappers @0x4d0e20 / @0x4d0e40]
	bool send_host_player_added(const HostPlayerSlot &player);
	bool send_host_player_removed(int player_number);
	// ClientPlayerEnterRequest for a joiner entering the hosted game
	// [orig: CNapiGameSession_SendPlayEnterRequest @0x4d02a0 — itself ungated;
	// retail's only gate is its caller's NovaWorld arm, `byte_B60100 & 0x40`
	// @0x4c8b88 in CNapiNetwork_CheckPlayerTimeouts @0x4c8ad0]. Queued only once
	// hosting is established: our shell-side policy (the join tickets are armed
	// only once registered), not a witness.
	bool send_player_enter_request(uint32_t connection_id, uint32_t ip_address,
	                               uint32_t port_number, const std::string &join_ticket);
	// ClientStopHosting (states 5/6 -> 4); true when the statement was queued.
	// [orig: CGameSession_StopHosting @0x4d0e60]
	bool stop_hosting();
	HostState host_state() const { return host_state_; }
	const ServerResultFields &host_result() const { return host_result_; }
	// The GSID the successful ServerHostResult's HostCommands carried (128-char cap): the
	// value the in-match host publishes as its 0x81 SUS1. Empty until Established, and again
	// from a connection teardown until a re-host's ServerHostResult re-supplies it.
	// [orig: HandleHostVerifyResponse @0x4d59d0 @0x4d5bd6..0x4d5c0f -> server_user_string1;
	//  CNapiGameSession_OnDisconnect @0x4cfc3b..0x4cfc54 clears both]
	const std::string &host_gsid() const { return host_gsid_; }
	int host_requires_join_ticket() const { return host_requires_join_ticket_; }

	// ---- the play leg -------------------------------------------------------
	// ClientPlayRequest (state 4 -> 7) with the PlaySetup vars (make_play_setup_vars); the
	// session keeps them for a reconnect's replay. `currently_playing` is the reconnect flag
	// (0 on the fresh path). False when the session is not in state 4 or the request could not
	// be queued (state back to 4). [orig: CNapiGameSession_StartPlayingSession @0x4d45e0 ->
	//  SendPlayRequest @0x4d3920; g_SessionConnectVarList filled by ConnectOrHost
	//  @0x4d53bb..0x4d542b]
	bool request_playing(const std::vector<ClientVar> &play_setup, int currently_playing = 0);
	// ClientStopPlaying (states 7/8 -> 4); true when the statement was queued.
	// [orig: CGameSession_StopPlaying @0x4d0ec0]
	bool stop_playing();
	PlayState play_state() const { return play_state_; }
	const ServerResultFields &play_result() const { return play_result_; }

	// ---- notifications ------------------------------------------------------
	// Notices accumulated by handle_datagram, oldest first; the call drains them.
	std::vector<Notice> take_notices();
	// g_MissionExitReason = 12 once a ServerStopPlaying / ServerLeaveNovaWorld landed.
	int mission_exit_reason() const { return mission_exit_reason_; }

	// ---- the session words the match reads ----------------------------------
	// The capability flags word (session+0x120; its low byte is byte_B60100), kept by the
	// state setter's per-state clear/set table: bit 1 in state 1, bit 2 in states 2..8, bits 2
	// and 8 together in states 4..8 (the HUD's NovaWorld N tests both). A peer close, the reap
	// or a punt drops the state to 0 and clears it.
	// [orig: CGameSession_SetState @0x4ce140 — the clears @0x4ce183..0x4ce1bc, the sets
	//  @0x4ce1f3..0x4ce288; CNapiGameSession_OnDisconnect @0x4cfaa0 -> SetState(0) @0x4cfb68]
	uint32_t session_flags() const { return session_flags_; }
	// The hosting/playing word (session+0x128, dword_B60108). Only the verify replies, the stop
	// legs, the punt and the local reset write it; a peer close or the reap leaves it as it was.
	// [orig: the writers HandleConnectVerifyResponse @0x4d5800, HandleHostVerifyResponse
	//  @0x4d59d0, HandleVerifyResponse @0x4d1e00, HandleServerMessage @0x4d1c50,
	//  HandleServerDisconnectMsg @0x4d1fa0, HandlePuntNotification @0x4d20b0,
	//  CGameSession_StopHosting @0x4d0e60, CGameSession_StopPlaying @0x4d0ec0,
	//  ResetToDisconnected @0x4d0890; the reader Game_ProcessMainFrame @0x52656d]
	static constexpr int32_t kSessionRoleNone = 0;
	static constexpr int32_t kSessionRoleVerified = 1;
	static constexpr int32_t kSessionRoleHosting = 2;
	static constexpr int32_t kSessionRolePlaying = 3;
	int32_t session_role() const { return session_role_; }

	// ---- the reconnect ------------------------------------------------------
	// Armed while the hosting/playing word is nonzero (the NP connection's +0x710 mirror of
	// every word write). [orig: the word writers above; CNapiNPConnection_PumpStateMachine
	//  @0x6294a5 reads it]
	bool reconnect_armed() const { return session_role_ != kSessionRoleNone; }
	// Torn down (the peer's goodbye or description record, the reap) while armed and not yet
	// back up: the session owns the 0x41 re-probe and the 0x42 re-join from here (the owner's
	// first-connect retransmits and deadlines stand down; retransmit_stage_datagram() is
	// empty) until the re-join's 0x82 lands. [orig: the reconnect tail's guard
	//  @0x629487..0x6294a5 — !conn_flag0 && conn_flag1 && is_client && +0x710]
	bool reconnecting() const;
	// The connection's counted disconnects (DCNT, +0x734) and the last 0x82's reconnect count
	// (RCNT, +0x738): both ride the re-join's 0x42.
	uint32_t disconnect_count() const { return dcnt_; }
	uint32_t reconnect_count() const { return rcnt_; }
	// The HostSetup ReconnectCounter (session+0x500): 0 on a fresh host request, one up per
	// re-host.
	int host_reconnect_counter() const { return reconnect_counter_; }
	// The teardown's disconnect-packet burst: CS recv_max_per_tick clamped to 0..32, the first
	// send always going. [orig: TeardownActiveConnection @0x6253ef..0x625424]
	size_t disconnect_burst_count() const {
		return opennova::disconnect_burst_count(cs_.recv_max_per_tick);
	}
	// The peer's / reap's latched disconnect record once the session Closed on it.
	bool disconnected_by_peer() const { return disconnected_by_peer_; }
	const DisconnectEvent &disconnect_event() const { return disconnect_event_; }
	// Datagrams tossed at the envelope/NWU layer (retail's "NAPI TOSSED" log line).
	uint32_t tossed_datagrams() const { return tossed_datagrams_; }

	State state() const { return state_; }
	bool is_verified() const { return state_ == State::Verified; }

	uint32_t client_index() const { return cfg_.client_index; }
	uint32_t client_key() const { return cfg_.client_key; }
	uint32_t server_host_key() const { return server_hk_; }
	uint32_t server_key() const { return server_sk_; }
	const std::string &client_scrk() const { return client_scrk_; }
	const std::string &server_scrk() const { return server_scrk_; }
	// The NovaworldWebDomainNameAndPortNumber CU delivered in the ServerSessionInit
	// (0x82), e.g. "207.178.209.204:80". On live NW the gate's startupurl carries a
	// "[domainname]" placeholder; this is the real web host the client substitutes
	// for the HTTP login/GSB/join legs. Empty until the SessionInit is parsed; a connection
	// teardown resets it to "???" and clears the NWUID until the next 0x82 re-supplies them
	// [orig: CNapiGameSession_OnDisconnect @0x4cfb8e / @0x4cfbe3].
	const std::string &server_web_domain() const { return server_web_domain_; }
	const std::string &server_nwuid() const { return server_nwuid_; }
	const std::string &sess_id_string() const { return sess_id_string_; }
	const std::string &last_error() const { return last_error_; }
	// The connection's cs_dir0 block: the NOVAWORLDUDP template overlaid by the 0x82 and the
	// H:0x00 CS updates.
	const CsConfig &connection_settings() const { return cs_; }
	// The verify Cookie set with the SessionInit NWUID substituted — what every
	// Cookie-bearing statement serializes.
	std::vector<ClientVar> cookie_vars() const;

private:
	std::vector<uint8_t> build_client_hello();
	std::vector<uint8_t> build_client_auth();
	// QueueMessage: one lobby container as a layer-4 record (tag 0) on the outgoing queue, retained
	// until ACK once built. False unless the NP connection is up (Verifying/Verified).
	bool queue_lobby_record(const NapiMessage &container);
	// The ClientRequestVerifyResult: a SessIdString field plus the "Cookie" var-list (NW-S5,
	// NWUID echoed from the ServerSessionInit) — rebuilt on EVERY ServerStartVerify. Bare when no
	// cookie vars are configured.
	bool queue_verify_request();
	bool queue_glsvss_request();
	// BuildOutgoingPackets over the queue and a pending ACK, then the boundary prune.
	void build_outgoing_packets(std::vector<std::vector<uint8_t>> &out);
	// The 0x84: rebuild each requested packet from its retained records and send it at once.
	void on_server_resend_list(const std::vector<uint8_t> &body,
	                           std::vector<std::vector<uint8_t>> &out);
	// The 0x44 carrying the server's key and the missing sequences.
	void send_missing_sequence_list(std::vector<std::vector<uint8_t>> &out);
	// Sequencing for a fresh NP connection: seq 1, the ordered receive gate, and the queue and
	// pool bounds from cs_ (fields 10/11).
	void reset_sequencing();

	void on_server_hello(const std::vector<uint8_t> &body,
	                     std::vector<std::vector<uint8_t>> &out);
	void on_server_auth(const std::vector<uint8_t> &body);
	void on_server_goodbye(const std::vector<uint8_t> &body,
	                       std::vector<std::vector<uint8_t>> &out);
	void on_server_protocol_message(const std::vector<uint8_t> &body,
	                                std::vector<std::vector<uint8_t>> &out);
	void dispatch_server_container(const NapiMessage &container,
	                               std::vector<std::vector<uint8_t>> &out);
	void apply_cs_config_update(const ProtocolMessage &pm);
	void latch_disconnect(const DisconnectEvent &event);
	// CNapiNPConnection_TeardownActiveConnection over a connected connection: DCNT counted,
	// the 0x46 burst echoing the latched record, the disconnect callback, and the reconnect
	// schedule restarted.
	void teardown_connection(std::vector<std::vector<uint8_t>> &out);
	// The connection's teardown callback: Closed, the session state back to 0, and the
	// session strings the callback clears.
	void on_disconnected();
	// The reconnect's halves of the pump: the enumerator's 0x41 re-announce, the 0x42 leg
	// (case 3) and the schedule tail.
	void pump_reconnect(std::vector<std::vector<uint8_t>> &out);
	void grow_reconnect_gap();
	// The host and play legs' requests over the session-owned lists. [orig:
	//  CNapiGameSession_StartHostingSession @0x4d4540 / StartPlayingSession @0x4d45e0]
	bool send_host_request(int currently_hosting);
	bool send_play_request(int currently_playing);
	void set_lobby_state(int state); // the CGameSession_SetState mirror (flags, GLSVSS arming)
	void fail(std::string reason);

	Config cfg_;
	State state_ = State::Idle;
	HostState host_state_ = HostState::Idle;
	PlayState play_state_ = PlayState::Idle;

	uint32_t server_hk_ = 0;       // ServerHello.hk — echoed in ClientAuth
	uint32_t server_sk_ = 0;       // ServerAuth.sk — session_id on our 0x43s
	std::string client_scrk_;      // we generate; encrypts our 0x43 inner stream
	std::string server_scrk_;      // ServerAuth.scrk; decrypts inbound 0x83 inner
	std::string server_nwuid_;     // ServerSessionInit NWUID — echoed in the verify
	std::string server_web_domain_;// ServerSessionInit NovaworldWebDomainNameAndPortNumber
	std::string sess_id_string_;   // ServerVerifyResult.SessIdString
	std::string last_error_;
	CsConfig cs_ = novaworld_service_cs_config();

	// The host / play legs' last results and the two HostCommands values the
	// host leg keeps.
	ServerResultFields host_result_;
	ServerResultFields play_result_;
	std::string host_gsid_;
	int host_requires_join_ticket_ = 0;
	std::vector<Notice> notices_;
	int mission_exit_reason_ = 0;
	DisconnectEvent disconnect_event_;
	bool disconnect_latched_ = false;
	bool disconnected_by_peer_ = false;
	uint32_t tossed_datagrams_ = 0;

	// The clocks (all in the owner's ms clock).
	uint32_t clock_ms_ = 0;
	uint32_t last_framed_send_ms_ = 0;   // conn->last_send_interval_tick
	uint32_t last_receive_ms_ = 0;       // conn->_pad4[20], refreshed per admitted datagram
	bool receive_clock_armed_ = false;   // state-5 entry (the accepted 0x82)
	uint32_t glsvss_deadline_ms_ = 0;    // session+579 (0 = disarmed)
	uint32_t glsvss_poll_ms_ = 0;        // session+578
	int lobby_state_ = 0;                // the CGameSession state (session+0x11C)
	uint32_t session_flags_ = 0;         // session+0x120
	int32_t session_role_ = kSessionRoleNone; // session+0x128

	// The session-owned var lists the host and play legs serialize: HostSetup (session+0x1CC),
	// Host (+0x214), PlayerList (+0x25C) and the play leg's PlaySetup
	// (g_SessionConnectVarList). A reconnect's re-request re-sends them whole.
	// [orig: BuildHostVarLists @0x4d0b50; Server_PlayerAdd @0x51d421..0x51d4aa (the PlayerList
	//  SetOrCreates); ConnectOrHost @0x4d53bb..0x4d542b (PlaySetup)]
	std::vector<ClientVar> host_setup_;
	std::vector<ClientVar> host_list_;
	std::vector<ClientVar> player_list_;
	std::vector<ClientVar> play_setup_;
	int reconnect_counter_ = 0;          // session+0x500

	// The NP connection's reconnect state. [orig: NapiNPConnection — conn_flag1 +0x29, the
	//  enumerator's timer_active / timer_start_tick / last_send_tick, +0x72C gap, +0x730 next,
	//  +0x5E0 the state-3 entry, +0x5E4 the last 0x42]
	struct Reconnect {
		bool was_connected = false;      // conn_flag1
		bool probing = false;            // enumerator timer_active
		uint32_t window_start_ms = 0;    // enumerator timer_start_tick
		bool probe_sent = false;         // enumerator last_send_tick != 0
		uint32_t last_probe_ms = 0;      // enumerator last_send_tick
		uint32_t gap_ms = SESSION_RECONNECT_GAP_INITIAL_MS; // +0x72C
		uint32_t next_ms = 0;            // +0x730
		uint32_t join_started_ms = 0;    // +0x5E0
		uint32_t join_last_send_ms = 0;  // +0x5E4
	};
	Reconnect reconnect_;
	uint32_t dcnt_ = 0;                  // conn+0x734
	uint32_t rcnt_ = 0;                  // conn+0x738

	// The 0-default is deliberately preserved (start() resets it to 1 — see Risk #1 / capture frame 9739).
	SessionSequencing seq_{0, 0}; // outbound seq + last inbound ack [ADR 0013 shared framing]
	// The connection's outgoing message queue (conn+0x750, its count +0x758) and the pending-ACK
	// flag a packet with records sets (has_pending_out, conn+0x650).
	// [orig: ParseMessages @0x625dff; BuildOutgoingPackets @0x62847e (the header-only ACK
	//  packet), @0x62861f..0x628629 (cleared once the queue is empty)]
	std::vector<ProtocolMessage> send_queue_;
	bool ack_pending_ = false;
	// The last admitted packet or timed missing-sequence send (conn+0x648), the timed leg's clock.
	uint32_t last_recv_activity_ms_ = 0;
	bool sent_client_connected_ = false;
	ProtocolReassemblyState reassembly_;
};

// Shared identity-message builders — the ClientHello / ClientAuth struct-fill that BOTH the lobby
// ClientSession (above) and the in-match inmatch::JoinerConnection (engine/runtime/inmatch) use. The two builders
// differ ONLY in the CO source (lobby company vs the joiner's player name) and which envelope frames
// the bytes (both directions now share npwire's nw_encode_outbound / nw_decode_inbound), so the
// struct-fill is dedup'd here.
// [orig: one CNapiNPConnection identity block — CNapiNPConnection_SendClientJoin @0x61fe20 sources it
// from the same protocol config for both the 0x41 hello and the 0x42 join; the lobby/game paths split
// AFTER 0x82, not in the identity emit.]
ClientHello make_client_hello(const ClientSession::Config &cfg, std::string_view co);
ClientAuth make_client_auth(const ClientSession::Config &cfg, std::string_view co, uint32_t server_hk,
                            std::string_view client_scrk);

// The gate-sourced inputs to the 0x42-join CU var set. Both the join (client)
// and the host-registration (host) connection carry this same set — retail
// builds it once in CNapiGameSession_ConnectToNovaWorld, which serves both
// directions. `gate_tag` is the const protocol/gate tag (our ClientAuth `na`,
// e.g. "jop:cus2" — stru_B5FF50.protocol); the rest come straight from the
// gate response (ProcessResponse @ 0x4ced20) and are empty against the
// permissive OpenNova gate.
struct NovaWorldJoinCu {
	std::string application = "OpennovaGodotClient.exe";  // exe basename
	std::string gate_tag;        // GateTag  <- stru_B5FF50.protocol (== na)
	std::string met_tag;         // MetTag   <- gate VAR METLABEL  (byte_B5F4BC)
	std::string udp_code1;       // UdpCode1 <- gate VAR UDPCODE1  (byte_B5F8E8)
	std::string udp_code2;       // UdpCode2 <- gate VAR UDPCODE2  (byte_B5F908)
	// MaxPacketSize: the game.cfg `mpmaxpacketsize` value printed raw with "%ld"
	// (no clamp; the stock value is 1300) — the same configured value a host
	// advertises as CS field 13 (inmatch::GameConfig::max_packet_size).
	// [orig: CNapiGameSession_ConnectToNovaWorld @0x4d4913..0x4d493e — sprintf of
	//  g_GameConfigState.maxPacketSize_338 -> CU "MaxPacketSize"]
	int32_t max_packet_size = 1300;
};

// Build the 0x42-join CU var set exactly as
// [orig: CNapiGameSession_ConnectToNovaWorld @ 0x4d4640] does: the session+316
// var list (CNapiVarList_SetOrCreate @ 0x6318c0), emitted as CU chunks by
// CNapiNPConnection_SendClientJoin @ 0x61fe20. Retail order is fixed: Application,
// BuildDateAndTime, Debug, CountryName, Language, TimeZoneBias, GateTag, MetTag,
// UdpCode1, UdpCode2, MaxPacketSize. CountryName/Language/TimeZoneBias are EMPTY
// on the join (retail fills locale only in the verify "Cookie"). All chunks use
// type 2 (HandleClientJoin @ 0x62B750's CU loop accepts 1 or 2; the genuine .204
// capture frame 8977 shows type 2). The byte-global -> CU-var mapping was
// grilled (2026-06-24): the Kong setter names CMissionInfo_SetGateTag /
// _SetMetTag are misnomers — SetGateTag writes byte_B5F8E8 (the UdpCode1 CU),
// SetMetTag writes byte_B5F908 (the UdpCode2 CU); MetTag (byte_B5F4BC) is the
// METLABEL field (CMissionInfo_SetTargetName, +108). See docs/net §8 NW-S3.
std::vector<ClientSession::Config::CuVar> make_novaworld_join_cu(const NovaWorldJoinCu &in);

} // namespace opennova
