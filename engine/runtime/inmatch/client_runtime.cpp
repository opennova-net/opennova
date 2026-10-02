#include <runtime/inmatch/client_runtime.h>
#include <runtime/devtools/tick_profile.h>
#include <runtime/inmatch/novaworld_link.h> // the NovaWorld exit

#include <net/npwire/wire_handle.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>

#include <runtime/world/world.h>

#include <base/io/le.h>
#include <base/io/tick_rate.h>

#include <algorithm>
#include <cstring>
#include <utility>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::inmatch {

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

// Little-endian u32 body (the 0x34 currentTick body and the 0x2C timestamp prefix).
std::vector<uint8_t> le32(uint32_t v) {
	std::vector<uint8_t> body;
	body.reserve(4);
	opennova::io::append_u32_le(body, v);
	return body;
}

const std::string &empty_runtime_string() {
	static const std::string empty;
	return empty;
}

// Chat_StripHtmlTags @0x4983F0: copy every character outside a `<...>` run;
// a `<` with no closing `>` swallows the rest of the line.
std::string chat_strip_tags(std::string_view src) {
	std::string out;
	out.reserve(src.size());
	size_t i = 0;
	while (i < src.size()) {
		const char ch = src[i];
		if (ch == '<') {
			while (i < src.size() && src[i] != '>') ++i;
			if (i >= src.size()) break;
			++i; // past the '>'
		} else {
			out.push_back(ch);
			++i;
		}
	}
	return out;
}

} // namespace

void ClientRuntime::reset_local_round_state() {
    // [orig: Game_InitNewRound @0x4227ce..0x4227d4]
    deployed_ = true;
    if (view_.state().local_medic_reviving) {
        view_.state().local_medic_reviving = false;
        view_.state().mark_changed();
    }
}

ClientRuntime::ClientRuntime(std::string player_name)
		: role_(Role::Joiner),
		  joiner_(std::make_unique<JoinerConnection>(std::move(player_name))) {
	// A remote joiner's folds STAGE and its rows chase (net-re §5.38e,
	// D-NET-196); the HostClient loopback keeps the snap fold (full-rate view;
	// the authority never interpolates, D-NET-89).
	view_.set_remote_motion_mode(true);
}

ClientRuntime::ClientRuntime(std::string player_name,
		JoinerConnection::MonotonicMilliseconds monotonic_milliseconds)
		: role_(Role::Joiner),
		  joiner_(std::make_unique<JoinerConnection>(
		          std::move(player_name), std::move(monotonic_milliseconds))) {
	view_.set_remote_motion_mode(true);
}

ClientRuntime::ClientRuntime(replication::ISessionTransport &host_loopback)
		: role_(Role::HostClient), loopback_(&host_loopback) {
	// The host's own player parses the retail header-only loopback 0x0A
	// (D-NET-140 closed) [orig: NapiNPClientMsg_0x00A @0x430174].
	view_.set_authority_recipient(true);
}

const std::string &ClientRuntime::server_name() const {
	return joiner_ ? joiner_->server_name() : empty_runtime_string();
}

const std::string &ClientRuntime::mission_name() const {
	return joiner_ ? joiner_->mission_name() : empty_runtime_string();
}

const SessionVars &ClientRuntime::session_vars() const {
	static const SessionVars kNone;
	return joiner_ ? joiner_->session_vars() : kNone;
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
	// The contest counts ride the same message onto the zone entity
	// [orig: @0x428e79/@0x428e7f].
	entry.contest_owner = value.byte544;
	entry.contest_other = value.byte545;
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
	adopt_tracked_window(window);
}

void ClientRuntime::adopt_tracked_window(const ZoneTimerWindow &window) {
	// [orig: NapiNPClientMsg_ZoneTimerWindow @0x428ae0, the cluster arm
	//  @0x428c39..0x428d60]
	const int32_t scaled_start = timer_scale_62(static_cast<int32_t>(window.start_s));
	const int32_t scaled_end = timer_scale_62(static_cast<int32_t>(window.end_s));
	bool adopt = false;
	bool reseed_progress = false;
	if (tracked_window_.tracked() && tracked_window_.zone == window.zone_handle) {
		// Same entity: a changed modeB re-seeds the progress (LABEL_35), the
		// same modeB keeps it (LABEL_36).
		adopt = true;
		reseed_progress = tracked_window_.mode_b != window.mode_b;
	} else if (!tracked_window_.tracked()) {
		adopt = true; // nothing tracked: take it, progress untouched
	} else {
		// Another entity: adopted only when at least as near as the tracked one
		// and within 20.0 u of the local player. The local position is the
		// recipient's 0x0A anchor (retail reads g_LocalPlayerEntity->Position).
		const replication::ClientState &cs = view_.state();
		auto dist_to = [&](uint16_t handle) -> double {
			const replication::ClientEntityState *row = cs.find(handle);
			if (row == nullptr) return 1.0e18;
			const double dx = std::fabs(static_cast<double>(cs.anchor_x) - row->x);
			const double dy = std::fabs(static_cast<double>(cs.anchor_y) - row->y);
			const double dz = std::fabs(static_cast<double>(cs.anchor_z) - row->z);
			return std::sqrt(dx * dx + dy * dy + dz * dz);
		};
		const double old_dist = dist_to(tracked_window_.zone);
		const double new_dist = dist_to(window.zone_handle);
		if (old_dist >= new_dist && new_dist <= 1310720.0) {
			adopt = true;
			reseed_progress = true;
		}
	}
	if (!adopt) return;
	if (reseed_progress) tracked_window_.progress = scaled_start;
	tracked_window_.rate = window.rate;
	tracked_window_.zone = window.zone_handle;
	tracked_window_.mode_a = window.mode_a;
	tracked_window_.mode_b = window.mode_b;
	tracked_window_.target = scaled_start;
	tracked_window_.limit = scaled_end;
	if (scaled_start >= scaled_end) {
		// A start at or past the end drops the cluster (progress survives).
		tracked_window_.rate = 0;
		tracked_window_.zone = 0xFFFF;
		tracked_window_.mode_a = 0;
		tracked_window_.mode_b = 0;
		tracked_window_.target = 0;
		tracked_window_.limit = 0;
	}
}

void ClientRuntime::advance_tracked_window() {
	// [orig: Client_ProcessNetworkFrame @0x42c2eb..0x42c347]
	TrackedCaptureWindow &w = tracked_window_;
	if (w.limit == 0) return;
	if (w.target == w.limit) {
		w.zone = 0xFFFF;
		w.target = 0;
		w.limit = 0;
		w.rate = 1;
		w.progress = 0;
		return;
	}
	w.progress = timer_add(w.progress, w.rate);
	if (w.rate > 0) {
		if (w.progress > w.target) w.progress = w.target;
	} else if (w.rate < 0) {
		if (w.progress < 0) w.progress = 0;
	}
}

