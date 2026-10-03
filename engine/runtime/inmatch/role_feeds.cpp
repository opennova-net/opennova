// The role feeds — see role_feeds.h. [orig: NapiNPClientMsg_0x01D @0x430840;
// UI_UpdateDeathScreenContent @0x5536a0; HUD_DrawFriendlyTagsPass @0x5a4480]

#include <runtime/inmatch/role_feeds.h>

#include <base/gameprofile/game_type.h>
#include <base/io/fixed.h>
#include <runtime/replication/client_scoreboard_view.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/game_config.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_message_dispatch.h> // host_session_vars
#include <runtime/inmatch/session_status.h> // the authority's own 0x58 report
#include <runtime/hud/hud_toggles.h> // hud_deploy_key_pick
#include <runtime/hud/session_rules_text.h>
#include <runtime/menu/command_map_screen.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/replication/client_roster_tags.h>
#include <runtime/replication/entity_wire_bridge.h> // entity_class_of
#include <runtime/world/collision.h>
#include <runtime/world/radar_contacts.h> // radar_hud_frame
#include <runtime/world/radio_call.h> // capture_zone_max_coverage
#include <runtime/world/user_waypoints.h>

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

int deploy_key_pick(const RoleView &view, const world::SpawnZoneRegistry &zones, int vk) {
	if (view.kernel == nullptr) return -1;
	const world::World &w = view.kernel->world;
	hud::HudDeployKeyInput in;
	in.vk = vk;
	in.in_session = w.rules.mp_session; // g_NapiNPCtx.is_in_session
	in.single_player_respawn =
			(w.tables.mission_attrib_flags & world::MissionTables::kMissionAttribSinglePlayerRespawn) != 0;
	in.local_dead = local_player_dead(view);
	in.deploy_overlay = view.runtime != nullptr && view.runtime->state().deploy_overlay_active;
	if (const world::Entity *player = w.registry.get(w.cached.local_player))
		in.local_team = player->team;
	std::vector<int16_t> teams;
	teams.reserve(zones.entries.size());
	for (const world::EntityHandle handle : zones.entries) {
		const world::Entity *zone = w.registry.get(handle);
		teams.push_back(zone != nullptr ? static_cast<int16_t>(zone->team) : int16_t{-1});
	}
	in.zone_teams = teams.data();
	in.zone_count = teams.size();
	return hud::hud_deploy_key_pick(in);
}

uint32_t round_over_key(const RoleView &view, int vk, int restart_vk) {
	if (view.kernel == nullptr) return 0;
	world::World &w = view.kernel->world;
	// The leg runs only behind the round-over gate: the authority's round end,
	// a joiner's 0x1D [orig: `cmp g_SpawnSuccessGate, 0` @0x49c7cf; the writers
	// Server_ProcessRoundEnd @0x5168E4 and NapiNPClientMsg_0x01D @0x430858].
	const bool gate = view.joiner
			? view.runtime != nullptr && view.runtime->state().spawn_success_gate
			: w.match.outcome().ended;
	if (!gate) return 0;
	hud::HudRoundOverKeyInput in;
	in.vk = vk;
	in.restart_vk = restart_vk;
	in.in_session = w.rules.mp_session; // g_NapiNPCtx.is_in_session
	in.game_type = w.match.rules().game_type;
	const uint32_t bits = hud::hud_round_over_key(in);
	// The world halves of the SP arm; the co-op in-session arm's exits are
	// not wired (interface/hud-re.md D-HUD-45).
	if (!in.in_session) {
		if ((bits & hud::hud_round_over::kRestart) != 0) w.round_over_restart();
		if ((bits & hud::hud_round_over::kExit) != 0) w.round_over_exit();
	}
	return bits;
}

