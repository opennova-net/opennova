#include "npruntime/client_runtime.h"

#include <npwire/ingame_encode.h>
#include <npwire/ingame_message_id.h>

#include <utility>

namespace opennova::np {

namespace {

// §5.44 housekeeping cadences/constants, witnessed in [orig: Client_ProcessNetworkFrame @0x42c180].
constexpr uint32_t kKeepaliveInterval = 29760; // 0x34 keepalive period in ticks [orig @0x42c1c0]
constexpr uint32_t kNetQualityInterval = 310;  // 0x4C net-quality report period [orig @0x42c23e]
constexpr uint32_t kTag2CCooldown = 62;        // 0x2C vestigial/telemetry set-value [orig @0x42c412], never a gate

// Retail's x86 IMUL/ADD timer arithmetic wraps at 32 bits. Perform the
// operations in unsigned space, then recover the same signed two's-complement
// value without relying on implementation-defined unsigned-to-signed casts.
int32_t signed_dword(uint32_t bits) {
	if (bits <= 0x7FFFFFFFu) return static_cast<int32_t>(bits);
	return -1 - static_cast<int32_t>(0xFFFFFFFFu - bits);
}

int32_t timer_scale_62(int32_t value) {
	return signed_dword(static_cast<uint32_t>(value) * 62u);
}

int32_t timer_add(int32_t value, int32_t delta) {
	return signed_dword(
			static_cast<uint32_t>(value) + static_cast<uint32_t>(delta));
}

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

bool ClientRuntime::lfp_cam_percent(uint16_t zone_handle, int32_t &out) const {
	const auto it = zone_states_.find(zone_handle);
	if (it == zone_states_.end()) return false;
	const ZoneState::Entry &entry = it->second.entry;
	if (entry.value_limit == 0) {
		out = 0x10000;
		return true;
	}
	// BoneCallback_gnrc_World loads the two signed DWORDs through x87 FILD,
	// divides, multiplies by exactly 65536, then truncates through _ftol2_sse.
	// The accumulator is clamped before presentation, so this integer rational
	// form gives the same fixed-point truncation without host-FPU dependence.
	out = static_cast<int32_t>(
			(static_cast<int64_t>(entry.value_current) * 0x10000ll) /
			static_cast<int64_t>(entry.value_limit));
	return true;
}

void ClientRuntime::apply_zone_timer_value(const ZoneTimerValue &value) {
	auto inserted = zone_states_.try_emplace(value.zone_handle);
	ZoneState &zone = inserted.first->second;
	zone.has_value = true;
	zone.value = value;

	ZoneState::Entry &entry = zone.entry;
	entry.mode_a = value.mode;
	entry.mode_b = value.mode;
	const int32_t scaled_value = timer_scale_62(value.value_s);
	if (inserted.second) entry.value_current = scaled_value;
	entry.value_target = scaled_value;
	entry.value_limit = timer_scale_62(value.limit_s);
	entry.value_rate = value.rate; // i16 is sign-extended by the retail handler
	entry.window_active = false;
	entry.value_active = true;
}

void ClientRuntime::apply_zone_timer_window(const ZoneTimerWindow &window) {
	auto inserted = zone_states_.try_emplace(window.zone_handle);
	ZoneState &zone = inserted.first->second;
	zone.has_window = true;
	zone.window = window;

	ZoneState::Entry &entry = zone.entry;
	const int32_t scaled_start =
			timer_scale_62(static_cast<int32_t>(window.start_s));
	const int32_t scaled_end =
			timer_scale_62(static_cast<int32_t>(window.end_s));
	if (inserted.second) {
		entry.window_current = scaled_start;
		entry.value_current = 0;
		entry.value_rate = 0;
	} else if (entry.mode_b != window.mode_b) {
		entry.window_current = scaled_start;
	}
	entry.mode_a = window.mode_a;
	entry.mode_b = window.mode_b;
	entry.window_target = scaled_start;
	entry.window_limit = scaled_end;
	entry.window_rate = window.rate; // the wire byte is zero-extended
	entry.value_target = 0;
	entry.value_limit = 0;
	entry.window_active = true;
	entry.value_active = false;
}

bool ClientRuntime::apply_zone_timer_body(
		uint8_t tag, const std::vector<uint8_t> &body) {
	std::size_t consumed = 0;
	if (tag == 0x6F) {
		ZoneTimerValue value;
		if (decode_zone_timer_value(
					body.data(), body.size(), value, consumed) &&
		    consumed == body.size())
			apply_zone_timer_value(value);
		return true;
	}
	if (tag == 0x53) {
		ZoneTimerWindow window;
		if (decode_zone_timer_window(
					body.data(), body.size(), window, consumed) &&
		    consumed == body.size())
			apply_zone_timer_window(window);
		return true;
	}
	return false;
}

void ClientRuntime::advance_zone_timers() {
	for (auto &kv : zone_states_) {
		ZoneState::Entry &entry = kv.second.entry;
		if (entry.window_active) {
			const int32_t delta = entry.window_rate;
			entry.window_current = timer_add(entry.window_current, delta);
			// Exact @0x537D81..0x537D8F order: high clamp, then low clamp.
			if (entry.window_current > entry.window_limit)
				entry.window_current = entry.window_limit;
			if (entry.window_current < 0) entry.window_current = 0;
			if (entry.window_target == entry.window_limit &&
			    entry.window_current == entry.window_limit &&
			    delta > 0)
				entry.window_active = false;
		}
		if (entry.value_active) {
			entry.value_current =
					timer_add(entry.value_current, entry.value_rate);
			// Exact @0x537DB1..0x537DC0 order: high clamp, then low clamp.
			if (entry.value_current > entry.value_limit)
				entry.value_current = entry.value_limit;
			if (entry.value_current < 0) entry.value_current = 0;
		}
	}
}

std::vector<uint8_t> ClientRuntime::start() {
	if (role_ != Role::Joiner || joiner_ == nullptr) return {};

	// ClientRuntime is reusable across disconnect/reconnect. All state below
	// belongs to the prior wire session and must be gone before the new Hello:
	// queued datagrams/actions must not authenticate under the new keys, and
	// authoritative/UI state must not leak into the next world.
	recv_fifo_.clear();
	gameplay_send_queue_.clear();
	framed_send_queue_.clear();
	pre_send_queue_.clear();
	zone_states_.clear();
	authoritative_loadout_ = WeaponLoadout{};
	authoritative_loadout_revision_ = 0;
	deployed_ = false;
	deployment_release_revision_ = 0;
	self_team_revision_ = 0;
	cleared_player_slots_.clear();
	pending_loadout_resubmit_ = false;
	pending_deployment_pick_set_ = false;
	pending_deployment_pick_ = 0xFFFFu;
	current_tick_ = 0;
	last_keepalive_tick_ = 0;
	net_quality_timer_ = 0;
	tag2c_send_cooldown_ = 0;
	net_quality_ = 0;
	send_holdoff_countdown_ = 0;
	replay_mode_ = false;
	view_.state() = netsim::ClientState{};
	view_.drain_round_events();
	view_.drain_weapon_reloads();
	view_.set_game_type(0);
	return joiner_->start();
}

void ClientRuntime::receive(const uint8_t *raw, std::size_t len) {
	if (role_ != Role::Joiner || raw == nullptr || len == 0) return;
	recv_fifo_.emplace_back(raw, raw + len);
}

std::vector<std::vector<uint8_t>> ClientRuntime::disconnect() {
	if (role_ != Role::Joiner || joiner_ == nullptr) return {};
	return joiner_->disconnect();
}

bool ClientRuntime::queue_fired_round(const ClientFiredRound &round) {
	if (role_ != Role::Joiner || joiner_ == nullptr || !joiner_->in_match() ||
	    !deployed_ || !joiner_->has_self_handle() ||
	    round.shooter_handle != joiner_->self_handle())
		return false;
	// NO freshness gate on this path. Retail's fire action splits on is_authority
	// (`cmp g_napi_np_ctx.is_authority / jz loc_42BF41` @0x42bdfd..0x42be03): only the
	// AUTHORITY arm runs PlayerSlot_IsActive @0x4fc760 for its own local player and bails
	// at @0x42be44. A joiner (is_authority == 0) jumps straight to the client arm, spawns
	// its predicted round (RoundData_SpawnRound @0x42c030) and queues the 0x06
	// unconditionally — an unseeded joiner still fires locally and still transmits, and
	// the HOST is what discards the shot (its own PlayerSlot_IsActive on the packet's
	// tick, @0x51358d, rejects tick == 0). Gating here instead would be our own invention.
	// The authority-side leg is unported and tracked as D-NET-174.
	// [orig: Entity_FireWeaponAndSendPacket @0x42bd80 — the is_authority split @0x42bdfd,
	//  the client arm @0x42bf46..0x42c074]
	ClientFiredRound stamped = round;
	// The retail producer reads the client network role's currentTick at the
	// fire action, before the next Client_ProcessNetworkFrame increment. It is
	// independent of the local World's logic clock, which starts after load.
	// [orig: NetPacket_WriteEntityPositionUpdate @0x42A62F]
	stamped.current_tick = current_tick_;
	gameplay_send_queue_.push_back(
			make_protocol_message(c2s::FIRED_ROUND, encode_client_fired_round(stamped)));
	return true;
}

bool ClientRuntime::queue_reload_request(const WeaponReload &reload) {
	if (role_ != Role::Joiner || joiner_ == nullptr || !joiner_->in_match() ||
	    !deployed_ || !joiner_->has_self_handle() ||
	    reload.entity_handle != joiner_->self_handle())
		return false;
	gameplay_send_queue_.push_back(
			make_protocol_message(c2s::WEAPON_RELOAD_REQUEST, encode_weapon_reload(reload)));
	return true;
}

bool ClientRuntime::queue_vehicle_attach(
		uint16_t vehicle_handle, uint8_t model_bone_index) {
	if (role_ != Role::Joiner || joiner_ == nullptr || !joiner_->in_match() ||
	    !deployed_ || !joiner_->has_self_handle() ||
	    vehicle_handle == 0xFFFFu || model_bone_index == 0)
		return false;
	const uint16_t self = joiner_->self_handle();
	gameplay_send_queue_.push_back(make_protocol_message(
			0x26,
			{
					static_cast<uint8_t>(self),
					static_cast<uint8_t>(self >> 8),
					static_cast<uint8_t>(vehicle_handle),
					static_cast<uint8_t>(vehicle_handle >> 8),
					model_bone_index,
					0,
			}));
	return true;
}

bool ClientRuntime::queue_vehicle_detach(uint16_t vehicle_handle) {
	if (role_ != Role::Joiner || joiner_ == nullptr || !joiner_->in_match() ||
	    !deployed_ || !joiner_->has_self_handle() || vehicle_handle == 0xFFFFu)
		return false;
	const uint16_t self = joiner_->self_handle();
	gameplay_send_queue_.push_back(make_protocol_message(
			0x27,
			{
					static_cast<uint8_t>(self),
					static_cast<uint8_t>(self >> 8),
					static_cast<uint8_t>(vehicle_handle),
					static_cast<uint8_t>(vehicle_handle >> 8),
					0,
					0,
			}));
	return true;
}

std::vector<netsim::ClientRoundEvent> ClientRuntime::drain_round_events() {
	return view_.drain_round_events();
}

std::vector<WeaponReload> ClientRuntime::drain_reload_notifications() {
	return view_.drain_weapon_reloads();
}

void ClientRuntime::seed_session(uint32_t session_id, uint32_t client_key,
                                 std::string client_scrk, std::string server_scrk,
                                 uint32_t next_seq, uint32_t last_ack,
                                 uint16_t self_handle, uint16_t self_type,
                                  uint32_t game_type, uint32_t tick_seed,
                                  bool replay_mode) {
	if (role_ != Role::Joiner) return;
	// A replayed client is mid-session: its network-role clock was already seeded by the
	// capture's S2C 0x61, so seed it here too or the tick stays parked at zero and every
	// replayed 0x06 stamps 0. [orig: @0x4297f8/@0x4297fd]
	current_tick_ = tick_seed;
	last_keepalive_tick_ = tick_seed;
	joiner_->seed_in_match(session_id, client_key, std::move(client_scrk),
	                       std::move(server_scrk), next_seq, last_ack, self_handle, self_type,
	                       game_type);
	view_.set_game_type(game_type);
	deployed_ = true;     // a seeded replay is post-deploy (the captured client was uplinking)
	replay_mode_ = replay_mode;
}

std::vector<std::vector<uint8_t>> ClientRuntime::run_frame(const PlayerExtendedUplink *uplink,
                                                           uint32_t now_tick) {
	std::vector<std::vector<uint8_t>> outbound;
	std::vector<ProtocolMessage> send_messages;

	// Per-frame tick bump — SKIPPED entirely while the clock is unseeded. The client's
	// network-role tick is anchored by the host's S2C 0x61 seed, never free-run from zero:
	// retail tests currentTick and jumps past both the increment and the keepalive leg
	// while it is 0. [orig: Client_ProcessNetworkFrame @0x42c193 cmp/jz -> loc_42C1F2;
	// ++ @0x42c1ab, store @0x42c1b5]
	if (current_tick_ != 0) ++current_tick_;

	// (0x34) keepalive — runs for EVERYONE (NOT authority-gated), emitted before the recv pump in the
	// original [orig @0x42c1a9..0x42c1ec]. Only a Joiner has a 0x43 framing path here (the HostClient's
	// own-loopback keepalive is a no-op over the wire — deferred-and-logged, P6 §5.44). Suppressed in
	// golden-replay mode.
	if (joiner_ != nullptr && !replay_mode_ && current_tick_ != 0 &&
	    current_tick_ - last_keepalive_tick_ > kKeepaliveInterval) {
		pre_send_queue_.push_back(
				make_protocol_message(c2s::KEEPALIVE, le32(current_tick_)));
		last_keepalive_tick_ = current_tick_;
	}

	// (1) RECV pump — fold S2C into ClientState, recv-before-send [orig: PumpClientProtocolRecv
	// @0x42c228 runs before the SEND block].
	if (role_ == Role::HostClient) {
		// The SP host's own loopback carries inner {tag,body} (ADR 0011 §3 SP crypto bypass).
		if (loopback_ != nullptr) {
			netsim::Datagram datagram;
			while (loopback_->client_recv(datagram)) {
				if (!apply_zone_timer_body(datagram.tag, datagram.body))
					view_.apply(datagram.tag, datagram.body);
			}
		}
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
			// JoinerConnection has already allocated sequence numbers for exact
			// admission packets and retained-session reconstruction. They still
			// leave through PumpClientProtocolSend: queue their wire images so a
			// field-3 update decoded later in this same receive result can close
			// the whole frame's send boundary without reframing them.
			for (std::vector<uint8_t> &reply : pr.outbound)
				framed_send_queue_.push_back(std::move(reply));
			for (const auto &tb : pr.inbound_world) view_.apply(tb.first, tb.second); // 0x0C/0x0D/0x10/0x20
			for (const auto &tb : pr.inbound_gameplay) view_.apply(tb.first, tb.second);
			// The host's tick seed anchors our whole network-role clock. A seed of ZERO is a
			// real, witnessed value (the round-end disarm form), so it is applied like any
			// other — it parks the tick, which is exactly what retail does.
			// [orig: NapiNPClientMsg_HandleSessionKey @0x4297c0 — both globals take the seed
			//  @0x4297f8/@0x4297fd; the disarm sender Server_SendRandomSeedToPlayer(ctx, 0)
			//  @0x510237]
			if (pr.tick_seed_set) {
				current_tick_ = pr.tick_seed;
				last_keepalive_tick_ = pr.tick_seed;
			}
			if (pr.send_holdoff_set)
				send_holdoff_countdown_ = pr.send_holdoff;
			for (const WeaponLoadout &grant : pr.loadout_grants) {
				authoritative_loadout_ = grant;
				++authoritative_loadout_revision_;
			}
			for (const JoinerConnection::ZoneTimerUpdate &update :
			     pr.zone_timer_updates) {
				if (const auto *value = std::get_if<ZoneTimerValue>(&update))
					apply_zone_timer_value(*value);
				else if (const auto *window =
				         std::get_if<ZoneTimerWindow>(&update))
					apply_zone_timer_window(*window);
			}
			// S2C 0x50 leg 2 — the decoded entity's Team on a non-authority client.
			// [orig: NapiNPClientMsg_0x050 @0x431910 team store @0x4319ee]
			for (const auto &assign : pr.entity_team_assigns)
				view_.apply_team_assign(assign.first, assign.second);
			if (pr.self_team_changed) ++self_team_revision_;
			// S2C 0x5D — the host's empty pool-0 slots. Retire the decoded rows so
			// the render pass AND the projectile person/dynamic proxies (which
			// deliberately ignore seen_this_frame) both drop the entity.
			// [orig: NapiNPClientMsg_DestroyEntityList @0x429730]
			for (uint16_t index : pr.destroyed_pool0_slots)
				view_.destroy_pool0_slot(index);
			// S2C 0x46 bit15 — roster bookkeeping only; the entity row stays.
			// [orig: PlayerSlot_ClearAndUnlink @0x434730 via @0x431411..0x43144c]
			for (uint8_t slot : pr.cleared_player_slots)
				cleared_player_slots_.push_back(slot);
			// The name-match may precede loadout by dozens of world-stream packets. Retail only
			// opens its uplink gate after that handle is known and the post-loadout 0x5A arrives.
			if (pr.reached_in_match) {
				deployed_ = true;
				++deployment_release_revision_;
			}
			for (const std::vector<uint8_t> &a : pr.inbound_0a) {
				const uint32_t health_before = view_.state().health_updates_applied;
				view_.apply(0x0A, a); // per-frame 0x0A
				// Evaluate each decoded tail in receive order. A later positive
				// sample in the same pump cannot erase an earlier death edge.
				if (view_.state().health_updates_applied != health_before &&
				    view_.state().local_health <= 0) {
					deployed_ = false;
					joiner_->begin_redeployment();
				}
			}
			// Periodic replies are produced by the receive handlers, but retail does not
			// frame them until PumpClientProtocolSend. Defer them behind the shared holdoff
			// gate. In particular, C2S 0x3D already pages the renderer-finalized loaded-model
			// snapshot; decoded S2C entity records must never rebuild or mutate that domain.
			for (ProtocolMessage &reply : pr.queued_send_messages) {
				pre_send_queue_.push_back(std::move(reply));
			}
		}
		// The recipient-specific 0x0A tail is the authoritative local health channel. Close the
		// deployed gate on a fresh death frame before this same client frame reaches its send block.
		// Positive health deliberately does not reopen it: respawn remains owned by the deploy flow.
	}

