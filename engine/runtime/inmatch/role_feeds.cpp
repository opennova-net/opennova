// The role feeds — see role_feeds.h. [orig: NapiNPClientMsg_0x01D @0x430840;
// UI_UpdateDeathScreenContent @0x5536a0; HUD_DrawFriendlyTagsPass @0x5a4480]

#include <runtime/inmatch/role_feeds.h>

#include <base/gameprofile/game_type.h>
#include <base/io/fixed.h>
#include <base/title/title.h> // Title::hud_exp_points
#include <runtime/replication/client_scoreboard_view.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/game_config.h>
#include <runtime/inmatch/minimap_overlays.h> // entity_display_name
#include <runtime/inmatch/mission_exit.h> // kMissionExitRoundOver / kMissionExitQuit
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_message_dispatch.h> // host_session_vars
#include <runtime/inmatch/session.h> // Role::store_mission_exit
#include <runtime/inmatch/session_status.h> // the authority's own 0x58 report
#include <runtime/hud/hud_toggles.h> // hud_deploy_key_pick
#include <runtime/hud/session_rules_text.h>
#include <runtime/menu/command_map_screen.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/replication/client_roster_tags.h>
#include <runtime/replication/client_world_materializer.h> // replica_row_entity
#include <runtime/replication/entity_wire_bridge.h> // entity_class_of
#include <runtime/world/collision.h>
#include <runtime/world/entity_spawn.h> // max_health_with_difficulty
#include <runtime/world/radar_contacts.h> // radar_hud_frame
#include <runtime/world/radio_call.h> // capture_zone_max_coverage, voice_macro_text
#include <runtime/world/user_waypoints.h>

#include <algorithm>
#include <cstdio>

