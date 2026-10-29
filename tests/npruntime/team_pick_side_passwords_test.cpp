// npruntime_team_pick_side_passwords: the team pick's side passwords are the mission start's
// pair, not game_settings' (D-NET-1103's DIVERGENT).
//
// Retail's pick compares a joiner's JSP with g_SidePasswordA / g_SidePasswordB, which only the
// mission start writes: every first-load start copies the cfg block's two side passwords in
// through strncpy 0x11. The create's clear of a game that is no team game empties game_settings'
// words alone, and an admin SET writes the block and game_settings' at once but the pick's pair
// only at the next mission start. So:
//   - a session created on a deathmatch row with the block's side passwords set, continued into
//     a team mission, picks the side whose password a JSP matches (both set and no match: no
//     team, 0), while the join legs still read the create's cleared words;
//   - an admin SET SideAPassword mid-mission leaves the pick on the old password until the next
//     mission start;
//   - a 17-character side password fills its 17-byte field with no NUL, so the pick's string
//     runs on into the field behind it (side A's into g_SidePasswordB, side B's into
//     g_SessionCountryCode).
// And the remote add's TDM preference test reads neither copy: it takes the cfg block's own game
// type and side passwords, live (D-NET-1105).
// [orig: Server_AssignPlayerTeam @0x4FE310 - g_SidePasswordA @0x4FE41C / _stricmp @0x4FE43F,
//  side A's team g_RotationSideTeam0 @0x4FE456, g_SidePasswordB @0x4FE464 / @0x4FE47D, side B's
//  g_RotationSideTeam1 @0x4FE492, both set and no match +416 = 0 @0x4FE4BA, return 0 @0x4FE4CA;
//  Game_ApplySessionSettingsToGlobals @0x551500 - strncpy(g_SessionCountryCode, country_3C5, 3)
//  @0x55200D, strncpy(g_SidePasswordA, sideAPassword_383, 0x11) @0x552043, (g_SidePasswordB,
//  sideBPassword_394, 0x11) @0x552054; g_SidePasswordA @0x24D1FF5, g_SidePasswordB @0x24D2006,
//  g_SessionCountryCode @0x24D2017; Game_StartMission @0x524360 - the first-load flag @0x5243A4,
//  the apply @0x524662; CNapiGameSession_CreateSession @0x4C97C0 - the non-team clear
//  @0x4C98E2..0x4C990F; CAdminServer_HandleSetCommand @0x405A60 - strncpy(sideAPassword_383, ..,
//  0x11) @0x406230, CAdminServer_SetSidePassword(game_settings) @0x406284]

#include "host_test_setup.h"

#include <base/gameprofile/game_type.h>
#include <formats/admincfg/admin_cfg.h>
#include <formats/gamecfg/game_cfg.h>
#include <net/admin/admin_server.h>
#include <runtime/inmatch/admin_console.h>
#include <runtime/inmatch/game_config.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/server_spawn.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "../common/test_expect.h"

