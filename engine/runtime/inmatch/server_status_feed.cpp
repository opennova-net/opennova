// The server-status page feed (server_status_feed.h).
#include <runtime/inmatch/server_status_feed.h>

#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_console.h>
#include <runtime/world/world.h>

namespace opennova::inmatch {

void fill_server_status_page(hud::ServerStatusPageState &page, const NapiNPServerCtx &ctx,
		const world::World *world) {
	page.mp_session_peer = ctx.is_mp_session_peer != 0;
	// The slot table's capacity and the session's slot limit: the authority
	// allocates the table with the very limit it stores
	// [orig: Server_InitNewRoundState @0x51c976 (dword_24D211C) ->
	//  Server_AllocatePlayerSlotTable @0x51c97b (g_PlayerSlotCapacity)].
	const int capacity = static_cast<int>(ctx.config.total_player_slot_capacity());
	page.capacity = capacity;
	page.slot_limit = capacity;
	page.slots.assign(static_cast<size_t>(capacity), hud::ServerStatusSlot{});
	for (const NapiNPConnection &c : ctx.np_protocol.connection_list) {
		// The slot's active byte (+4) [orig: Server_PlayerAdd @0x51CBC0].
		if (!player_slot_active(c) || c.reply.player_slot >= capacity) continue;
		hud::ServerStatusSlot &slot = page.slots[c.reply.player_slot];
		slot.active = true;
		// +5: the host's own slot, the loopback connection
		slot.local = c.link.mode == replication::TransportMode::Loopback;
		// +0x20 == 6 and +0x188E3: the port's completed initial-state burst
		// is the slot's in-game state, anything before it the loading byte
		// (napi_np_connection.h is_in_match).
		slot.in_game = is_in_match(c);
		slot.loading = !slot.in_game;
		slot.spectator = c.link.spectator; // +0x188D7
		slot.name = c.reply.player_name;   // +0x28
		// The seconds since the connection last parsed a session packet; the
		// host's own loopback carries no reap clock [orig: slot+0x1C -> +0xB8
		// -> +0x5E8 @0x50a525..0x50a561; the clock napi_np_connection.h
		// receive_inactive_ms].
		slot.idle_seconds = slot.local ? 0 : static_cast<int32_t>(c.receive_inactive_ms / 1000u);
		slot.team = c.assigned_team_valid ? c.assigned_team : uint8_t{0};
		if (world == nullptr) continue;
		if (const world::Entity *e = world->registry.get(c.link.owned_entity)) {
			// The team byte and the class word through the bound entity
			// (D-NET-132; the medic gate reads the class the same way), the
			// dead test on its signed health [orig: `*(__int16 *)(entity + 286)
			// <= 0` @0x50a674..0x50a68e].
			slot.team = e->team;
			slot.class_word = e->player_class;
			slot.entity_dead = e->health <= 0;
		}
		if (const world::MatchPlayer *p = world->match.player(c.link.owned_entity)) {
			slot.points = p->stats[world::MatchStats::kPoints];               // field 0x1C
			slot.flag_captures = p->stats[world::MatchStats::kFlagCaptures]; // field 0x0B
			slot.objective_seconds = p->objective_ticks;                     // +0x170AC
		}
	}
	// The server line [orig: transport_mode @0x50a7db; sub_4C4DB0 @0x50a7fa;
	// g_ServerNameStr @0x50a808].
	page.novaworld = ctx.transport_mode == NetworkType::NovaWorld;
	page.session_key = ctx.novaworld_app_id;
	page.server_name = ctx.config.server_name;
	page.game_type = world != nullptr ? world->match.rules().game_type : ctx.config.game_type;
	// The round tallies [orig: g_RoundWinsTeam1/2, g_TotalRoundsPlayed].
	page.round_wins_team1 = ctx.round_wins[0];
	page.round_wins_team2 = ctx.round_wins[1];
	page.rounds_played = ctx.rounds_played;
	page.total_logins = ctx.total_logins; // ctx+0x11A8
	page.frames = ctx.stats_frames_last_second; // dword_24C193C
	page.cpu_percent = ctx.stats_cpu_percent;   // g_StatsCpuPercent
	// A host with no client of its own shows its CHAT ring's four newest
	// slots [orig: HUD_DrawServerConsoleLines @0x5ba0bd on a non-peer]; a
	// listen host's rows are its HUD's.
	page.console_rows.clear();
	if (!page.mp_session_peer) {
		for (const ServerConsoleLine &row : server_console_rows(ctx.console_chat))
			page.console_rows.push_back(hud::ServerStatusConsoleRow{row.text, row.color});
	}
	if (world == nullptr) return;
	for (uint8_t t = 0; t < 2; ++t) {
		const world::MatchStats &stats = world->match.team_stats(static_cast<uint8_t>(t + 1));
		page.team_points[t] = stats[world::MatchStats::kPoints];
		page.team_flag_captures[t] = stats[world::MatchStats::kFlagCaptures];
		page.team_hold_seconds[t] = world->match.team_hold_ticks(static_cast<uint8_t>(t + 1));
	}
	page.round_time_remaining = world->match.remaining_ticks(); // g_RoundTimeRemaining
	page.pre_round_delay = world->preround_delay_seconds;     // g_PreRoundDelayTimer
}

} // namespace opennova::inmatch
