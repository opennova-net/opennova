#include <runtime/inmatch/server_net_quality.h>

#include <net/npwire/ingame_decode.h>   // kPlayerSyncHasQuality
#include <net/npwire/ingame_encode.h>   // encode_player_sync
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/replication_model.h>
#include <net/npwire/session_hello.h>   // DisconnectEvent
#include <runtime/inmatch/server_tick.h> // Server_StageHostDisconnect, CONTROL_REQUEST_LIVE_GATE_TICKS
#include <runtime/replication/net_quality.h>
#include <runtime/world/world.h>

namespace opennova::inmatch {

namespace {

constexpr uint32_t kPingRingSamples = 10;
constexpr uint32_t kPingRingCursor = 10;  // +100408 = entry 10 of the +100368 ring
constexpr uint16_t kPingStrikeLimit = 20; // strictly-greater after the increment
constexpr uint32_t kPingChatMin = 36;
constexpr uint32_t kPingChatMax = 37;
constexpr uint32_t kNetQualitySampleFrames = 62; // dword_24D1DDC reload

// CNapiNPConnection_SendChatMessage(conn, "", code, tag) is the chat-coded
// punt: the same H:0x03 description the join watchdog stages.
// [orig: CNapiNPConnection_SendChatMessage @0x4C7EF0]
void stage_ping_punt(NapiNPConnection &conn, uint32_t code, const char *tag) {
	DisconnectEvent event;
	event.ds = 1;
	event.dc = 2;
	event.dpc = code;
	event.ddstr = tag;
	Server_StageHostDisconnect(conn, event);
}

} // namespace

void Server_RecordPingSample(const GameConfig &config, NapiNPConnection &conn,
		uint32_t sent_ms, uint32_t now_ms, bool in_session) {
	SessionReplyState &st = conn.reply;
	const bool local_slot = conn.link.mode == replication::TransportMode::Loopback;
	st.rtt_ms = now_ms - sent_ms;                 // @0x515121
	if (local_slot) st.rtt_ms = 0;                // slot+5 (the local slot) @0x515129
	// ++cursor; > 10 -> 0; store at cursor. Cursor 10 is the cursor dword.
	uint32_t &cursor = st.rtt_ring[kPingRingCursor];
	if (++cursor > kPingRingSamples) cursor = 0;  // @0x515141..0x515143
	st.rtt_ring[cursor] = st.rtt_ms;              // @0x515155 (aliases the cursor at 10)
	// The policy [orig: @0x515171]: in session, not the local slot, and the
	// NetPlayer's +216 word clear (no writer on this host).
	if (!in_session || local_slot) return;
	if (config.do_min_ping_check) {               // @0x515183
		if (st.rtt_ms >= config.min_ping) {
			st.min_ping_strikes = 0;                  // @0x5151C9
		} else if (static_cast<int16_t>(++st.min_ping_strikes) > kPingStrikeLimit) {
			// slot+96481 is read-but-never-set on this host, so the second gate
			// is always open.
			stage_ping_punt(conn, kPingChatMin, "minping"); // @0x5151C2
		}
	}
	if (config.do_max_ping_check) {               // @0x5151D6
		if (st.rtt_ms <= config.max_ping) {
			st.max_ping_strikes = 0;                  // @0x515224
		} else if (static_cast<int16_t>(++st.max_ping_strikes) > kPingStrikeLimit) {
			stage_ping_punt(conn, kPingChatMax, "maxping"); // @0x51521A
		}
	}
}

uint32_t Server_PlayerAveragePingMs(const NapiNPConnection &conn) {
	uint32_t sum = 0;
	for (uint32_t i = 0; i < kPingRingSamples; ++i) sum += conn.reply.rtt_ring[i];
	return sum / kPingRingSamples;
}

void Server_StoreClientQuality(NapiNPConnection &conn, uint8_t reported) {
	// The handler clamps the byte above 4 before the setter, which clamps
	// again to 0..4 and dirties on change only. [orig: @0x5111FD; @0x5006E6..0x500707]
	const uint8_t level = reported > 4u ? uint8_t{4} : reported;
	const uint8_t previous = conn.reply.client_quality;
	conn.reply.client_quality = level;
	if (previous != level) conn.reply.client_quality_dirty = true;
}

void Server_EmitQualityResends(NapiNPServerCtx &ctx, const world::World &world) {
	// [orig: @0x51DE79] is_in_session && !g_preround_delay_timer &&
	// !g_spawn_success_gate (the latter is not copied into this context).
	if (!ctx.is_in_session || world.preround_delay_seconds != 0) return;
	std::vector<NapiNPConnection> &roster = ctx.np_protocol.connection_list;
	const int32_t capacity = static_cast<int32_t>(roster.size());
	if (capacity == 0) return;
	int32_t cursor = ctx.quality_broadcast_slot_cursor;
	if (cursor < 0 || cursor >= capacity) cursor = 0; // @0x51DEB0..0x51DEC4
	int sent = 0;
	for (int32_t walked = 0; walked < capacity; ++walked) {
		NapiNPConnection &conn = roster[static_cast<size_t>(cursor)];
		// active && dirty && connected [orig: @0x51DECF]
		if (is_in_match(conn) && conn.reply.client_quality_dirty &&
				conn.link.transport != nullptr) {
			conn.reply.client_quality_dirty = false;   // @0x51DEE5
			PlayerReplicationState rep;
			rep.player_slot = conn.reply.player_slot;
			rep.entity_handle = conn.link.owned_entity.packed;
			rep.quality = conn.reply.client_quality;
			const std::vector<uint8_t> body =
					encode_player_sync(rep, kPlayerSyncHasQuality); // @0x51DF05 (1024)
			// send_mask 128 [orig: @0x51DF0A]: every active player slot.
			for (NapiNPConnection &recipient : roster) {
				if (!is_in_match(recipient) || recipient.link.transport == nullptr) continue;
				recipient.link.transport->host_send(s2c::PLAYER_SYNC, body);
			}
			if (++sent >= 8) break;                    // @0x51DF1B
		}
		if (++cursor >= capacity) cursor = 0;         // @0x51DF2F..0x51DF3E
	}
	ctx.quality_broadcast_slot_cursor = cursor;
}

void Server_SampleHostNetQuality(NapiNPServerCtx &ctx) {
	// The 62-frame countdown runs every frame; the sample is in-session only.
	// [orig: Game_ProcessMainFrame @0x52656A..0x52658B]
	if (ctx.net_quality_sample_countdown > 0) --ctx.net_quality_sample_countdown;
	if (ctx.net_quality_sample_countdown > 0) return;
	ctx.net_quality_sample_countdown = kNetQualitySampleFrames;
	if (!ctx.is_in_session) return;
	// The pre-round hold clears the send window instead of sampling it
	// [orig: CNetQuality_UpdateMetrics @0x4C52C0 — `g_net_spawn_suspended ||
	//  g_spawn_success_gate || g_preround_delay_timer` -> CNetStats_ClearSendCounters
	//  @0x4C2F50]; the two spawn gates have no host-side model here.
	if (ctx.world != nullptr && ctx.world->preround_delay_seconds != 0) {
		replication::net_quality_window_clear(ctx.host_quality_window);
		ctx.host_network_quality = 0;
		return;
	}
	// The send window's three inputs [orig: @0x4C531B..0x4C54C6].
	uint32_t ping_sum = 0;
	uint32_t active = 0;
	for (const NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		// *(player-1) && !*player (not the local slot) && !slot+96478 &&
		// slot+383 >= 1860 [orig: @0x4C53C0..0x4C53F1]
		if (!is_in_match(conn) ||
				conn.link.mode == replication::TransportMode::Loopback ||
				conn.reply.control_live_ticks < CONTROL_REQUEST_LIVE_GATE_TICKS)
			continue;
		uint32_t mean = Server_PlayerAveragePingMs(conn);
		if (mean > 0xFFu) mean = 0xFFu;               // per-slot clamp @0x4C540B
		ping_sum += mean;
		++active;
	}
	// With no eligible slot the ping and loss samples are stored as 0, not the
	// floor 1 [orig: LABEL_23 — `*(this + cursor + 17) = 0; *(this + cursor + 22) = 0`].
	// The slots' loss counters (+100359) are unmodeled: an eligible slot reads 0,
	// which the loss term floors to 1.
	const uint32_t avg_ping = active != 0 ? ping_sum / active : 0u;
	replication::net_quality_window_push(ctx.host_quality_window,
			replication::net_quality_bandwidth_metric(
					static_cast<int32_t>(io::kTicksPerSecondInt)),
			active != 0 ? replication::net_quality_host_ping_metric(avg_ping) : 0,
			active != 0 ? replication::net_quality_loss_metric(0.0) : 0);
	// S2C 0x79 carries CNetQuality+0x0C, the send window's folded quality.
	ctx.host_network_quality =
			static_cast<uint8_t>(ctx.host_quality_window.quality & 0xFF);
}

} // namespace opennova::inmatch