void ClientRuntime::apply_zone_presence_count(
		const ZonePresenceCount &presence) {
	// The tracked cluster's rate override: only when the handle IS the tracked
	// entity [orig: NapiNPClientMsg_0x06C @0x428fc0, the compare @0x42902d].
	if (tracked_window_.tracked() && tracked_window_.zone == presence.zone_handle)
		tracked_window_.rate = presence.count;
	const auto found = zone_states_.find(presence.zone_handle);
	// The per-zone diagnostic image the tests read: retail resolves the
	// handle, then updates only when it is the currently selected timed-window
	// entity; a witnessed 0x53 is the selection prerequisite. [orig: @0x428FC0]
	if (found == zone_states_.end() || !found->second.has_window) return;
	found->second.has_presence = true;
	found->second.presence_count = presence.count;
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
	if (tag == s2c::ZONE_PRESENCE_COUNT) {
		ZonePresenceCount presence;
		if (decode_zone_presence_count(
					body.data(), body.size(), presence, consumed) &&
		    consumed == body.size())
			apply_zone_presence_count(presence);
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

void ClientRuntime::tick_roster_revive_countdown() {
	// The walk visits the S2C 0x4C pointer table's slots, in table order
	// [orig: Client_ProcessNetworkFrame @0x42C27E..0x42C2DA — g_PlayerSlotPtrTable
	//  entry+4 @0x42c29f..0x42c2cb].
	if (++slot_refresh_frames_ <= 62) return;
	replication::ClientState &cs = view_.state();
	for (const replication::ClientVisiblePlayer &entry : cs.visible_players) {
		replication::ClientRosterSlot &slot = cs.roster[entry.slot];
		// slot+0x0D active, slot+0x24 entity, slot+0x10 > 0 (unsigned).
		if (!slot.bound || slot.entity_slot < 0 || slot.downed_revive_seconds == 0)
			continue;
		// PlayerSlot_SetDownedState(seconds - 1, slot+0x2C): the medic-request
		// latch is re-stored unchanged [orig: @0x42C2B9..0x42C2C4].
		--slot.downed_revive_seconds;
		view_.state().mark_changed();
	}
	slot_refresh_frames_ = 0;
}

void ClientRuntime::drain_visible_refreshes() {
	// Each pair is QUEUED as one reliable C2S 0x22 {slot, fields} then the empty
	// C2S 0x23, retained until ACK (user param 0); a joiner's pair leaves inside
	// the next open send boundary with the rest of the queue, never framed (or
	// aging the flush counter) at receive time [orig:
	// CNapiNetwork_QueueReliableMessage(0x22, 1, 0, .., 3) @0x43181d / @0x431ae4,
	// then (0x23, 1, 0, .., 0) @0x43183e / @0x431b05]; the listen client's pair
	// rides its loopback.
	std::vector<replication::ClientVisiblePlayersRefresh> &pending =
			view_.state().pending_visible_refreshes;
	for (const replication::ClientVisiblePlayersRefresh &r : pending) {
		std::vector<uint8_t> sync{r.slot, static_cast<uint8_t>(r.fields & 0xFFu),
				static_cast<uint8_t>(r.fields >> 8)};
		if (role_ == Role::HostClient) {
			if (loopback_ == nullptr) continue;
			loopback_->client_send(c2s::PLAYER_SYNC_REQUEST, std::move(sync));
			loopback_->client_send(c2s::VISIBLE_PLAYERS_REQUEST, {});
			continue;
		}
		if (joiner_ == nullptr) continue;
		send_queue_.push_back(make_protocol_message(c2s::PLAYER_SYNC_REQUEST, std::move(sync)));
		send_queue_.push_back(make_protocol_message(c2s::VISIBLE_PLAYERS_REQUEST, {}));
	}
	pending.clear();
}

std::vector<uint8_t> ClientRuntime::start() {
	if (role_ != Role::Joiner || joiner_ == nullptr) return {};

	// ClientRuntime is reusable across disconnect/reconnect. All state below
	// belongs to the prior wire session and must be gone before the new Hello:
	// queued datagrams/actions must not authenticate under the new keys, and
	// authoritative/UI state must not leak into the next world.
	recv_fifo_.clear();
	framed_send_queue_.clear();
	send_queue_.clear();
	pending_reload_notifications_.clear();
	zone_states_.clear();
	tracked_window_ = TrackedCaptureWindow{};
	authoritative_loadout_ = WeaponLoadout{};
	authoritative_loadout_revision_ = 0;
	authoritative_ammo_pools_.fill(0);
	authoritative_ammo_pools_revision_ = 0;
	deployed_ = false;
	deployment_release_revision_ = 0;
	authoritative_spawn_released_ = false;
	authoritative_spawn_release_revision_ = 0;
	self_team_revision_ = 0;
	cleared_player_slots_.clear();
	current_tick_ = 0;
	slot_refresh_frames_ = 0;
	last_keepalive_tick_ = 0;
	net_quality_timer_ = 0;
	tag2c_send_cooldown_ = 0;
	send_holdoff_countdown_ = 0;
	send_holdoff_ticks_ = 0;
	send_pump_loop_ = JoinerConnection::SendPumpLoop::NetworkFrame;
	send_pump_loop_last_ms_ = 0;
	replay_mode_ = false;
	view_.state() = replication::ClientState{};
	// The client connection start resets the quality object [orig:
	// CNapiNetwork_StartClientConnection @0x4ca3f7 / @0x4ca401 ->
	// CNetQuality_Reset @0x4c58c0], its cooldown at the fresh clock.
	hud::net_quality_reset(net_indicators_, view_.state().local_clock_ms);
	view_.drain_round_events();
	view_.drain_game_events();
	view_.drain_weapon_reloads();
	view_.drain_script_remote_commands();
	view_.drain_objective_notifications();
	view_.drain_effect_commands();
	view_.set_game_type(0);
	view_.set_mp_session(true); // a joiner is in-session by definition
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
	    !is_deployed() || !joiner_->has_self_handle() ||
	    round.shooter_handle != joiner_->self_handle())
		return false;
	// NO freshness gate on this path. Retail's fire action splits on is_authority
	// (`cmp g_NapiNPCtx.is_authority / jz loc_42BF41` @0x42bdfd..0x42be03): only the
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
	ProtocolMessage fire = make_protocol_message(c2s::FIRED_ROUND, encode_client_fired_round(stamped));
	// The fire's retained node lives 62 send flushes (~1 s) and is then pruned
	// unsent — after a lossy stretch a stale shot is NOT replayed by the NACK
	// path, while a retail host would still apply it (no age gate on its side)
	// [orig: CNapiNetwork_QueueReliableMessage(ctx, 6, 62, ...) in
	//  Entity_FireWeaponAndSendPacket @0x42bd80; the deadline
	//  send_flush_counter + userParam - 1 in BuildOutgoingPackets @0x628430].
	fire.retention_flushes = 62;
	send_queue_.push_back(std::move(fire));
	return true;
}

bool ClientRuntime::queue_reload_request(const WeaponReload &reload) {
	if (role_ != Role::Joiner || joiner_ == nullptr || !joiner_->in_match() ||
	    !is_deployed() || !joiner_->has_self_handle() ||
	    wire_handle::is_batch_end_sentinel(reload.entity_handle))
		return false;
	send_queue_.push_back(
			make_protocol_message(c2s::WEAPON_RELOAD_REQUEST, encode_weapon_reload(reload)));
	return true;
}

bool ClientRuntime::queue_medic_request() {
	// The retail gate is is_in_session, not the deploy gate: a dead player is
	// back in the deploy flow (Driving) and the call still ships. It rides the
	// held one-shot queue that flushes at the next open send boundary rather
	// than the deploy-gated gameplay queue [orig: QueueReliableMessage(0x2E)
	// @0x49b50c, outside the 0x0C/0x2C deploy gate of Client_ProcessNetworkFrame].
	if (role_ != Role::Joiner || joiner_ == nullptr || !joiner_->in_session() ||
	    !joiner_->has_self_handle())
		return false;
	MedicRequest request;
	request.entity_index = joiner_->self_handle();
	ProtocolMessage medic = make_protocol_message(c2s::MEDIC_REQUEST, encode_medic_request(request));
	// A 310-flush (~5 s) finite lifetime like the 0x4C quality report
	// [orig: CNapiNetwork_QueueReliableMessage(ctx, 0x2E, 310, ...)].
	medic.retention_flushes = 310;
	send_queue_.push_back(std::move(medic));
	return true;
}

// The stance key SELECT QUEUES the change like every other input-side producer:
// [i16 action id] (0xA9 crouch / 0xAA prone / 0xAC stand) as one reliable C2S
// 0x1D, user param 0 = retained until ACK. It is framed only inside the next open
// send boundary, in queue order behind what was queued before it, so it neither
// mints a sequence between boundaries nor ages the flush counter. The server
// applies it with mutual exclusion.
// [orig: Input_HandleActionBinding_0 @0x4e0420, cases 169/170/172 @0x4e0d77/
//  @0x4e0df3/@0x4e0e3e -> NetPacket_WriteInt16C @0x4e0dca ->
//  CNapiNetwork_QueueReliableMessage(0x1D, 1, 0, .., 2) @0x4e0de7 ->
//  CNapiNPConnection_QueueMessage @0x628640 -> NapiNPMessage_Create @0x627fc0
//  (the connection's outgoing list only); applied by
//  NapiNPServerMsg_HandleStanceChange @0x501c60]
bool ClientRuntime::queue_stance_change(uint16_t action_id) {
	// The authority's own client queues it too: the senders carry no
	// is_authority test, and its connection is the local one.
	const bool host_path = role_ == Role::HostClient && loopback_ != nullptr;
	if (!host_path && (role_ != Role::Joiner || joiner_ == nullptr || !joiner_->in_match()))
		return false;
	std::vector<uint8_t> body;
	io::append_u16_le(body, action_id);
	send_queue_.push_back(make_protocol_message(c2s::STANCE_CHANGE, std::move(body)));
	return true;
}

bool ClientRuntime::queue_door_request(uint16_t handle, int16_t state, uint8_t section) {
	if (role_ != Role::Joiner || joiner_ == nullptr || !joiner_->in_match()) return false;
	DoorSlotAction request;
	request.entity_handle = handle;
	request.state = state;
	request.number = section;
	send_queue_.push_back(make_protocol_message(c2s::DOOR_SLOT_REQUEST,
			encode_door_slot_action(request)));
	return true;
}

// [orig: Chat_CheckFloodControl @0x498F60] The 16-entry table of recent lines
// `[u32 frame][char[64] text]`: a line longer than 59 characters is cut to 59
// first (`message[59] = 0` @0x498f80); an unseen line shifts the table down
// and lands in the newest slot (@0x498fe0..0x498fed); a seen line within
// 0x500 main frames of its entry is refused (@0x499028); an older repeat is
// moved to the newest slot, the entries after it shifting down (@0x49904a).
bool ClientRuntime::chat_flood_control(std::string &text, uint32_t frame) {
	if (text.size() > 0x3B) text.resize(59);
	size_t index = 0;
	while (index < chat_flood_.size() && chat_flood_[index].text != text) ++index;
	if (index >= chat_flood_.size()) {
		for (size_t i = 0; i + 1 < chat_flood_.size(); ++i) chat_flood_[i] = chat_flood_[i + 1];
		chat_flood_.back() = ChatFloodEntry{frame, text};
		return true;
	}
	if (frame - chat_flood_[index].frame <= 0x500u) return false;
	for (size_t i = index; i + 1 < chat_flood_.size(); ++i) chat_flood_[i] = chat_flood_[i + 1];
	chat_flood_.back() = ChatFloodEntry{frame, text};
	return true;
}

hud::ChatSendResult ClientRuntime::queue_chat_message(uint8_t channel, std::string &text,
		uint32_t frame) {
	using Result = hud::ChatSendResult;
	const bool joiner_path = role_ == Role::Joiner && joiner_ != nullptr && joiner_->in_session();
	const bool host_path = role_ == Role::HostClient && loopback_ != nullptr;
	if (!joiner_path && !host_path) return Result::Refused;
	// The local (13) and crew (11) senders test `!g_DeathScreenActive` alone;
	// the rest `(!g_DeathScreenActive || g_SpawnSuccessGate)`, the round-over
	// latch the folded 0x1D header raises [orig: @0x49A868 / @0x49A7A8;
	// @0x49A931 / @0x49A6E1 / @0x49AA81 / @0x49ACA1 / @0x49ABD1].
	const bool death_screen = view_.state().death_screen_active;
	const bool spawn_gate = view_.state().spawn_success_gate;
	if ((channel == 13 || channel == 11) ? death_screen : (death_screen && !spawn_gate))
		return Result::Refused;
	if (text.empty()) return Result::Refused; // [orig: `message && *message`]
	// A refused repeat: the sender echoes the line locally instead
	// [orig: the Chat_AddMessageChannel1 else-arms].
	if (!chat_flood_control(text, frame)) return Result::Flooded;
	// The red/blue pair sends only from a non-peer, and a HUD-bearing
	// process is always a session peer [orig: `if (!is_mp_session_peer)`
	// @0x49ACE2 / @0x49AC12].
	if (channel == 4 || channel == 5) return Result::Refused;
	const std::string stripped = chat_strip_tags(text); // [orig: Chat_StripHtmlTags]
	// NetPacket_WriteByteAndString: [u8 channel][cstr] [orig: @0x42A900].
	std::vector<uint8_t> body;
	body.reserve(stripped.size() + 2);
	body.push_back(channel);
	body.insert(body.end(), stripped.begin(), stripped.end());
	body.push_back(0);
	if (host_path) {
		// The listen host's client half queues it on its loopback — retail's
		// same QueueReliableMessage over transport mode 1 — to its own server.
		loopback_->client_send(c2s::CHAT_MESSAGE, std::move(body));
		return Result::Sent;
	}
	ProtocolMessage chat = make_protocol_message(c2s::CHAT_MESSAGE, std::move(body));
	// QueueReliableMessage(0xD, 1, 310): the same 310-flush finite lifetime
	// the 0x4C report and the medic call carry; the senders run outside the
	// client net frame, so the line rides the held one-shot queue.
	chat.retention_flushes = 310;
	send_queue_.push_back(std::move(chat));
	return Result::Sent;
}

bool ClientRuntime::queue_voice_menu_pick(uint8_t tag, int16_t value) {
	if (tag != c2s::EMOTE_REQUEST && tag != c2s::RADIO_CALL_REQUEST) return false;
	std::vector<uint8_t> body;
	io::append_i16_le(body, value); // [orig: @0x42c137 / @0x42c167]
	if (role_ == Role::HostClient && loopback_ != nullptr) {
		loopback_->client_send(tag, std::move(body));
		return true;
	}
	if (role_ != Role::Joiner || joiner_ == nullptr || !joiner_->in_session()) return false;
	ProtocolMessage pick = make_protocol_message(tag, std::move(body));
	pick.retention_flushes = 1; // the user param [orig: `push 1` @0x42c12c / @0x42c15c]
	send_queue_.push_back(std::move(pick));
	return true;
}

bool ClientRuntime::queue_team_change_request() {
	if (role_ == Role::HostClient && loopback_ != nullptr) {
		loopback_->client_send(c2s::TEAM_CHANGE_REQUEST, {});
	} else if (role_ == Role::Joiner && joiner_ != nullptr && joiner_->in_session()) {
		send_queue_.push_back(make_protocol_message(c2s::TEAM_CHANGE_REQUEST, {}));
	} else {
		return false;
	}
	view_.age_minimap_overlays(0x48A8u);
	return true;
}

// [orig: Game_ProcessMainFrame — `if (--dword_24D1DDC <= 0) { dword_24D1DDC =
//  62; if (is_in_session) { CNetQuality_UpdateMetrics(); CNetQuality_SetLevel
//  (&g_NetQuality, level); } }`, ahead of the client net frame]. The peer
// (RECEIVE) half of CNetQuality_UpdateMetrics @0x4C52C0: cleared while any of
// g_NetSpawnSuspended / dword_81474C / g_SpawnSuccessGate /
// g_PreRoundDelayTimer holds the peer, else one sample of frame pressure,
// the ping ring's mean and the loss counter; the combined scalar is
// max(host, client) and the host window never runs on a non-authority, so it
// folds as 0. The loss counter (stru_A86920.aimPoint.Y) is read-and-zeroed
// here and NOTHING in the binary increments it: the term is the floor 1.
int32_t ClientRuntime::mission_exit_reason() const {
	if (mission_exit_reason_ != kMissionExitNone) return mission_exit_reason_;
	if (joiner_ != nullptr && joiner_->has_disconnect_event()) {
		return mission_exit_reason_for_disconnect(joiner_->last_disconnect_event());
	}
	return kMissionExitNone;
}

void ClientRuntime::update_net_quality() {
	if (--quality_update_countdown_ > 0) return;
	quality_update_countdown_ = 62;
	if (joiner_ == nullptr || !joiner_->in_session()) return;
	// The NovaWorld exit leads the block [orig: @0x52655d..0x52657c].
	if (novaworld_session_ended(novaworld_link_.novaworld, novaworld_link_.nwu_in_use,
				novaworld_link_.nwu_session_role)) {
		mission_exit_reason_ = kMissionExitNovaWorld;
	}
	const bool held = !deployed_ || !authoritative_spawn_released_ ||
			view_.state().preround_delay_seconds != 0;
	if (held) {
		replication::net_quality_window_clear(client_quality_window_);
	} else {
		replication::net_quality_window_push(client_quality_window_,
				replication::net_quality_bandwidth_metric(observed_frame_rate_),
				replication::net_quality_client_ping_metric(joiner_->client_average_ping_ms()),
				replication::net_quality_loss_metric(0.0));
	}
	const int32_t combined = std::max(0, client_quality_window_.quality);
	// CNetQuality_SetLevel(&g_NetQuality, level) [orig: @0x52659b]: the level
	// the 0x4C report carries and the quality icon's channels ramp toward.
	hud::net_quality_set_level(net_indicators_, replication::net_quality_level(combined));
	replication::ClientNetQuality &net = view_.state().net;
	net.level = net_quality_level();
	net.ping_ms = joiner_->client_ping_ms();
	net.average_ping_ms = joiner_->client_average_ping_ms();
}

void ClientRuntime::set_net_quality_level(int32_t level) {
	hud::net_quality_set_level(net_indicators_, level);
}

void ClientRuntime::raise_net_quality_link_errors(uint32_t mask) {
	apply_net_quality_link_errors(mask);
}

// The link-error callbacks at the clock they fire on: bit 0 raises flag 1
// (outgoing), bit 1 flag 2 (incoming) [orig: CNetQuality_SetLinkErrorFlag
// @0x4c34f0, which Network_LogOutgoingPacketError @0x4c4943 / sub_4C62A0
// @0x4c62ce and Network_LogIncomingPacketError @0x4c48b3 / @0x4c468f inline].
void ClientRuntime::apply_net_quality_link_errors(uint32_t mask) {
	const uint32_t now = view_.state().local_clock_ms;
	if ((mask & kNetQualityLinkErrorOutgoing) != 0)
		hud::net_quality_set_flag(net_indicators_, hud::kNetLinkErrorOutgoing, now);
	if ((mask & kNetQualityLinkErrorIncoming) != 0)
		hud::net_quality_set_flag(net_indicators_, hud::kNetLinkErrorIncoming, now);
}

bool ClientRuntime::queue_mounted_weapon_slot_selection(bool use_parent_slot) {
	if (role_ != Role::Joiner || joiner_ == nullptr || !joiner_->in_match() ||
	    !is_deployed() || !joiner_->has_self_handle())
		return false;
	MountedWeaponSlotSelection selection;
	selection.use_parent_slot = use_parent_slot;
	send_queue_.push_back(make_protocol_message(
			c2s::MOUNTED_WEAPON_SLOT_SELECT,
			encode_mounted_weapon_slot_selection(selection)));
	return true;
}

bool ClientRuntime::queue_vehicle_attach(
		uint16_t vehicle_handle, uint8_t model_bone_index) {
	if (role_ != Role::Joiner || joiner_ == nullptr || !joiner_->in_match() ||
	    !is_deployed() || !joiner_->has_self_handle() ||
	    vehicle_handle == 0xFFFFu || model_bone_index == 0)
		return false;
	const uint16_t self = joiner_->self_handle();
	send_queue_.push_back(make_protocol_message(
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
	    !is_deployed() || !joiner_->has_self_handle() || vehicle_handle == 0xFFFFu)
		return false;
	const uint16_t self = joiner_->self_handle();
	send_queue_.push_back(make_protocol_message(
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

std::vector<replication::ClientRoundEvent> ClientRuntime::drain_round_events() {
	return view_.drain_round_events();
}

std::vector<replication::ClientGameEvent> ClientRuntime::drain_game_events() {
	return view_.drain_game_events();
}



std::vector<ScriptRemoteCommand> ClientRuntime::drain_script_remote_commands() {
	return view_.drain_script_remote_commands();
}

std::vector<ObjectiveNotification> ClientRuntime::drain_objective_notifications() {
	return view_.drain_objective_notifications();
}

std::vector<WeaponReload> ClientRuntime::drain_reload_notifications() {
	std::vector<WeaponReload> notifications;
	notifications.swap(pending_reload_notifications_);
	// Preserve the public view() seam for callers that deliberately fold a body
	// directly instead of running the framed receive pump.
	std::vector<WeaponReload> direct = view_.drain_weapon_reloads();
	notifications.insert(notifications.end(), direct.begin(), direct.end());
	return notifications;
}

void ClientRuntime::stage_reload_notifications_before_body_tick() {
	std::vector<WeaponReload> decoded = view_.drain_weapon_reloads();
	for (const WeaponReload &reload : decoded) {
		// The retail receive handler applies this side effect immediately. It is
		// therefore visible to Entity_UpdateInfantryPlayerBody later in the same
		// Game_ProcessMainFrame. Self reloads take the WeaponSlot_ReloadAmmo branch;
		// remote non-Person refill remains deliberately unmodeled here.
		// [orig: NapiNPClientMsg_WeaponReload_0x049 @0x42c0a0;
		//  remote Person stamp @0x42c10b; Client_ProcessNetworkFrame @0x526692;
		//  Entity_UpdateAllEntities @0x52674b]
		if (role_ == Role::Joiner &&
				(!joiner_->has_self_handle() ||
					reload.entity_handle != joiner_->self_handle())) {
			replication::ClientEntityState *peer =
					view_.state().find(reload.entity_handle);
			if (peer != nullptr &&
					(peer->cls == EntityClass::Player ||
						peer->cls == EntityClass::Infantry))
				peer->arms_dip_ticks = 80;
		}
		pending_reload_notifications_.push_back(reload);
	}
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
	view_.set_mp_session(true);
	deployed_ = true;     // a seeded replay is post-deploy (the captured client was uplinking)
	authoritative_spawn_released_ = true;
	replay_mode_ = replay_mode;
}

std::vector<std::vector<uint8_t>> ClientRuntime::run_frame(
		const PlayerExtendedUplink *uplink, uint32_t now_tick) {
	devtools::ProfileLap lap(profile_);
	std::vector<std::vector<uint8_t>> outbound;
	std::vector<ProtocolMessage> send_messages;
	// Session loss is a terminal owner state, not only a receive-side event. A
	// prior host description (or the silence reap below) must stop the next frame
	// before its tick/housekeeping producers can mint fresh C2S traffic.
	if (joiner_ != nullptr && joiner_->session_lost()) {
		recv_fifo_.clear();
		framed_send_queue_.clear();
		send_queue_.clear();
		pending_reload_notifications_.clear();
		return outbound;
	}

	// The main frame's 62-frame quality fold runs ahead of the client net frame
	// (the level it stores is what the 0x4C leg below reports).
	if (role_ == Role::Joiner) update_net_quality();
	// Then the connection indicators step once, in a session, still ahead of
	// the client net frame whose callbacks raise their flags [orig:
	// Game_ProcessMainFrame — `cmp is_in_session` @0x526680 ->
	// CNetQuality_UpdateIndicators @0x52668d, Client_ProcessNetworkFrame
	// @0x526692]. The session word is the view's: a joiner is always in one,
	// the listen host's own client only on a networked host.
	if (view_.mp_session()) hud::net_quality_update_indicators(net_indicators_, novaworld_link_);

	// Per-frame tick bump — SKIPPED entirely while the clock is unseeded. The client's
	// network-role tick is anchored by the host's S2C 0x61 seed, never free-run from zero:
	// retail tests currentTick and jumps past both the increment and the keepalive leg
	// while it is 0. [orig: Client_ProcessNetworkFrame @0x42c193 cmp/jz -> loc_42C1F2;
	// ++ @0x42c1ab, store @0x42c1b5]
	if (current_tick_ != 0) ++current_tick_;
	// The client's local millisecond clock the S2C 0x58 fold stamps and the
	// RULES text's elapsed read compares (retail's GetTickCount): free-running
	// one fixed tick per client net frame, independent of the seeded tick
	// above [orig: SessionStatus_ParseFromBuffer @0x5310a5 stamps
	// GetTickCount; SessionStatus_GetElapsedMS @0x52d5fc reads it].
	view_.state().local_clock_ms += static_cast<uint32_t>(io::kTickMs);

	// (0x34) keepalive — runs for EVERYONE (NOT authority-gated), emitted before the recv pump in the
	// original [orig @0x42c1a9..0x42c1ec]. Only a Joiner has a 0x43 framing path here (the HostClient's
	// own-loopback keepalive is a no-op over the wire — deferred-and-logged, P6 §5.44). Suppressed in
	// golden-replay mode.
	if (joiner_ != nullptr && !replay_mode_ && current_tick_ != 0 &&
	    current_tick_ - last_keepalive_tick_ > kKeepaliveInterval) {
		send_queue_.push_back(
				make_protocol_message(c2s::KEEPALIVE, le32(current_tick_)));
		last_keepalive_tick_ = current_tick_;
	}
	// A joiner that has heard nothing for three whole seconds raises the
	// incoming link error every frame, ahead of the receive pump [orig:
	// Client_ProcessNetworkFrame @0x42c1f8..0x42c21e — `is_mp_session_peer &&
	// !is_authority && CNapiNetwork_GetSessionUptime(ctx) > 2`; that getter
	// @0x4c6ed0 is (GetTickCount() - napi_conn+0x5E8) / 1000, the connection's
	// last-admitted-receive stamp, not a session uptime].
	if (role_ == Role::Joiner && joiner_ != nullptr &&
			joiner_->milliseconds_since_last_receive() / 1000u > 2u)
		hud::net_quality_set_flag(net_indicators_, hud::kNetLinkErrorIncoming,
				view_.state().local_clock_ms);
	lap.mark(devtools::Slot::SIM_CLIENT_SETUP);

	// (1) RECV pump — fold S2C into ClientState, recv-before-send [orig: PumpClientProtocolRecv
	// @0x42c228 runs before the SEND block].
	if (role_ == Role::HostClient) {
		// The SP host's own loopback carries inner {tag,body} (ADR 0011 §3 SP crypto bypass).
		if (loopback_ != nullptr) {
			replication::Datagram datagram;
			while (loopback_->client_recv(datagram)) {
				if (!apply_zone_timer_body(datagram.tag, datagram.body))
					view_.apply(datagram.tag, datagram.body);
				// The host's own client takes its looped-back tick seed like any
				// client: the clock its own fire is admitted against. A short body
				// reads 0. [orig: NapiNPClientMsg_HandleSessionKey @0x4297c0 --
				//  no role gate; g_ClientCurrentTick / g_LastKeepaliveTick
				//  @0x4297f8/@0x4297fd]
				if (datagram.tag == s2c::TICK_SEED) {
					current_tick_ = datagram.body.size() >= 4 ? io::read_u32_le(datagram.body.data()) : 0u;
					last_keepalive_tick_ = current_tick_;
				}
				// The world-state load's completion burst carries this
				// client's first player-slot refresh: C2S 0x22 {0, 0x5CF7}
				// then the 0x23 snapshot request. The burst's other members
				// (0x28, 0x29, 0x2D, 0x32) are the host self-stream's own
				// residual. [orig: NapiNPClientMsg_0x00F @0x42e66c..0x42e6ab]
				if (datagram.tag == s2c::WORLD_STATE_LOAD) {
					loopback_->client_send(c2s::PLAYER_SYNC_REQUEST, {0x00, 0xF7, 0x5C});
					loopback_->client_send(c2s::VISIBLE_PLAYERS_REQUEST, {});
				}
			}
			drain_visible_refreshes();
		}
		// Host authority already spawned every accepted round/refill. Its decoded
		// listen-client replica pipeline must not retain duplicate visual gameplay
		// events (the header-only loopback 0x0A carries no round events at all).
		// Deaths too: the authority's own damage pass ran the death chain
		// (and its 0x13 broadcast skips the loopback — mask 0x90 NOT_HOST).
		view_.drain_weapon_reloads();
	} else {
		// A remote joiner: framed datagrams. JoinerConnection decodes the 0x83 SESSION envelope and
		// surfaces the inner bodies, which we fold via ClientReplicaPipeline::apply (the single remote-wire
		// fold path; pump(transport) is the loopback path — exactly one is active per role).
		while (!recv_fifo_.empty()) {
			std::vector<uint8_t> dg = std::move(recv_fifo_.front());
			recv_fifo_.pop_front();
			JoinerConnection::PollResult pr = joiner_->handle_datagram(dg.data(), dg.size());
			// A 0x84's rebuilt packets and a 0x85's pong are written to the socket by
			// their opcode handlers, inside this receive pump: they leave at this
			// datagram's position, ahead of (and regardless of) the field-3 gate below
			// [orig: NapiNP_HandleResendList -> CNapiNPConnection_SendSessionPacket
			//  @0x6239b6; Nwu_HandlePing -> CNapiNPConnection_SendPing @0x623c6f].
			for (std::vector<uint8_t> &sent_now : pr.immediate_outbound)
				outbound.push_back(std::move(sent_now));
			// A 0x84 resend list that named a sequence is the outgoing link
			// error, at its datagram's position [orig: NapiNP_HandleResendList
			// @0x6239ef..0x623a37 -> cb_client_3 = Network_LogOutgoingPacketError
			// @0x4c4920 -> flag 1].
			apply_net_quality_link_errors(joiner_->take_net_quality_link_errors());
			if (joiner_->session_lost()) {
				// Host teardown sends its keyed goodbye burst synchronously, then
				// destroys both pending and outgoing semantic queues. It does not
				// wait for the normal field-3 send boundary, and nothing queued
				// before or after the terminal record may follow the goodbyes
				// (what earlier datagrams of this pump already wrote stays sent).
				// [orig: CNapiNPConnection_TeardownActiveConnection @0x6253C0 ->
				//  NapiNPDSPQueue_ClearPendingList @0x62556B;
				//  NapiNPDSPQueue_ClearOutgoing @0x625574]
				recv_fifo_.clear();
				framed_send_queue_.clear();
				send_queue_.clear();
				pending_reload_notifications_.clear();
				for (std::vector<uint8_t> &goodbye : pr.outbound)
					outbound.push_back(std::move(goodbye));
				return outbound;
			}
			// 0x08/0x7B update the reducer's layout at their wire position.
			// The connection's final game type may belong to a later message
			// in this datagram and must not reinterpret an earlier body.
			view_.set_viewer_handle(joiner_->has_self_handle()
					? joiner_->self_handle()
					: 0xFFFFu);
			// The spectate walk starts from and skips the local player
			// [orig: Spectator_CycleTarget_0 @0x52ac56 / @0x52ad09].
			view_.set_spectate_local_handle(joiner_->has_self_handle()
					? joiner_->self_handle()
					: 0xFFFFu);
			view_.set_mp_attributes(joiner_->mp_attributes());
			view_.set_local_player_slot(joiner_->local_player_slot());
			// JoinerConnection has already allocated sequence numbers for the exact
			// handshake/admission packets. They still leave through
			// PumpClientProtocolSend: queue their wire images so a field-3 update
			// decoded later in this same receive result can close the whole frame's
			// send boundary without reframing them.
			for (std::vector<uint8_t> &reply : pr.outbound)
				framed_send_queue_.push_back(std::move(reply));
			// World, live-frame, map, and gameplay bodies cross one reducer stream
			// in wire order. The family vectors on PollResult are diagnostics only.
			// Every handler queues what it asks the host for while it runs, so the
			// fold's own requests and the connection's replies join the one queue
			// entry by entry, in the datagram's wire order.
			// [orig: inside PumpClientProtocolRecv @0x42c228: the 0x16 handler's 0x22
			//  @0x42fc35, the 0x6A walk's 0x4E @0x43266c, the 0x0A handler's 0x0F
			//  @0x4307E9, the 0x57 pong @0x43226d]
			std::size_t reply = 0;
			auto queue_replies_after = [&](std::size_t entries_folded) {
				while (reply < pr.queued_send_messages.size() &&
				       (reply >= pr.queued_send_after.size() ||
				        pr.queued_send_after[reply] <= entries_folded))
					send_queue_.push_back(std::move(pr.queued_send_messages[reply++]));
			};
			auto queue_fold_requests = [&] {
				// A record the fold dropped over an unresolvable slot or carrier asks
				// the host for that entity at once (reply: S2C 0x18, §5.46).
				// [orig: NapiNPClientMsg_0x00A @0x4307E9 -> QueueReliableMessage(0x0F,
				//  1, 0); the vehicle record @0x4608b3]
				queue_carrier_repair_requests();
				// Every 0x16 row whose connection slot the roster has not bound yet
				// is dropped by the reducer and re-requested: one reliable C2S 0x22
				// {slot, 0x1CF7} per dropped row
				// [orig: NapiNPClientMsg_PlayerList @0x42fc05..0x42fc3a ->
				//  CNapiNetwork_QueueReliableMessage(ctx, 0x22, 1, 0,
				//  {slot, 0xF7, 0x1C}, 3) @0x42fc35].
				std::vector<uint8_t> &sync_retries =
						view_.state().scoreboard.pending_sync_requests;
				for (const uint8_t slot : sync_retries) {
					send_queue_.push_back(make_protocol_message(
							c2s::PLAYER_SYNC_REQUEST, std::vector<uint8_t>{slot, 0xF7, 0x1C}));
				}
				sync_retries.clear();
				// Each S2C 0x6A action-3 walk reply queues the next step of the
				// clan-registry walk: one reliable C2S 0x4E {netId}
				// [orig: NapiNPClientMsg_HandlePlayerJoinLeave ->
				//  CNapiNetwork_QueueReliableMessage(ctx, 0x4E, 1, 0, {netId}, 4)
				//  @0x43265c..0x43266c].
				std::vector<uint32_t> &clan_walk = view_.state().pending_clan_walk_requests;
				for (const uint32_t net_id : clan_walk) {
					ClanRosterWalkRequest request;
					request.after_account_id = net_id;
					send_queue_.push_back(make_protocol_message(
							c2s::GAME_START_ACK, encode_clan_roster_walk_request(request)));
				}
				clan_walk.clear();
				drain_visible_refreshes();
			};
			queue_replies_after(0);
			std::size_t entries_folded = 0;
			bool death_edge_in_poll = false;
			for (const auto &tb : pr.inbound_reducer) {
				const uint32_t health_before =
						tb.first == s2c::PER_FRAME_UPDATE
						? view_.state().health_updates_applied : 0;
				view_.apply(tb.first, tb.second);
				// Every 0x0F a joiner handles clears the link errors and holds
				// new ones off for 10 s [orig: NapiNPClientMsg_0x00F — the
				// `!is_authority` burst @0x42e5b7, CNetQuality_SetLinkErrorFlag
				// (&g_NetQuality, 4) @0x42e660].
				if (tb.first == s2c::WORLD_STATE_LOAD)
					hud::net_quality_set_flag(net_indicators_, hud::kNetLinkErrorClear,
							view_.state().local_clock_ms);
				// Evaluate every 0x0A tail at its original position. A later
				// positive sample cannot erase an earlier death edge.
				if (tb.first == s2c::PER_FRAME_UPDATE &&
						view_.state().health_updates_applied != health_before &&
						view_.state().local_health <= 0) {
					death_edge_in_poll = true;
					authoritative_spawn_released_ = false;
					joiner_->begin_redeployment();
				}
				queue_fold_requests();
				queue_replies_after(++entries_folded);
			}
			queue_fold_requests();
			queue_replies_after(pr.inbound_reducer.size());
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
			if (pr.send_holdoff_set) {
				// The CS handler stores the dictated period only. The earlier
				// join-response leg reset this connection's counter to zero, so
				// the first boundary remains open; PumpFlags reloads the period
				// only after that send. A later CS update likewise does not move
				// an already-running boundary.
				// [orig: HandleCSConfigUpdate @0x621940;
				//  ResetSendHoldoffCounter @0x61e140;
				//  PumpFlags reload @0x6297f3..0x629802]
				send_holdoff_ticks_ = pr.send_holdoff;
			}
			for (const WeaponLoadout &grant : pr.loadout_grants) {
				authoritative_loadout_ = grant;
				++authoritative_loadout_revision_;
			}
			// The 0x0F pool image follows the 0x5A grants in retail's burst; the
			// embedder applies it AFTER the slot rebuild, as the client handler's
			// copy-then-recalc does [orig: NapiNPClientMsg_0x00F @0x42e324/@0x42e424].
			if (pr.ammo_pools_set) {
				authoritative_ammo_pools_ = pr.ammo_pools;
				++authoritative_ammo_pools_revision_;
			}
			for (const JoinerConnection::ZoneTimerUpdate &update :
			     pr.zone_timer_updates) {
				if (const auto *value = std::get_if<ZoneTimerValue>(&update))
					apply_zone_timer_value(*value);
				else if (const auto *window =
				         std::get_if<ZoneTimerWindow>(&update))
					apply_zone_timer_window(*window);
				else if (const auto *presence =
				         std::get_if<ZonePresenceCount>(&update))
					apply_zone_presence_count(*presence);
			}
			// The local-team latch has client-session consequences beyond entity state.
			if (pr.self_team_changed) ++self_team_revision_;
			// S2C 0x46 bit15 — roster bookkeeping only; the entity row stays.
			// [orig: PlayerSlot_ClearAndUnlink @0x434730 via @0x431411..0x43144c]
			for (uint8_t slot : pr.cleared_player_slots)
				cleared_player_slots_.push_back(slot);
			// WeaponLoadout_ApplyFromBuffer clears dword_81474C for EVERY valid
			// S2C 0x5A. That is distinct from the causal spawn release below.
			if (pr.gameplay_release_applied || pr.reached_in_match) deployed_ = true;
			// The owner-ID match may precede loadout by dozens of world-stream packets. Retail only
			// opens its authoritative spawn latch after H and the applicable deployment release meet.
			if (pr.reached_in_match) {
				++deployment_release_revision_;
				if (!authoritative_spawn_released_) {
					authoritative_spawn_released_ = true;
					++authoritative_spawn_release_revision_;
				}
			}
			if (pr.gameplay_hold_rearmed) deployed_ = false;
			// A death edge decoded in THIS poll keeps the spawn latch closed even
			// when the same datagram carried a (re)covering deployment release:
			// the joiner was just reset into redeployment, and only the deploy
			// flow reopens the latch. The release edges above are batched per
			// poll, so without this hold they would out-order the wire-position
			// 0x0A tail that closed the latch inside the reducer loop.
			if (death_edge_in_poll) authoritative_spawn_released_ = false;
			// (The receive handlers' replies joined the queue during the fold above; retail
			// frames them only at PumpClientProtocolSend. C2S 0x3D already pages the
			// renderer-finalized loaded-model snapshot; decoded S2C entity records must
			// never rebuild or mutate that domain.)
		}
		// The receive pump ends with the connection's missing-sequence check: a future S2C
		// packet seen in this batch whose gap the batch did not close sends ONE C2S 0x44 now,
		// every client frame, outside the field-3 gate (finish_receive_pump carries the
		// witness). A request that named a sequence raises the incoming link error at that
		// send [orig: NapiNPProtocol_PumpRecvQueues @0x6269bb..0x6269d6 ->
		//  CNapiNPConnection_SendMissingSeqList @0x623780..0x6237bd -> cb_client_2 =
		//  Network_LogIncomingPacketError @0x4c4890 -> flag 2].
		std::vector<uint8_t> missing_request = joiner_->finish_receive_pump();
		if (!missing_request.empty()) outbound.push_back(std::move(missing_request));
		apply_net_quality_link_errors(joiner_->take_net_quality_link_errors());
		// The recipient-specific 0x0A tail is the authoritative local health channel. Close the
		// deployed gate on a fresh death frame before this same client frame reaches its send block.
		// Positive health deliberately does not reopen it: respawn remains owned by the deploy flow.
	}
	lap.mark(devtools::Slot::SIM_CLIENT_RECEIVE);
	if (role_ == Role::Joiner) {
		stage_reload_notifications_before_body_tick();
		// The session var the 0x81 tone ladder reads, mirrored from the
		// connection's server-info landing [orig: g_SessionVarExpFanfare].
		view_.state().exp_fanfare = joiner_->exp_fanfare();
	}

	// The roster revive countdown precedes the zone-list advance in the frame
	// [orig: Client_ProcessNetworkFrame @0x42C27E..0x42C2DA, then @0x42C2E1].
	tick_roster_revive_countdown();
	// Retail advances this shared list once after the complete receive pump,
	// including on the authority's HostClient loopback path, then pumps the
	// tracked-window cluster.
	// [orig: Client_ProcessNetworkFrame @0x42C2E1..0x42C2E6, then @0x42C2EB]
	advance_zone_timers();
	advance_tracked_window();
	view_.refresh_minimap_live_markers();
	const bool preround_active = view_.state().preround_delay_seconds != 0;
	// Client_ProcessNetworkFrame and its maintenance continue, but the later
	// Entity_UpdateAllEntities body is skipped while the phase-0 mirror is
	// nonzero. These portable movers are that entity body, not network work.
	// [orig: network pump @0x526692; entity gate @0x52672C]
	// The remote lean integrator runs once per client frame regardless of role —
	// the body tick that owns it in retail. [orig: decay @0x4b5c97, then the ramp
	// @0x4b7dbf/@0x4b7dd6]
	if (!preround_active) view_.tick_lean();
	// The remote arms dip rides the same body tick as the lean integrator.
	// [orig: lean @0x4b5c97 and dip @0x4b5cab, both inside Entity_UpdateInfantryPlayerBody]
	if (!preround_active) view_.tick_arms_dip();
	// [orig: Entity_UpdateAllEntities @0x4C2221 -> sub_590950]
	if (!preround_active && view_.state().tracked_target.ticks_remaining)
		--view_.state().tracked_target.ticks_remaining;

	// The per-class between-update mover: one step per 62.5 Hz tick after the
	// recv fold (retail order: net frame first, entity movers after). No-op on
	// the HostClient role (mode never enabled — the authority never
	// interpolates, D-NET-89). [net-re §5.38e, D-NET-196]
	if (!preround_active)
		view_.tick_remote_motion(joiner_ != nullptr && joiner_->has_self_handle()
		                                 ? joiner_->self_handle()
		                                 : 0xFFFFu);
	lap.mark(devtools::Slot::SIM_CLIENT_MAINTENANCE);

	if (role_ == Role::HostClient) {
		// host: no connect-drive, no housekeeping send, no 0x0C. Its send block
		// opens every frame, so the held one-shots leave here, after the
		// receive fold: the local connection has no peer address, and its own
		// server dictates such a connection a one-tick holdoff in both
		// directions whatever the session's period (LAN lanmode 1 and
		// NovaWorld dictate 12 to the rest). Its datagram lands in the
		// manager's FIFO, which only the NEXT frame's head drains toward the
		// server tick's receive pump.
		// [orig: NapiNPServer_UpdateHoldoffTicks -- `cmp byte ptr [esi+2Eh], 0`
		//  @0x4c5f53, 1 for both directions @0x4c5f59..0x4c5f69; +0x2E = (peer
		//  address +0x30 == 0) @0x62bf1c..0x62bf21 (NapiNPProtocol_HandleClientJoin)
		//  and @0x62607f..0x626093 (CNapiNPConnection_OnStateChange);
		//  Client_ProcessNetworkFrame @0x42c3dd -> PumpClientProtocolSend
		//  @0x42c4bc; CNapiNPConnection_SendSessionPacket @0x61f039 ->
		//  CNapiNPManager_SendTo `addr == 0` @0x61ec59 -> NapiFifo_WritePacketAtomic
		//  @0x61eccd; CNapiGameSession_CreateSession @0x4c9b9c..0x4c9c67 sets no
		//  address; Game_ProcessMainFrame -> CNapiNetwork_PumpManagerReceive @0x526528]
		while (!send_queue_.empty()) {
			ProtocolMessage &held = send_queue_.front();
			loopback_->client_send(held.tag, std::move(held.payload));
			send_queue_.pop_front();
		}
		return outbound;
	}

	// (0x4C) net-quality / anti-cheat report — after the recv pump; gated is_in_session &&
	// is_mp_session_peer (a Joiner in-match satisfies both). [orig @0x42c23e..0x42c279]
	if (!replay_mode_ && joiner_->in_match()) {
		if (++net_quality_timer_ > kNetQualityInterval) {
			net_quality_timer_ = 0;
			ProtocolMessage quality = make_protocol_message(
					0x4C, std::vector<uint8_t>{net_quality_level()});
			// This is neither indefinitely reliable nor one-send: retail passes
			// userParam=310 and ages it in open logical send boundaries.
			// [orig: QueueReliableMessage @0x42C279 -> user_param1 310]
			quality.retention_flushes = kNetQualityInterval;
			send_queue_.push_back(std::move(quality));
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
	// whole block when set). [orig @0x42c3dd] The transport recv pump decrements
	// the countdown once per tick BEFORE this gate [orig: PumpFlags 0x10 via
	// PumpClientProtocolRecv flags=26 @0x4c4fe0], and the send pump RE-ARMS it
	// from the stored CS field-3 period when the boundary opened [orig: PumpFlags
	// 0x200 reload-when-0 @0x629802 via PumpClientProtocolSend flags=738
	// @0x4c5000] — so the uplink period is exactly send_holdoff_ticks (a
	// NovaWorld host dictates 12 → ~5.2 Hz; LAN lanmode 2/3/4 → 6/4/3; an
	// unconfigured connection stays 0 = per-tick). A joiner that forgot the
	// period after one skip flooded retail hosts at 12x their expected rate.
	// Until mission loading ends the joiner is not in this frame at all but in
	// the loading loop of its admission stage, which calls the send pump its own
	// way (step_send_pump_loop, D-NET-254).
	const bool send_block_open = step_send_pump_loop();
	if (send_block_open) {
		// These packets already own the connection's earliest allocated
		// sequences. Preserve wire/retention fidelity by releasing them unchanged
		// and before framing any later semantic work below.
		// One build sends at most cs_dir0 field 14's packets (the template's -1 is
		// unbounded); what it leaves stays queued for the next build, in order.
		// [orig: BuildOutgoingPackets @0x62844e, @0x62860b..0x628619]
		const std::size_t build_budget =
				max_packets_per_build(joiner_->session_timeouts().max_packets_per_tick);
		std::size_t packets_built = 0;
		while (!framed_send_queue_.empty() && packets_built < build_budget) {
			outbound.push_back(std::move(framed_send_queue_.front()));
			framed_send_queue_.pop_front();
			++packets_built;
		}
		// The connection's one queue, in the order its producers ran: the housekeeping,
		// the receive handlers' replies, the input one-shots, the deploy pick (input case
		// 12 queues it @0x49b17b), the armory's loadout re-submit (@0x42d085) and the
		// gameplay records. Once queued they leave at this boundary whatever the deploy
		// gate now says (D-NET-235): each producer gated itself when it queued, only the
		// 0x2C and 0x0C builds below sit behind the gate, and nothing drains the list on
		// death.
		// [orig: CNapiNetwork_QueueReliableMessage @0x4c4fa0 -> CNapiNPConnection_QueueMessage
		//  @0x628640; Client_ProcessNetworkFrame tail @0x42c4b1 -> PumpClientProtocolSend
		//  @0x42c4bc in every branch; DrainMessageQueues @0x625600 runs only at join
		//  and destroy]
		while (!send_queue_.empty()) {
			send_messages.push_back(std::move(send_queue_.front()));
			send_queue_.pop_front();
		}
		// The witnessed deploy gate the 0x2C RTT ping and the 0x0C uplink share: is_in_session &&
		// !is_authority && !dword_81474C && !g_SpawnSuccessGate. A Joiner is always !is_authority;
		// deployed_ and authoritative_spawn_released_ model those two independent gates.
		const bool deployed_joiner =
				joiner_->in_match() && is_deployed() && joiner_->has_self_handle();

		// (0x2C) RTT timestamp ping — Joiner only. [orig @0x42c3fa..0x42c44a]. Body = [u32 ts][u8 1]
		// (NetPacket_WriteInt32AndByte; echoFlag=1 requests the S2C 0x57 pong, §5.34). Retail uses
		// wall-clock milliseconds (GetTickCount), not the network-role simulation tick.
		if (!replay_mode_ && deployed_joiner) {
			tag2c_send_cooldown_ = kTag2CCooldown;
			std::vector<uint8_t> ping = le32(joiner_->monotonic_milliseconds32());
			ping.push_back(0x01);
			ProtocolMessage ping_message = make_protocol_message(
					c2s::RTT_CONSUMED, std::move(ping));
			ping_message.reliable = false; // Client_ProcessNetworkFrame @0x42C44A userParam=1
			send_messages.push_back(std::move(ping_message));
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
			ProtocolMessage uplink_message = make_protocol_message(
					c2s::ENTITY_UPLINK, std::move(payload));
			uplink_message.reliable = false; // Client_ProcessNetworkFrame @0x42C4A3 userParam=1
			send_messages.push_back(std::move(uplink_message));
		}
		JoinerConnection::FrameMessagesResult framed;
		if (packets_built < build_budget) {
			framed = joiner_->frame_messages_detailed(send_messages,
					joiner_->packet_ceiling_bytes(), build_budget - packets_built);
		} else {
			framed.unbuilt = std::move(send_messages);
		}
		for (std::vector<uint8_t> &datagram : framed.datagrams)
			outbound.push_back(std::move(datagram));
		if (framed.frame_failed) {
			// Capacity rejection is final, like retail's failed per-node Create.
			// Only an admitted node that could not be encoded/framed remains owned
			// by this runtime and is retried ahead of later producers.
			for (std::size_t i = framed.framed_count;
			     i < framed.admitted_count; ++i) {
				send_queue_.push_back(std::move(send_messages[i]));
			}
		}
		for (auto it = framed.unbuilt.rbegin(); it != framed.unbuilt.rend(); ++it)
			send_queue_.push_front(std::move(*it));
		// Connection send boundary: advance the pre-spawn drive, emit retained-message
		// active probes, and flush an ACK only if no substantive C2S producer above
		// already carried it. Retail places this pump inside the same field-3 gate
		// (the 0x44 request is not here: the receive pump above sends it).
		for (std::vector<uint8_t> &d : joiner_->pump(now_tick))
			outbound.push_back(std::move(d));
		// Retail builds every packet at connection+0x64C, prunes message nodes
		// against that same value, then increments it exactly once. MTU splits
		// therefore remain one flush, and held frames never age finite records.
		joiner_->complete_send_flush();
		// (step_send_pump_loop re-armed the countdown from the stored dictated
		// period when it counted out [orig: the PumpFlags 0x200 reload-when-0
		// @0x629802]: the in-match boundary reopens every send_holdoff_ticks_
		// ticks, 0 = per-tick.)
	}
	// The stale-carrier sweep runs in the entity update, after the client net frame,
	// so its 0x0F requests queue behind this frame's send block and leave at the next
	// boundary ahead of that frame's producers. Our movers step inside run_frame,
	// before the block; queue what they raised here.
	// [orig: Entity_UpdateTransformAndTurret @0x440e29 -> QueueReliableMessage(0x0F);
	//  Game_ProcessMainFrame: Client_ProcessNetworkFrame @0x526692, then
	//  Entity_UpdateAllEntities @0x52674b]
	queue_carrier_repair_requests();
	lap.mark(devtools::Slot::SIM_CLIENT_SEND);
	return outbound;
}

void ClientRuntime::queue_carrier_repair_requests() {
	for (const uint16_t handle : view_.drain_carrier_repair_requests()) {
		std::vector<uint8_t> body;
		body.push_back(static_cast<uint8_t>(handle & 0xFFu));
		body.push_back(static_cast<uint8_t>(handle >> 8));
		send_queue_.push_back(make_protocol_message(c2s::ENTITY_INFO_QUERY, std::move(body)));
	}
}

// One frame of the joiner's send cadence. The frame's receive pump steps a
// running countdown first [orig: PumpFlags 0x10 @0x6297ac via
// PumpClientProtocolRecv flags=26]; what follows depends on the loop the
// admission stage waits in (JoinerConnection::send_pump_loop):
//   - a frame loop calls the send pump once, and its build needs the countdown
//     at zero: the in-match gate `cmp [conn+648h],0; ja` skips the call, the
//     join state machine's UI frames call it and PumpEnumeratorAndSend skips
//     the build, the same cadence [orig: @0x42c3dd; the `!+0x648` test @0x62927b];
//   - a busy spin passes receive + send every few microseconds, so a running
//     countdown is out before the next datagram and every frame builds;
//   - a timed loop's entry pump builds only on a countdown already at zero (the
//     template holdoff 0); after that its passes run the countdown out and the
//     send pump is called, and builds, once MORE than the pace passed since its
//     last call, measured like retail on the 32-bit tick (signed difference).
// The countdown itself keeps the frame cycle in every loop: it reloads the
// dictated period whenever it counts out [orig: PumpFlags 0x200 reload-when-0
// @0x6297f3..0x629802]. Its value when loading ends depends on how many passes
// the loops ran, which no frame model can know; the frame cycle is the
// deterministic choice, and the one the in-match gate then continues.
// [orig: SaveFile_SendAndWaitForServerAck @0x5204b0 (stamp, entry pump, `> 50`);
//  NapiClient_WaitForDisconnect @0x42cb20 (stamp @0x42cb70, entry pump @0x42cb4d,
//  `> 100` @0x42cbb5); CNapiGameSession_InitRandomSeedOrRequest @0x51e8f0 and
//  NapiClient_WaitForGameStart @0x42cc10 (send every pass)]
bool ClientRuntime::step_send_pump_loop() {
	const bool counted_out = send_holdoff_countdown_ <= 1;
	if (send_holdoff_countdown_ > 0) --send_holdoff_countdown_;
	if (counted_out) send_holdoff_countdown_ = send_holdoff_ticks_;
	if (joiner_ == nullptr) return counted_out;
	const JoinerConnection::SendPumpLoop loop = joiner_->send_pump_loop();
	const bool entered = loop != send_pump_loop_;
	send_pump_loop_ = loop;
	const int32_t pace_ms = JoinerConnection::send_pump_loop_pace_ms(loop);
	if (loop == JoinerConnection::SendPumpLoop::NetworkFrame) return counted_out;
	if (pace_ms == 0) return true; // a busy spin
	const uint32_t now_ms = joiner_->monotonic_milliseconds32();
	if (entered) {
		send_pump_loop_last_ms_ = now_ms;
		return counted_out;
	}
	if (static_cast<int32_t>(now_ms - send_pump_loop_last_ms_) <= pace_ms) return false;
	send_pump_loop_last_ms_ = now_ms;
	return true;
}

std::vector<std::vector<uint8_t>>
ClientRuntime::Client_ProcessNetworkFrame(const PlayerExtendedUplink &uplink,
		uint32_t now_tick) {
	return run_frame(&uplink, now_tick);
}

std::vector<std::vector<uint8_t>> ClientRuntime::Client_ProcessNetworkFrame(
		uint32_t now_tick) {
	return run_frame(nullptr, now_tick);
}

} // namespace opennova::inmatch