	// Retail advances this shared list once after the complete receive pump,
	// including on the authority's HostClient loopback path.
	// [orig: Client_ProcessNetworkFrame @0x42C2E1..0x42C2E6]
	advance_zone_timers();

	// The remote lean integrator runs once per client frame regardless of role —
	// the body tick that owns it in retail. [orig: decay @0x4b5c97, then the ramp
	// @0x4b7dbf/@0x4b7dd6]
	view_.tick_lean();
	// The remote arms dip rides the same body tick as the lean integrator.
	// [orig: lean @0x4b5c97 and dip @0x4b5cab, both inside Entity_UpdateInfantryPlayerBody]
	view_.tick_arms_dip();

	if (role_ == Role::HostClient) return outbound; // host: no connect-drive, no housekeeping send, no 0x0C

	// (0x4C) net-quality / anti-cheat report — after the recv pump; gated is_in_session &&
	// is_mp_session_peer (a Joiner in-match satisfies both). [orig @0x42c23e..0x42c279]
	if (!replay_mode_ && joiner_->in_match()) {
		if (++net_quality_timer_ > kNetQualityInterval) {
			net_quality_timer_ = 0;
			pre_send_queue_.push_back(make_protocol_message(
					0x4C, std::vector<uint8_t>{net_quality_}));
		}
	}

