#pragma once

#include "npruntime/joiner_connection.h"

#include <netsim/net_client_view.h>     // NetClientView / ClientState
#include <netsim/session_transport.h>   // ISessionTransport

#include <novaworld/client_session.h>   // ClientSession::Config (shared JO identity)
#include <npwire/ingame_decode.h>     // PlayerExtendedUplink (the §5.10 0x0C body)

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

// P5 — the headless, socket-free CLIENT runtime. The faithful reimpl of the per-frame client net
// role [orig: Client_ProcessNetworkFrame @0x42c180] composed with the connect-leg state machine
// (np::JoinerConnection) and the S2C->ClientState fold (netsim::NetClientView). The original runs
// the SAME per-frame function on every machine (it is NOT authority-gated); only the 0x0C uplink
// inside it is gated !is_authority. So this runtime has two roles, mirroring that:
//
//   Role::Joiner     — a remote client joining a host. Drives the in-match connect legs
//                      (Hello->Auth->Driving spawn-gate burst->InMatch via JoinerConnection; the
//                      lobby GATE->VERIFY->READY->PLAY legs are the SEPARATE ADR-0010 ClientSession
//                      matchmaking flow, owned by the binding, NOT here), then per frame folds
//                      inbound S2C into ClientState and emits the C2S 0x0C uplink.
//   Role::HostClient — the SP listen-server host's OWN local view (is_authority == 1). Its player
//                      is a server-side entity (ADR 0011/0012), so there is NO handshake and NO 0x0C
//                      uplink (the witnessed !is_authority gate). Per frame it ONLY folds its own
//                      0x0A off the in-process loopback the host's Server_TickUpdate emits onto.
//
// FRAMING LAYERS (kept strictly separate — the P5-review fix):
//   - Joiner traffic is whole NWU-framed datagrams (0x41/0x42/0x43/0x81/0x82/0x83). The owner ships
//     start()/Client_ProcessNetworkFrame() return values over the socket and feeds received
//     datagrams to receive(). JoinerConnection does the NWU+SCRK decode and surfaces the inner
//     {tag,body} bodies, which we fold via NetClientView::apply (NOT through a transport).
//   - HostClient inbound is the inner {tag,body} the host's Server_TickUpdate host_send()s onto the
//     in-process LoopbackChannel (the ADR-0011 §3 SP crypto bypass — no 0x83 framing on the
//     loopback), folded via NetClientView::pump(transport).
//
// [orig: Client_ProcessNetworkFrame @0x42c180; PumpClientProtocolRecv @0x42c228 / Send @0x42c4bc;
//  Player_BuildTag0CInputBody @0x42a550; docs/net §5.44]. No socket I/O lives here.
namespace opennova::np {

class ClientRuntime {
public:
	enum class Role : uint8_t { Joiner, HostClient };

	// Remote-joiner runtime. Transport-less: framed bytes in via receive(), framed bytes out via
	// the start()/Client_ProcessNetworkFrame() return values (the owner pumps the socket).
	ClientRuntime(ClientSession::Config config, std::string player_name);

	// SP host-as-client runtime. `host_loopback` is the in-process channel the host's
	// Server_TickUpdate emits S2C onto (non-owning). is_authority is implicit (no 0x0C uplink).
	explicit ClientRuntime(netsim::ISessionTransport &host_loopback);

	Role role() const { return role_; }
	bool is_authority() const { return role_ == Role::HostClient; }

	// --- connect / recv (Joiner) ---
	// Begin the handshake; returns the framed ClientHello to ship (Idle->Hello). Empty for HostClient.
	std::vector<uint8_t> start();

	// Deposit one received FRAMED datagram (off the socket) for the next frame's recv pump to drain.
	// Mirrors CNapiNetwork_PumpManagerReceive feeding the byte recv FIFO ahead of the frame's recv
	// pump. No-op for HostClient (it reads its loopback transport directly).
	void receive(const uint8_t *raw, std::size_t len);

	// The per-frame client net role [orig: Client_ProcessNetworkFrame @0x42c180], in witnessed order:
	//   (1) recv pump: Joiner drains the recv FIFO -> JoinerConnection decodes -> drive the connect
	//       legs + fold inner S2C bodies into ClientState [orig: PumpClientProtocolRecv @0x42c228];
	//       HostClient pumps the loopback transport.
	//   (1b) connect-drive (Joiner): while Driving, pump() the next spawn-gate burst stage.
	//   (2) send (Joiner only): if InMatch && deployed -> frame_c2s_uplink(self_handle, type, uplink)
	//       [orig: Player_BuildTag0CInputBody->QueueReliableMessage(0x0C) @0x42c482]. The host
	//       (is_authority) never uplinks its own player (witnessed !is_authority gate); HostClient
	//       sends nothing.
	// Returns the framed datagrams to ship (handshake replies + burst + the per-frame housekeeping +
	// the 0x0C). `uplink` is the §5.10 0x0C extended body (the OUTPUT of Player_BuildTag0CInputBody; the
	// raw-input->entity motor, step 5a, runs host-side per ADR 0012 R1 and is NOT part of the headless
	// client). The witnessed per-frame housekeeping is now PORTED (P6, §5.44): the 0x34 keepalive
	// (29760-tick), the 0x4C net-quality report (310-tick), the 0x2C RTT ping (every deployed frame),
	// and the send_holdoff_countdown send-block gate — all on the Joiner role (HostClient's own-loopback
	// housekeeping stays deferred-and-logged). seed_session() replay mode suppresses them for byte-parity.
	std::vector<std::vector<uint8_t>> Client_ProcessNetworkFrame(const PlayerExtendedUplink &uplink,
	                                                             uint32_t now_tick = 0);
	// No-uplink frame (HostClient, or a pre-deploy Joiner): recv pump + connect-drive only, no 0x0C.
	std::vector<std::vector<uint8_t>> Client_ProcessNetworkFrame(uint32_t now_tick = 0);

