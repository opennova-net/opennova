// The authority's METPROTOCOL gate reports (gate_metrics_reports.h, D-NET-NEW-L9-22).
#include <runtime/inmatch/gate_metrics_reports.h>

#include <base/gameprofile/game_type.h> // host_abbreviation_key (GAMETYPE)
#include <base/io/crt_ftol.h>           // retail_atol (TZB)
#include <base/io/strutil.h>            // iequals (Napi_StrCaseEqual)
#include <net/npwire/session_hello.h>   // ClientAuth, parse_client_cu_chunk
#include <runtime/inmatch/napi_np_server_ctx.h>

#include <algorithm>
#include <string_view>
#include <utility>
#include <vector>

namespace opennova::inmatch {

namespace {

// Napi_CopyString(dst, src, cap): at most cap - 1 characters, stopping at an inner NUL.
// [orig: Napi_CopyString @0x617E10]
std::string copy_capped(std::string_view src, size_t cap) {
	const size_t nul = src.find('\0');
	if (nul != std::string_view::npos) src = src.substr(0, nul);
	return std::string(src.substr(0, cap - 1));
}

struct SlotCounts {
	int32_t players = 0;
	int32_t team1 = 0;
	int32_t team2 = 0;
};

constexpr uint32_t kGameTypeTeamBit = 0x10000u; // g_GameType & 0x10000: a team game

// [orig: CServerTick_UpdateMaxPlayerCounts @0x4FA130 - @0x4FA15C..0x4FA1B1]
SlotCounts slot_counts(const NapiNPServerCtx &ctx) {
	SlotCounts counts;
	const bool team_game = (ctx.config.game_type & kGameTypeTeamBit) != 0;
	auto count = [&](bool local, uint8_t team) {
		// The local slot counts only on a host that is a session peer [orig: @0x4FA180].
		if (local && ctx.is_mp_session_peer == 0) return;
		++counts.players;
		if (!team_game) return;
		if (team == 1) ++counts.team1;
		else if (team == 2) ++counts.team2;
	};
	// No slot is a bot here: the bot byte (+96483) has no writer on this host.
	for (const NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (!player_slot_active(conn)) continue;
		count(slot_is_local(conn), conn.assigned_team_valid ? conn.assigned_team : uint8_t{0});
	}
	for (const OrphanedPlayerRow &orphan : ctx.orphaned_player_rows) {
		const NapiNPConnection &row = orphan.row;
		count(false, row.assigned_team_valid ? row.assigned_team : uint8_t{0});
	}
	if (ctx.host_slot.active) count(true, ctx.host_slot.team);
	return counts;
}

void construct_clock(const NapiNPServerCtx &ctx, GateServerTick &tick) {
	const SlotCounts counts = slot_counts(ctx);
	tick.construct(ctx.tick_count_ms, ctx.cc_bin, static_cast<int32_t>(ctx.config.max_players),
			counts.players, counts.team1, counts.team2);
}

// The reporters' common gates: in a session on the NovaWorld network type, with a sink to send
// through (the gate address is the sink's test).
// [orig: Server_SendServerMetricsToGate @0x4FA4EA; Server_SendMissionMetrics @0x4FB3DA;
//  Server_SendPlayerMetricsToGate @0x4FAA61..0x4FAA8F; Server_SendPlayerMissionMetrics
//  @0x4FB92A..0x4FB958]
bool reporting(const NapiNPServerCtx &ctx) {
	return ctx.is_in_session != 0 && ctx.transport_mode == NetworkType::NovaWorld &&
			ctx.gate_metrics.send && ctx.gate_metrics.host;
}

// GameType_GetAbbreviation(g_GameType, 1): the GateTypeAbbrev text of the session's game type.
// [orig: Server_SendServerMetricsToGate @0x4FA5F8]
std::string game_type_text(const GateMetricsSink &sink, uint32_t game_type) {
	return hud::game_text(sink.game_text, "GateTypeAbbrev",
			game_type::host_abbreviation_key(game_type), "");
}
std::string game_type_text(const NapiNPServerCtx &ctx) {
	return game_type_text(ctx.gate_metrics, ctx.config.game_type);
}

// The player strings of a slot's net player: its ClientAuth strings and its PCID.
GateMetricsPlayer player_strings(const NapiNPConnection &net_player) {
	GateMetricsPlayer player = net_player.gate_player;
	player.pid = net_player.account.pcid;
	return player;
}

// [orig: Server_SendServerMetricsToGate @0x4FA4B0]
void Server_SendServerMetricsToGate(const NapiNPServerCtx &ctx) {
	if (!reporting(ctx)) return;
	ctx.gate_metrics.send(gate_metrics_server_text(ctx.gate_metrics.host(), game_type_text(ctx),
			ctx.gate_clocks.process, ctx.tick_count_ms));
}

// [orig: Server_SendMissionMetrics @0x4FB3A0]
void Server_SendMissionMetrics(const NapiNPServerCtx &ctx) {
	if (!reporting(ctx)) return;
	ctx.gate_metrics.send(gate_metrics_server_mission_text(ctx.gate_metrics.host(),
			game_type_text(ctx), ctx.gate_clocks.mission, ctx.tick_count_ms));
}

// [orig: Server_SendPlayerMetricsToGate @0x4FAA30 - the local and bot bytes @0x4FAA7D, the net
//  player +0x1C @0x4FAA96; GCC byte_C87044 (the process clock's)]
void Server_SendPlayerMetricsToGate(const NapiNPServerCtx &ctx, const NapiNPConnection &slot,
		const NapiNPConnection &net_player) {
	if (!reporting(ctx) || slot_is_local(slot)) return;
	ctx.gate_metrics.send(gate_metrics_player_text(ctx.gate_metrics.host(), game_type_text(ctx),
			ctx.gate_clocks.process.gcc, slot.gate_tick, player_strings(net_player),
			ctx.tick_count_ms));
}

// [orig: Server_SendPlayerMissionMetrics @0x4FB8F0 - the active byte @0x4FB91B, the local and bot
//  bytes @0x4FB946, the net player @0x4FB964, the slot state 6 or 7 @0x4FB975]
void Server_SendPlayerMissionMetrics(const NapiNPServerCtx &ctx, const NapiNPConnection &slot,
		const NapiNPConnection *net_player) {
	if (!reporting(ctx) || slot_is_local(slot) || net_player == nullptr) return;
	if (slot.slot_state != PlayerSlotState::Joined && slot.slot_state != PlayerSlotState::RoundOver)
		return;
	ctx.gate_metrics.send(gate_metrics_player_mission_text(ctx.gate_metrics.host(),
			game_type_text(ctx), ctx.gate_clocks.mission, slot.gate_mission_tick,
			player_strings(*net_player), ctx.tick_count_ms));
}

} // namespace

GateMetricsPlayer parse_client_gate_metrics_player(const ClientAuth &auth) {
	GateMetricsPlayer player;
	for (const auto &blob : auth.cu) {
		uint8_t cu_type = 0;
		std::string name;
		std::string value;
		if (!parse_client_cu_chunk(blob.data(), blob.size(), cu_type, name, value) || cu_type != 2)
			continue;
		if (strutil::iequals(name, "VERSIONSTRING")) player.version = copy_capped(value, 32);
		else if (strutil::iequals(name, "COUNTRYCODE")) player.country_code = copy_capped(value, 8);
		else if (strutil::iequals(name, "CNT")) player.country_name = copy_capped(value, 32);
		else if (strutil::iequals(name, "LNG")) player.language = copy_capped(value, 32);
		else if (strutil::iequals(name, "TZB")) player.tz_bias = io::retail_atol(value.c_str());
	}
	return player;
}

void gate_update_max_player_counts(const NapiNPServerCtx &ctx, GateServerTick &tick) {
	const SlotCounts counts = slot_counts(ctx);
	tick.update_max_player_counts(counts.players, counts.team1, counts.team2);
}

void gate_clocks_session_create(NapiNPServerCtx &ctx, std::string cc_bin) {
	ctx.cc_bin = std::move(cc_bin);
	construct_clock(ctx, ctx.gate_clocks.process);
	construct_clock(ctx, ctx.gate_clocks.mission);
}

void gate_clocks_mission_start(NapiNPServerCtx &ctx) {
	if (ctx.is_authority == 0) return;
	ctx.gate_clocks.process.set_state(1, ctx.tick_count_ms);
	ctx.gate_clocks.mission.set_state(1, ctx.tick_count_ms);
}

void gate_clocks_mission_loaded(NapiNPServerCtx &ctx, uint32_t preround_delay_seconds) {
	if (ctx.is_authority == 0) return;
	// `cmp ecx(0), g_PreRoundDelayTimer; sbb esi, esi; add esi, 3` @0x525E2F..0x525E3E
	const uint32_t state = preround_delay_seconds != 0 ? 2u : 3u;
	ctx.gate_clocks.process.set_state(state, ctx.tick_count_ms);
	ctx.gate_clocks.mission.set_state(state, ctx.tick_count_ms);
}

void gate_slot_clocks_player_added(NapiNPServerCtx &ctx, NapiNPConnection &slot) {
	construct_clock(ctx, slot.gate_tick);
	construct_clock(ctx, slot.gate_mission_tick);
}

void gate_clocks_player_added_marks(NapiNPServerCtx &ctx) {
	gate_update_max_player_counts(ctx, ctx.gate_clocks.process);
	gate_update_max_player_counts(ctx, ctx.gate_clocks.mission);
}

void gate_slot_clocks_game_state(NapiNPConnection &net_player, uint8_t new_state, uint32_t now,
                                 uint32_t preround_delay_seconds) {
	if (net_player.burst.game_state == new_state) return;
	switch (new_state) {
	case 9:
		net_player.gate_tick.set_state(1, now);
		net_player.gate_mission_tick.set_state(1, now);
		break;
	case 10:
		net_player.gate_tick.set_state(preround_delay_seconds != 0 ? 2u : 3u, now);
		net_player.gate_mission_tick.set_state(preround_delay_seconds != 0 ? 2u : 3u, now);
		break;
	case 11:
		net_player.gate_tick.set_state(4, now);
		net_player.gate_mission_tick.set_state(4, now);
		break;
	default: break;
	}
}

void Server_ProcessPeriodicTimerUpdate(NapiNPServerCtx &ctx) {
	GateMetricsClocks &clocks = ctx.gate_clocks;
	gate_update_max_player_counts(ctx, clocks.process);
	gate_update_max_player_counts(ctx, clocks.mission);
	clocks.process.close_phase(ctx.tick_count_ms);
	clocks.mission.close_phase(ctx.tick_count_ms);
	// The NovaWorld leg between them (@0x4FC061..0x4FC07A) calls an unnamed stub with 0 that
	// returns -1 [orig: the stub @0x4CE130, `or eax, -1; retn 4`]: nothing to port.
	Server_SendServerMetricsToGate(ctx);
	clocks.process.reset(ctx.tick_count_ms);
	clocks.mission.reset(ctx.tick_count_ms);
}

void Server_ReportPlayerLeave(NapiNPServerCtx &ctx, NapiNPConnection &slot) {
	if (ctx.is_authority == 0 || slot_is_local(slot)) return;
	slot.gate_tick.set_state(0, ctx.tick_count_ms);
	Server_SendPlayerMetricsToGate(ctx, slot, slot);
}

void Server_ReportHeldPlayerLeave(const GateMetricsSink &sink, bool novaworld, uint32_t game_type,
                                  const std::string &process_gcc, uint32_t now_ms,
                                  NapiNPConnection &slot) {
	if (slot_is_local(slot)) return;
	slot.gate_tick.set_state(0, now_ms);
	if (!novaworld || !sink.send || !sink.host) return;
	sink.send(gate_metrics_player_text(sink.host(), game_type_text(sink, game_type),
			copy_capped(process_gcc, 0x20), slot.gate_tick, player_strings(slot), now_ms));
}

void Server_ReportLingerExpiry(NapiNPServerCtx &ctx) {
	GateMetricsClocks &clocks = ctx.gate_clocks;
	const uint32_t now = ctx.tick_count_ms;
	const std::string mission_file = copy_capped(ctx.config.mission_file, 0x20);
	clocks.mission.close_phase(now);
	clocks.mission.mission_file = mission_file;
	Server_SendMissionMetrics(ctx);
	construct_clock(ctx, clocks.mission);
	clocks.mission.mission_file = mission_file;
	clocks.mission.gcc = copy_capped(clocks.process.gcc, 0x20);
	clocks.mission.max_players = clocks.process.max_players;
	gate_update_max_player_counts(ctx, clocks.mission);
	// The slot walk in slot order: the connections' rows and the orphaned rows (a Serve Only
	// host's own slot 0 is local: no report, and nothing reads its clock).
	struct Row {
		NapiNPConnection *slot;
		OrphanedPlayerRow *orphan;
	};
	std::vector<Row> rows;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list)
		if (player_slot_active(conn)) rows.push_back({&conn, nullptr});
	for (OrphanedPlayerRow &orphan : ctx.orphaned_player_rows) rows.push_back({&orphan.row, &orphan});
	std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
		return a.slot->reply.player_slot < b.slot->reply.player_slot;
	});
	for (const Row &row : rows) {
		NapiNPConnection &slot = *row.slot;
		slot.gate_mission_tick.close_phase(now);
		const NapiNPConnection *net_player =
				row.orphan != nullptr ? orphan_net_player(ctx, *row.orphan) : row.slot;
		Server_SendPlayerMissionMetrics(ctx, slot, net_player);
		construct_clock(ctx, slot.gate_mission_tick);
	}
}

} // namespace opennova::inmatch