	// Vestigial/telemetry counter self-decrement [orig @0x42c386]. The 0x2C
	// send below sets it to 62, but the full @0x42C180 control flow never
	// compares it: after the field-3 gate @0x42C3DD, the deployed branch writes
	// 62 @0x42C412 and unconditionally queues 0x2C @0x42C44A. It therefore fires
	// every eligible send frame; retaining this write/decrement models retail
	// state, not a throttle.
	if (tag2c_send_cooldown_ > 0) --tag2c_send_cooldown_;

	// (2) SEND BLOCK — gated send_holdoff_countdown == 0 (NapiNPConnection+0x648; the original skips the
	// whole block when set). [orig @0x42c3dd]
	if (send_holdoff_countdown_ == 0) {
		// These packets already own the connection's earliest allocated
		// sequences. Preserve wire/retention fidelity by releasing them unchanged
		// and before framing any later semantic work below.
		while (!framed_send_queue_.empty()) {
			outbound.push_back(std::move(framed_send_queue_.front()));
			framed_send_queue_.pop_front();
		}
		// Input case 12 queues the deployment pick before Client_ProcessNetworkFrame.
		// Keep it first at this send boundary: prepare_deployment_pick records the
		// connection's next sequence, so the 0x0E must occupy that first packet even
		// when held challenge replies force the batch to split at the MTU.
		if (pending_deployment_pick_set_) {
			pending_deployment_pick_set_ = false;
			ProtocolMessage pick;
			if (joiner_->prepare_deployment_pick(
						pending_deployment_pick_, pick))
				send_messages.push_back(std::move(pick));
		}
		// The periodic producers above execute before this gate in retail, but
		// PumpClientProtocolSend is inside it. Preserve that queued-vs-sent
		// distinction: held housekeeping flushes on the first open boundary.
		while (!pre_send_queue_.empty()) {
			send_messages.push_back(
					std::move(pre_send_queue_.front()));
			pre_send_queue_.pop_front();
		}
		// The armory-ACCEPT loadout re-submission is another binding-queued
		// one-shot framed at this shared boundary.
		if (pending_loadout_resubmit_) {
			pending_loadout_resubmit_ = false;
			ProtocolMessage resubmit;
			if (joiner_->prepare_loadout_resubmit(resubmit))
				send_messages.push_back(std::move(resubmit));
		}
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
			send_messages.push_back(std::move(msg));
		}

