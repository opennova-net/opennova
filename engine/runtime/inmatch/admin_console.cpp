// The remote-admin console's verbs (admin_console.h).
#include <runtime/inmatch/admin_console.h>

#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_ban_lists.h>
#include <runtime/inmatch/server_console.h> // Server_SendConsoleChat, the CHAT ring
#include <runtime/inmatch/server_initial_state.h> // build_server_config_flags
#include <runtime/world/world.h>

#include <base/gameprofile/game_type.h>
#include <base/io/crt_ftol.h>
#include <base/io/strutil.h>
#include <formats/gamecfg/game_cfg.h>
#include <net/npwire/ingame_message_id.h>
#include <net/napi/session.h> // tokenize_quoted (String_TokenizeQuotedToArray)
#include <runtime/hud/hud_chat_entry.h> // kChatDispatchGlobal, chat_dispatch_flood_color

#include <cstdio>
#include <string>

namespace opennova::inmatch {

namespace {

constexpr const char *kGetUsage = "USAGE -  GET [GAMESTATE] [GAMESETTINGS]";
constexpr const char *kSetUsage =
		"USAGE -  SET [AutoBalanceOnRecycle | LevelRestrict | PuntVote | VotePercent | VoteNumPlayersReq | "
		"ChangeTeam | ChangeTeamInterval | ChangeTeamPenalty | ChangeTeamDelay | StartDelay | DoMinPingCheck | "
		"MinPing | DoMaxPingCheck | MaxPing | MaxFriendlyKills | GameTime | FriendlyFire | FriendlyTags | "
		"TeamTriggerClaymore | Tracers | KOTHLimit | KillLimit | FatBullets | OneShotKill] #";
constexpr const char *kMissionUsage =
		"USAGE -  MISSION [LIST | AVAILABLE | ADD | REMOVE | CLEAR | CYCLE | SETNEXT ] [#]";
constexpr const char *kMissionAddUsage =
		"USAGE -  MISSION ADD 'FILENAME' [AUTO_SWITCH_SIDES (1|0)] [INSERT_AT] [ONESHOT (1|0)]";
constexpr const char *kWeaponUsage = "USAGE -  WEAPON [LIST | SET] [# | ALL] [ALWAYS | NEVER | ARMORY]";
constexpr const char *kGotoUsage = "USAGE -  GOTO [GAMESTATE | MENUSTATE]";
constexpr const char *kChatUsage = "USAGE -  CHAT [GET | SEND]";
constexpr const char *kNoMissions = "No missions in queue.";
constexpr const char *kNotImplemented = "ERROR - This feature not yet implemented.";
constexpr const char *kCycling = "OK - Server is cycling...";
constexpr const char *kMustBeInGame = "ERROR - Must be in Game State to cycle server.";

// The admin CYCLE / GOTO linger in place of the 2790 the round end stored.
// [orig: g_EndRoundLingerTimer = 620 @0x40661F, @0x404AA5]
constexpr uint32_t kCycleLingerTicks = 620;
// GET GAMESETTINGS formats each row in a 52-byte stack buffer: a value past 27 characters
// overruns into the frame's security cookie and ends the process (D-NET-359).
// [orig: CAdminServer_HandleGet — the buffer at frame +0x18, the cookie at +0x4C]
constexpr size_t kGameSettingsValueMaxChars = 52 - 21 - 2 - 1 - 1;

bool is_space(char c) {
	return c == ' ' || (c >= '\t' && c <= '\r');
}

bool ieq(std::string_view a, std::string_view b) {
	return strutil::iequals(a, b);
}

int32_t atol_of(const std::string &s) {
	return io::retail_atol(s.c_str());
}

// "%-21s= %s\n" with the row's key pre-padded in retail's literal.
std::string settings_row(const char *key, std::string value) {
	if (value.size() > kGameSettingsValueMaxChars) value.resize(kGameSettingsValueMaxChars);
	std::string row = key;
	row.resize(21, ' ');
	return row + "= " + value + "\n";
}

std::string itos(int64_t v) {
	return std::to_string(v);
}

} // namespace

std::vector<std::string> admin_split_command(std::string_view line) {
	std::vector<std::string> tokens;
	size_t start = std::string_view::npos;
	bool expect_new = true;
	size_t count = 0;
	for (size_t i = 0; i < line.size(); ++i) {
		if (count >= 25) break;
		if (is_space(line[i])) {
			if (start != std::string_view::npos) tokens.emplace_back(line.substr(start, i - start));
			start = std::string_view::npos;
			expect_new = true;
		} else if (expect_new) {
			expect_new = false;
			start = i;
			++count;
		}
	}
	// The last token runs to the line's end: the walk ended there, or the cap stopped it
	// writing terminators.
	if (start != std::string_view::npos) tokens.emplace_back(line.substr(start));
	return tokens;
}

std::string admin_usage_reply(uint32_t rights) {
	using namespace admincfg::right;
	std::string out = "USAGE - [QUIT";
	if (rights & kGet) out += " | GET";
	if (rights & kSet) out += " | SET";
	if (rights & kMission) out += " | MISSION";
	if (rights & kPlayer) out += " | PLAYER";
	if (rights & kWeapon) out += " | WEAPON";
	if (rights & kCmd) out += " | CMD";
	if (rights & kGoto) out += " | GOTO";
	if (rights & kChatListed) out += " | CHAT";
	if (rights & kAdminListed) out += " | ADMIN";
	if (rights & kBanListListed) out += " | BANLIST";
	if (rights & kPeterRabbit) out += " | PETERRABBIT";
	return out + "]";
}

AdminConsole::AdminConsole(NapiNPServerCtx &ctx, Seams seams) : ctx_(ctx), seams_(std::move(seams)) {}

// [orig: CAdminServer_DispatchCommand @0x406720 — the verbs compared case-insensitively in
//  this order, each gated on its rights bit (the low byte for every verb up to BANLIST
//  @0x406833..0x406A43, the dword's sign for PETERRABBIT @0x406A6D); a verb whose right is
//  missing falls through to the next compare and ends in the usage reply; QUIT returns 0]
bool AdminConsole::dispatch(const AdminSession &session, std::string_view line,
		std::vector<std::string> &replies) {
	using namespace admincfg::right;
	const std::vector<std::string> tokens = admin_split_command(line);
	const uint32_t rights = session.rights;
	if (!tokens.empty()) {
		const std::string &verb = tokens[0];
		const Args args(tokens.begin() + 1, tokens.end());
		const auto stub = [&](const char *usage) {
			const bool known = !args.empty() &&
					(ieq(args[0], "LIST") || ieq(args[0], "DELETE") || ieq(args[0], "CREATE"));
			replies.emplace_back(known ? kNotImplemented : usage);
		};
		if (ieq(verb, "QUIT")) return false;
		if (ieq(verb, "GET") && (rights & kGet)) {
			handle_get(args, replies);
		} else if (ieq(verb, "SET") && (rights & kSet)) {
			handle_set(args, replies);
		} else if (ieq(verb, "MISSION") && (rights & kMission)) {
			handle_mission(args, replies);
		} else if (ieq(verb, "PLAYER") && (rights & kPlayer)) {
			handle_player(args, replies);
		} else if (ieq(verb, "WEAPON") && (rights & kWeapon)) {
			handle_weapon(args, replies);
		} else if (ieq(verb, "CMD") && (rights & kCmd)) {
			handle_cmd(args, replies);
		} else if (ieq(verb, "GOTO") && (rights & kGoto)) {
			handle_goto(args, replies);
		} else if (ieq(verb, "CHAT") && (rights & kGoto)) {
			handle_chat(args, replies);
		} else if (ieq(verb, "ADMINUSER") && (rights & kGoto)) {
			// [orig: CAdminServer_HandleAdminUserCommand @0x404CB0, @0x404CB8..0x404D5C]
			stub("USAGE -  ADMINUSER [LIST | DELETE | CREATE]");
		} else if (ieq(verb, "BANLIST") && (rights & kGoto)) {
			// Its usage says BAN. [orig: CAdminServer_HandleBanCommand @0x404D70,
			//  @0x404D78..0x404E1C]
			stub("USAGE -  BAN [LIST | DELETE | CREATE]");
		} else if (ieq(verb, "PETERRABBIT") && (rights & kPeterRabbit)) {
			handle_fun(args, replies);
		} else {
			replies.push_back(admin_usage_reply(rights));
		}
		return true;
	}
	replies.push_back(admin_usage_reply(rights));
	return true;
}

// [orig: CAdminServer_HandleGet @0x403390 — GAMESTATE @0x4033C4..0x40340B]
void AdminConsole::handle_get(const Args &args, std::vector<std::string> &replies) {
	if (args.empty()) {
		replies.emplace_back(kGetUsage);
	} else if (ieq(args[0], "GAMESTATE")) {
		replies.emplace_back(scene_ == AdminScene::MainMenu  ? "OK - Current State = Menus"
		                     : scene_ == AdminScene::GameLoop ? "OK - Current State = Game"
		                                                      : "OK - Current State = Unknown");
	} else if (ieq(args[0], "GAMESETTINGS")) {
		replies.push_back(game_settings_reply());
	} else {
		replies.emplace_back(kGetUsage);
	}
}

// The 29 rows, the live rule globals and the cfg block's strings. The rules word is the
// world's live g_RulesFlags, the block's copy outside a mission.
// [orig: CAdminServer_HandleGet @0x403434..0x403D42]
std::string AdminConsole::game_settings_reply() const {
	const GameConfig &c = ctx_.config;
	const world::World *world = ctx_.world;
	const gamecfg::GameCfg *block = seams_.config_block;
	const uint32_t rules = world != nullptr ? world->rules.mpattrib : c.mp_attributes;
	// GameTime's remaining whole minutes: g_RoundTimeRemaining / 60 / 62, the C division
	// truncating toward zero, so an untimed round (-1) reads 0.
	const int32_t remaining = world != nullptr ? world->match.remaining_ticks() : 0;
	std::string out;
	out += settings_row("AutoBalanceOnRecycle", itos(c.auto_balance_enabled));
	out += settings_row("PuntVote", itos(c.voting_enabled));
	// %f: six decimals through the game's CRT. [orig: "VotePercent          = %f\n" @0x7C0CCC]
	out += settings_row("VotePercent", gamecfg::format_fixed(static_cast<double>(c.voting_percent), 6));
	out += settings_row("VoteNumPlayersReq", itos(c.voting_min_players));
	out += settings_row("ChangeTeam", itos((rules >> 2) & 1u));
	out += settings_row("ChangeTeamInterval", itos(c.change_team_interval_seconds));
	out += settings_row("ChangeTeamPenalty", itos(c.change_team_penalty_seconds));
	out += settings_row("ChangeTeamDelay", itos(c.capture_duration_seconds));
	out += settings_row("StartDelay", itos(static_cast<int32_t>(c.start_delay)));
	out += settings_row("DoMinPingCheck", itos(c.do_min_ping_check));
	out += settings_row("MinPing", itos(static_cast<int32_t>(c.min_ping)));
	out += settings_row("DoMaxPingCheck", itos(c.do_max_ping_check));
	out += settings_row("MaxPing", itos(static_cast<int32_t>(c.max_ping)));
	out += settings_row("MaxFriendlyKills", itos(c.max_friendly_kills));
	out += settings_row("GameTime", itos(remaining / 60 / 62) + "/" + itos(static_cast<int32_t>(c.respawn_time)));
	out += settings_row("FriendlyFire", itos((rules & GameConfig::kMpAttribNoFriendlyFire) == 0));
	out += settings_row("FriendlyTags", itos((rules & GameConfig::kMpAttribNoFriendlyTag) == 0));
	out += settings_row("TeamTriggerClaymore", itos((rules >> 15) & 1u));
	out += settings_row("Tracers", itos((rules & GameConfig::kMpAttribNoTracers) == 0));
	out += settings_row("KOTHLimit", itos(static_cast<int32_t>(c.time_limit_minutes)));
	out += settings_row("KillLimit", itos(static_cast<int32_t>(c.score_limit)));
	out += settings_row("MaxScore", itos(static_cast<int32_t>(c.max_score)));
	out += settings_row("FatBullets", itos(c.fat_bullets));
	out += settings_row("OneShotKill", itos(c.one_shot_kill));
	out += settings_row("ArmoryTimer", itos(block != nullptr ? block->armory_reuse_time : 0));
	out += settings_row("ServerName", block != nullptr ? block->game_name : std::string());
	out += settings_row("ServerPassword", block != nullptr ? block->mp_host_game_password : std::string());
	out += settings_row("SideAPassword", block != nullptr ? block->mp_host_side_password_a : std::string());
	out += settings_row("SideBPassword", block != nullptr ? block->mp_host_side_password_b : std::string());
	return out;
}

namespace {

// The rules word onto the world the round reads it from: the word itself and the per-mission
// facts the boot derives from it (host_role.cpp, host_session.cpp), so a SET of a rule bit is
// live at once, as retail's single g_RulesFlags is.
// [orig: g_RulesFlags @0x24D1E34 — the FriendlyFire / FriendlyTags / TeamTriggerClaymore /
//  Tracers sets @0x405DE1..0x405F48]
void set_live_rules_word(NapiNPServerCtx &ctx, uint32_t word) {
	world::World *world = ctx.world;
	if (world == nullptr) {
		ctx.config.mp_attributes = word;
		return;
	}
	world->rules.mpattrib = word;
	world->rules.no_friendly_fire = (word & GameConfig::kMpAttribNoFriendlyFire) != 0;
	world->rules.auto_scope_zero = (word & GameConfig::kMpAttribAutoScopeZero) != 0;
	world->throwables.team_trigger_claymore = (word & GameConfig::kMpAttribClaymorePref) != 0;
	world->round_sim.no_tracers_rule = (word & GameConfig::kMpAttribNoTracers) != 0;
}

uint32_t live_rules_word(const NapiNPServerCtx &ctx) {
	return ctx.world != nullptr ? ctx.world->rules.mpattrib : ctx.config.mp_attributes;
}

// strncpy(field, value, 0x11): a 17-character value keeps no terminator (D-NET-359).
std::string password_field(const std::string &value) {
	return value.substr(0, 17);
}

// The protocol's copies after a password change: the join PW and game_settings (Napi_CopyString
// 32 of the 17-byte field), then the advertised flag words recomputed.
// [orig: CAdminServer_HandleSetCommand — Napi_CopyString(np_protocol->log_buffer, ...) and
//  CAITask_SetName @0x402BD0 / CAdminServer_SetSidePassword @0x402BF0, then
//  server_flags = game_settings.game_type and build_flags = CNapiServerConfig_BuildFlags
//  @0x4C4DC0]
void republish_flags(NapiNPServerCtx &ctx) {
	ctx.np_protocol.server_flags = ctx.config.game_type;
	ctx.np_protocol.build_flags = build_server_config_flags(ctx);
}

} // namespace

// [orig: CAdminServer_HandleSetCommand @0x405A60 — the long usage @0x405A9A..0x405B93; a lone
//  ServerPassword / SideAPassword / SideBPassword clears it (and, with the protocol up, its
//  copies and the flag words) and answers like a set; the keys compared case-insensitively in
//  order, values by atol (VotePercent atof); then the rules word copied into its shadow
//  `multiplayerAttributeFlags_34C` @0x406185, `OK - Setting Changed.` and Game_SaveConfig
//  @0x40618D..0x4061AE; an unknown key the usage, then `ERROR - Setting Not Found.`, unsaved]
void AdminConsole::handle_set(const Args &args, std::vector<std::string> &replies) {
	GameConfig &c = ctx_.config;
	gamecfg::GameCfg *block = seams_.config_block;
	// The protocol block exists only with a session (np_protocol is built at its creation).
	const bool protocol_up = ctx_.is_in_session != 0;
	world::World *world = ctx_.world;
	if (args.empty()) {
		replies.emplace_back(kSetUsage);
		return;
	}
	const std::string &key = args[0];
	if (args.size() < 2) {
		if (ieq(key, "ServerPassword")) {
			if (block != nullptr) block->mp_host_game_password.clear();
			if (protocol_up) {
				c.server_password.clear();
				republish_flags(ctx_);
			}
		} else if (ieq(key, "SideAPassword")) {
			if (block != nullptr) block->mp_host_side_password_a.clear();
			if (protocol_up) {
				c.side_a_password.clear();
				republish_flags(ctx_);
			}
		} else if (ieq(key, "SideBPassword")) {
			if (block != nullptr) block->mp_host_side_password_b.clear();
			if (protocol_up) {
				c.side_b_password.clear();
				republish_flags(ctx_);
			}
		} else {
			replies.emplace_back(kSetUsage);
			return;
		}
		replies.emplace_back("OK - Setting Changed.");
		if (seams_.save_config) seams_.save_config();
		return;
	}
	const std::string &value = args[1];
	const int32_t v = atol_of(value);
	bool not_found = false;
	uint32_t rules = live_rules_word(ctx_);
	const auto set_rule_bit = [&](uint32_t bit, bool on) {
		rules = on ? (rules | bit) : (rules & ~bit);
		set_live_rules_word(ctx_, rules);
	};
	if (ieq(key, "AutoBalanceOnRecycle")) {
		// No shadow write: the save keeps the old autoBalanceOnRecycle_528. [orig: @0x405BC3]
		c.auto_balance_enabled = v;
	} else if (ieq(key, "PuntVote")) {
		c.voting_enabled = v;
		if (block != nullptr) block->mpvoting = v;
	} else if (ieq(key, "VotePercent")) {
		c.voting_percent = static_cast<float>(io::retail_atof(value.c_str()));
		if (block != nullptr) block->mpvoting_percent = c.voting_percent;
	} else if (ieq(key, "VoteNumPlayersReq")) {
		c.voting_min_players = v;
		if (block != nullptr) block->mpvoting_min_players = v;
	} else if (ieq(key, "ChangeTeam")) {
		set_rule_bit(GameConfig::kMpAttribTeamChoose, v != 0);
		if (block != nullptr) block->mpchangeteam = v;
	} else if (ieq(key, "ChangeTeamInterval")) {
		c.change_team_interval_seconds = v;
		if (block != nullptr) block->mpchangeteam_interval = v;
	} else if (ieq(key, "ChangeTeamPenalty")) {
		c.change_team_penalty_seconds = v;
		if (block != nullptr) block->mpchangeteam_penalty = v;
	} else if (ieq(key, "ChangeTeamDelay")) {
		c.capture_duration_seconds = v;
		if (world != nullptr) world->match.live_rules().capture_duration_seconds = v;
		if (block != nullptr) block->teamchange_time = v;
	} else if (ieq(key, "StartDelay")) {
		c.start_delay = static_cast<uint32_t>(v);
		if (block != nullptr) block->startdelay = v;
	} else if (ieq(key, "DoMinPingCheck")) {
		c.do_min_ping_check = v;
		if (block != nullptr) block->dominpingcheck = v;
	} else if (ieq(key, "MinPing")) {
		c.min_ping = static_cast<uint32_t>(v);
		if (block != nullptr) block->minping = v;
	} else if (ieq(key, "DoMaxPingCheck")) {
		c.do_max_ping_check = v;
		if (block != nullptr) block->domaxpingcheck = v;
	} else if (ieq(key, "MaxPing")) {
		c.max_ping = static_cast<uint32_t>(v);
		if (block != nullptr) block->maxping = v;
	} else if (ieq(key, "MaxFriendlyKills")) {
		c.max_friendly_kills = v;
		if (block != nullptr) block->numallowablefriendlykills = v;
	} else if (ieq(key, "GameTime")) {
		// The round clock restarts at the full new limit: g_RoundTimeRemaining = 3720 * value.
		c.respawn_time = static_cast<uint32_t>(v);
		if (block != nullptr) block->time_limit = v;
		if (world != nullptr) {
			world->match.live_rules().game_time_minutes = static_cast<uint32_t>(v);
			world->match.set_remaining_ticks(static_cast<int32_t>(3720u * static_cast<uint32_t>(v)));
		}
	} else if (ieq(key, "FriendlyFire")) {
		set_rule_bit(GameConfig::kMpAttribNoFriendlyFire, v == 0); // inverted
	} else if (ieq(key, "FriendlyTags")) {
		set_rule_bit(GameConfig::kMpAttribNoFriendlyTag, v == 0); // inverted
	} else if (ieq(key, "TeamTriggerClaymore")) {
		set_rule_bit(GameConfig::kMpAttribClaymorePref, v != 0);
	} else if (ieq(key, "Tracers")) {
		set_rule_bit(GameConfig::kMpAttribNoTracers, v == 0); // inverted
	} else if (ieq(key, "KOTHLimit")) {
		c.time_limit_minutes = static_cast<uint32_t>(v);
		if (world != nullptr) world->match.live_rules().hill_limit_minutes = static_cast<uint32_t>(v);
		if (block != nullptr) block->koth_limit = v;
	} else if (ieq(key, "KillLimit")) {
		// No 500 fold here: a live 500 until the next session apply reads it as unlimited.
		c.score_limit = static_cast<uint32_t>(v);
		if (world != nullptr) world->match.live_rules().score_limit = static_cast<uint32_t>(v);
		if (block != nullptr) block->max_kills = v;
	} else if (ieq(key, "MaxScore")) {
		c.max_score = static_cast<uint32_t>(v);
		if (world != nullptr) world->match.live_rules().max_score = static_cast<uint32_t>(v);
		if (block != nullptr) block->max_score = v;
	} else if (ieq(key, "FatBullets")) {
		c.fat_bullets = v;
		if (world != nullptr) world->rules.fat_bullets = v != 0;
		if (block != nullptr) block->fatbullets = v;
	} else if (ieq(key, "OneShotKill")) {
		c.one_shot_kill = v;
		if (world != nullptr) world->rules.one_shot_kill = v != 0;
		if (block != nullptr) block->oneshotonekill = v;
	} else if (ieq(key, "ArmoryTimer")) {
		// The cfg field is the live value. [orig: armoryReuseTime_A38]
		c.armory_reuse_time = static_cast<uint32_t>(v);
		if (block != nullptr) block->armory_reuse_time = v;
	} else if (ieq(key, "ServerName")) {
		// Rebuilt from every value token, each followed by one space, a token skipped when it
		// would take the name past 30 characters; then the protocol's session name.
		// [orig: @0x4060BF..0x40615A]
		std::string name;
		for (size_t i = 1; i < args.size(); ++i)
			if (args[i].size() + name.size() + 2 < 0x20) name += args[i] + " ";
		if (block != nullptr) block->game_name = name;
		if (protocol_up) ctx_.np_protocol.session_name = name;
	} else if (ieq(key, "ServerPassword")) {
		const std::string pw = password_field(value);
		if (block != nullptr) block->mp_host_game_password = pw;
		if (protocol_up) {
			c.server_password = pw;
			republish_flags(ctx_);
		}
	} else if (ieq(key, "SideAPassword")) {
		const std::string pw = password_field(value);
		if (block != nullptr) block->mp_host_side_password_a = pw;
		if (protocol_up) {
			c.side_a_password = pw;
			republish_flags(ctx_);
		}
	} else if (ieq(key, "SideBPassword")) {
		const std::string pw = password_field(value);
		if (block != nullptr) block->mp_host_side_password_b = pw;
		if (protocol_up) {
			c.side_b_password = pw;
			republish_flags(ctx_);
		}
	} else {
		replies.emplace_back(kSetUsage);
		not_found = true;
	}
	// The rules word into its shadows: the cfg block's, and the port's one dword_2550A04
	// store (GameConfig::mp_attributes). [orig: @0x406185]
	rules = live_rules_word(ctx_);
	c.mp_attributes = rules;
	if (block != nullptr) block->mpattrib = static_cast<int32_t>(rules);
	if (not_found) {
		replies.emplace_back("ERROR - Setting Not Found.");
		return;
	}
	replies.emplace_back("OK - Setting Changed.");
	if (seams_.save_config) seams_.save_config();
}

// The rotation's lines: `%d: %s - %s %s %s %s %s\n` per entry, the marks `(2x)` for the row's
// launch option, `(IS FLIPPED)` while the half toggle is set on the cursor entry's row,
// `(ONE_SHOT)`, `<CURRENT MISSION>` on every entry of the cursor entry's row, `<NEXT
// MISSION>` on the alt cursor's; an absent mark prints `()` / `<>` in MISSION LIST and an
// empty string in the QUERY report. The cursor entry is read without a range check in retail;
// a cursor out of range marks no entry here (D-NET-367).
// [orig: CAdminServer_HandleMissionCommand @0x406338..0x406483 (the format @0x7C0874);
//  CAdminServer_HandleStatus @0x40327E, @0x403296, @0x4032A9, @0x4032C0, @0x4032E1]
std::string AdminConsole::rotation_lines(bool query_fillers) const {
	const RotationAdmin::List list = seams_.rotation->list();
	const std::vector<RotationAdmin::CatalogRow> catalog = seams_.rotation->catalog();
	const char *paren = query_fillers ? "" : "()";
	const char *angle = query_fillers ? "" : "<>";
	const bool cursor_valid = list.cursor >= 0 && static_cast<size_t>(list.cursor) < list.entries.size();
	const size_t current_row = cursor_valid ? list.entries[static_cast<size_t>(list.cursor)].catalog_index : 0;
	std::string out;
	for (size_t i = 0; i < list.entries.size(); ++i) {
		const RotationAdmin::Entry &entry = list.entries[i];
		const bool current = cursor_valid && entry.catalog_index == current_row;
		const RotationAdmin::CatalogRow *row =
				entry.catalog_index < catalog.size() ? &catalog[entry.catalog_index] : nullptr;
		out += itos(static_cast<int64_t>(i)) + ": " + (row != nullptr ? row->file : std::string()) + " - ";
		out += std::string(row != nullptr && row->launch_option != 0 ? "(2x)" : paren) + " ";
		out += std::string(list.half_flipped && current ? "(IS FLIPPED)" : paren) + " ";
		out += std::string(entry.one_shot ? "(ONE_SHOT)" : paren) + " ";
		out += std::string(current ? "<CURRENT MISSION>" : angle) + " ";
		out += std::string(list.alt_cursor == static_cast<int32_t>(i) ? "<NEXT MISSION>" : angle) + "\n";
	}
	return out;
}

// The round end with no winner and the 620-tick linger in the Game Loop, else the refusal.
// [orig: CAdminServer_HandleMissionCommand @0x4065E7..0x40662A; CAdminServer_HandleGotoCommand
//  @0x404A74..0x404AAF -> Server_ProcessRoundEnd(0) @0x5164F0]
void AdminConsole::cycle_tail(std::vector<std::string> &replies) {
	if (in_game() && ctx_.world != nullptr) {
		ctx_.world->process_round_end(0);
		ctx_.round_end_linger_override_ticks = kCycleLingerTicks;
		replies.emplace_back(kCycling);
	} else {
		replies.emplace_back(kMustBeInGame);
	}
}

// [orig: CAdminServer_HandleMissionCommand @0x4062E0 — LIST @0x406338..0x406483, AVAILABLE
//  @0x406490..0x40654C, CLEAR @0x406567..0x4065CD, CYCLE @0x4065E7..0x40662A, ADD
//  @0x406637..0x406655, REMOVE @0x406667..0x406691, SETNEXT @0x4066B0..0x4066EA]
void AdminConsole::handle_mission(const Args &args, std::vector<std::string> &replies) {
	if (args.empty()) {
		replies.emplace_back(kMissionUsage);
		return;
	}
	const std::string &sub = args[0];
	if (ieq(sub, "LIST")) {
		// An existing but empty rotation sends retail's unwritten 0-byte allocation; "" here
		// (D-NET-367).
		if (!in_game() || seams_.rotation == nullptr || !seams_.rotation->list().exists)
			replies.emplace_back(kNoMissions);
		else
			replies.push_back(rotation_lines(false));
		return;
	}
	if (ieq(sub, "AVAILABLE")) {
		std::string out;
		if (seams_.rotation != nullptr) {
			const std::vector<RotationAdmin::CatalogRow> catalog = seams_.rotation->catalog();
			for (size_t i = 0; i < catalog.size(); ++i)
				out += itos(static_cast<int64_t>(i)) + ". " + catalog[i].file + " (" + catalog[i].title + ")\n";
		}
		replies.push_back(out);
		return;
	}
	if (ieq(sub, "CLEAR")) {
		if (seams_.rotation != nullptr) seams_.rotation->clear(in_game());
		replies.emplace_back("OK - Mission list reset.");
		return;
	}
	if (ieq(sub, "CYCLE")) {
		cycle_tail(replies);
		return;
	}
	if (ieq(sub, "ADD")) {
		handle_mission_add(Args(args.begin() + 1, args.end()), replies);
		return;
	}
	if (ieq(sub, "REMOVE") && args.size() >= 2) {
		if (seams_.rotation != nullptr) seams_.rotation->remove(atol_of(args[1]));
		replies.emplace_back("OK - Mission Removed.");
		return;
	}
	if (ieq(sub, "SETNEXT") && args.size() >= 2) {
		if (seams_.rotation != nullptr) seams_.rotation->set_next(atol_of(args[1]));
		replies.emplace_back("OK - Next Mission Set.");
		return;
	}
	replies.emplace_back(kMissionUsage);
}

// [orig: CAdminServer_HandleMissionAdd @0x403D90 — ONESHOT the literal fourth argument
//  @0x403DAA..0x403DBF; the first catalog row whose file matches case-insensitively, else
//  `ERROR - File Not Found.`; its launch option atol(argument 2) or 1 @0x403E5E..0x403E91; in
//  the Game Loop the append or, with a third argument, the insert; `OK - Entry Added\n` in
//  every scene @0x403D9F..0x403EDD]
void AdminConsole::handle_mission_add(const Args &args, std::vector<std::string> &replies) {
	const bool one_shot = args.size() > 3 && ieq(args[3], "ONESHOT");
	if (args.empty()) {
		replies.emplace_back(kMissionAddUsage);
		return;
	}
	if (seams_.rotation == nullptr) {
		replies.emplace_back("ERROR - File Not Found.");
		return;
	}
	const std::vector<RotationAdmin::CatalogRow> catalog = seams_.rotation->catalog();
	size_t row = 0;
	while (row < catalog.size() && !ieq(catalog[row].file, args[0])) ++row;
	if (row >= catalog.size()) {
		replies.emplace_back("ERROR - File Not Found.");
		return;
	}
	seams_.rotation->set_launch_option(row, args.size() <= 1 ? 1 : atol_of(args[1]));
	if (in_game()) {
		std::optional<int32_t> at;
		if (args.size() > 2) at = atol_of(args[2]);
		seams_.rotation->add(row, at, one_shot);
	}
	replies.emplace_back("OK - Entry Added\n");
}

// [orig: CAdminServer_HandleWeaponCommand @0x4045A0 — LIST in any scene @0x404619..0x4047A4
//  (the `%3d.  ` index, `NEVER `, `ARMORY` or `ALWAYS` from g_ArmoryWeaponAvailability
//  @0x24D5600, a tab, the row's name, over every AdmDef_GetEntryByIndex @0x53FC80 row whose
//  +936 word is set); SET: the target parsed before the sub-verb is read, ARMORY 2, NEVER 0,
//  any other word 1]
void AdminConsole::handle_weapon(const Args &args, std::vector<std::string> &replies) {
	const world::WeaponTable *table = ctx_.world != nullptr ? &ctx_.world->tables.weapons : nullptr;
	const auto listed = [&](int index) {
		const world::WeaponTableEntry *row =
				table != nullptr ? table->by_index(static_cast<uint8_t>(index)) : nullptr;
		return row != nullptr && row->loadout_selectable != 0 ? row : nullptr;
	};
	// The 255-entry table as the port keeps it: the restricted (0 / 2) entries in index
	// order, every other index ALWAYS (server_initial_state.cpp's S2C 0x66).
	const auto availability = [&](int index) -> int {
		for (const auto &e : ctx_.weapon_restrictions)
			if (e.first == index) return e.second;
		return 1;
	};
	const auto set_availability = [&](int index, int value) {
		auto &table_entries = ctx_.weapon_restrictions;
		for (auto it = table_entries.begin(); it != table_entries.end(); ++it) {
			if (it->first != index) continue;
			table_entries.erase(it);
			break;
		}
		if (value != 0 && value != 2) return;
		auto at = table_entries.begin();
		while (at != table_entries.end() && at->first < index) ++at;
		table_entries.insert(at, {static_cast<uint8_t>(index), static_cast<uint8_t>(value)});
	};
	if (args.empty()) {
		replies.emplace_back(kWeaponUsage);
		return;
	}
	if (ieq(args[0], "LIST")) {
		std::string out;
		for (int i = 0; i < 255; ++i) {
			const world::WeaponTableEntry *row = listed(i);
			if (row == nullptr) continue;
			char index[16];
			std::snprintf(index, sizeof(index), "%3d.  ", i);
			const int value = availability(i);
			out += index;
			out += value == 0 ? "NEVER " : value == 2 ? "ARMORY" : "ALWAYS";
			out += "\t" + row->name + "\n";
		}
		replies.push_back(out);
		return;
	}
	if (args.size() < 3) {
		replies.emplace_back(kWeaponUsage);
		return;
	}
	const bool all = ieq(args[1], "ALL");
	const int index = all ? 0 : static_cast<uint8_t>(atol_of(args[1]));
	if (!ieq(args[0], "SET")) {
		replies.emplace_back(kWeaponUsage);
		return;
	}
	const int value = ieq(args[2], "ARMORY") ? 2 : (ieq(args[2], "NEVER") ? 0 : 1);
	if (all) {
		for (int i = 0; i < 255; ++i)
			if (listed(i) != nullptr) set_availability(i, value);
		replies.emplace_back("OK - All weapons availbility changed.");
		return;
	}
	// Index 255 writes one past retail's 255-entry table (D-NET-370); dropped here.
	if (index < 255) set_availability(index, value);
	replies.emplace_back("OK - Weapon availbility changed.");
}

// The host's own console line. A session peer queues it to its own server as reliable C2S
// 0x04; an authority that is not a peer runs its arms in order until one takes it.
// Client_ProcessLocalCommand's commands, the NETLOG toggle and NapiClientCmd_ParseAndExecute's
// input bindings are not modeled (D-NET-371); its one-second guard compares the tick with the
// last stamp minus 1000, so it never drops a line.
// [orig: Client_ProcessConsoleCommand @0x520E80 — the guard, Client_ProcessLocalCommand
//  @0x520BF0, the peer's QueueReliableMessage(4) with NetPacket_WriteNullableString @0x42A430,
//  the authority's Server_HandleAdminNetlogCommand @0x5175D0 (a `STAT` prefix taken whole),
//  Server_HandleBanPuntCommand(line, 0) @0x50B380, NapiClientCmd_ParseAndExecute @0x4329C0]
void AdminConsole::console_command(const std::string &line) {
	if (ctx_.is_mp_session_peer != 0) {
		if (seams_.host_client != nullptr) {
			std::vector<uint8_t> body(line.begin(), line.end());
			body.push_back(0);
			seams_.host_client->queue_host_message(c2s::ADMIN_NETLOG_COMMAND, std::move(body));
		}
		return;
	}
	if (ctx_.is_authority == 0) return;
	if (!tokenize_quoted(line).empty() && strutil::starts_with_icase(line, "STAT")) return;
	Server_HandleBanPuntCommand(ctx_, line);
}

// [orig: CAdminServer_HandleCmdCommand @0x4048D0 — the arguments joined, each followed by one
//  space, in a 100-byte stack buffer with no bound (D-NET-362) @0x4048F8..0x404990, then
//  j_Client_ProcessConsoleCommand @0x404997 and `OK - Command executed.` @0x4049AE]
void AdminConsole::handle_cmd(const Args &args, std::vector<std::string> &replies) {
	if (args.empty()) {
		replies.emplace_back("USAGE -  CMD 'IN GAME COMMAND STRING'");
		return;
	}
	std::string line;
	for (const std::string &arg : args) line += arg + " ";
	console_command(line);
	replies.emplace_back("OK - Command executed.");
}

// [orig: CAdminServer_HandleGotoCommand @0x4049D0 — no argument the usage alone @0x4049E4;
//  GAMESTATE @0x4049FB..0x404A0B; MENUSTATE @0x404A2F..0x404A5D; the cycle tail for every form
//  with an argument @0x404A74..0x404AAF]
void AdminConsole::handle_goto(const Args &args, std::vector<std::string> &replies) {
	if (args.empty()) {
		replies.emplace_back(kGotoUsage);
		return;
	}
	if (ieq(args[0], "GAMESTATE")) {
		replies.emplace_back(in_game() ? "ERROR - In 'Game' State." : kNotImplemented);
	} else if (ieq(args[0], "MENUSTATE")) {
		if (scene_ != AdminScene::MainMenu) {
			// Input action 3 passes its binding's gate on every host: the dispatcher reads the
			// flag dword by action code after the start-up re-lay, and record 3 is the `exit` row
			// (flags 0x04000000), which carries none of the head gate's bits, so a Serve Only
			// host quits too (the cycle tail below runs first, in the same frame).
			// [orig: Input_HandleActionBinding @0x49AD8D (dword_8159AC[27 * code]) and its head
			//  gate @0x49AD9C..0x49AE23; KeyBinding_SortBySequentialId @0x498260 (record i holds
			//  the row whose code is i); case 3 @0x49AF1F: g_MissionExitReason = 1 and
			//  CNapiNetwork_DisconnectActiveConnection("I.C:CIDEMIS")]
			if (seams_.quit_to_menu) seams_.quit_to_menu();
		} else {
			replies.emplace_back("ERROR - Already in 'Menu' State");
		}
	} else {
		replies.emplace_back(kGotoUsage);
	}
	cycle_tail(replies);
}

// The global chat sender's authority arm on a host that is not a session peer: S2C 0x14
// [10][255][text] to every in-match slot (mask 0x80) and the line into the host's own window;
// a peer (a listen host) sends C2S 0x0D channel 1 on its own connection instead, its sender
// running the gates. The gates: nothing on the death screen before the spawn gate (a Serve
// Only host has no death screen), nothing empty, the flood table (a refusal echoes the line
// into the host's window), then the `<...>` strip.
// The flood table is the process's one (g_ChatFloodTable @0xB3B788, which only
// Chat_CheckFloodControl reads), which every chat sender checks: on a host with no client the
// console's typed line and this verb share it (`ctx.console_chat_flood`, server_console.h),
// keyed on the one per-main-frame counter (set_main_frame). On that host the authority arm is
// Server_SendConsoleChat's, which posts the sent line to the host's CHAT ring
// (`ctx.console_chat`), and a refusal's echo lands in the same ring in Global's flood colour.
// [orig: Chat_SendTeamMessage @0x49A900 (an IDB misnomer; the global sender) — the gates
//  @0x49A931, Chat_CheckFloodControl @0x49A93B, Chat_StripHtmlTags @0x49A971 into 64 bytes,
//  the peer's QueueReliableMessage(0xD) @0x49A9B4, the authority's SendFiltered(0x14, 1, 310)
//  with mask 0x80 @0x49AA1D and Chat_DispatchToChannel(0xFF, 10, message) @0x49AA2A, the
//  refusal's Chat_AddMessageChannel1(message, palette[3], 930) @0x49A953]
void AdminConsole::chat_send(std::string text) {
	if (ctx_.is_mp_session_peer != 0) {
		if (seams_.host_client == nullptr) return;
		if (seams_.host_client->queue_chat_message(1, text, main_frame_) == hud::ChatSendResult::Flooded &&
				seams_.chat_echo)
			seams_.chat_echo(text);
		return;
	}
	if (Server_SendConsoleChat(ctx_, hud::kChatDispatchGlobal, text, main_frame_) ==
			hud::ChatSendResult::Flooded)
		server_console_post(ctx_.console_chat, text, hud::chat_dispatch_flood_color(hud::kChatDispatchGlobal));
}

// [orig: CAdminServer_HandleChatCommand @0x404AC0 — GET @0x404B0F..0x404BB3 (the window's 40
//  slots of 128 bytes, slot 40 first, each non-empty line and "\r\n", into a 4880-byte reply,
//  D-NET-363); SEND @0x404BE8..0x404C74 (the words, each followed by a space, in a 122-byte
//  buffer with no bound, D-NET-362, then `OK - Chat sent.` whatever the sender did)]
void AdminConsole::handle_chat(const Args &args, std::vector<std::string> &replies) {
	if (!args.empty() && ieq(args[0], "GET")) {
		// A host with no client reads its CHAT ring, oldest first: the raw lines, where retail
		// lists the window's word-wrapped display lines (D-NET-372); a listen host reads its
		// HUD's window through the seam.
		std::vector<std::string> window;
		if (ctx_.is_mp_session_peer == 0) {
			for (const ServerConsoleLine &line : ctx_.console_chat) window.push_back(line.text);
		} else if (seams_.chat_window) {
			window = seams_.chat_window();
		}
		std::string out;
		for (const std::string &line : window)
			if (!line.empty()) out += line + "\r\n";
		replies.push_back(out);
		return;
	}
	if (!args.empty() && ieq(args[0], "SEND")) {
		std::string text;
		for (size_t i = 1; i < args.size(); ++i) text += args[i] + " ";
		chat_send(std::move(text));
		replies.emplace_back("OK - Chat sent.");
		return;
	}
	replies.emplace_back(kChatUsage);
}

} // namespace opennova::inmatch