namespace {

namespace inmatch = opennova::inmatch;
namespace game_type = opennova::game_type;

using test_expect::expect;

// A mission's config as the cfg block hands it in: `type`, two teams, and the block's side
// passwords "blue1" / "red1".
inmatch::GameConfig block_config(uint32_t type) {
	inmatch::GameConfig config;
	config.max_players = 4;
	config.game_type = type;
	config.num_teams = 2;
	config.side_a_password = "blue1";
	config.side_b_password = "red1";
	return config;
}

// A LAN Serve Only host created on a mission of `type`.
std::unique_ptr<inmatch::NapiNPServerCtx> host(uint32_t type) {
	auto ctx = std::make_unique<inmatch::NapiNPServerCtx>();
	inmatch::test::bring_up_host(*ctx, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan,
			/*host_key=*/0, nullptr, block_config(type));
	return ctx;
}

// The team the pick gives a fresh joiner with JSP `password` and team request `tr` (0xFF:
// automatic).
uint8_t pick(const inmatch::NapiNPServerCtx &ctx, const char *password, uint8_t tr = 0xFF) {
	std::vector<inmatch::NapiNPConnection> roster(1);
	roster.front().join_password = password;
	roster.front().char_vars.team_request = tr;
	return inmatch::Server_ReservePlayerTeam(ctx.config, true, roster, roster.front());
}

// A remote admin's SET line over the host's console (the SET right alone).
bool admin_set(inmatch::NapiNPServerCtx &ctx, const char *line) {
	opennova::gamecfg::GameCfg block;
	inmatch::AdminConsole::Seams seams;
	seams.config_block = &block;
	seams.config_title = opennova::title_by_code("jo");
	inmatch::AdminConsole console(ctx, std::move(seams));
	opennova::AdminSession session;
	session.user = "admin";
	session.rights = opennova::admincfg::right::kSet;
	std::vector<std::string> replies;
	(void)console.dispatch(session, line, replies);
	return replies.size() == 1 && replies[0] == "OK - Setting Changed.";
}

bool run_a_deathmatch_create_continued_into_a_team_mission() {
	auto ctx = host(game_type::kDeathmatch);
	if (!expect(ctx->config.side_a_password.empty() && ctx->config.side_b_password.empty(),
	            "a deathmatch create clears game_settings' side passwords"))
		return false;
	if (!expect(ctx->config.mission_side_a_password == "blue1" &&
	                    ctx->config.mission_side_b_password == "red1",
	            "the clear leaves the mission start's pair, the cfg block's words"))
		return false;
	inmatch::continue_session(*ctx, block_config(game_type::kTeamDeathmatch), nullptr);
	if (!expect(ctx->config.side_a_password.empty() && ctx->config.side_b_password.empty(),
	            "the map change keeps game_settings' cleared words"))
		return false;
	if (!expect(pick(*ctx, "BLUE1") == 1, "a JSP matching side A picks side A's team"))
		return false;
	if (!expect(pick(*ctx, "red1") == 2, "a JSP matching side B picks side B's team"))
		return false;
	return expect(pick(*ctx, "wrong") == 0,
	              "both sides set and no match: no team (0), never the balance");
}

bool run_an_admin_set_reaches_the_pick_at_the_next_start() {
	auto ctx = host(game_type::kTeamDeathmatch);
	if (!expect(admin_set(*ctx, "SET SideAPassword green2") &&
	                    ctx->config.side_a_password == "green2",
	            "SET SideAPassword writes game_settings' side A at once"))
		return false;
	if (!expect(pick(*ctx, "blue1") == 1 && pick(*ctx, "green2") == 0,
	            "the pick keeps the mission start's side A until the next start"))
		return false;
	// The next mission's start applies the block as the SET left it.
	inmatch::GameConfig next = block_config(game_type::kTeamDeathmatch);
	next.side_a_password = "green2";
	inmatch::continue_session(*ctx, next, nullptr);
	return expect(pick(*ctx, "green2") == 1 && pick(*ctx, "blue1") == 0,
	              "at the next start the pick takes the SET's side A");
}

bool run_a_17_character_side_password_runs_on() {
	inmatch::GameConfig config;
	config.country = "USA";
	config.side_a_password = "abcdefghijklmnop"; // 16 characters: its NUL fits
	config.side_b_password = "red";
	inmatch::apply_mission_side_passwords(config);
	if (!expect(config.mission_side_a_password == "abcdefghijklmnop" &&
	                    config.mission_side_b_password == "red",
	            "a word shorter than 17 characters is copied whole"))
		return false;
	config.side_a_password = "abcdefghijklmnopq"; // 17 characters: no NUL in the field
	inmatch::apply_mission_side_passwords(config);
	if (!expect(config.mission_side_a_password == "abcdefghijklmnopqred",
	            "a 17-character side A runs on into g_SidePasswordB"))
		return false;
	config.side_b_password = "ABCDEFGHIJKLMNOPQ";
	inmatch::apply_mission_side_passwords(config);
	if (!expect(config.mission_side_b_password == "ABCDEFGHIJKLMNOPQUSA" &&
	                    config.mission_side_a_password == "abcdefghijklmnopqABCDEFGHIJKLMNOPQUSA",
	            "a 17-character side B runs on into g_SessionCountryCode"))
		return false;
	// The pick compares the JSP's 16-character copy with the run-on string, which no JSP
	// reaches: with both sides set the joiner gets no team.
	config.game_type = game_type::kTeamDeathmatch;
	config.num_teams = 2;
	std::vector<inmatch::NapiNPConnection> roster(1);
	roster.front().join_password = "abcdefghijklmnopq";
	roster.front().char_vars.team_request = 0xFF;
	return expect(inmatch::Server_ReservePlayerTeam(config, true, roster, roster.front()) == 0,
	              "a JSP of the 17 characters matches neither run-on side");
}

// D-NET-1105: the remote add's TDM preference test reads the cfg block's own words, its game
// type (not the mission's) and its side passwords, which an admin SET writes at once: on a
// four-team TDM mission with TeamChoose, a side-B request (TR 1) is honored (team 2) unless the
// block's type is TDM and a block side password is set, when it is forced to automatic and the
// empty table's balance gives team 1.
// [orig: Server_PlayerAdd @0x51CBC0 - eax 0 @0x51CBDF, `cmp gameType_3F0, 10000h` @0x51CC6C,
//  sideAPassword_383 @0x51CC78, sideBPassword_394 @0x51CC85, event+97 = -1 @0x51CC91;
//  Server_AssignPlayerTeam @0x4FE310 - the team-choose legs @0x4FE52E..0x4FE56D, the balance
//  @0x4FE588..0x4FE723; CAdminServer_HandleSetCommand @0x405A60 - the block's side A @0x406230]
std::unique_ptr<inmatch::NapiNPServerCtx> four_team_host(int32_t block_game_type) {
	auto ctx = std::make_unique<inmatch::NapiNPServerCtx>();
	inmatch::GameConfig config;
	config.max_players = 8;
	config.game_type = game_type::kTeamDeathmatch;
	config.num_teams = 4;
	config.mp_attributes |= inmatch::GameConfig::kMpAttribTeamChoose;
	config.cfg_game_type = block_game_type;
	inmatch::test::bring_up_host(*ctx, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan,
			/*host_key=*/0, nullptr, config);
	return ctx;
}

bool run_the_add_reads_the_blocks_words() {
	auto dm_block = four_team_host(static_cast<int32_t>(game_type::kDeathmatch));
	if (!expect(pick(*dm_block, "", 1) == 2, "no side password: the side-B request is honored"))
		return false;
	if (!expect(admin_set(*dm_block, "SET SideAPassword blue1") &&
	                    dm_block->config.mission_side_a_password.empty() &&
	                    pick(*dm_block, "", 1) == 2,
	            "a deathmatch block's type never forces the request, on a TDM mission too"))
		return false;
	auto tdm_block = four_team_host(static_cast<int32_t>(game_type::kTeamDeathmatch));
	if (!expect(pick(*tdm_block, "", 1) == 2,
	            "a TDM block with no side password honors the side-B request"))
		return false;
	if (!expect(admin_set(*tdm_block, "SET SideAPassword blue1") &&
	                    tdm_block->config.mission_side_a_password.empty(),
	            "SET SideAPassword writes the block's word, not the pick's pair"))
		return false;
	return expect(pick(*tdm_block, "", 1) == 1,
	              "the block's side password forces the request at once: the balance's team 1");
}

} // namespace

int main() {
	int failures = 0;
	if (!run_a_deathmatch_create_continued_into_a_team_mission()) ++failures;
	if (!run_an_admin_set_reaches_the_pick_at_the_next_start()) ++failures;
	if (!run_a_17_character_side_password_runs_on()) ++failures;
	if (!run_the_add_reads_the_blocks_words()) ++failures;
	if (failures != 0) return 1;
	std::printf("OK\n");
	return 0;
}