		// (0x2C) RTT timestamp ping — Joiner only. [orig @0x42c3fa..0x42c44a]. Body = [u32 ts][u8 1]
		// (NetPacket_WriteInt32AndByte; echoFlag=1 requests the S2C 0x57 pong, §5.34). Retail uses
		// wall-clock milliseconds (GetTickCount), not the network-role simulation tick.
		if (!replay_mode_ && deployed_joiner) {
			tag2c_send_cooldown_ = kTag2CCooldown;
			std::vector<uint8_t> ping = le32(joiner_->monotonic_milliseconds32());
			ping.push_back(0x01);
			send_messages.push_back(
					make_protocol_message(c2s::RTT_CONSUMED, std::move(ping)));
		}

		// (0x0C) the C2S player uplink — unchanged P5 path, same deploy gate. [orig @0x42c46f..0x42c4a3]
		if (uplink != nullptr && deployed_joiner) {
			EntityPacketSubHeader sub;
			sub.handle = joiner_->self_handle();
			sub.item_type_id = joiner_->spawn_pose().item_type_id;
			sub.sub_op = ENTITY_SUB_OP_EXTENDED;
			std::vector<uint8_t> payload = encode_entity_packet_sub_header(sub);
			std::vector<uint8_t> extended = encode_player_extended_uplink(*uplink);
			payload.insert(payload.end(), extended.begin(), extended.end());
			send_messages.push_back(
					make_protocol_message(c2s::ENTITY_UPLINK, std::move(payload)));
		}
		for (std::vector<uint8_t> &datagram : joiner_->frame_messages(send_messages))
			outbound.push_back(std::move(datagram));
		// Connection send boundary: advance the pre-spawn drive, emit retained-message
		// active probes, and flush an ACK only if no substantive C2S producer above
		// already carried it. Retail places this pump inside the same field-3 gate.
		for (std::vector<uint8_t> &d : joiner_->pump(now_tick))
			outbound.push_back(std::move(d));
	} else {
		--send_holdoff_countdown_;
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
