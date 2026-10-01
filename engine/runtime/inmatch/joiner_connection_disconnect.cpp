// JoinerConnection — the disconnect / session-loss family: the host's description punt
// and 0x86 goodbye receive legs, the silence reap, the latched disconnect record and the
// 0x46 leave burst, and the terminal fail transition — plus the 0x85 outer connection
// ping, the other CK-keyed outer opcode that stamps the same reap clock. Split out of
// joiner_connection.cpp; the class header is the shared declaration.
#include <runtime/inmatch/joiner_connection.h>

#include <base/io/log.h>
#include <net/npwire/nw_session_framing.h> // nw_encode_outbound (the 0x46 / 0x45 envelope)
#include <net/npwire/session_hello.h>      // DisconnectEvent / parse_disconnect_event / client_goodbye_to_bytes
#include <net/npwire/session_keys.h>       // SESSION_OPCODE_CLIENT_GOODBYE / SESSION_OPCODE_CLIENT_PING
#include <net/npwire/session_ping.h>       // the shared 0x45/0x85 body codec

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace opennova::inmatch {

// [orig: Nwu_HandlePing @0x623A70]: the body after the NWU layer is
// `[u32 receiver-local key][flat TLVs]`; the key must equal OUR local key (CK)
// @0x623BC2 or the datagram is dropped silently; the TLV walk reads `WR` (u8)
// and `MS` (u32) case-insensitively and stops at an empty name; then the
// connection's reap clock is stamped @0x623C56, and either a 0x45 pong with WR
// clear and the same MS goes back @0x623C5C (CNapiNPConnection_SendPing
// @0x61F080: opcode 0x45 for a client, `[u32 remote key][WR][MS]`) or the rtt
// lands in session_keys.rtt_ms @0x623C9B. The pong leaves from inside the
// receive pump: SendPing writes it straight to the socket (CNapiNPManager_SendTo
// @0x61F261), so the host's round trip never includes our send-holdoff wait.
void JoinerConnection::on_server_ping(const std::vector<uint8_t> &body, PollResult &out) {
	SessionPingBody ping;
	if (!parse_session_ping_body(body.data(), body.size(), ping)) return;
	if (ping.receiver_local_key != client_key_) return;
	last_receive_ms_ = monotonic_milliseconds_();
	receive_clock_armed_ = true;
	if (ping.wants_reply) {
		out.immediate_outbound.push_back(nw_encode_outbound(SESSION_OPCODE_CLIENT_PING,
				build_session_ping_body(conn_.server_sk, /*wants_reply=*/false,
						ping.timestamp_ms)));
		return;
	}
	session_rtt_ms_ = monotonic_milliseconds32() - ping.timestamp_ms;
}

// The host closed the session on its own terms (docs/net/novaworld-net-re.md §5.64 — the punt
// families and the captured bytes). Retail's receiver stores the event only when its
// slot is still empty, so the FIRST record wins and a repeat cannot restate the cause, then it
// leaves the active state. The receive path queues the ordinary four-packet keyed goodbye burst
// before Phase::Error becomes terminal; pump() then stops producing keepalives, the deployment
// sub-state clears so a parked deploy screen stops taking clicks, and the owner's next session-loss
// read carries the decoded reason.
// [orig: CNapiNPConnection_HandleDescriptionPacket @0x621ae0 — the store gate @0x621d3c, the
//  pending-disconnect transition @0x621d53..0x621d6b ->
//  CNapiNPConnection_TeardownActiveConnection @0x6253c0]
void JoinerConnection::on_host_disconnect(const DisconnectEvent &event) {
	if (!host_disconnect_reason_.empty()) return;
	latch_disconnect_event(event, 2); // no-op when the receive path already latched it
	io::logf(io::LogLevel::kWarn,
			"np joiner: host description punt: dc=%u dpc=%u ddstr='%s' dstr='%s' (stage: %s)",
			static_cast<unsigned>(event.dc), static_cast<unsigned>(event.dpc),
			event.ddstr.c_str(), event.dstr.c_str(), post_auth_stage_name());
	// The client's exit-reason switch keys on DPC and only runs for the DC == 2 family; both
	// therefore belong in the reason, alongside the sender's own tag and text (for the
	// witnessed deploy-screen idle punt: DPC 33, DC 2, "LogPuntEvent", "t35").
	// [orig: the DC gate @0x4c6563 and the DPC switch @0x4c6569]
	// The game-layer join gate answers its spectator failures through this
	// same record with empty strings — DPC 14 disabled / 15 full / 16 bad
	// password. [orig: Server_ValidatePlayerJoinRequest @0x512100 via
	// CNapiNPConnection_SendChatMessage @0x4c7ef0]
	if (event.dc == 2) {
		switch (event.dpc) {
		case 14:
			host_disconnect_reason_ = "Spectators are disabled on this server";
			fail(host_disconnect_reason_);
			return;
		case 15:
			host_disconnect_reason_ = "The spectator slots are full";
			fail(host_disconnect_reason_);
			return;
		case 16:
			host_disconnect_reason_ = "The spectator password is incorrect";
			fail(host_disconnect_reason_);
			return;
		case 18:
			host_disconnect_reason_ = "The team password is incorrect";
			fail(host_disconnect_reason_);
			return;
		case 19:
			host_disconnect_reason_ = "The blue team password is incorrect";
			fail(host_disconnect_reason_);
			return;
		case 20:
			host_disconnect_reason_ = "The red team password is incorrect";
			fail(host_disconnect_reason_);
			return;
		case 21:
			host_disconnect_reason_ = "The squad password is incorrect";
			fail(host_disconnect_reason_);
			return;
		case 22:
			host_disconnect_reason_ = "The requested team is invalid";
			fail(host_disconnect_reason_);
			return;
		default:
			break;
		}
	}
	host_disconnect_reason_ = "the host closed the session (reason " +
			std::to_string(event.dpc) + ", class " + std::to_string(event.dc) + ")";
	if (!event.ddstr.empty()) host_disconnect_reason_ += ": " + event.ddstr;
	if (!event.dstr.empty()) host_disconnect_reason_ += " " + event.dstr;
	fail(host_disconnect_reason_);
}

// [orig: the cs_dir0/cs_dir1 timeout_ms = 120000 reap installed by CNapiNetwork_Init
//  @0x4ca4a0 -> CNapiNetwork_OnDisconnectedFromServer @0x4c63d0, which clears the
//  session strings and maps the disconnect code onto g_MissionExitReason]
bool JoinerConnection::session_lost() const {
	// An explicit close is terminal at any stage; the silence reap runs for the whole
	// accepted-0x82 state (Driving = every join/deploy stage, InMatch) with no gameplay gate,
	// and a negative timeout (the host's `_NSTMOUT.TXT` NEVER) disables it.
	// [orig: CNapiNPConnection_PumpStateMachine @0x6292E0 case 5 @0x6295a2..0x62961c —
	//  `timeout_ms >= 0` @0x6295a2, elapsed = now - conn+0x5E8 @0x6295b2, strictly `>`]
	if (!host_disconnect_reason_.empty() || silence_timeout_latched_) return true;
	// A self-initiated teardown (the pool-overflow MSGCRE) is terminal with its record latched.
	if (phase_ == Phase::Error && disconnect_event_set_) return true;
	if (!receive_clock_armed_ || conn_.timeouts.timeout_ms < 0 ||
	    (phase_ != Phase::Driving && phase_ != Phase::InMatch)) {
		return false;
	}
	return milliseconds_since_last_receive() >
			static_cast<uint64_t>(static_cast<uint32_t>(conn_.timeouts.timeout_ms));
}

bool JoinerConnection::poll_session_loss() {
	if (!session_lost()) return false;
	if (phase_ != Phase::Error) {
		const std::string reason = session_loss_reason();
		// The reap's own record, latched before the teardown so a later leave burst
		// carries it. [orig: {role, 3, elapsed, timeout, "", 0, "NP.C:PT:CLNTTMOUT"}
		//  @0x629605..0x62961c, latch-if-invalid @0x6293f4..0x629406]
		latch_disconnect_event(make_disconnect_event(2, 3,
				static_cast<uint32_t>(milliseconds_since_last_receive()),
				static_cast<uint32_t>(conn_.timeouts.timeout_ms), "", 0,
				"NP.C:PT:CLNTTMOUT"), 2);
		silence_timeout_latched_ = true;
		fail(reason);
	}
	return true;
}

std::string JoinerConnection::session_loss_reason() const {
	if (!host_disconnect_reason_.empty()) return host_disconnect_reason_;
	if (!session_lost()) return {};
	if (!silence_timeout_latched_ && disconnect_event_set_) {
		// The connection tore itself down (the outbound pool overflow); its own tag names it.
		return "the connection was closed (" + last_disconnect_event_.ddstr + ")";
	}
	// Retail maps THIRTEEN distinct disconnect codes onto distinct exit reasons
	// (@0x4c63d0); this is the one cause with no code on the wire at all — silence
	// past the reap window (D-NET-177).
	return "lost connection to the host (no traffic for " +
	       std::to_string(static_cast<uint32_t>(conn_.timeouts.timeout_ms) / 1000u) +
	       " seconds)";
}

const char *JoinerConnection::post_auth_stage_name() const {
	switch (post_auth_stage_) {
	case PostAuthStage::Inactive: return "inactive";
	case PostAuthStage::AwaitServerSettings: return "awaiting the host's initial settings";
	case PostAuthStage::AwaitJoinAck: return "awaiting the JOIN acknowledgement";
	case PostAuthStage::AwaitPaddingProbe: return "awaiting the join probe";
	case PostAuthStage::AwaitGameStart: return "awaiting the game-start flag";
	case PostAuthStage::AwaitServerInfo: return "awaiting the server-info transfer";
	case PostAuthStage::AwaitMissionData: return "awaiting the mission-data transfer";
	case PostAuthStage::AwaitPlayerList: return "awaiting the player list";
	case PostAuthStage::AwaitInitialSyncTail: return "awaiting the pre-world sync tail";
	case PostAuthStage::AwaitWorldStreamEnd: return "awaiting the world stream";
	case PostAuthStage::AwaitDeployment: return "awaiting the loadout grants";
	case PostAuthStage::AwaitDeployPick: return "awaiting the player's deployment pick";
	case PostAuthStage::AwaitDeployRelease: return "awaiting the deployment release";
	case PostAuthStage::Complete: return "complete";
	}
	return "unknown";
}

// The retail teardown of an active (or still-connecting) client connection sends a burst of
// disconnect packets so a lossy LAN still hears the leave, then clears the key material. The
// burst size is cs_dir0.recv_max_per_tick clamped to [0, 32] — 4 for the JOINTOPERATIONS
// template. Without this leg the host's only teardown trigger never fires from our client and
// every leave leaks an admitted peer plus its spawned player.
// [orig: CNapiNPConnection_TeardownActiveConnection @0x6253c0 (burst loop @0x6253ef..0x625424);
//  CNapiNPConnection_SendDisconnectPacket @0x61f2a0; CNapiNetwork_Init @0x4ca4a0 stores
//  recv_max_per_tick = 4 @0x4cab60]
std::vector<std::vector<uint8_t>> JoinerConnection::disconnect() {
	if (goodbye_sent_ || conn_.server_sk == 0) return {};
	goodbye_sent_ = true;
	// A leave with nothing latched is the user's: CNapiNetwork_DisconnectActiveConnection
	// latches {role, DC 2, 0, 0, <caller tag>, 0, ""} — the in-match leave action's tag is
	// "I.C:CIDEMIS". Every other producer (a host punt or goodbye, the CLNTTMOUT reap, the
	// MSGCRE overflow) latched its record first, so the burst echoes that.
	// [orig: Input_HandleActionBinding case 3 @0x49af38..0x49af42 ->
	//  CNapiNetwork_DisconnectActiveConnection @0x4C9140 (DC=2 @0x4c91ef, DSTR=message
	//  @0x4c91ff, latch-if-!valid @0x4c922d..0x4c923e); SendDisconnectPacket @0x61f3b1..0x61f4aa]
	latch_disconnect_event(make_disconnect_event(2, 2, 0, 0, "I.C:CIDEMIS", 0, ""), 2);
	std::vector<uint8_t> datagram = nw_encode_outbound(
			SESSION_OPCODE_CLIENT_GOODBYE,
			client_goodbye_to_bytes(conn_.server_sk, last_disconnect_event_));
	return std::vector<std::vector<uint8_t>>(disconnect_burst_count(), std::move(datagram));
}

// [orig: Nwu_HandleServerGoodbye @0x624310 -> Nwu_HandleDisconnect(type 2) @0x623CE0]: the
// leading dword must equal OUR local key (CK) @0x623e74 — a mismatch (or a body too short to
// carry one) is dropped silently; the TLV run is walked leniently (DS discarded, an empty run
// yields zeros @0x623eb2), the record is latched with the PEER role 1 (DSTR 128 / DDSTR 32)
// @0x624065..0x6240ba, then RequestDisconnect @0x6240c4 -> SetState(6) @0x61e0fa ->
// TeardownActiveConnection @0x62549e..0x6254e7: the 0x46 burst echoing the latched record and
// CNapiNetwork_OnDisconnectedFromServer @0x4C63D0 (the exit reason from DC/DPC).
void JoinerConnection::on_server_goodbye(const std::vector<uint8_t> &body, PollResult &out) {
	if (body.size() < 4) return;
	const uint32_t receiver_local_key =
			static_cast<uint32_t>(body[0]) |
			(static_cast<uint32_t>(body[1]) << 8) |
			(static_cast<uint32_t>(body[2]) << 16) |
			(static_cast<uint32_t>(body[3]) << 24);
	if (receiver_local_key != client_key_) return;
	DisconnectEvent event;
	(void)parse_disconnect_event(body.data() + 4, body.size() - 4, event);
	// Teardown owns this receive result, exactly like the H:0x03 description punt.
	out = PollResult{};
	latch_disconnect_event(event, 1);
	std::vector<std::vector<uint8_t>> goodbye = disconnect();
	out.outbound.insert(out.outbound.end(),
			std::make_move_iterator(goodbye.begin()),
			std::make_move_iterator(goodbye.end()));
	on_host_disconnect(event);
}

void JoinerConnection::latch_disconnect_event(const DisconnectEvent &event, uint32_t role) {
	if (disconnect_event_set_) return;
	last_disconnect_event_ = make_disconnect_event(role, event.dc, event.dp1, event.dp2,
			event.dstr, event.dpc, event.ddstr);
	disconnect_event_set_ = true;
}

void JoinerConnection::fail(std::string reason) {
	io::logf(io::LogLevel::kWarn, "np joiner: session failed (stage: %s): %s",
			post_auth_stage_name(), reason.c_str());
	last_error_ = std::move(reason);
	handshake_retry_datagram_.clear();
	handshake_retry_clock_armed_ = false;
	post_auth_stage_ = PostAuthStage::Inactive;
	session_last_send_ms_ = 0;
	session_send_clock_armed_ = false;
	session_ack_pending_ = false;
	phase_ = Phase::Error;
}

} // namespace opennova::inmatch