	// Deterministic golden replay (Joiner): seed the connection keys + seq/ack + self handle/type so
	// frame_c2s_uplink reproduces a captured C2S 0x0C datagram byte-for-byte. [ROADMAP "Determinism"]
	void seed_session(uint32_t session_id, std::string client_scrk, std::string server_scrk,
	                  uint32_t next_seq, uint32_t last_ack, uint16_t self_handle,
	                  uint16_t self_type, uint32_t game_type = 0);

	// The "deployed" predicate gating the 0x0C uplink. It defaults true on reaching InMatch;
	// a complete recipient-local 0x0A tail with health <= 0 closes it before the same frame's send.
	// Positive health does not reopen it. The explicit deploy/respawn exchange that reopens the gate
	// remains deferred; set_deployed(true) is the integration seam for that future edge.
	void set_deployed(bool v) { deployed_ = v; }
	bool deployed() const { return deployed_; }

	// Joiner state passthrough (HostClient: never InMatch, no self handle).
	bool in_match() const { return joiner_ && joiner_->in_match(); }
	bool has_self_handle() const { return joiner_ && joiner_->has_self_handle(); }
	uint16_t self_handle() const { return joiner_ ? joiner_->self_handle() : 0; }
	JoinerConnection::Phase phase() const {
		return joiner_ ? joiner_->phase() : JoinerConnection::Phase::Idle;
	}
	// The host-advertised self-spawn pose learned at the name-match — full precision (not the lossy
	// ClientState position), so the binding spawns its local player L from it. Joiner-only; valid once
	// in_match() (the caller gates on that). Mirrors the self_handle() passthrough.
	const JoinerConnection::SelfSpawn &spawn_pose() const { return joiner_->spawn_pose(); }

	const netsim::ClientState &state() const { return view_.state(); }
	netsim::NetClientView &view() { return view_; }
	std::size_t unknown_tags() const { return view_.unknown_tags(); }

private:
	// Shared body for both Client_ProcessNetworkFrame overloads. `uplink` is nullptr for a no-uplink
	// frame.
	std::vector<std::vector<uint8_t>> run_frame(const PlayerExtendedUplink *uplink, uint32_t now_tick);

	Role role_;
	std::unique_ptr<JoinerConnection> joiner_;        // Joiner only
	netsim::NetClientView view_;
	netsim::ISessionTransport *loopback_ = nullptr;   // HostClient only (non-owning)
	std::deque<std::vector<uint8_t>> recv_fifo_;      // Joiner: framed inbound awaiting the recv pump
	bool deployed_ = false;

	// --- §5.44 per-frame housekeeping counters (P6) — mirror the witnessed per-instance globals of
	// [orig: Client_ProcessNetworkFrame @0x42c180]. The 0x34 keepalive / 0x4C net-quality / 0x2C RTT
	// emits and the send-holdoff send-block gate, deferred-and-logged at P5, ported here. ---
	uint32_t current_tick_ = 0;          // [orig: currentTick @0xA8229C] bumped once per run_frame
	uint32_t last_keepalive_tick_ = 0;   // [orig: g_lastKeepaliveTick @0xA822A0] 0x34 send latch
	uint32_t net_quality_timer_ = 0;     // [orig: g_netQualityReportTimer @0xA85B84] 0x4C cadence
	uint32_t tag2c_send_cooldown_ = 0;   // [orig: g_tag2CSendCooldown @0xA860D8] set 62 on a 0x2C send,
	                                     // decremented per frame; NOT read as a send gate in this fn
	                                     // (a nuance vs §5.44 "62-tick holdoff" — see ROADMAP/re-doc).
	uint8_t  net_quality_ = 0;           // [orig: g_netQuality byte @0x82BF88] 0 = best (host clamps 0..4)
	uint32_t send_holdoff_countdown_ = 0;// [orig: NapiNPConnection+0x648] 0 = send block open (default)

	// seed_session() golden-replay mode: suppress the live per-frame housekeeping (0x34/0x4C/0x2C) so a
	// seeded single-frame emission reproduces ONLY the captured 0x0C datagram byte-for-byte (the
	// determinism contract npruntime_golden_client asserts — seed_session is "for replay/parity only").
	bool replay_mode_ = false;
};

} // namespace opennova::np
