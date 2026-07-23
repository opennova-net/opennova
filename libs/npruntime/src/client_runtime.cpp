#include "npruntime/client_runtime.h"

#include <npwire/ingame_encode.h>

#include <utility>

namespace opennova::np {

namespace {

// §5.44 housekeeping cadences/constants, witnessed in [orig: Client_ProcessNetworkFrame @0x42c180].
constexpr uint32_t kKeepaliveInterval = 29760; // 0x34 keepalive period in ticks [orig @0x42c1c0]
constexpr uint32_t kNetQualityInterval = 310;  // 0x4C net-quality report period [orig @0x42c23e]
constexpr uint32_t kTag2CCooldown = 62;        // 0x2C cooldown set-value [orig @0x42c412] (vestigial gate)

// Little-endian u32 body (the 0x34 currentTick body and the 0x2C timestamp prefix). Mirrors the inline
// LE write the 0x48 ack uses (joiner_connection.cpp); there is no NetPacket_Write* helper in libs.
std::vector<uint8_t> le32(uint32_t v) {
	return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
	        static_cast<uint8_t>(v >> 24)};
}

const std::string &empty_runtime_string() {
	static const std::string empty;
	return empty;
}

} // namespace

ClientRuntime::ClientRuntime(std::string player_name)
		: role_(Role::Joiner),
		  joiner_(std::make_unique<JoinerConnection>(std::move(player_name))) {}

ClientRuntime::ClientRuntime(std::string player_name,
		JoinerConnection::MonotonicMilliseconds monotonic_milliseconds)
		: role_(Role::Joiner),
		  joiner_(std::make_unique<JoinerConnection>(
		          std::move(player_name), std::move(monotonic_milliseconds))) {}

ClientRuntime::ClientRuntime(netsim::ISessionTransport &host_loopback)
		: role_(Role::HostClient), loopback_(&host_loopback) {}

const std::string &ClientRuntime::server_name() const {
	return joiner_ ? joiner_->server_name() : empty_runtime_string();
}

const std::string &ClientRuntime::mission_name() const {
	return joiner_ ? joiner_->mission_name() : empty_runtime_string();
}

const std::string &ClientRuntime::map_file() const {
	return joiner_ ? joiner_->map_file() : empty_runtime_string();
}

const std::string &ClientRuntime::expansion() const {
	return joiner_ ? joiner_->expansion() : empty_runtime_string();
}

const std::string &ClientRuntime::last_error() const {
	return joiner_ ? joiner_->last_error() : empty_runtime_string();
}

std::vector<uint8_t> ClientRuntime::start() {
	if (role_ != Role::Joiner) return {};
	return joiner_->start();
}

void ClientRuntime::receive(const uint8_t *raw, std::size_t len) {
	if (role_ != Role::Joiner || raw == nullptr || len == 0) return;
	recv_fifo_.emplace_back(raw, raw + len);
}

bool ClientRuntime::queue_fired_round(const ClientFiredRound &round) {
	if (role_ != Role::Joiner || joiner_ == nullptr || !joiner_->in_match() ||
	    !deployed_ || !joiner_->has_self_handle() ||
	    round.shooter_handle != joiner_->self_handle())
		return false;
	ClientFiredRound stamped = round;
	// The retail producer reads the client network role's currentTick at the
	// fire action, before the next Client_ProcessNetworkFrame increment. It is
	// independent of the local World's logic clock, which starts after load.
	// [orig: NetPacket_WriteEntityPositionUpdate @0x42A62F]
	stamped.current_tick = current_tick_;
	gameplay_send_queue_.push_back(
			make_protocol_message(0x06, encode_client_fired_round(stamped)));
	return true;
}

bool ClientRuntime::queue_reload_request(const WeaponReload &reload) {
	if (role_ != Role::Joiner || joiner_ == nullptr || !joiner_->in_match() ||
	    !deployed_ || !joiner_->has_self_handle() ||
	    reload.entity_handle != joiner_->self_handle())
		return false;
	gameplay_send_queue_.push_back(
			make_protocol_message(0x25, encode_weapon_reload(reload)));
	return true;
}

std::vector<netsim::ClientRoundEvent> ClientRuntime::drain_round_events() {
	return view_.drain_round_events();
}