namespace {

// Authority: the Match clock; joiner: the folded 0x0A sub-block-1 copy
// [orig: g_RoundTimeRemaining @0x24C1958, the joiner store
// @0x430219..0x430235].
int32_t round_ticks_remaining(const RoleView &view) {
	const int32_t remaining = view.joiner
			? view.runtime->state().round_time_remaining_ticks
			: (view.kernel != nullptr ? view.kernel->world.match.remaining_ticks() : -1);
	return std::max(0, remaining);
}

// The local player's team the end-round overlay compares the winner against
// [orig: HUD_DrawEndRoundStatsOverlay @0x5b7cd0 reads byte_A85B48 @0x5b7f56].
// A joiner latches it from S2C 0x04 (ClientRuntime::assigned_team); the
// listen host's HostClient runtime never receives that record (the latch
// stays 0), so the authority reads its own player entity — retail's host
// latches the same value from its loopback 0x04 @0x425499.
int local_team_of(const RoleView &view) {
	if (view.joiner) return view.runtime != nullptr ? view.runtime->assigned_team() : 0;
	const world::Entity *player = view.kernel != nullptr ? view.kernel->local.player() : nullptr;
	return player != nullptr ? player->team : 0;
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
	v.local_team = local_team_of(view);
	// The team-mode arm stat.mnu's RADIO_TAB_* trio rides (the g_GameType
	// 0x10000 bit, base/gameprofile/game_type.h; the show callback's witness is
	// stat_screen_feed.h's).
	v.team_mode = game_type::is_team(view.runtime->game_type());
	// The round-cycle handoff's session half: the host's post-round linger
	// expiry closes the session [orig: Server_TickUpdate's drain sets
	// g_MissionExitReason = 3 @0x51db63 — the map cycle]; a joiner's session
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
	in.local_team = static_cast<uint8_t>(local_team_of(view));
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
	// [orig: g_RoundTimeRemaining @0x24C1958 — the game-time line and the
	// timed/untimed arm picks read it on every role].
	in.round_time_remaining_ticks = round_ticks_remaining(view);
	return in;
}

BreathBarFacts breath_bar_facts(const RoleView &view) {
	BreathBarFacts out;
	if (view.runtime != nullptr) {
		const replication::ClientState &cs = view.runtime->state();
		out.samples = cs.breath_samples;             // word_A85B7C
		out.spawn_success_gate = cs.spawn_success_gate;
		if (view.joiner) out.breath_time = cs.breathtime;
	}
	// The authority's frame carries no sub-block 1 to its own loopback, and
	// its HUD reads the host's own named value [orig: g_WacVarBreathTime].
	if (!view.joiner && view.kernel != nullptr)
		out.breath_time = view.kernel->world.script.wac_values.breathtime;
	return out;
}

namespace {

// One F9 / F10 menu's title and rows over the local player (hud_role_facts'
// doc carries the witness).
void fill_voice_macro_menu(const RoleView &view, const world::Entity &local, bool radio,
		hud::HudVoiceMacroMenuState &menu) {
	const world::World &w = view.kernel->world;
	const rtxt::File &macros = w.tables.voice_macros;
	const auto lookup = [&macros](const std::string &key) -> const std::string * {
		const rtxt::Entry *entry = macros.find_in_section("macrotext", key);
		return entry != nullptr ? &entry->text : nullptr;
	};
	const std::string *title = lookup(radio ? "RADIO_TITLE" : "EMOTES_TITLE");
	menu.title = title != nullptr ? *title : (radio ? "!Radio_Title" : "!EMOTES_Title");
	const uint32_t game_type = view.runtime != nullptr ? view.runtime->game_type() : 0u;
	const bool in_zone = game_type == 0x10010u && view.runtime != nullptr &&
			in_active_radio_zone(w, local, *view.runtime);
	const uint8_t flags = radio ? 6u : 0xCu;
	for (int i = 1; i <= 10; ++i) {
		const std::string key = world::radio_call_key(w, local, i, flags, game_type, in_zone);
		const std::string *text = lookup(key);
		menu.texts[static_cast<size_t>(i - 1)] = text != nullptr ? *text : key;
	}
}

} // namespace

HudRoleFacts hud_role_facts(const RoleView &view, uint32_t voice_menus) {
	HudRoleFacts out;
	out.breath = breath_bar_facts(view);
	if (view.runtime != nullptr) out.squad_orders = view.runtime->state().squad_orders;
	// The connection indicators: the role's replica runtime keeps g_NetQuality
	// (the listen host's own client included); the N icon draws on a
	// NovaWorld session whose NWU session is in use [orig:
	// CNetQuality_DrawIndicators @0x4c33d4..0x4c33f0].
	if (view.runtime != nullptr) {
		out.net_quality.indicators = view.runtime->net_quality_indicators();
		const hud::NovaWorldLinkFacts &nw = view.runtime->novaworld_link();
		out.net_quality.novaworld_icon = nw.novaworld && nw.nwu_in_use;
	}
	if (view.kernel == nullptr) return out;
	// The drawer's session gate, the same session bit as the MP lines below
	// [orig: `cmp is_in_session, 0` @0x4c3210].
	out.net_quality.in_session = view.kernel->world.rules.mp_session;
	if (voice_menus != 0u) {
		const world::World &vw = view.kernel->world;
		if (const world::Entity *local = vw.registry.get(vw.cached.local_player)) {
			if ((voice_menus & kHudVoiceMenuEmotes) != 0u) {
				out.emotes_menu.shown = true;
				fill_voice_macro_menu(view, *local, false, out.emotes_menu);
			}
			if ((voice_menus & kHudVoiceMenuRadio) != 0u) {
				out.radio_menu.shown = true;
				fill_voice_macro_menu(view, *local, true, out.radio_menu);
			}
		}
	}
	const world::World &w = view.kernel->world;
	hud::HudSessionState &s = out.session;
	// g_NapiNPCtx.is_in_session: the session bit the world carries (our SP
	// runs a listen host, but retail's single player never sets it).
	s.in_session = w.rules.mp_session;
	s.game_type = view.runtime != nullptr ? view.runtime->game_type() : 0; // g_GameType
	// g_RoundTimeRemaining, UNclamped — the -1 untimed seed is the timer's gate
	// [orig: HUD_DrawGameTimer @0x593D99]: the authority's Match clock, a
	// joiner's folded 0x0A copy.
	s.round_time_remaining = view.joiner
			? (view.runtime != nullptr ? view.runtime->state().round_time_remaining_ticks : -1)
			: w.match.remaining_ticks();
	if (view.runtime != nullptr) {
		const replication::ClientState &cs = view.runtime->state();
		const replication::ClientScoreboard &sb = cs.scoreboard;
		s.permanent_death = cs.permanent_death;              // byte_A821EF
		s.remaining_count = sb.alive_player_count;            // g_ScoreboardDeadRowCount
		s.row_count = static_cast<int>(sb.rows.size());       // g_ScoreboardRowCount
		s.spectator_count = sb.spectator_count;               // g_ScoreboardSpectatorCount
		// Teams 1 and 2 of the 0x16 team table, score1 read signed like the
		// movsx that stores it [orig: the team rows 0xA85AEC + 16t, the
		// stores @0x42fe00..0x42fe42; read @0x59CDF4 / @0x59CEA8 / @0x59CF54].
		for (size_t t = 0; t < 2; ++t) {
			if (sb.teams.size() <= t + 1) break;
			s.team_score1[t] = static_cast<int16_t>(sb.teams[t + 1].score1);
			s.team_koth[t] = sb.teams[t + 1].koth_hold;
		}
		if (view.joiner) s.time_limit_minutes = cs.session_time_limit_minutes; // dword_A821C0
	}
	// The authority reads its own g_TimeLimitMinutes [orig: @0x59CCDB..0x59CCE3].
	if (!view.joiner && view.host != nullptr)
		s.time_limit_minutes = static_cast<int32_t>(view.host->config.time_limit_minutes);
	const world::Entity *player = w.registry.get(w.cached.local_player);
	if (player != nullptr) {
		s.team = player->team; // hudInfo+0x176 = entity+0x162 [orig: @0x4B8464]
		// [orig: CaptureZone_FindMaxProximityCoverage(&g_LocalPlayerEntity->boundRadius)
		//  @0x59CDE8]
		s.zone_coverage = world::capture_zone_max_coverage(w, *player);
	}
	// The death screen's spectate arm rebuilds the HUD info for the target and
	// draws the health bar and the TEAMID line off it, then restores the local
	// build: the team byte (+0x176 = entity+0x162), the health ratio and the
	// name the line reads first. A joiner's target is a decoded row, the
	// authority's a registry entity; the max is the def hp (a row's player
	// def) without the difficulty term, as the friendly tags read it.
	// [orig: HUD_RenderOverlays @0x5a7bc5..0x5a7c25 — the gate
	//  `dword_A860F4 && dword_A860F0` @0x5a7bdb; HUD_BuildEntityInfo @0x5a7bf3
	//  (team @0x4b8464, ratio @0x4b87a2..0x4b87d3); HUD_DrawTeamIdLine's name
	//  @0x59ac3d]
	if (view.runtime != nullptr) {
		const replication::ClientState &cs = view.runtime->state();
		if (cs.death_screen_active && cs.spectate_target != 0xFFFF &&
				cs.death_screen_submode != 0) {
			int64_t health = 0;
			int64_t max_hp = 1;
			bool found = false;
			if (view.joiner) {
				if (const replication::ClientEntityState *row = cs.find(cs.spectate_target)) {
					found = true;
					s.team = row->team_known ? row->team : 0;
					s.spectated_name = row->display_name;
					health = row->health_known ? static_cast<int16_t>(row->health_word) : 0;
					max_hp = std::max<int32_t>(1, w.tables.player.item_hp);
				}
			} else if (const world::Entity *e =
								w.registry.get(world::EntityHandle{cs.spectate_target})) {
				found = true;
				s.team = e->team;
				s.spectated_name = e->display_name;
				health = static_cast<int16_t>(e->health);
				max_hp = std::max<int32_t>(1, e->health_max);
			}
			if (found) {
				s.spectating = true;
				// The signed Health word over the max, read back unsigned, so a
				// negative Health caps to a full bar like any ratio past 1
				// [orig: `idiv` @0x4b87c7, the unsigned cap @0x4b87d1..0x4b87d3].
				uint32_t ratio = static_cast<uint32_t>(
						static_cast<int32_t>((health * 65536) / max_hp));
				if (ratio > 0x10000u) ratio = 0x10000u;
				s.spectated_health_fraction = static_cast<float>(ratio) / 65536.0f;
			}
		}
	}
	s.attack_defend = view.kernel->local.attack_defend_role; // dword_B78FE8
	// The HUDLS scan over the local slot table and each category's first
	// def's slot-bar icon [orig: HUD_DrawWeaponSlotBar @0x599D0A..0x599D69;
	// def+0x1B8 <- hud_loadout_select, WeaponDefs_ParseLineCallback @0x544A44].
	if (view.kernel->local.inventory_valid) {
		out.slot_bar = world::weapon_inventory_slot_bar_scan(w.tables.weapons,
				view.kernel->local.inventory);
		for (size_t c = 0; c < out.slot_bar.size(); ++c) {
			const int16_t adm = out.slot_bar[c].adm_index;
			if (adm < 0) continue;
			if (const world::WeaponTableEntry *def =
							w.tables.weapons.by_index(static_cast<uint8_t>(adm)))
				out.slot_bar_icons[c] = def->hud_loadout_select;
		}
	}
	return out;
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
	// The pass-level facts (retail g_DeathScreenActive / g_GameType): the
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
	// The player walk's slot owner: the listen client's own S2C 0x4C table,
	// each entry's slot (retail's PlayerSlot +0x24 entity, +0x10 revive
	// seconds, +0x2C medic request, the +0x14 / +0x20 label) as its loopback
	// folds it. [orig: HUD_DrawFriendlyTagsPass @0x5a4507..0x5a4597 over
	//  g_PlayerSlotPtrTable]
	const replication::ClientState *client =
			view.runtime != nullptr ? &view.runtime->state() : nullptr;
	const world::PlayerSlotLookup authority_slot_lookup =
			[client](world::EntityHandle entity, world::PlayerSlotFacts &facts) {
				if (client == nullptr || entity.pool() != 0) return false;
				for (const replication::ClientVisiblePlayer &entry : client->visible_players) {
					const replication::ClientRosterSlot &slot = client->roster[entry.slot];
					if (!slot.bound || slot.entity_slot != entity.slot()) continue;
					facts.revive_seconds = slot.downed_revive_seconds;
					facts.medic_request = slot.medic_request_active;
					facts.label = replication::roster_tag_label(slot);
					facts.squad_color = slot.squad_color;
					return true;
				}
				return false;
			};
	if (!view.joiner) ctx.slot_lookup = &authority_slot_lookup;
	world::collect_friendly_tags(w, *player, out, ctx);
	if (view.joiner && view.runtime != nullptr && !ctx.rules_no_friendly_tags) {
		// A joiner's players are decoded rows, not World twins: the roster walk
		// over ClientState supplies them (replication/client_roster_tags.h).
		// Both walks compare with the local player's entity Team, not the S2C
		// latch; spawn_from_self seeds it before any 0x04/0x50 lands
		// [orig: g_LocalPlayerEntity+0x162 @0x5a455c / @0x5a3c71].
		const int32_t player_hp = w.tables.player.item_hp;
		replication::collect_roster_tags(view.runtime->state(),
				view.runtime->has_self_handle() ? view.runtime->self_handle() : 0xFFFFu,
				player->team, ctx.death_screen, ctx.game_type, out,
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
		return hud::game_text(gametext, section, key, fallback);
	};
	v.respawn_text = world::deploy_status_text(line,
			text("Overlays", "STROVER_PENALTYTIMER", "Respawn penalty"),
			text("WPNames", zone_key, "Spawn Point"));
	// The team-service pair. The rules word follows the ctx+0x68 pick
	// [orig: @0x553445]: that word is the hosted-session latch, so a hosting
	// process reads its own multiplayerAttributeFlags_34C (the staged
	// GameConfig::mp_attributes) and a joiner its S2C 0x08 copy (dword_A821E4)
	// [orig: set by CNapiGameSession_CreateSession @0x4C9D16 (called from the
	//  host and single-player starts only), cleared by
	//  CNapiGameSession_ResetActiveSession @0x4C8AC0; the joiner's
	//  CNapiNetwork_StartClientConnection @0x4CA160 never writes it].
	world::DeployTeamButtonsInput buttons;
	buttons.in_session = view.kernel != nullptr && view.kernel->world.rules.mp_session;
	buttons.team = player != nullptr ? player->team : uint8_t{0};
	buttons.dead = instructions.dead;
	buttons.rules_word = view.staged_mp_attributes;
	if (view.runtime != nullptr) {
		const replication::ClientState &cs = view.runtime->state();
		buttons.game_type = view.runtime->game_type();
		buttons.permanent_death = cs.permanent_death;
		if (view.joiner) buttons.rules_word = cs.session_rules_flags;
	}
	v.team_buttons_shown = world::deploy_team_buttons_shown(buttons);
	return v;
}

bool collect_lfp_zones(const RoleView &view, const world::SpawnZoneRegistry &zones,
		int local_team, std::vector<hud::HudLfpZone> &out) {
	out.clear();
	if (view.kernel == nullptr || view.runtime == nullptr ||
			!view.kernel->world.cached.local_player.valid())
		return false;
	const world::World &w = view.kernel->world;
	const world::Entity *local = w.registry.get(w.cached.local_player);
	if (local == nullptr) return false;
	const ClientRuntime *runtime = view.runtime;
	// The zone-timer entry as the marker reads it.
	const world::LfpZoneTimerLookup timer = [runtime](world::EntityHandle h,
													  world::LfpZoneTimer &t) {
		const auto it = runtime->zone_states().find(h.packed);
		if (it == runtime->zone_states().end() || !it->second.has_value) return false;
		const auto &e = it->second.entry;
		t.team = e.mode_a;
		t.value = e.value_current;
		t.control = e.value_target;
		t.limit = e.value_limit;
		t.rate = e.value_rate;
		t.active = e.value_active;
		t.count_owner = e.contest_owner;
		t.count_other = e.contest_other;
		return true;
	};
	// The transient minimap slot's flag byte for the zone; the 0x6B ring slots
	// land in the special bank here, so both are searched.
	const world::LfpCaptureFlagsLookup capture_flags = [runtime](world::EntityHandle h) -> uint8_t {
		const replication::ClientMinimapState &map = runtime->state().minimap;
		for (const auto &slot : map.transient)
			if (slot.active && slot.handle == h.packed) return slot.flags;
		for (const auto &slot : map.special)
			if (slot.active && slot.handle == h.packed) return slot.flags;
		return 0;
	};
	world::build_lfp_zones(w, zones, *local, local_team, timer, capture_flags, out);
	return true;
}

namespace {

// Math_FixedPointTransformPoint22: the 3x3 Q22 rotation with the +0x200000
// rounding bias before each >> 22, then the translation column.
// [orig: Math_FixedPointTransformPoint22 @0x615810]
void transform_point22(const world::CollisionMatrix &m, const int32_t in[3], int32_t out[3]) {
	for (int row = 0; row < 3; ++row) {
		const int64_t sum = static_cast<int64_t>(in[0]) * m.m[row * 4 + 0] +
				static_cast<int64_t>(in[1]) * m.m[row * 4 + 1] +
				static_cast<int64_t>(in[2]) * m.m[row * 4 + 2] + 0x200000;
		out[row] = static_cast<int32_t>(static_cast<uint32_t>(sum >> 22) +
				static_cast<uint32_t>(m.m[row * 4 + 3]));
	}
}

} // namespace

bool death_map_facts(const RoleView &view, const world::SpawnZoneRegistry &zones,
		hud::DeathMapFacts &out) {
	out = hud::DeathMapFacts{};
	if (view.kernel == nullptr) return false;
	const world::World &w = view.kernel->world;
	if (const world::Entity *player = w.registry.get(w.cached.local_player)) {
		out.player_present = true;
		out.player_x = io::float_to_fp16_16_sat(player->position.x);
		out.player_y = io::float_to_fp16_16_sat(player->position.y);
		out.player_z = io::float_to_fp16_16_sat(player->position.z);
		out.player_team = player->team;
	}
	out.bounds_min_x = zones.min_x;
	out.bounds_min_y = zones.min_y;
	out.bounds_max_x = zones.max_x;
	out.bounds_max_y = zones.max_y;
	const ClientRuntime *runtime = view.runtime;
	if (runtime != nullptr) {
		const replication::ClientState &cs = runtime->state();
		// [orig: g_DeployScreenActive @0xA860DC; dword_A85B68 (the 0x0A
		//  sub-block-0 hold byte); g_GameType; word_A85BC0]
		out.deploy_screen_active = cs.deploy_overlay_active;
		out.hold_seconds = cs.spawn_hold_seconds;
		out.game_type = runtime->game_type();
		if (cs.spawn_waves.known) out.self_zone_handle = cs.spawn_waves.self_zone_handle;
	}
	// g_NapiNPCtx.is_in_session and the HUD info entity's team (the capture-
	// point pick's gates [orig: Minimap_GetCapturePointInfo @0x5971ec /
	// @0x597263]).
	out.in_session = view.joiner ? (runtime != nullptr && runtime->in_session())
			: w.rules.mp_session;
	out.hud_team = out.player_team;
	// The CMAP's placed-waypoint table: every non-null entry, read through
	// its row whatever the row now holds (world::user_waypoint_row)
	// [orig: CMapWindow_HandleEvent `if (entry)` @0x549dc8 / @0x549b7a, the
	//  entity +4 read @0x549dd7 / @0x549ba0].
	for (int i = 0; i < world::UserWaypointTable::kCapacity; ++i) {
		const world::EntityHandle handle = w.user_waypoints.entries[static_cast<size_t>(i)].handle;
		if (!handle.valid()) continue;
		const world::UserWaypointRow row = world::user_waypoint_row(w, handle);
		hud::DeathMapFacts::UserWaypoint &fact = out.user_waypoints[static_cast<size_t>(i)];
		fact.live = true;
		fact.x = row.x;
		fact.y = row.y;
		fact.name = row.name;
	}
	// The zone walk is the minimap banks' (banked_spawn_zones) [orig:
	// MapOverlay_DrawView @0x5a5a4d..0x5a5d2c].
	std::vector<world::EntityHandle> banked;
	banked_spawn_zones(view, banked);
	for (const world::EntityHandle handle : banked) {
		const world::Entity *e = w.registry.get(handle);
		if (e == nullptr) continue;
		hud::DeathMapZone zone;
		zone.handle = e->handle.packed;
		zone.index = static_cast<int32_t>(world::spawn_zone_index_of(zones, handle));
		zone.team = e->team;
		if (runtime != nullptr) {
			const auto live = runtime->zone_states().find(e->handle.packed);
			if (live != runtime->zone_states().end() && live->second.has_value) {
				zone.team = static_cast<uint8_t>(live->second.entry.mode_a);
				zone.timer_ready = !(live->second.entry.value_target <
						live->second.entry.value_limit);
			}
			// The S2C 0x53 mode_b lands at the zone entity's +0x223 and stays
			// [orig: ZoneTimerList_SetEntryWindow @0x537DE0 via the 0x53
			//  handler @0x428AE0].
			if (live != runtime->zone_states().end() && live->second.has_window)
				zone.capture_team = live->second.window.mode_b;
			// The zone entity's +550 / +548 words keep the last 0x6E values
			// (replication ClientZoneWaveCounts).
			for (const replication::ClientZoneWaveCounts &c : runtime->state().zone_wave_counts) {
				if (c.zone_handle != e->handle.packed) continue;
				zone.queued = c.member_count;
				zone.countdown = c.wave_countdown;
			}
		}
		// sub_597FD0's entity reads (hud_map_view.h death_map_zone_blip).
		zone.has_def = e->has_item_def;
		zone.def_type = e->has_item_def ? e->item_type : 0u;
		zone.def_attrib = e->has_item_def ? e->item_attrib : 0u;
		zone.def_id = e->item_id;
		const uint32_t eflags = e->flags | e->engine_flags;
		zone.dead = (eflags & world::kEntityFlagDead) != 0;
		zone.carried = (eflags & world::kEntityFlagCarried) != 0;
		zone.local_player = e->handle == w.cached.local_player;
		if (const world::Entity *parent = w.registry.get(e->ground_target))
			zone.parent_item = parent->has_item_def && parent->item_type == 1;
		zone.zone_number = e->zone_number;
		if (const world::Entity *occupant = w.registry.get(e->primary_occupant)) {
			zone.occupant_present = true;
			zone.occupant_team = occupant->team;
		}
		// The anchor: the Euler matrix (no scale) about the position applied
		// to the bbox centre [orig: sub_59C300 @0x59c311..0x59c327].
		int32_t euler[3];
		world::entity_live_euler_bam(*e, euler);
		const int32_t position[3] = {io::float_to_fp16_16_sat(e->position.x),
				io::float_to_fp16_16_sat(e->position.y),
				io::float_to_fp16_16_sat(e->position.z)};
		const world::CollisionMatrix m =
				world::collision_matrix_from_euler(euler[0], euler[1], euler[2], position);
		const int32_t centre[3] = {io::float_to_fp16_16_sat(e->bbox_center.x),
				io::float_to_fp16_16_sat(e->bbox_center.y),
				io::float_to_fp16_16_sat(e->bbox_center.z)};
		int32_t anchor[3];
		transform_point22(m, centre, anchor);
		zone.anchor_x = anchor[0];
		zone.anchor_y = anchor[1];
		out.zones.push_back(zone);
	}
	return true;
}

void banked_spawn_zones(const RoleView &view, std::vector<world::EntityHandle> &out) {
	out.clear();
	if (view.kernel == nullptr || view.runtime == nullptr) return;
	const world::World &w = view.kernel->world;
	const replication::ClientMinimapState &banks = view.runtime->state().minimap;
	auto walk = [&](const auto &bank) {
		for (const replication::ClientMinimapOverlaySlot &slot : bank) {
			if (slot.handle == 0xFFFFu || (slot.flags & 0x40u) != 0) continue;
			const world::EntityHandle handle{slot.handle};
			const world::Entity *e = w.registry.get(handle);
			// The def (+32) and its spawn-zone attribute (+84 & 0x40000).
			if (e == nullptr || !e->has_item_def || !e->is_spawn_point) continue;
			out.push_back(handle);
		}
	};
	walk(banks.transient);
	walk(banks.persistent);
	walk(banks.special);
}

bool command_map_roster(const RoleView &view, menu::CommandMapRoster &out) {
	out = menu::CommandMapRoster{};
	if (view.kernel == nullptr || view.runtime == nullptr) return false;
	const world::World &w = view.kernel->world;
	const ClientRuntime &runtime = *view.runtime;
	const replication::ClientState &cs = runtime.state();
	const int local_slot = runtime.local_roster_slot();
	out.local_slot = static_cast<uint8_t>(local_slot < 0 ? 0xFF : local_slot);
	if (const world::Entity *player = w.registry.get(w.cached.local_player))
		out.local_team = static_cast<int8_t>(player->team);
	out.death_screen = local_death_screen_active(view);
	out.in_session = view.joiner ? runtime.in_session() : w.rules.mp_session;
	for (size_t i = 0; i < cs.roster.size(); ++i) {
		const replication::ClientRosterSlot &slot = cs.roster[i];
		if (!slot.bound) continue;
		menu::CommandMapPlayer p;
		p.slot = static_cast<uint8_t>(i);
		p.team = slot.team;
		p.name = slot.name;
		p.has_entity = slot.entity_slot >= 0;
		if (p.has_entity) {
			if (const world::Entity *e = w.registry.get(world::EntityHandle::make(
						0, static_cast<uint16_t>(slot.entity_slot))))
				p.player_class = e->player_class;
		}
		p.spectator = slot.spectator;
		p.leader = slot.squad_leader;
		p.fireteam = slot.fireteam;
		p.mute = slot.radio_mute_flags;
		p.squad_color = slot.squad_color;
		p.punt_mark = slot.punt_mark;
		out.players.push_back(std::move(p));
	}
	return true;
}

bool command_map_locations(const RoleView &view, const world::SpawnZoneRegistry &zones,
		menu::CommandMapLocations &out) {
	out = menu::CommandMapLocations{};
	if (view.kernel == nullptr) return false;
	const world::World &w = view.kernel->world;
	for (const world::UserWaypointTable::Entry &entry : w.user_waypoints.entries)
		out.user_waypoints.push_back(world::user_waypoint_row(w, entry.handle).name);
	std::vector<world::EntityHandle> banked;
	banked_spawn_zones(view, banked);
	for (const world::EntityHandle handle : banked)
		out.zone_indices.push_back(world::spawn_zone_index_of(zones, handle));
	if (view.runtime != nullptr && !view.runtime->state().location_names.empty())
		out.location_names = view.runtime->state().location_names;
	else if (view.host != nullptr)
		out.location_names = view.host->mission_location_names;
	return true;
}

bool death_shroud_revealed(const RoleView &view) {
	if (view.kernel == nullptr) return false;
	const bool deploy_active =
			view.runtime != nullptr && view.runtime->state().deploy_overlay_active;
	// The death stamp is the local player view's (world/local_player_view.cpp
	// death_cam.start_tick, stamped on the local dead edge); the difference
	// is retail's signed int.
	const uint32_t now = view.kernel->world.logic_tick;
	const int32_t since_death =
			static_cast<int32_t>(now - view.kernel->local.view.death_cam.start_tick);
	return world::death_shroud_revealed(deploy_active, since_death);
}

hud::HudMapGridOrigin hud_map_grid_origin(const RoleView &view) {
	hud::HudMapGridOrigin v;
	v.present = view.kernel != nullptr && view.kernel->world.tables.map_grid_origin_present;
	v.x_q16 = v.present ? view.kernel->world.tables.map_grid_origin_x : 0;
	v.y_q16 = v.present ? view.kernel->world.tables.map_grid_origin_y : 0;
	if (!v.present && view.runtime != nullptr)
		v.present = replication::client_minimap_grid_origin(view.runtime->state(), v.x_q16, v.y_q16);
	return v;
}

bool joiner_deploy_hold_ready(const RoleView &view) {
	if (!view.joiner || view.runtime == nullptr) return false;
	const ClientRuntime &runtime = *view.runtime;
	return runtime.awaiting_deploy_pick() && runtime.has_self_handle() &&
			runtime.last_error().empty() && !runtime.session_lost();
}

bool joiner_in_match_ready(const RoleView &view, bool auto_deploy) {
	if (!view.joiner || view.runtime == nullptr || view.kernel == nullptr) return false;
	const ClientRuntime &runtime = *view.runtime;
	return view.kernel->world.cached.local_player.valid() && runtime.in_match() &&
			(!auto_deploy || !runtime.deployment_pick_pending());
}

bool deploy_zone_rows(const RoleView &view, const world::SpawnZoneRegistry &zones,
		std::vector<world::DeployZoneRow> &out) {
	out.clear();
	if (view.kernel == nullptr || !view.joiner || view.runtime == nullptr) return false;
	const ClientRuntime &runtime = *view.runtime;
	const world::World &w = view.kernel->world;
	const uint8_t team = runtime.assigned_team();
	const replication::ClientState &cs = runtime.state();
	const uint16_t self_handle = runtime.has_self_handle() ? runtime.self_handle() : 0xFFFFu;
	// The zone walk is the minimap banks' (banked_spawn_zones): a zone the
	// server never sent is not listed, the rows follow first arrival, and a
	// banked zone outside the SpawnZoneList lists as index -1.
	std::vector<world::EntityHandle> banked;
	banked_spawn_zones(view, banked);
	for (const world::EntityHandle handle : banked) {
		const world::Entity *e = w.registry.get(handle);
		if (e == nullptr) continue;
		const int index = world::spawn_zone_index_of(zones, handle);
		uint8_t effective_team = e->team;
		int32_t effective_control = e->zone_control;
		int32_t effective_limit = 0x10000;
		const auto live = runtime.zone_states().find(e->handle.packed);
		if (live != runtime.zone_states().end()) {
			// The DEATH list reads the value entry's team and exact value >= limit
			// gate. 0x53 is the separate timed-capture window; retaining an old
			// window after a later 0x6F must not overwrite this ownership channel.
			// [orig: UI_UpdateDeathScreenContent @0x5536a0; §5.49/§5.61]
			if (live->second.has_value) {
				effective_team = live->second.value.mode;
				effective_control = live->second.value.value_s;
				effective_limit = live->second.value.limit_s;
			}
		}
		if (effective_team != team) continue;
		world::DeployZoneRow row;
		// No -1 guard in the first loop: an unregistered zone lists as '@'
		// with STRWPNAME000 and value 0 [orig: @0x553bd6..0x553c1f].
		row.index = index;
		row.letter = static_cast<char>('A' + index);
		char name_key[32];
		std::snprintf(name_key, sizeof(name_key), "STRWPNAME%03d", index + 1);
		row.name_key = name_key;
		// The first-loop gate: a zone whose live timer entry sits below its limit
		// is NOT listed; no entry (or level >= limit) lists it. There is no zone-
		// number term (an earlier port carried one) — the retail list walk tests
		// only the timer entry's level against its limit and then the def's
		// spawn-zone attribute (engine record: deploy_screen_feed.h cites the
		// UI_UpdateDeathScreenContent list loop).
		row.secured = !(effective_control < effective_limit);
		// The 0x6E wave group on this zone: its countdown (entity+548) and the
		// queued members, named through the roster the way retail reads the
		// member entity's Name (the player entity's name IS the roster name)
		// [orig: dword_A85BC4[idx] / unk_A85CC4 @0x553cd0..0x553d8b, see world/deploy_screen_feed.h].
		if (cs.spawn_waves.known) {
			for (const SpawnWaveGroup &g : cs.spawn_waves.value.groups) {
				if (g.zone_handle != e->handle.packed) continue;
				row.wave_countdown = static_cast<uint16_t>(g.wave_countdown);
				for (uint16_t member : g.members) {
					world::DeployOccupant o;
					o.handle = member;
					std::string name;
					const world::EntityHandle mh{ member };
					for (const replication::ClientRosterSlot &slot : cs.roster) {
						if (slot.bound && slot.entity_slot == mh.slot() && mh.pool() == 0) {
							name = slot.name;
							break;
						}
					}
					if (name.empty()) {
						if (const replication::ClientEntityState *row_state = cs.find(member))
							name = row_state->display_name;
					}
					o.name = name;
					o.self = member == self_handle;
					row.occupants.push_back(o);
				}
			}
		}
		out.push_back(row);
	}
	return true;
}

namespace {

// The contained half of one drawn entity's lighting from its first blink hit
// (entity+0x1D0): the containing pool-2 building's identity and blink volume
// section, and for a PERSON the building's ItemDef+0x218 daylight: only the
// person wave loads the aux [orig: Terrain_RenderSectorEntitiesBySide
// @0x5c7da8..0x5c7db6 (Pool_GetEntryUnchecked(2, hit >> 20)) and
// @0x5c7f7a..0x5c7f93 (entry+0x20 -> ItemDef+0x218 when non-null)].
EntityLighting contained_lighting(const world::World &w, uint32_t first_hit, bool person) {
	EntityLighting out;
	out.interior = true;
	out.interior_section = world::BlinkAccum::hit_section(first_hit);
	const world::Entity *parent = w.registry.get(world::EntityHandle::make(
			2, world::BlinkAccum::hit_pool_entity_index(first_hit)));
	if (parent != nullptr) {
		out.interior_bms = parent->bms_id;
		if (person && parent->has_item_def) out.light_transfer = parent->light_transfer;
	}
	return out;
}

} // namespace

void EntityLightingFeed::collect(const RoleView &view, const int32_t sun_step_q16[3],
		const std::unordered_set<int32_t> &culled_bms,
		const std::unordered_set<int32_t> &culled_wire, int64_t layout_revision,
		std::vector<EntityLightingChange> &out) {
	out.clear();
	if (view.kernel == nullptr) return;
	mission::MissionKernel &kernel = *view.kernel;
	world::World &w = kernel.world;
	if (layout_revision_seen != layout_revision) {
		last_by_wire.clear();
		layout_revision_seen = layout_revision;
	}

	// One registry entity's context. Contained entities route through the
	// interior light group; the outdoor factor stays 1.0 [orig: the blink-ref
	// branch @0x5c74b7]. The +0x1C0 slice gate [orig: @0x5c6808] lives inside
	// the ray walk: an entity with no proximity-candidate slice blocks nothing
	// and holds quality 4 — statics never ray, and only slice candidates
	// (structures overlapping the entity's inflated bubble) can shade it.
	const auto entity_lighting = [&](const world::Entity &e) {
		if (e.blink_hits[0] != 0)
			return contained_lighting(w, e.blink_hits[0], e.item_type == 3);
		EntityLighting outdoor;
		const int blocked = kernel.collision.sun_visibility_blocked_rays(w, e, sun_step_q16);
		outdoor.quality = static_cast<uint8_t>(4 - blocked);
		return outdoor;
	};

	// The local player is a spawned entity (bms_id 0, outside the placed-node
	// walk); its quality feeds the presenter seam only.
	const world::Entity *local = w.registry.get(w.cached.local_player);
	local_quality = local != nullptr ? entity_lighting(*local).quality : 4;

	w.registry.for_each([&](const world::Entity &e) {
		// The pool-2 decorations and foliage are entity-collected, so the
		// entity wave lights them too; only the Building-type defs draw in the
		// building pass. [orig: Terrain_RenderSectorEntities walks
		// g_SectorEntityList — which Terrain_CollectVisibleEntities_0 fills from
		// the non-building statics — and sets each one's context
		// (Terrain_SetupEffectForEntity @ 0x5c7bf1); world::building_def_row]
		if ((e.kind == world::EntityKind::Building && world::building_def_row(e)) ||
				e.kind == world::EntityKind::Marker)
			return;
		// Only authored placements have a placed node addressed by BMS id.
		// Runtime-spawned rows can also carry a nonzero bms_id (players use
		// their net id), but the wire walk owns their rendering.
		if (e.bms_id == 0 || e.spawn_origin == world::kSpawnOriginNone) return;
		if (e.handle == w.cached.local_player) return;
		// Retail only lights a drawn entity; a culled one keeps its last
		// context until it renders again (the stack slot is simply never pushed).
		if (culled_bms.count(e.bms_id) != 0) return;
		const EntityLighting lighting = entity_lighting(e);
		const auto it = last_by_bms.find(e.bms_id);
		const EntityLighting last = it != last_by_bms.end() ? it->second : EntityLighting{};
		if (lighting == last) return;
		last_by_bms[e.bms_id] = lighting;
		out.push_back(EntityLightingChange{ false, e.bms_id, 0, lighting });
	});

	// Every rendered role consumes ClientState. Placed rows above continue to
	// address their placed node by BMS id; only rows without authored identity
	// reach the wire walk and therefore need a wire-handle lighting update.
	// On a joiner, pool-0 H must NEVER be cast to a local EntityHandle (H=0 and
	// L=0 can coexist); streamed pool-1 twins are allowed only after the type
	// check below. Host/SP rows use their authoritative exact-handle entity.
	// Cadence: like the registry walk above, the casts run per display frame.
	// Retail casts inside the sector render walk for every drawn entity every
	// frame; only the candidate SLICE the walker iterates refreshes on the
	// 17-tick arena edge, which CollisionWorld::build_tick_tables already
	// mirrors for wire rows [orig: Terrain_RenderSectorEntities @0x5c7bf1 /
	// Terrain_RenderSectorEntitiesBySide @0x5c7f9a -> Terrain_SetupEffectForEntity
	// @0x5c74a0 -> Entity_ComputeSunVisibility @0x5c6800 per frame; the slice
	// gate g_ProxSliceRefreshCounter >= 0x10 @0x4c240f ->
	// Entity_BuildProximityListsFromPools @0x4c2418, see
	// docs/render/render-lighting-re.md]. Throttling the casts themselves to
	// that cadence would hold a moving vehicle's sun factor stale for up to 16
	// ticks; the per-handle cache below only suppresses unchanged emits.
	if (!view.joiner) {
		// The host presents its own pools (D-NET-140 closed): the wire-rendered
		// rows are the runtime-spawned pool-0 organics and pool-1 dynamics with
		// no authored identity; placed rows went through the walk above.
		for (int pool = 0; pool <= 1; ++pool) {
			w.registry.for_each_in_pool(pool, [&](const world::Entity &e) {
				const uint16_t handle = e.handle.packed;
				if (e.item_id == 0 || e.handle == w.cached.local_player ||
						e.spawn_origin != world::kSpawnOriginNone) {
					last_by_wire.erase(handle);
					return;
				}
				// A hidden row is not drawn, so retail does not push a new stack
				// value; preserve the last emitted context (see the wire loop).
				if ((e.flags & 0x01u) != 0) return;
				const EntityClass cls = replication::entity_class_of(e);
				const bool person_source = pool == 0 &&
						(cls == EntityClass::Player || cls == EntityClass::Infantry);
				const bool dynamic_source = pool == 1 &&
						kernel.wire_collision_shape_for_type(static_cast<uint16_t>(e.item_id))
								.pool1_candidate_source_eligible;
				// A drawn row without a candidate slice never rays (quality 4)
				// but still takes the interior route when contained.
				EntityLighting lighting;
				if (e.blink_hits[0] != 0) {
					lighting = contained_lighting(w, e.blink_hits[0], e.item_type == 3);
				} else if (person_source || dynamic_source) {
					lighting = entity_lighting(e);
				}
				const auto it = last_by_wire.find(handle);
				const EntityLighting last = it != last_by_wire.end() ? it->second : EntityLighting{};
				if (lighting == last) return;
				last_by_wire[handle] = lighting;
				out.push_back(EntityLightingChange{ true, 0, handle, lighting });
			});
		}
	} else if (view.runtime != nullptr) {
		const ClientRuntime &runtime = *view.runtime;
		for (const replication::ClientEntityState &es : runtime.state().entities) {
			const uint16_t handle = es.handle;
			if (handle == world::EntityHandle::kInvalid || es.type_id == 0 ||
					(runtime.has_self_handle() && handle == runtime.self_handle())) {
				last_by_wire.erase(handle);
				continue;
			}
			// Retail only lights a drawn entity; a culled one keeps its last
			// context until it renders again (see the registry walk above).
			if (culled_wire.count(static_cast<int32_t>(handle)) != 0) continue;
			// A hidden row is not drawn, so retail does not push a new stack
			// value. Preserve the last emitted context: if it moves while hidden,
			// the first visible frame must compare against that retained material
			// state and emit the restoration instead of assuming the default.
			if (es.state_flags_known && (es.state_flags & 0x01u) != 0) continue;

			const world::EntityHandle h{ handle };
			const world::Entity *joiner_twin = nullptr;
			if (h.pool() != 0) {
				const world::Entity *candidate = w.registry.get(h);
				if (candidate != nullptr && static_cast<uint16_t>(candidate->item_id) == es.type_id)
					joiner_twin = candidate;
			}
			// The wire walk defers authored rows to their placed node (or static
			// batch). Do not repeat the same native ray query and cache an update
			// for a wire node that deliberately does not exist.
			if (h == w.cached.local_player ||
					(joiner_twin != nullptr && joiner_twin->spawn_origin != world::kSpawnOriginNone)) {
				last_by_wire.erase(handle);
				continue;
			}

			const bool person_source = h.pool() == 0 &&
					(es.cls == EntityClass::Player || es.cls == EntityClass::Infantry);
			const world::ResolvedCollisionShape shape = kernel.wire_collision_shape_for_type(es.type_id);
			const bool dynamic_source = h.pool() == 1 && shape.pool1_candidate_source_eligible;

			// The client's own +0x1D0 for the row: a pool-1 twin carries it; a
			// decoded source without one runs the client-side blink walk at its
			// decoded position [orig: Entity_BuildProximityList @0x4b3dc0 from
			// the client entity updates, e.g. Entity_UpdatePool1Slot @0x4b8e25].
			uint32_t first_hit = 0;
			if (joiner_twin != nullptr) {
				first_hit = joiner_twin->blink_hits[0];
			} else {
				const int32_t pos[3] = { es.x, es.y, es.z };
				world::BlinkAccum blink;
				kernel.collision.query_wire_blink_boxes_at_point(w, handle, pos,
						person_source || shape.item_type == 1 || shape.item_type == 3, blink);
				first_hit = blink.hits[0];
			}
			EntityLighting lighting;
			if (first_hit != 0) {
				lighting = contained_lighting(w, first_hit, person_source || shape.item_type == 3);
			} else if (person_source || dynamic_source) {
				const int blocked = kernel.collision.wire_sun_visibility_blocked_rays(w, handle,
						world::FixedVec3{ es.x, es.y, es.z }, shape.bbox_center_q16, sun_step_q16);
				lighting.quality = static_cast<uint8_t>(4 - blocked);
			}

			const auto it = last_by_wire.find(handle);
			const EntityLighting last = it != last_by_wire.end() ? it->second : EntityLighting{};
			if (lighting == last) continue;
			last_by_wire[handle] = lighting;
			out.push_back(EntityLightingChange{ true, 0, handle, lighting });
		}
	}
}

namespace {

// One entity's team, playerClass and name as retail's drawers read them off
// the entity (+0x162, +0x294, +0xF4): the authority's own pools, or a
// joiner's decoded row — its compact field-17 low nibble once a compact
// landed (the client apply rewrites playerClass [orig:
// Entity_SetHealthFromDifficultyByte @0x4AD580]), else the spawn's class.
struct EntityReads {
	bool found = false;
	uint8_t team = 0;
	uint8_t player_class = 0;
	std::string name;
};
EntityReads entity_reads(const RoleView &view, uint16_t handle) {
	EntityReads r;
	if (view.joiner) {
		const replication::ClientEntityState *row =
				view.runtime != nullptr ? view.runtime->state().find(handle) : nullptr;
		if (row == nullptr) return r;
		r.found = true;
		r.team = row->team_known ? row->team : 0;
		r.player_class = row->net_has_compact
				? static_cast<uint8_t>(row->health_class_byte & 0x0Fu)
				: row->spawn_player_class;
		r.name = row->display_name;
		return r;
	}
	if (view.kernel == nullptr) return r;
	const world::Entity *e = view.kernel->world.registry.get(world::EntityHandle{handle});
	if (e == nullptr) return r;
	r.found = true;
	r.team = e->team;
	r.player_class = e->player_class;
	r.name = e->display_name;
	return r;
}

} // namespace

SessionVars scoreboard_session_vars(const RoleView &view) {
	if (view.joiner)
		return view.runtime != nullptr ? view.runtime->session_vars() : SessionVars{};
	if (view.host == nullptr)
		return {};
	SessionVars vars = host_session_vars(view.host->config);
	if (view.host->is_mp_session_peer != 0) {
		const std::vector<uint8_t> stream = encode_session_vars(vars);
		decode_session_vars(stream.data(), stream.size(), vars);
	}
	return vars;
}

bool command_map_rules_text(const RoleView &view, const hud::GameTextLookup &gametext,
		std::string &out) {
	if (view.runtime == nullptr || view.kernel == nullptr) return false;
	const replication::ClientState &state = view.runtime->state();
	world::World &w = view.kernel->world;
	replication::ClientSessionStatus status;
	hud::RulesBriefingInputs inputs;
	if (view.joiner) {
		status = state.session_status;
		inputs.config_first = replication::server_config_first_text(state.server_config_strings);
		inputs.config_second = replication::server_config_second_text(state.server_config_strings);
	} else if (view.host != nullptr) {
		const std::vector<uint8_t> body = serialize_session_status(view.host->config,
				view.host->np_protocol.host_run_duration_ms, view.host->is_in_session != 0, &w);
		SessionStatusBlock block;
		(void)decode_session_status(body.data(), body.size(), block);
		status = replication::fold_session_status(block, state.local_clock_ms);
		inputs.authority = true;
		// MissionText info/briefing3 and info/briefing2, else info/briefing
		// [orig: HUD_BuildRulesAndBriefingText @0x5b92d0 — the authority arm].
		inputs.briefing3 = view.host->mission_briefing3;
		inputs.briefing2 = view.host->mission_briefing2;
	} else {
		return false;
	}
	hud::SessionStatusView sv;
	sv.valid = status.valid;
	sv.server_name = status.server_name;
	sv.mission_name = status.mission_name;
	sv.max_players = status.max_players;
	sv.elapsed_ms = replication::session_status_elapsed_ms(status, state.local_clock_ms);
	sv.stats = status.stats;
	for (const replication::ClientSessionStatus::Option &o : status.options)
		sv.options.emplace_back(o.key, o.value);
	inputs.game_type = view.runtime->game_type();
	// [orig: `g_LocalPlayerEntity ? ->Team (+0x162) : 0` @0x5b92de..0x5b92f1]
	if (const world::Entity *local = w.registry.get(w.cached.local_player))
		inputs.local_team = static_cast<int8_t>(local->team);
	inputs.spawn_zones = w.zones.has_spawn_zone(); // sub_43B910
	// [orig: g_ScoreboardInGameCount, the 0x16 trailer @0x42fe70]
	return hud::build_end_game_stats_text(sv, state.scoreboard.in_game_count, inputs, gametext, out);
}

void scoreboard_feed(const RoleView &view, hud::HudScoreboardState &out) {
	out.rows.clear();
	out.team_count = 0;
	out.teams = {};
	out.flag_carrier = false;
	out.flag_carrier_name.clear();
	out.flag_carrier_team = 0;
	out.local_team = -1;
	if (view.runtime == nullptr) return;
	const replication::ClientState &state = view.runtime->state();
	replication::project_scoreboard(state, out.rows, [&view](uint16_t handle) {
		const EntityReads r = entity_reads(view, handle);
		replication::ScoreboardEntityFacts facts;
		facts.found = r.found;
		facts.team = r.team;
		facts.player_class = r.player_class;
		return facts;
	});
	// [orig: g_ScoreboardTeamCount @0x42fdda; the table 0xA85AEC + 16t]
	out.team_count = static_cast<int>(state.scoreboard.team_count);
	for (size_t t = 0; t < out.teams.size() && t < state.scoreboard.teams.size(); ++t) {
		out.teams[t].score1 = state.scoreboard.teams[t].score1;
		out.teams[t].ctf_flag = state.scoreboard.teams[t].ctf_flag;
		out.teams[t].koth_hold = state.scoreboard.teams[t].koth_hold;
	}
	out.status_suffix = state.scoreboard_status_suffix != 0; // [orig: @0x423ef8]
	out.timed = state.scoreboard.timed;                       // [orig: g_ScoreboardFlags & 2]
	// [orig: `is_authority ? g_TimeLimitMinutes : dword_A821C0` @0x423a4b]
	out.time_limit = !view.joiner && view.host != nullptr
			? static_cast<int>(view.host->config.time_limit_minutes)
			: static_cast<int>(state.session_time_limit_minutes);
	// The local player's own entity team [orig: g_LocalPlayerEntity->Team
	// @0x423d64].
	if (view.joiner) {
		if (view.runtime->has_self_handle()) {
			const EntityReads self = entity_reads(view, view.runtime->self_handle());
			if (self.found) out.local_team = self.team;
		}
	} else if (view.kernel != nullptr) {
		if (const world::Entity *player = view.kernel->local.player()) out.local_team = player->team;
	}
	// The latched carrier [orig: dword_A860C4 @0x423944; +0x162 @0x42396f;
	// +0xF4 @0x4239c3]. The latch stores whatever the attach handle names;
	// the drawer's `if (dword_A860C4)` is the entity's presence.
	if (state.flag_carrier_handle != 0xFFFFu) {
		const EntityReads carrier = entity_reads(view, state.flag_carrier_handle);
		if (carrier.found) {
			out.flag_carrier = true;
			out.flag_carrier_name = carrier.name;
			out.flag_carrier_team = carrier.team;
		}
	}
}

hud::ChatEntryFacts chat_entry_facts(const RoleView &view, bool novaworld, uint32_t frame) {
	hud::ChatEntryFacts f;
	f.frame = frame;
	f.novaworld = novaworld;
	f.death_screen = local_death_screen_active(view); // [orig: g_DeathScreenActive]
	if (view.runtime != nullptr) {
		const replication::ClientState &state = view.runtime->state();
		f.spawn_gate = state.spawn_success_gate;  // [orig: g_SpawnSuccessGate]
		f.reset_hold = state.round_reset_hold;         // [orig: dword_24C195C]
		f.team_game = (view.runtime->game_type() & 0x10000u) != 0u; // [orig: @0x49b9bf]
	}
	// [orig: g_NapiNPCtx.is_in_session / is_authority] — the authority's
	// multiplayer session is the world's mp_session rule: the SP listen
	// server runs its own replication loop but is never in a session.
	f.in_session = view.joiner ? (view.runtime != nullptr && view.runtime->in_session())
			: (view.kernel != nullptr && view.kernel->world.rules.mp_session);
	f.authority = !view.joiner && view.host != nullptr;
	f.mp_session_peer = f.in_session;
	if (view.kernel != nullptr) {
		const world::World &w = view.kernel->world;
		if (const world::Entity *local = w.registry.get(w.cached.local_player)) {
			f.has_local_player = true;
			// [orig: Entity_FindChildByDefType(local, 1, 0) @0x49ba38 — the
			// groundEntity walk for a def-type-1 link]
			f.in_vehicle = world::friendly_tag_aboard_vehicle(w, local->ground_target);
		}
	}
	return f;
}

void step_hud_radar(mission::MissionKernel &kernel, ClientRuntime *runtime, bool pass_runs,
		bool map_site, bool menu_paused, hud::HudMinimapRadar &out) {
	world::World &w = kernel.world;
	// Retail's update reads g_CurrentTick [orig: @0x5a8176 / @0x5a7914].
	const int32_t aged = world::radar_hud_frame(w, w.logic_tick, pass_runs, map_site,
			menu_paused, out);
	// [orig: Radar_UpdateContacts `test edi, edi; jz` @0x59a9c9 ->
	//  MapOverlay_UpdateTimers @0x59a9ce]
	if (aged != 0 && runtime != nullptr)
		runtime->view().age_minimap_overlays(static_cast<uint32_t>(aged));
}

} // namespace opennova::inmatch