namespace opennova::inmatch {

bool local_death_screen_active(const RoleView &view) {
	return view.runtime != nullptr && view.runtime->state().death_screen_active;
}

uint32_t session_rules_word(const RoleView &view) {
	// A joiner's word is the world's copy of the mission data's, which an admin SET then holds
	// until the next copy [orig: NapiNPClientMsg_HandleMissionDataChunk @0x432410 (the memcpy
	//  @0x432470); CAdminServer_HandleSetCommand @0x405A60 (@0x405C80..0x405F86)].
	return view.kernel != nullptr ? view.kernel->world.rules.mpattrib : 0u;
}

bool local_player_dead(const RoleView &view) {
	// The local entity's dead bit (entity+0x24 & 2). A joiner's is its body's own latch, never
	// its self record's state bit: the record's flag fold masks bit 1 out for every entity
	// [orig: NetPacket_SerializePlayerState @0x4C09C0 - `and ebx, 0E1h` @0x4C12FF (the local
	//  player), `and ebx, 0FDh` @0x4C1307, the fold `xor [edi+24h], ebx` @0x4C1311].
	if (view.joiner) return view.runtime != nullptr && view.runtime->local_player_dead();
	return view.kernel != nullptr && view.kernel->local.local_player_dead();
}

bool death_menu_triggered(const RoleView &view) {
	if (view.runtime == nullptr || view.kernel == nullptr) return false;
	if (view.runtime->state().deploy_overlay_active) return true;
	const world::World &w = view.kernel->world;
	const bool respawns = w.rules.mp_session ||
			(w.tables.mission_attrib_flags & world::MissionTables::kMissionAttribSinglePlayerRespawn) != 0;
	if (!respawns) return false;
	return local_player_dead(view);
}

bool hud_overlays_called(const RoleView &view, bool help_open) {
	if (view.kernel == nullptr) return false;
	// The death.mnu triggers take the frame to the menu's open leg [orig:
	//  Render_ProcessMainSceneFrame @0x5CAB2B..0x5CAB58 (the dead bit in session or under
	//  the respawn attribute), @0x5CAC81..0x5CAC88 (g_DeployScreenActive) -> loc_5CAB5E].
	if (death_menu_triggered(view)) return false;
	// A latch left from an open takes it to the close leg [orig: `cmp g_DeathMenuOpenLatch`
	//  @0x5CAC8E -> Game_CloseInGameScreens @0x5CAC9C].
	if (view.runtime != nullptr && view.runtime->state().death_menu_open_latch) return false;
	const world::PlayerViewState &v = view.kernel->local.view;
	// Camera mode 0, or the death screen: the pass for a body without the dead bit, the F1
	// help shut [orig: @0x5CACA6..0x5CACB7; @0x5CACED..0x5CACFE (entity+0x24 & 2,
	//  g_HelpScreenOpen); the call @0x5CAD04].
	if (v.camera_mode == 0 || v.death_screen_active) return !local_player_dead(view) && !help_open;
	// The chase view: the pass from a control seat alone (parentEntity, parentSlot 2 / 5)
	// [orig: @0x5CACB9..0x5CACDD; the call @0x5CACE3]; any other mode draws none.
	if (v.camera_mode != 1) return false;
	const world::World &w = view.kernel->world;
	const world::Entity *local = w.registry.get(w.cached.local_player);
	return local != nullptr && local->mounted && world::is_vehicle_control_seat(local->mount_type);
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

uint32_t round_over_key(const RoleView &view, Role &role, int vk, int restart_vk) {
	if (view.kernel == nullptr) return 0;
	world::World &w = view.kernel->world;
	// The leg runs only behind the round-over gate: the authority's round end,
	// a joiner's 0x1D [orig: `cmp g_RoundOver, 0` @0x49c7cf; the writers
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
	// The world halves of the SP arm.
	if (!in.in_session) {
		if ((bits & hud::hud_round_over::kRestart) != 0) w.round_over_restart();
		if ((bits & hud::hud_round_over::kExit) != 0) w.round_over_exit();
		return bits;
	}
	// The co-op session arm stores the same reasons on the role's
	// g_MissionExitReason; the epilog word the RESTART clears first is the SP
	// cine's, never raised in a session.
	// [orig: Input_HandleSpecialKeys @0x49c5c0 (RESTART: g_MissionExitReason = 4 @0x49c8ad)]
	if ((bits & hud::hud_round_over::kRestart) != 0)
		role.store_mission_exit(kMissionExitRoundOver);
	// [orig: Input_HandleSpecialKeys @0x49c5c0 (ESC: g_MissionExitReason = 1 @0x49c8e2)]
	if ((bits & hud::hud_round_over::kExit) != 0) role.store_mission_exit(kMissionExitQuit);
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
// [orig: HUD_DrawEndRoundStatsOverlay @0x5b7cd0 reads g_LocalTeam @0x5b7f56].
// A joiner latches it from S2C 0x04 (ClientRuntime::assigned_team); the
// listen host's HostClient runtime never receives that record (the latch
// stays 0), so the authority reads its own player entity — retail's host
// latches the same value from its loopback 0x04 @0x425499.
int local_team_of(const RoleView &view) {
	if (view.joiner) return view.runtime != nullptr ? view.runtime->assigned_team() : 0;
	const world::Entity *player = view.kernel != nullptr ? view.kernel->local.player() : nullptr;
	return player != nullptr ? player->team : 0;
}

// A Serve Only host folds no 0x1D: its end-round words are the board its own round end built,
// whose build writes the globals the overlay reads (the winner, the two team scores, the draw
// flag, rows 0..2's names and primary scores), then raises g_RoundOver. Its status page's
// end-round leg runs the transition on that latch; the transition's STAT arm waits on
// g_ScoreboardDirty, which only a client's 0x56 landing sets (a client's 0x1D landing and the
// round-end progress save's memset clear it), so the page never opens stat.mnu. Null on any
// other role, or before the round ends.
// [orig: Server_ProcessRoundEnd @0x5164F0 (Server_BuildEndOfRoundScoreboard(1, winTeam) @0x516590,
//  g_RoundOver @0x5168E4); Server_BuildEndOfRoundScoreboard @0x508F30 (g_EndRoundWinnerTeam
//  @0x508F77, g_ScoreTeamScore0 / 1 @0x508FD2..0x508FE0, the rows from 0x24C1A94: the name
//  @0x5090D3, the primary @0x509152); Server_DrawStatusScreen @0x50A2D0 (@0x50B286..0x50B2A3);
//  UI_ProcessEndRoundScreenTransition @0x5B8600 (`cmp g_ScoreboardDirty, 0` @0x5B8600, the
//  first pass @0x5B864B..0x5B866D, HUD_DrawEndRoundStatsOverlay @0x5B8679); g_ScoreboardDirty's
//  writers: NapiNPClientMsg_0x056 (the set @0x4321BE), NapiNPClientMsg_0x01D (the clear
//  @0x430A2D), PlayerProfile_SaveRoundEndProgress (the memset @0x54D6B0)]
const world::MatchResult *serve_only_round_board(const RoleView &view) {
	if (view.runtime != nullptr || view.host == nullptr || view.kernel == nullptr) return nullptr;
	const world::Match &match = view.kernel->world.match;
	return match.outcome().ended && match.result().ready ? &match.result() : nullptr;
}

} // namespace

EndRoundSessionState end_round_session_state(const RoleView &view) {
	EndRoundSessionState v;
	if (const world::MatchResult *board = serve_only_round_board(view)) {
		v.header_known = true;
		v.game_type = board->game_type;
		v.winner = board->winner_team;
		v.team_score_0 = board->team_scores[0];
		v.team_score_1 = board->team_scores[1];
		v.draw = board->draw;
		v.round_ticks = round_ticks_remaining(view);
		v.team_mode = game_type::is_team(board->game_type);
		v.session_open = view.host->is_in_session != 0;
		return v;
	}
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
	// The local player entity's slot id the stat fill selects its row by
	// [orig: StatScreen_PopulateStatResultsList - g_LocalPlayerEntity +0x154, read
	// @0x562724]: the local body's slot byte (local_body_slot: the slot the role's runtime
	// latched, a joiner's S2C 0x04 or the listen host's own connection, 0xFF while the
	// body is destroyed in place), as the command map's roster reads it.
	v.local_slot = view.runtime->local_body_slot();
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
	// Out of a session no end-round screen runs: single player's own client folds the 0x1D its
	// round end sends, and the overlay panels' pass takes the end-round transition only in a
	// session (world.rules.mp_session is single player's is_in_session; a joiner is always in
	// one) [orig: HUD_DrawOverlayPanels @0x5C0060 - `g_RoundOver && is_in_session` @0x5C0069,
	// else Game_CloseInGameScreens @0x5C0079 / UI_ProcessMenuScreenFrame @0x5C007E].
	if (!view.joiner && view.kernel != nullptr && !view.kernel->world.rules.mp_session) {
		v.header_known = false;
		v.board_known = false;
	}
	return v;
}

hud::EndRoundOverlayInput end_round_overlay_input(const RoleView &view) {
	hud::EndRoundOverlayInput in;
	if (const world::MatchResult *board = serve_only_round_board(view)) {
		// The board build's globals: a row's name is copied into its 32-byte entry, its primary
		// score kept whole (the host's words are 32-bit, the 0x1D's i16 only on the wire); a
		// Serve Only host has no local team and no death screen.
		// [orig: Server_BuildEndOfRoundScoreboard @0x508F30 (Napi_CopyString(entry + 4, name,
		//  32) @0x5090D3, +0x40 @0x509152); HUD_DrawEndRoundStatsOverlay @0x5B7CD0]
		in.game_type = board->game_type;
		in.draw = board->draw;
		in.winner_team = board->winner_team;
		in.team_scores[0] = board->team_scores[0];
		in.team_scores[1] = board->team_scores[1];
		for (size_t i = 0; i < 3 && i < board->players.size(); ++i) {
			in.player_names[i] = board->players[i].identity.name.substr(0, 31);
			in.player_scores[i] = board->players[i].primary_score;
		}
		in.round_time_remaining_ticks = round_ticks_remaining(view);
		return in;
	}
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
		out.round_over = cs.spawn_success_gate;
		if (view.joiner) out.breath_time = cs.breathtime;
	} else if (view.host != nullptr && view.kernel != nullptr) {
		// A Serve Only host has no client to fold a 0x1D: its g_RoundOver is the authority's
		// own round-end latch, which its status page's end-round leg tests
		// [orig: Server_ProcessRoundEnd @0x5164F0 (`mov g_RoundOver, ebp` @0x5168E4);
		//  Server_DrawStatusScreen @0x50A2D0 (`cmp g_RoundOver, 0` @0x50B286)].
		out.round_over = view.kernel->world.match.outcome().ended;
	}
	// The authority's frame carries no sub-block 1 to its own loopback, and
	// its HUD reads the host's own named value [orig: g_WacVarBreathTime].
	if (!view.joiner && view.kernel != nullptr)
		out.breath_time = view.kernel->world.script.wac_values.breathtime;
	return out;
}

namespace {

// A spawn zone's Team byte as this client holds it (defined with the death
// screen's readers below).
uint8_t zone_team_byte(const RoleView &view, const world::Entity &zone);

// One F9 / F10 menu's title and rows over the local player (hud_role_facts'
// doc carries the witness).
void fill_voice_macro_menu(const RoleView &view, const world::Entity &local, bool radio,
		hud::HudVoiceMacroMenuState &menu) {
	const world::World &w = view.kernel->world;
	// The override table first, as every section lookup [orig:
	// HUD_DrawRadioTitleMenu @0x5BFB90 (the calls @0x5BFBD5 / @0x5BFC5D),
	// HUD_DrawEmotesMenu @0x5BFF00 (the calls @0x5BFF45 / @0x5BFFCD) ->
	// TextResource_FindEntryBySectionAndKey @0x75D250].
	const auto lookup = [&w](const std::string &key) -> const std::string * {
		return world::voice_macro_text(w, key);
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
	// The EXPPOINTS element's words (DFX2-36): the 0x81 score and its fanfare arms, the tracker's
	// valid word and its two values [orig: dword_A7BA40 / dword_A7BA4C / dword_A7BA54 /
	//  dword_A7BA58, read by sub_59B7E0 @0x59B998 / @0x59BA38..0x59BA48 (dfx2.exe)].
	out.exp_points.element = view.title != nullptr && view.title->hud_exp_points;
	if (view.runtime != nullptr) {
		const replication::ClientState &cs = view.runtime->state();
		out.exp_points.score = cs.score_feedback.score;
		out.exp_points.fade_arms = cs.score_feedback.fanfare_arms;
		out.exp_points.fade_tone = cs.score_feedback.fanfare_tone;
		out.exp_points.tracker_valid = cs.score_tracker.valid;
		out.exp_points.session_points = cs.score_tracker.elapsed;
		out.exp_points.total_points = cs.score_tracker.time;
	}
	// The connection indicators: the role's replica runtime keeps g_NetQuality
	// (the listen host's own client included); the N icon draws on a
	// NovaWorld session whose NWU session is in use [orig:
	// CNetQuality_DrawIndicators @0x4c33d4..0x4c33f0].
	if (view.runtime != nullptr) {
		out.net_quality.indicators = view.runtime->net_quality_indicators();
		const hud::NovaWorldLinkFacts &nw = view.runtime->novaworld_link();
		out.net_quality.novaworld_icon = nw.novaworld && nw.nwu_in_use;
	} else if (view.host != nullptr) {
		// A Serve Only host's g_NetQuality is its context's, which its main frame steps
		// (D-NET-971); the N icon's gate the same two words off its context.
		out.net_quality.indicators = view.host->net_quality_indicators;
		out.net_quality.novaworld_icon =
				view.host->transport_mode == NetworkType::NovaWorld && view.host->nwu_in_use;
	}
	out.frame_rate.average_fps = view.frame_rate.average_fps;
	out.frame_rate.min_fps = view.frame_rate.min_fps;
	out.frame_rate.max_fps = view.frame_rate.max_fps;
	out.frame_rate.cpu_percent = view.frame_rate.cpu_percent;
	if (view.kernel == nullptr) return out;
	out.frame_rate.shown = view.kernel->world.rules.frame_rate_overlay;
	// [orig: HUD_DrawPingDisplay @0x59D9D0 - is_in_session @0x59D9F4, is_authority @0x59DA01,
	//  g_RoundOver @0x59DA0E, CNapiNetwork_GetSecondsSinceLastReceive called @0x59DA20, `cmp
	//  eax, 2; jle` @0x59DA25; HUD_DrawTimerOverlay @0x59DAF0 - g_ClientPreRoundTimer @0x59DB27]
	if (view.joiner && view.runtime != nullptr && view.kernel->world.rules.mp_session &&
			!view.runtime->state().spawn_success_gate) {
		out.connect_status.seconds = static_cast<int32_t>(view.runtime->seconds_since_last_receive());
		out.connect_status.shown = out.connect_status.seconds > 2;
	}
	out.start_timer_seconds = static_cast<int32_t>(view.joiner
					? view.kernel->world.preround_delay_seconds
					: view.kernel->world.cached.client_preround_timer);
	// The drawer's session gate, the same session bit as the MP lines below
	// [orig: `cmp is_in_session, 0` @0x4c3210].
	out.net_quality.in_session = view.kernel->world.rules.mp_session;
	// The weapon inset's friendly name: the HUD target cursor (the lock word,
	// whatever it holds, else, a zero word only, the head-look target) as the
	// scene frame found it, ahead of that frame's HUD pass: the last pass's
	// record (world::HudTargetCursor), stale through every frame whose pass
	// returned before its build. Its display name is the lookup's over the
	// session rules word, the HUD's team and the role's 0x4C slot table. A lock
	// on a round (a flare the heat lock took) is the cursor too: pool 0 holds no
	// round, so the inset's pool test draws neither brackets nor a name for it. On a joiner the
	// cursor may name a decoded remote person, the pool-0 entity retail's client built for its
	// row: the lookup reads that row (row_display_name).
	// [orig: Render_WeaponInsetScene @0x5C9740 -- g_HUDTargetCursorEntity @0x5CA0A4, the pool
	//  test @0x5C9F78, the call @0x5CA0AB; Render_ProcessMainSceneFrame @0x5CA0F0 -- the inset's
	//  call @0x5CA949 ahead of the HUD pass's @0x5CACE3 / @0x5CAD04; Entity_GetDisplayName
	//  @0x59BF70 (its pool test @0x59BF87..0x59BF94, the slot walk @0x59C07A..0x59C0BB)]
	{
		const world::World &tw = view.kernel->world;
		const world::Entity *local = tw.registry.get(tw.cached.local_player);
		const world::HudTargetCursor &at_scene = view.kernel->local.hud_target_cursor_at_scene();
		const world::Entity *cursor = world::hud_target_cursor_entity(tw, at_scene);
		const replication::ClientState *cs =
				view.runtime != nullptr ? &view.runtime->state() : nullptr;
		const replication::ClientEntityState *row =
				cursor == nullptr && at_scene.wire_person != 0xFFFF && cs != nullptr
				? cs->find(at_scene.wire_person)
				: nullptr;
		if (local != nullptr && cursor != nullptr)
			(void)entity_display_name(*cursor, local->team, session_rules_word(view), cs,
					out.inset_target_name);
		else if (local != nullptr && row != nullptr)
			(void)row_display_name(*row, local->team, session_rules_word(view), cs,
					out.inset_target_name);
	}
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
		s.permanent_death = cs.permanent_death;              // g_SessionPermanentDeath
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
	// The authority reads its own g_KothLimitMinutes [orig: @0x59CCDB..0x59CCE3].
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
	//  `g_DeathScreenTarget && g_DeathScreenSubMode` @0x5a7bdb; HUD_BuildEntityInfo @0x5a7bf3
	//  (team @0x4b8464, ratio @0x4b87a2..0x4b87d3); HUD_DrawTeamIdLine's name
	//  @0x59ac3d]
	if (view.runtime != nullptr) {
		const replication::ClientState &cs = view.runtime->state();
		// The friendly tag's spectated legs read the tracked target and the
		// sub-mode as they stand [orig: HUD_DrawEntityLabel @0x5a3a05..0x5a3a2e,
		// @0x5a3b0e, @0x5a3b62].
		s.spectate_target = cs.spectate_target;
		s.spectate_submode = cs.death_screen_submode;
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
					max_hp = std::max<int32_t>(1, w.tables.player.item_hp.value_or(0));
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
	s.attack_defend = view.kernel->local.attack_defend_role; // g_AttackDefendRole
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

std::vector<StatScreenRow> end_round_rows(const RoleView &view) {
	// The PLAYER SLOT table retail walks is the roster every role's view folds
	// from 0x46 (name / clan / team). Retail joins the board by the row's
	// stored slot id; the local row is the fill's, by the local player entity's
	// slot id (end_round_session_state's local_slot; fill_stat_results has the
	// witness).
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
	// Every row: the tab filter hides filled rows (stat_screen_filter_rows, D-MNU-53).
	out = stat_screen_rows(cs.end_round.board, players, false);
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
	// The session's rules word (session_rules_word): the World word, a joiner's
	// copy of the S2C 0x64 fixed block (+44). Bit 0x400 = the host option
	// FriendlyTag 0 [orig: `test g_RulesFlags, 400h` @0x5A4480].
	ctx.rules_no_friendly_tags =
			(session_rules_word(view) & GameConfig::kMpAttribNoFriendlyTag) != 0;
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
		const int32_t player_hp = w.tables.player.item_hp.value_or(0);
		replication::collect_roster_tags(view.runtime->state(),
				view.runtime->has_self_handle() ? view.runtime->self_handle() : 0xFFFFu,
				player->team, ctx.death_screen, ctx.game_type, out,
				[player_hp](uint16_t) { return player_hp; }, &w);
	}
	// The speaking entity's tag, by the voice channel's speaker (a joiner's decoded
	// speaker by its row's handle) [orig: HUD_DrawEntityLabel @0x5a3e89].
	const world::EntityHandle speaker = w.script.voice.speaker();
	for (world::FriendlyTagSource &tag : out) tag.speaking = speaker.valid() && tag.entity == speaker;
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
				// The zone entity's own Team byte as this client holds it: the
				// authority's entity on the host, the 0x50 / 0x51 store on a joiner
				// [orig: NapiNPClientMsg_TeamAssign @0x4319E1..0x4319EE].
				const auto team = zone_team_byte(view, *zone);
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
			text(hud::kGameTextOverlays, "STROVER_PENALTYTIMER", "Respawn penalty"),
			text(hud::kGameTextWPNames, zone_key, "Spawn Point"));
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
	v.loadout_shown =
			world::deploy_loadout_shown(buttons.team, buttons.permanent_death, buttons.dead);
	v.restart_level = world::deploy_restart_level_allowed(
			buttons.game_type, buttons.in_session, buttons.dead);
	v.shroud_main = view.title != nullptr && view.title->death_shroud_main;
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
	// The zone's first regular overlay slot's source byte (+4), whose 0xC0 bits
	// are the host's capture bits; a SpawnPoint row rides the persistent bank
	// (flags 0x10). No slot reads 0 [orig: HUD_DrawZoneStatusPanel @0x5A2517..0x5A2565
	//  / @0x5A269B..0x5A26E6, the inlined MapOverlay_FindRegularSlotByEntity @0x5BE870
	//  and `mov dl, [esi+4]` @0x5A2562].
	const world::LfpCaptureFlagsLookup capture_flags = [runtime](world::EntityHandle h) -> uint8_t {
		const replication::ClientMinimapOverlaySlot *slot =
				replication::find_regular_overlay_slot(runtime->state().minimap, h.packed);
		return slot != nullptr ? slot->source : 0;
	};
	// The zone's Team byte as this client holds it (a joiner's replica row once
	// a team store lands), sign-extended as the marker reads it [orig:
	// HUD_DrawZoneMarker `mov al, [ebp+162h]; movsx eax, al` @0x598734..0x59873a].
	const world::LfpZoneTeamLookup team_byte = [&view](const world::Entity &zone) {
		return static_cast<int>(static_cast<int8_t>(zone_team_byte(view, zone)));
	};
	world::build_lfp_zones(w, zones, *local, local_team, timer, capture_flags, team_byte, out);
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

// A spawn zone's Team byte (+0x162) as this client holds it, the byte the
// DEATH screen's list and map read: the authority's own entity; on a joiner
// the replica row once a 0x50 / 0x51 team store or a world-stream team byte
// has landed (retail's non-authority store; the joiner's world entity never
// takes the 0x50 retarget), else the placed team. A zone timer entry's mode
// is never the team: the 0x6F handler stores only +544 / +545 on the zone.
// [orig: NapiNPClientMsg_TeamAssign @0x4319EE; NapiNPClientMsg_HandlePlayerSpawn
//  @0x431C6D; NapiNPClientMsg_ZoneTimerValue @0x428D60]
uint8_t zone_team_byte(const RoleView &view, const world::Entity &zone) {
	if (view.joiner && view.runtime != nullptr) {
		const replication::ClientEntityState *row = view.runtime->state().find(zone.handle.packed);
		if (row != nullptr && row->team_known) return row->team;
	}
	return zone.team;
}

} // namespace

bool death_map_facts(const RoleView &view, const world::SpawnZoneRegistry &zones,
		hud::DeathMapFacts &out) {
	out = hud::DeathMapFacts{};
	if (view.kernel == nullptr) return false;
	const world::World &w = view.kernel->world;
	if (const world::Entity *player = w.registry.get(w.cached.local_player)) {
		out.player_present = true;
		out.player_x = player->position.x.q16;
		out.player_y = player->position.y.q16;
		out.player_z = player->position.z.q16;
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
		// The zone entity's own Team byte, never a timer entry's mode: the
		// letter colour, the own-team pulse and labels, and the blip's team
		// reads all take +0x162 [orig: MapOverlay_DrawView `v12[354]`
		//  @0x5A5AE9 and @0x5A5C73; Minimap_DrawEntityBlip `entity_ptr[354]` @0x598102].
		zone.team = zone_team_byte(view, *e);
		if (runtime != nullptr) {
			// Any timer entry gates the letter, a window-only one (zeros) as
			// ready [orig: CProximityList_FindEntryById @0x5A5B6E, `!EntryById ||
			//  EntryById[9] >= EntryById[10]` @0x5A5B77..0x5A5B7F].
			const auto live = runtime->zone_states().find(e->handle.packed);
			if (live != runtime->zone_states().end())
				zone.timer_ready = !(live->second.entry.value_target <
						live->second.entry.value_limit);
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
		// Minimap_DrawEntityBlip's entity reads (hud_map_view.h death_map_zone_blip).
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
		// to the bbox centre [orig: Entity_GetWorldBoundsCenter @0x59c311..0x59c327].
		int32_t euler[3];
		world::entity_live_euler_bam(*e, euler);
		const int32_t position[3] = {e->position.x.q16,
				e->position.y.q16,
				e->position.z.q16};
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
	// The command map's walks and sends read the local body's slot byte, 0xFF while the body is
	// destroyed in place [orig: CMap_PopulateTeamList_0 @0x548d80; CMap_BuildAndSendOrderCommand
	//  @0x5476cf; CMap_SendFireteamAssign @0x548d30; CMap_OnGoCodeButton
	//  @0x54829f; CMap_EntityWidgetHandler @0x548653].
	const int local_slot = runtime.local_body_slot();
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
	const int32_t reveal_ticks = view.title != nullptr ? view.title->death_shroud_reveal_ticks
			: Title().death_shroud_reveal_ticks;
	return world::death_shroud_revealed(deploy_active, since_death, reveal_ticks);
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
	// Held awaiting the post-auth pick, or with initial admission complete while
	// the host still holds the deploy-map overlay: a stock host's initial S2C
	// 0x5A grant opens the session and its C2S 0x0C uplinks while the DEATH
	// screen holds presentation until the first pick (net-re §5.61). Both take
	// the same C2S 0x0E pick. [orig: Input_HandleActionBinding @0x49AD40,
	// case 12 @0x49B0C5..0x49B17B]
	const bool held = runtime.awaiting_deploy_pick() ||
			(runtime.in_match() && runtime.initial_admission_complete() &&
			 runtime.state().deploy_overlay_active);
	return held && runtime.has_self_handle() &&
			runtime.last_error().empty() && !runtime.session_lost();
}

bool joiner_in_match_ready(const RoleView &view, bool auto_deploy) {
	if (!view.joiner || view.runtime == nullptr || view.kernel == nullptr) return false;
	const ClientRuntime &runtime = *view.runtime;
	return view.kernel->world.cached.local_player.valid() && runtime.in_match() &&
			(!auto_deploy || !runtime.deployment_pick_pending());
}

namespace {

// A 0x6E wave member's name as the death screens read it, the member entity's Name (+0xF4):
// the roster slot driving that pool-0 slot (the player entity's name IS the roster name), else
// the replica row's [orig: UI_UpdateDeathScreenContent @0x553d5a..0x553d7f; sub_551450
// @0x551be8 / @0x551bfd (dfx2.exe)].
std::string wave_member_name(const replication::ClientState &cs, uint16_t member) {
	const world::EntityHandle mh{ member };
	for (const replication::ClientRosterSlot &slot : cs.roster) {
		if (slot.bound && slot.entity_slot == mh.slot() && mh.pool() == 0) return slot.name;
	}
	if (const replication::ClientEntityState *row_state = cs.find(member))
		return row_state->display_name;
	return std::string();
}

// The self handle the death screens compare a member with (g_LocalPlayerEntity): the
// authority's own body, a joiner's bound self record [orig: `cmp eax, g_LocalPlayerEntity`
// @0x553d64; sub_551450 @0x551bd3 (dfx2.exe)].
uint16_t deploy_self_handle(const RoleView &view) {
	const ClientRuntime &runtime = *view.runtime;
	const world::EntityHandle local = view.kernel->world.cached.local_player;
	return view.joiner ? (runtime.has_self_handle() ? runtime.self_handle() : 0xFFFFu)
			: (local.valid() ? local.packed : 0xFFFFu);
}

// Whether the zone's shared timer-list entry lets it list: none, or its DWORD 9 (the value
// target) at its DWORD 10 (the limit) or past it, a signed compare [orig:
// CProximityList_FindEntryById, `EntryById[9] >= EntryById[10]` @0x553bd7..0x553bdd; sub_551450
// @0x5515e9..0x551608 (dfx2.exe)].
bool zone_timer_secured(const ClientRuntime &runtime, const world::Entity &zone) {
	const auto live = runtime.zone_states().find(zone.handle.packed);
	return live == runtime.zone_states().end() ||
			!(live->second.entry.value_target < live->second.entry.value_limit);
}

// HUD_GetEntityDisplayName over one handle: the player slot driving it, its name (256 bytes)
// and "<ch>" clan tag "<co>" for a non-empty tag; no slot, the entity's own name (+244). False
// when the handle names no entity with a definition this client holds: a world entity's own,
// else a joiner's decoded row.
// [orig: HUD_GetEntityDisplayName @0x58dd40 - PlayerSlot_FindByEntityPtr @0x58dd4d, the name
//  @0x58dd69, the tag @0x58dd78..0x58ddb0, the entity's name @0x58ddc0 (dfx2.exe); its twin
//  @0x58f8c0]
bool hud_entity_display_name(const RoleView &view, world::EntityHandle handle, std::string &out) {
	const world::World &w = view.kernel->world;
	const replication::ClientState &cs = view.runtime->state();
	const world::Entity *e = w.registry.get(handle);
	const replication::ClientEntityState *row = e == nullptr ? cs.find(handle.packed) : nullptr;
	if (e != nullptr ? !e->has_item_def : row == nullptr) return false;
	for (const replication::ClientRosterSlot &slot : cs.roster) {
		if (!slot.bound || handle.pool() != 0 || slot.entity_slot != handle.slot()) continue;
		out.clear();
		replication::string_append_n(out, slot.name, 256);
		if (!slot.registry_clan.empty()) {
			replication::string_append_n(out, "<ch>", 256);
			replication::string_append_n(out, slot.registry_clan, 256);
			replication::string_append_n(out, "<co>", 256);
		}
		return true;
	}
	out = e != nullptr ? e->display_name : row->display_name;
	return true;
}

// The seat whose fixed mountHandles slot is `retail_slot`, if the entity has one.
const world::Seat *seat_at_retail_slot(const world::Entity &e, int retail_slot) {
	for (const world::Seat &seat : e.seats)
		if (seat.type != world::SeatType::None && seat.retail_slot == retail_slot) return &seat;
	return nullptr;
}

} // namespace

uint8_t deploy_list_team(const RoleView &view) {
	if (view.joiner) return view.runtime != nullptr ? view.runtime->assigned_team() : 0;
	const world::Entity *player = view.kernel != nullptr ? view.kernel->local.player() : nullptr;
	return player != nullptr ? player->team : 0;
}

bool deploy_zone_rows(const RoleView &view, const world::SpawnZoneRegistry &zones,
		std::vector<world::DeployZoneRow> &out) {
	out.clear();
	if (view.kernel == nullptr || view.runtime == nullptr) return false;
	const ClientRuntime &runtime = *view.runtime;
	const world::World &w = view.kernel->world;
	const uint8_t team = deploy_list_team(view);
	const replication::ClientState &cs = runtime.state();
	// The self marker compares the member with the local player entity (deploy_self_handle).
	const uint16_t self_handle = deploy_self_handle(view);
	// The zone walk is the minimap banks' (banked_spawn_zones): a zone the
	// server never sent is not listed, the rows follow first arrival, and a
	// banked zone outside the SpawnZoneList lists as index -1.
	std::vector<world::EntityHandle> banked;
	banked_spawn_zones(view, banked);
	for (const world::EntityHandle handle : banked) {
		const world::Entity *e = w.registry.get(handle);
		if (e == nullptr) continue;
		const int index = world::spawn_zone_index_of(zones, handle);
		// Both loops compare the zone entity's own Team byte (zone_team_byte)
		// with the local entity's [orig: UI_UpdateDeathScreenContent `mov dl,
		//  [esi+162h]; cmp dl, [eax+162h]` @0x553BB5..0x553BC0 and the second
		//  loop's @0x553C5F..0x553DE3 team test].
		const uint8_t zone_team = zone_team_byte(view, *e);
		// The first-loop gate reads the zone's shared timer-list entry alone: no
		// entry lists the zone, and an entry lists it once its DWORD 9 (the value
		// target) reaches its DWORD 10 (the limit), a signed compare. Only the
		// client's 0x53 / 0x6F folds create entries, so a zone no capture event
		// ever touched (an unnumbered zone, whose entity control stays 0) lists;
		// the entity's own control word is never read here.
		// [orig: UI_UpdateDeathScreenContent -- CProximityList_FindEntryById
		//  @0x553bce, `test eax, eax; jz` @0x553bd3..0x553bd5, `cmp [eax+24h],
		//  [eax+28h]; jl` @0x553bd7..0x553bdd; the entry creators
		//  NapiNPClientMsg_ZoneTimerWindow @0x428bd4 and
		//  NapiNPClientMsg_ZoneTimerValue @0x428e6b]
		const bool secured = zone_timer_secured(runtime, *e);
		if (zone_team != team) continue;
		world::DeployZoneRow row;
		// No -1 guard in the first loop: an unregistered zone lists as '@'
		// with STRWPNAME000 and value 0 [orig: @0x553bd6..0x553c1f].
		row.index = index;
		row.letter = static_cast<char>('A' + index);
		char name_key[32];
		std::snprintf(name_key, sizeof(name_key), "STRWPNAME%03d", index + 1);
		row.name_key = name_key;
		// There is no zone-number term (an earlier port carried one): the walk
		// tests only the timer entry above and then the def's spawn-zone
		// attribute (engine record: deploy_screen_feed.h cites the list loop).
		row.secured = secured;
		// The 0x6E wave group on this zone: its countdown (entity+548) and the
		// queued members, named through the roster the way retail reads the
		// member entity's Name (the player entity's name IS the roster name)
		// [orig: dword_A85BC4[idx] / unk_A85CC4 @0x553ccd..0x553d8e, see world/deploy_screen_feed.h].
		if (cs.spawn_waves.known) {
			for (const SpawnWaveGroup &g : cs.spawn_waves.value.groups) {
				if (g.zone_handle != e->handle.packed) continue;
				row.wave_countdown = static_cast<uint16_t>(g.wave_countdown);
				for (uint16_t member : g.members) {
					world::DeployOccupant o;
					o.handle = member;
					o.name = wave_member_name(cs, member);
					o.self = member == self_handle;
					row.occupants.push_back(o);
				}
			}
		}
		out.push_back(row);
	}
	return true;
}

bool deploy_team_list_players(const RoleView &view, std::vector<world::DeployTeamListPlayer> &out) {
	out.clear();
	if (view.title == nullptr || !view.title->death_screen_team_lists) return false;
	if (view.kernel == nullptr || view.runtime == nullptr) return false;
	const ClientRuntime &runtime = *view.runtime;
	const world::World &w = view.kernel->world;
	const replication::ClientState &cs = runtime.state();
	const bool in_session = view.joiner ? runtime.in_session() : w.rules.mp_session;
	const uint16_t self_handle = deploy_self_handle(view);
	// The entity the walk holds: never reset between rows [orig: @0x5503cb, @0x550449].
	world::EntityHandle entity{};
	for (const replication::ClientScoreboardRow &row : cs.scoreboard.rows) {
		if (in_session && row.slot_id < cs.roster.size()) {
			const replication::ClientRosterSlot &slot = cs.roster[row.slot_id];
			if (slot.bound && slot.entity_slot >= 0)
				entity = world::EntityHandle::make(0, static_cast<uint16_t>(slot.entity_slot));
		}
		if (!entity.valid()) entity = w.cached.local_player;
		if (!entity.valid()) continue;
		world::DeployTeamListPlayer player;
		if (const world::Entity *e = w.registry.get(entity)) {
			player.team = e->team;
		} else if (const replication::ClientEntityState *state = cs.find(entity.packed)) {
			player.team = state->team_known ? state->team : uint8_t{0};
		}
		player.local = entity.packed == self_handle;
		hud_entity_display_name(view, entity, player.name);
		out.push_back(std::move(player));
	}
	return true;
}

bool deploy_table_zones(const RoleView &view, const world::SpawnZoneRegistry &zones,
		uint32_t stance_middle, uint32_t stance_bad, const hud::GameTextLookup &gametext,
		std::vector<world::DeployTableZone> &out) {
	out.clear();
	if (view.kernel == nullptr || view.runtime == nullptr) return false;
	const ClientRuntime &runtime = *view.runtime;
	const world::World &w = view.kernel->world;
	const replication::ClientState &cs = runtime.state();
	const uint8_t team = deploy_list_team(view);
	const uint16_t self_handle = deploy_self_handle(view);
	std::vector<world::EntityHandle> banked;
	banked_spawn_zones(view, banked);
	for (const world::EntityHandle handle : banked) {
		const world::Entity *e = w.registry.get(handle);
		if (e == nullptr) continue;
		// The team, the timer entry, the spawn-zone attribute (the bank walk's) and a
		// SpawnZoneList index [orig: @0x5515dd, @0x5515e9..0x551608, @0x551618..0x551624].
		if (zone_team_byte(view, *e) != team || !zone_timer_secured(runtime, *e)) continue;
		const int index = world::spawn_zone_index_of(zones, handle);
		if (index == -1) continue;
		world::DeployTableZone zone;
		zone.index = index;
		zone.vehicle_category = static_cast<uint8_t>(e->item_unit_type);
		zone.class_id = e->item_id;
		zone.health_color = world::deploy_table_health_color(e->health,
				world::max_health_with_difficulty(w, *e), stance_middle, stance_bad);
		// [orig: @0x551645..0x551673]
		zone.greyed = !e->primary_occupant.valid() && e->item_type == 1;
		std::string name;
		// The seats 8, 9, 0..7: a held one whose occupant has a definition
		// [orig: (i + 8) % 10 @0x5518c0, the handle @0x5518c6, its def @0x5518f6,
		//  HUD_GetEntityDisplayName @0x551901, the driver test @0x551928].
		for (int i = 0; i < 10; ++i) {
			const int slot = (i + 8) % 10;
			const world::Seat *seat = seat_at_retail_slot(*e, slot);
			if (seat == nullptr || !seat->occupant.valid()) continue;
			if (!hud_entity_display_name(view, seat->occupant, name)) continue;
			zone.lines.push_back(world::deploy_table_seat_line(name, slot,
					seat->occupant == e->primary_occupant));
		}
		// The mounted gunners: the pool-1 walk [orig: @0x5519db..0x551a2a - live (+28), Flags
		// bit 0 clear, a definition with attrib 0x20, groundEntity (+40) the zone; the driver
		// (+368) and the UseGun handle (+418) @0x551a35..0x551a4b].
		w.registry.for_each([&](const world::Entity &gun) {
			if (gun.handle.pool() != 1 || !gun.alive || (gun.flags & 1u) != 0) return;
			if (!gun.has_item_def || (gun.item_attrib & 0x20u) == 0) return;
			if (gun.ground_target != handle || !gun.primary_occupant.valid()) return;
			const world::Seat *use_gun = seat_at_retail_slot(gun, 9);
			if (use_gun == nullptr || !use_gun->occupant.valid()) return;
			// Entity_GetMountSlotBoneIndex: the vehicle zone's slot userpoint at the gun's slot
			// byte, 0 under a zone that is no vehicle [orig: @0x5438f0 - @0x54390f..0x54391f
			// (dfx2.exe)].
			uint8_t bone = 0;
			if ((e->item_attrib & 0x40u) != 0 && gun.emplacement_slot < e->def_slot_userpoints.size())
				bone = e->def_slot_userpoints[gun.emplacement_slot];
			// The slot weapon's attach text: the gun's own slot, or its groundEntity's under the
			// route bit, its weapon's attachtextid resolved through Overlays; none without one
			// [orig: Entity_GetWeaponSlots @0x543350 - @0x543364..0x543383; +32, +0x3A0
			// @0x551aaa..0x551ac5 (dfx2.exe)].
			const world::Entity *slot_owner = &gun;
			if ((gun.item_attrib & 0x40u) == 0 && gun.primary_weapon_slot.redirect_to_parent_slot)
				slot_owner = e;
			const world::WeaponTableEntry *weapon =
					w.tables.weapons.by_index(slot_owner->primary_weapon_slot_adm);
			std::string attach_text;
			const bool has_attach_text = weapon != nullptr && !weapon->attach_text_id.empty();
			if (has_attach_text)
				attach_text = hud::game_text(gametext, hud::kGameTextOverlays,
						weapon->attach_text_id.c_str(), "");
			// One line per gun slot of the zone's definition whose userpoint is the bone
			// [orig: the k walk @0x551a4d..0x551b7f, the compare @0x551a62, the occupant's def
			// @0x551a89, HUD_GetEntityDisplayName @0x551a94].
			for (const uint8_t userpoint : e->def_slot_userpoints) {
				if (userpoint != bone) continue;
				if (!hud_entity_display_name(view, use_gun->occupant, name)) continue;
				zone.lines.push_back(world::deploy_table_gunner_line(
						name, has_attach_text ? &attach_text : nullptr));
			}
		});
		// The zone's 0x6E wave members [orig: dword_A7F304[index], unk_A7F404 + 16 * index
		// @0x551b96..0x551c86 (dfx2.exe)].
		if (cs.spawn_waves.known) {
			for (const SpawnWaveGroup &g : cs.spawn_waves.value.groups) {
				if (g.zone_handle != e->handle.packed) continue;
				for (const uint16_t member : g.members)
					zone.lines.push_back(world::deploy_table_waiting_line(
							wave_member_name(cs, member), member == self_handle));
			}
		}
		out.push_back(std::move(zone));
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
	// L=0 can coexist); a streamed twin is only the replica rule's row of the
	// row's type below. Host/SP rows use their authoritative exact-handle entity.
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

			// The twin is the decoded row's own native row by the replica rule,
			// never a later same-type occupant of the handle (D-NET-676)
			// [orig: NapiNPClientMsg_EntityDeath @0x42EBC6, the slot address].
			const world::EntityHandle h{ handle };
			const world::Entity *joiner_twin = h.pool() != 0
					? replication::replica_row_entity(view.materializer, w, h, es.type_id)
					: nullptr;
			// The wire walk defers authored rows to their placed node (or static
			// batch). Do not repeat the same native ray query and cache an update
			// for a wire node that deliberately does not exist. The self row went
			// above by the self handle; H is never compared with L (D-NET-698):
			// retail lights every drawn entity it built from the wire
			// [orig: Terrain_SetupEffectForEntity @0x5C74A0 per drawn entity].
			if (joiner_twin != nullptr && joiner_twin->spawn_origin != world::kSpawnOriginNone) {
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
	SessionVars vars = host_session_vars(view.host->config, view.host->mission_text_title,
			view.host->session_title);
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
				session_uptime_ms(*view.host), view.host->is_in_session != 0, &w);
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
	// A joiner's clock runs from its 0x58's GetTickCount receipt (ClientRuntime::session_elapsed_ms).
	sv.elapsed_ms = view.joiner ? view.runtime->session_elapsed_ms()
	                            : replication::session_status_elapsed_ms(status, state.local_clock_ms);
	sv.stats = status.stats;
	for (const replication::ClientSessionStatus::Option &o : status.options)
		sv.options.emplace_back(o.key, o.value);
	inputs.game_type = view.runtime->game_type();
	// [orig: `g_LocalPlayerEntity ? ->Team (+0x162) : 0` @0x5b92de..0x5b92f1]
	if (const world::Entity *local = w.registry.get(w.cached.local_player))
		inputs.local_team = static_cast<int8_t>(local->team);
	inputs.spawn_zones = w.zones.has_spawn_zone(); // SpawnZoneList_HasAny
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
	// [orig: `is_authority ? g_KothLimitMinutes : dword_A821C0` @0x423a4b]
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
		f.round_over = state.spawn_success_gate;  // [orig: g_RoundOver]
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

void build_hud_entity_info(mission::MissionKernel &kernel, bool round_over) {
	if (round_over) return; // [orig: `cmp g_RoundOver, 0` @0x5A8084]
	world::LocalPlayer &local = kernel.local;
	// [orig: `cmp g_ScreenFlashWhiteHit, 0` @0x5A8098]
	if (world::screen_flash_hud_overlays_suppressed(local.view.flash)) return;
	// [orig: `test ebp, ebp; jz` @0x5A80A1..0x5A80A3]
	if (local.player() == nullptr) return;
	// [orig: memset(g_HUDEntityInfo, 0, 0x240) @0x5A80B1, the HUD_BuildEntityInfo call @0x5A80BC]
	local.build_hud_target_cursor();
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