std::vector<WeaponReload> ClientRuntime::drain_reload_notifications() {
	return view_.drain_weapon_reloads();
}

void ClientRuntime::seed_session(uint32_t session_id, std::string client_scrk,
                                 std::string server_scrk, uint32_t next_seq, uint32_t last_ack,
                                 uint16_t self_handle, uint16_t self_type,
                                 uint32_t game_type) {
	if (role_ != Role::Joiner) return;
	joiner_->seed_in_match(session_id, std::move(client_scrk), std::move(server_scrk), next_seq,
	                       last_ack, self_handle, self_type, game_type);
	view_.set_game_type(game_type);
	deployed_ = true;     // a seeded replay is post-deploy (the captured client was uplinking)
	replay_mode_ = true;  // reproduce ONLY the captured 0x0C — suppress the live housekeeping (§5.44)
}

std::vector<std::vector<uint8_t>> ClientRuntime::run_frame(const PlayerExtendedUplink *uplink,
                                                           uint32_t now_tick) {
	std::vector<std::vector<uint8_t>> outbound;

	// Per-frame tick bump [orig: currentTick++ @0x42c1ab] — drives the housekeeping cadences below.
	++current_tick_;

	// (0x34) keepalive — runs for EVERYONE (NOT authority-gated), emitted before the recv pump in the
	// original [orig @0x42c1a9..0x42c1ec]. Only a Joiner has a 0x43 framing path here (the HostClient's
	// own-loopback keepalive is a no-op over the wire — deferred-and-logged, P6 §5.44). Suppressed in
	// golden-replay mode.
	if (joiner_ != nullptr && !replay_mode_ && current_tick_ != 0 &&
	    current_tick_ - last_keepalive_tick_ > kKeepaliveInterval) {
		outbound.push_back(joiner_->frame_inner(0x34, le32(current_tick_)));
		last_keepalive_tick_ = current_tick_;
	}

	// (1) RECV pump — fold S2C into ClientState, recv-before-send [orig: PumpClientProtocolRecv
	// @0x42c228 runs before the SEND block].
	if (role_ == Role::HostClient) {
		// The SP host's own loopback carries inner {tag,body} (ADR 0011 §3 SP crypto bypass).
		if (loopback_ != nullptr) view_.pump(*loopback_);
		// Host authority already spawned every accepted round/refill. Its decoded
		// listen-client view must not retain duplicate visual gameplay events.
		view_.drain_round_events();
		view_.drain_weapon_reloads();
	} else {
		// A remote joiner: framed datagrams. JoinerConnection decodes the 0x83 SESSION envelope and
		// surfaces the inner bodies, which we fold via NetClientView::apply (the single remote-wire
		// fold path; pump(transport) is the loopback path — exactly one is active per role).
		while (!recv_fifo_.empty()) {
			std::vector<uint8_t> dg = std::move(recv_fifo_.front());
			recv_fifo_.pop_front();
			JoinerConnection::PollResult pr = joiner_->handle_datagram(dg.data(), dg.size());
			// S2C 0x7B may have updated the wire-invisible phase-3 layout hint
			// before this datagram's 0x0A bodies are folded.
			view_.set_game_type(joiner_->game_type());
			for (std::vector<uint8_t> &reply : pr.outbound) outbound.push_back(std::move(reply));
			for (const auto &tb : pr.inbound_world) view_.apply(tb.first, tb.second); // 0x0C/0x0D/0x10/0x20
			for (const auto &tb : pr.inbound_gameplay) view_.apply(tb.first, tb.second);
			if (pr.reached_in_match) deployed_ = true; // deploy on the spawn name-match (§5.44 gate)
			for (const std::vector<uint8_t> &a : pr.inbound_0a) {
				const uint32_t health_before = view_.state().health_updates_applied;
				view_.apply(0x0A, a); // per-frame 0x0A
				// Evaluate each decoded tail in receive order. A later positive
				// sample in the same pump cannot erase an earlier death edge.
				if (view_.state().health_updates_applied != health_before &&
				    view_.state().local_health <= 0) {
					deployed_ = false;
				}
			}
		}
		// The recipient-specific 0x0A tail is the authoritative local health channel. Close the
		// deployed gate on a fresh death frame before this same client frame reaches its send block.
		// Positive health deliberately does not reopen it: respawn remains owned by the deploy flow.
	}

	if (role_ == Role::HostClient) return outbound; // host: no connect-drive, no housekeeping send, no 0x0C

	// (1b) CONNECT-DRIVE — the same per-frame pump retransmits an unanswered pre-session 0x41/0x42,
	// then (once Driving) advances the in-match spawn-gate burst. [orig: NapiNPConnection active-send
	// interval; NapiClient_WaitForGameStart]
	for (std::vector<uint8_t> &d : joiner_->pump(now_tick)) outbound.push_back(std::move(d));

	// (0x4C) net-quality / anti-cheat report — after the recv pump; gated is_in_session &&
	// is_mp_session_peer (a Joiner in-match satisfies both). [orig @0x42c23e..0x42c279]
	if (!replay_mode_ && joiner_->in_match()) {
		if (++net_quality_timer_ > kNetQualityInterval) {
			net_quality_timer_ = 0;
			outbound.push_back(joiner_->frame_inner(0x4C, std::vector<uint8_t>{net_quality_}));
		}
	}

	// Cooldown self-decrement [orig @0x42c386]. The 0x2C send below sets it to 62, but it is NOT read
	// as a send gate — the only xrefs to g_tag2CSendCooldown @0xA860D8 are this decrement + that set,
	// so the 0x2C fires every deployed frame (corrects §5.44's "62-tick holdoff" note; re-doc).
	if (tag2c_send_cooldown_ > 0) --tag2c_send_cooldown_;

	// (2) SEND BLOCK — gated send_holdoff_countdown == 0 (NapiNPConnection+0x648; the original skips the
	// whole block when set). [orig @0x42c3dd]
	if (send_holdoff_countdown_ == 0) {
		// The witnessed deploy gate the 0x2C RTT ping and the 0x0C uplink share: is_in_session &&
		// !is_authority && !dword_81474C && !g_spawn_success_gate. A Joiner is always !is_authority;
		// deployed_ mirrors !g_spawn_success_gate (§5.44).
		const bool deployed_joiner = joiner_->in_match() && deployed_ && joiner_->has_self_handle();
		if (!deployed_joiner) gameplay_send_queue_.clear();

		// Weapon actions queue typed gameplay before Client_ProcessNetworkFrame;
		// PumpClientProtocolSend flushes them through this same sequenced 0x43 path.
		while (deployed_joiner && !gameplay_send_queue_.empty()) {
			ProtocolMessage msg = std::move(gameplay_send_queue_.front());
			gameplay_send_queue_.pop_front();
			outbound.push_back(joiner_->frame_inner(msg.tag, std::move(msg.payload)));
		}

		// (0x2C) RTT timestamp ping — Joiner only. [orig @0x42c3fa..0x42c44a]. Body = [u32 ts][u8 1]
		// (NetPacket_WriteInt32AndByte; echoFlag=1 requests the S2C 0x57 pong, §5.34). retail uses
		// GetTickCount; now_tick keeps it deterministic (the RTT value is not validated opennova↔opennova).
		if (!replay_mode_ && deployed_joiner) {
			tag2c_send_cooldown_ = kTag2CCooldown;
			std::vector<uint8_t> ping = le32(now_tick);
			ping.push_back(0x01);
			outbound.push_back(joiner_->frame_inner(0x2C, std::move(ping)));
		}

		// (0x0C) the C2S player uplink — unchanged P5 path, same deploy gate. [orig @0x42c46f..0x42c4a3]
		if (uplink != nullptr && deployed_joiner) {
			outbound.push_back(joiner_->frame_c2s_uplink(joiner_->self_handle(),
			                                             joiner_->spawn_pose().item_type_id, *uplink));
		}
	}
	return outbound;
}

std::vector<std::vector<uint8_t>>
ClientRuntime::Client_ProcessNetworkFrame(const PlayerExtendedUplink &uplink, uint32_t now_tick) {
	return run_frame(&uplink, now_tick);
}

std::vector<std::vector<uint8_t>> ClientRuntime::Client_ProcessNetworkFrame(uint32_t now_tick) {
	return run_frame(nullptr, now_tick);
}

} // namespace opennova::np
