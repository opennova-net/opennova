// The role feeds — see role_feeds.h. [orig: NapiNPClientMsg_0x01D @0x430840;
// UI_UpdateDeathScreenContent @0x5536a0; HUD_DrawFriendlyTagsPass @0x5a4480]

#include <runtime/inmatch/role_feeds.h>

#include <base/gameprofile/game_type.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/game_config.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/replication/client_roster_tags.h>

#include <algorithm>
#include <cstdio>

namespace opennova::inmatch {

bool local_death_screen_active(const RoleView &view) {
	return view.runtime != nullptr && view.runtime->state().death_screen_active;
}

bool local_player_dead(const RoleView &view) {
	if (view.joiner) return view.runtime != nullptr && view.runtime->local_player_dead();
	return view.kernel != nullptr && view.kernel->local.local_player_dead();
}

namespace {

// Authority: the Match clock; joiner: the folded 0x0A sub-block-1 copy
// [orig: g_round_time_remaining @0x24C1958, the joiner store
// @0x430219..0x430235].
int32_t round_ticks_remaining(const RoleView &view) {
	const int32_t remaining = view.joiner
			? view.runtime->state().round_time_remaining_ticks
			: (view.kernel != nullptr ? view.kernel->world.match.remaining_ticks() : -1);
	return std::max(0, remaining);
}

} // namespace

EndRoundSessionState end_round_session_state(const RoleView &view) {
	EndRoundSessionState v;
	if (view.runtime == nullptr) return v;
	const replication::ClientEndRoundStats &er = view.runtime->state().end_round;
	v.header_known = er.header_known;
	v.board_known = er.known;
	v.game_type = view.runtime->game_type();
	v.winner = static_cast<int>(er.header.winner_team);
	v.team_score_0 = static_cast<int>(er.header.team_score_0);
	v.team_score_1 = static_cast<int>(er.header.team_score_1);
	v.draw = er.header.draw != 0;
	v.my_index = static_cast<int>(er.header.player_index);
	v.round_ticks = round_ticks_remaining(view);
	v.death_screen = local_death_screen_active(view);
	v.local_team = static_cast<int>(view.runtime->assigned_team());
	// The team-mode arm stat.mnu's RADIO_TAB_* trio rides (the g_GameType
	// 0x10000 bit, base/gameprofile/game_type.h; the show callback's witness is
	// stat_screen_feed.h's).
	v.team_mode = game_type::is_team(view.runtime->game_type());
	// The round-cycle handoff's session half: the host's post-round linger
	// expiry closes the session [orig: Server_TickUpdate's drain sets
	// g_mission_exit_reason = 3 @0x51db63 — the map cycle]; a joiner's session
	// dies with the host's exit.
	v.session_open = view.joiner ? !view.runtime->session_lost()
								 : view.host != nullptr && view.host->is_in_session != 0;
	return v;
}

hud::EndRoundOverlayInput end_round_overlay_input(const RoleView &view) {
	hud::EndRoundOverlayInput in;
	if (view.runtime == nullptr) return in;
	const replication::ClientEndRoundStats &er = view.runtime->state().end_round;
	in.game_type = view.runtime->game_type();
	in.draw = er.header.draw != 0;
	in.winner_team = er.header.winner_team;
	in.local_team = view.runtime->assigned_team();
	in.death_screen = local_death_screen_active(view);
	in.team_scores[0] = er.header.team_score_0;
	in.team_scores[1] = er.header.team_score_1;
	// The non-team 0x1D form's three named players + primary scores; empty
	// names take the ladder's name-less arms. [orig: the 0x1D commit
	// @0x430a70..0x430abb into byte_24C1A98/B7C/C60 + dword_24C1AD4/BB8/C9C]
	for (int i = 0; i < 3; ++i) {
		in.player_names[i] = er.header.player_names[i];
		in.player_scores[i] = er.header.player_scores[i];
	}
	// [orig: g_round_time_remaining @0x24C1958 — the game-time line and the
	// timed/untimed arm picks read it on every role].
	in.round_time_remaining_ticks = round_ticks_remaining(view);
	return in;
}

std::vector<StatScreenRow> end_round_rows(const RoleView &view, int tab) {
	// The PLAYER SLOT table retail walks is the roster every role's view folds
	// from 0x46 (name / clan / team). The local row comes from the 0x1D
	// header's board index, resolved to a connection slot below.
	std::vector<StatScreenRow> out;
	if (view.runtime == nullptr) return out;
	const replication::ClientState &cs = view.runtime->state();
	if (!cs.end_round.known) return out;
	std::vector<StatScreenPlayer> players;
	for (size_t i = 0; i < cs.roster.size(); ++i) {
		const replication::ClientRosterSlot &slot = cs.roster[i];
		if (!slot.bound) continue;
		StatScreenPlayer p;
		p.slot = static_cast<uint8_t>(i);
		p.team = slot.team;
		p.name = slot.name;
		p.squad = slot.clan;
		players.push_back(p);
	}
	// The header's player_index is the recipient's index into the FROZEN
	// (points-descending) board array, not a connection slot. Retail joins the
	// board by the row's stored slot id and highlights the row whose slot
	// matches board[player_index].slot; the same fold works for both roles
	// because the listen host consumes its own loopback 0x1D.
	int local_slot = -1;
	const int8_t header_index = cs.end_round.header.player_index;
	if (header_index >= 0 &&
			static_cast<size_t>(header_index) < cs.end_round.board.players.size())
		local_slot = cs.end_round.board.players[static_cast<size_t>(header_index)].slot;
	for (const StatScreenRow &r : stat_screen_rows(cs.end_round.board, players, false, local_slot)) {
		// The tab filter (stat_screen_row_visible carries the witness).
		if (stat_screen_row_visible(tab, r.team)) out.push_back(r);
	}
	return out;
}

bool collect_friendly_tags(const RoleView &view, std::vector<world::FriendlyTagSource> &out) {
	out.clear();
	if (view.kernel == nullptr) return false;
	world::World &w = view.kernel->world;
	const world::Entity *player = w.registry.get(w.cached.local_player);
	if (player == nullptr) return false;
	// The pass-level facts (retail g_death_screen_active / g_GameType): the
	// death screen bit is the client's local latch, the game type every role's
	// view carries.
	world::FriendlyTagPassContext ctx;
	ctx.death_screen = local_death_screen_active(view);
	ctx.game_type = view.runtime != nullptr ? view.runtime->game_type() : 0;
	// The session's rules word: a joiner's S2C 0x64 fixed block (+44), the
	// host's own mp_attributes. Bit 0x400 = the host option FriendlyTag 0.
	const uint32_t rules_word = (view.joiner && view.runtime != nullptr)
			? view.runtime->view().mp_attributes()
			: view.staged_mp_attributes;
	ctx.rules_no_friendly_tags = (rules_word & GameConfig::kMpAttribNoFriendlyTag) != 0;
	// The player walk's slot owner. On the authority the connection table IS
	// the player-slot table: each link's owned entity, revive window, and
	// medic-request latch (retail's PlayerSlot +0x24/+0x10/+0x2C).
	const NapiNPServerCtx *host = view.host;
	const world::PlayerSlotLookup authority_slot_lookup =
			[host](world::EntityHandle entity, world::PlayerSlotFacts &facts) {
				if (host == nullptr) return false;
				for (const NapiNPConnection &conn : host->np_protocol.connection_list) {
					if (conn.link.owned_entity != entity) continue;
					facts.revive_seconds = static_cast<uint8_t>(
							std::min<uint32_t>(conn.link.downed_revive_seconds, 0xFFu));
					facts.medic_request = conn.link.medic_request_active;
					return true;
				}
				return false;
			};
	if (!view.joiner) ctx.slot_lookup = &authority_slot_lookup;
	world::collect_friendly_tags(w, *player, out, ctx);
	if (view.joiner && view.runtime != nullptr && !ctx.rules_no_friendly_tags) {
		// A joiner's players are decoded rows, not World twins: the roster walk
		// over ClientState supplies them (replication/client_roster_tags.h).
		const int32_t player_hp = w.tables.player.item_hp;
		replication::collect_roster_tags(view.runtime->state(),
				view.runtime->has_self_handle() ? view.runtime->self_handle() : 0xFFFFu,
				view.runtime->assigned_team(), ctx.death_screen, ctx.game_type, out,
				[player_hp](uint16_t) { return player_hp; }, &w);
	}
	return true;
}

world::DeployScreenStatus deploy_screen_status(const RoleView &view,
		const world::SpawnZoneRegistry &zones, const std::string &medic_key_label,
		const hud::GameTextLookup &gametext) {
	int penalty = 0;
	int revive = 0;
	int hold = 0;
	int self_zone_index = -1;
	bool self_zone_numbered = false;
	int self_zone_countdown = 0;
	if (view.joiner && view.runtime != nullptr) {
		const replication::ClientState &cs = view.runtime->state();
		penalty = cs.respawn_penalty_seconds;
		revive = cs.local_revive_seconds;
		hold = cs.spawn_hold_seconds;
		if (cs.spawn_waves.known && cs.spawn_waves.self_zone_handle != 0xFFFFu &&
				view.kernel != nullptr) {
			const world::EntityHandle zone{ cs.spawn_waves.self_zone_handle };
			self_zone_index = world::spawn_zone_index_of(zones, zone);
			if (const world::Entity *e = view.kernel->world.registry.get(zone))
				self_zone_numbered = e->zone_number != 0;
			for (const SpawnWaveGroup &g : cs.spawn_waves.value.groups) {
				if (g.zone_handle == cs.spawn_waves.self_zone_handle)
					self_zone_countdown = g.wave_countdown;
			}
		}
	}
	world::DeployStatusInput status_in;
	status_in.penalty_seconds = penalty;
	status_in.self_zone_index = self_zone_index;
	status_in.self_zone_numbered = self_zone_numbered;
	status_in.self_zone_countdown = self_zone_countdown;
	const world::DeployStatusLine line = world::build_deploy_status(status_in);
	world::DeployStaticsInput statics_in;
	statics_in.hold_seconds = hold;
	statics_in.revive_seconds = revive;
	// The +0x1E0 being-revived latch rides the joiner's ClientState (S2C 0x3A);
	// a listen host's own player has no revive sender ported yet, so it stays
	// clear there.
	statics_in.local_medic_reviving = view.joiner && view.runtime != nullptr
			? view.runtime->state().local_medic_reviving
			: false;

	world::DeployScreenStatus v;
	v.penalty_seconds = penalty;
	v.revive_seconds = revive;
	v.hold_seconds = hold;
	v.line = line;
	v.statics = world::deploy_statics_visibility(statics_in);
	v.medic_cooldown_ticks = view.kernel != nullptr
			? static_cast<int>(view.kernel->local.medic_request_cooldown_ticks)
			: 0;
	v.medic_request_serial =
			view.kernel != nullptr ? static_cast<int>(view.kernel->local.medic_request_serial) : 0;

	world::DeployInstructionsInput instructions;
	instructions.dead = local_player_dead(view);
	const world::Entity *player = view.kernel != nullptr ? view.kernel->local.player() : nullptr;
	if (player != nullptr) {
		instructions.team = player->team;
		instructions.player_name = player->display_name;
	}
	if (view.runtime != nullptr) {
		const replication::ClientState &cs = view.runtime->state();
		instructions.game_type = view.runtime->game_type();
		instructions.permanent_death = cs.permanent_death;
		instructions.spectators_allowed = cs.spectators_allowed;
		instructions.check_secured_spawn = cs.deploy_check_secured_spawn;
		instructions.kill_announcement = cs.kill_announcement.text;
		instructions.round_ticks = cs.round_time_remaining_ticks;
		instructions.alive_players = cs.scoreboard.alive_player_count;
		const replication::ClientRosterSlot &roster = cs.roster[view.runtime->local_player_slot()];
		if (roster.bound) {
			instructions.player_name = roster.name;
			instructions.clan = roster.clan;
		}
		if (view.joiner) instructions.team = view.runtime->assigned_team();
		if (view.kernel != nullptr) {
			instructions.has_spawn_zones = !zones.empty();
			for (const world::EntityHandle handle : zones.entries) {
				const world::Entity *zone = view.kernel->world.registry.get(handle);
				if (zone == nullptr) continue;
				const auto found = view.runtime->zone_states().find(handle.packed);
				if (found == view.runtime->zone_states().end()) continue;
				const auto *replica = cs.find(handle.packed);
				const auto team = replica != nullptr && replica->team_known ? replica->team
																			: zone->team;
				const auto &entry = found->second.entry;
				if (team == instructions.team && entry.value_target >= entry.value_limit)
					instructions.has_full_team_spawn = true;
			}
		}
	}
	v.instructions = world::build_deploy_instructions(instructions, gametext);
	v.statics_text = world::deploy_statics_text(statics_in, medic_key_label, gametext);
	char zone_key[32];
	std::snprintf(zone_key, sizeof zone_key, "STRWPNAME%03d", line.zone_index + 1);
	const auto text = [&gametext](const char *section, const char *key, const char *fallback) {
		return gametext ? gametext(section, key, fallback) : std::string(fallback);
	};
	v.respawn_text = world::deploy_status_text(line,
			text("Overlays", "STROVER_PENALTYTIMER", "Respawn penalty"),
			text("WPNames", zone_key, "Spawn Point"));
	return v;
}

} // namespace opennova::inmatch
