// The join check's side-password gate (D-NET-1100).
//
// Retail's game-layer join validator runs its side-password leg (codes 18 / 19 / 20 / 22) only
// when CNapiServerConfig_BuildFlags, called at the join, carries the team-choose bit 4. That bit is
// the cfg block's live mp-attribute word OR, in a team game, the create's game_settings copy of
// it, so it is not the cfg word alone:
//   - a team host created with TeamChoose keeps the leg after a remote admin's `SET ChangeTeam 0`
//     cleared the cfg word (the create's copy still arms it);
//   - a deathmatch host created without TeamChoose runs the leg once `SET ChangeTeam 1` armed the
//     cfg word;
//   - a deathmatch host created with TeamChoose drops the leg after `SET ChangeTeam 0`.
// A deathmatch create clears game_settings' side passwords (D-NET-1103), so there the leg
// compares the cleared words (any password passes) until an admin SET writes one again.
// [orig: Server_ValidatePlayerJoinRequest @0x512100 - CNapiServerConfig_BuildFlags(&g_NapiNPCtx)
//  @0x512495, `test al, 4` @0x51249A, side A's reject 19 @0x51256C; CNapiServerConfig_BuildFlags
//  @0x4C4DC0 - the cfg word's bit 4 @0x4C4DD2, `test [ecx+0F34h], 10000h` @0x4C4E30, `test
//  [ecx+0F38h], al` @0x4C4E3C; CAdminServer_HandleSetCommand @0x405A60 - ChangeTeam @0x405C80 /
//  @0x405C89, the cfg word @0x406190, CAdminServer_SetSidePassword @0x406247;
//  CNapiGameSession_CreateSession @0x4C97C0 - the non-team clear @0x4C98E2..0x4C990F; the
//  empty side A's admit `test esi, esi` @0x512547]

#include "host_test_setup.h"

#include <base/gameprofile/game_type.h>
#include <formats/admincfg/admin_cfg.h>
#include <formats/gamecfg/game_cfg.h>
#include <net/admin/admin_server.h>
#include <net/npwire/flat_tlv.h> // append_flat_tlv (the JOIN body)
#include <runtime/inmatch/admin_console.h>
#include <runtime/inmatch/game_config.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_join_check.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "../common/test_expect.h"

namespace {

namespace inmatch = opennova::inmatch;
namespace game_type = opennova::game_type;

using test_expect::expect;

constexpr uint32_t kTeamChoose = inmatch::GameConfig::kMpAttribTeamChoose;

// A LAN Serve Only host of `type` whose side A carries the password "blue1" and whose cfg
// mp-attribute word carries TeamChoose or not.
std::unique_ptr<inmatch::NapiNPServerCtx> host(uint32_t type, bool team_choose) {
	auto ctx = std::make_unique<inmatch::NapiNPServerCtx>();
	inmatch::GameConfig config;
	config.max_players = 4;
	config.game_type = type;
	config.mp_attributes = team_choose ? (config.mp_attributes | kTeamChoose)
	                                   : (config.mp_attributes & ~kTeamChoose);
	config.side_a_password = "blue1";
	inmatch::test::bring_up_host(*ctx, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan,
			/*host_key=*/0, nullptr, config);
	return ctx;
}

// A remote admin's SET line over the host's console (the SET right alone).
bool admin_set(inmatch::NapiNPServerCtx &ctx, const char *line) {
	opennova::gamecfg::GameCfg block;
	inmatch::AdminConsole::Seams seams;
	seams.config_block = &block;
	inmatch::AdminConsole console(ctx, std::move(seams));
	opennova::AdminSession session;
	session.user = "admin";
	session.rights = opennova::admincfg::right::kSet;
	std::vector<std::string> replies;
	(void)console.dispatch(session, line, replies);
	return replies.size() == 1 && replies[0] == "OK - Setting Changed.";
}

// The validator's code for a stock JO joiner (no spectator request) choosing side A (TR 0) with
// JSP `password`.
uint32_t join_code(inmatch::NapiNPServerCtx &ctx, const char *password) {
	std::vector<inmatch::NapiNPConnection> roster(1);
	inmatch::NapiNPConnection &joiner = roster.front();
	joiner.type = inmatch::NapiNPConnection::kTypeServerSide;
	joiner.join_environment = {0, 2, 1, 0, 20042002, 180};
	joiner.join_password = password;
	joiner.char_vars.team_request = 0;
	// The JOIN body a stock joiner of the host's expansion sends: its EXP and the zero CRC the
	// host's checksum (0 here) reads.
	std::vector<uint8_t> join;
	const std::string &exp = ctx.config.expansion;
	if (!exp.empty())
		opennova::append_flat_tlv(join, "EXP", reinterpret_cast<const uint8_t *>(exp.c_str()),
				static_cast<uint16_t>(exp.size() + 1));
	const uint8_t crc[] = {'0', 0};
	opennova::append_flat_tlv(join, "VERSIONCRCSTRING", crc, sizeof(crc));
	return inmatch::Server_ValidatePlayerJoinRequest(ctx.config, &ctx, joiner, roster, join);
}

bool run_a_team_host_keeps_the_leg_after_change_team_0() {
	auto ctx = host(game_type::kTeamDeathmatch, true);
	if (!expect(join_code(*ctx, "wrong") == 19 && join_code(*ctx, "BLUE1") == 0,
	            "a team host created with TeamChoose: a wrong side-A password is refused 19"))
		return false;
	if (!expect(admin_set(*ctx, "SET ChangeTeam 0") &&
	                    (ctx->config.mp_attributes & kTeamChoose) == 0,
	            "SET ChangeTeam 0 clears the cfg word's bit 4"))
		return false;
	return expect(join_code(*ctx, "wrong") == 19 && join_code(*ctx, "blue1") == 0,
	              "the create's copy keeps the leg: a wrong password is still refused 19");
}

// A deathmatch create clears the side passwords (D-NET-1103), so the cfg block's "blue1"
// never reaches the leg; an admin SET SideAPassword writes it again.
bool run_a_deathmatch_host_follows_the_cfg_word() {
	auto off = host(game_type::kDeathmatch, false);
	if (!expect(off->config.side_a_password.empty() && join_code(*off, "wrong") == 0,
	            "a deathmatch create clears side A; without TeamChoose no leg runs"))
		return false;
	if (!expect(admin_set(*off, "SET ChangeTeam 1"), "SET ChangeTeam 1 is accepted")) return false;
	if (!expect(join_code(*off, "wrong") == 0,
	            "the live cfg word arms the leg, and the cleared side A admits any password"))
		return false;
	if (!expect(admin_set(*off, "SET SideAPassword blue1"), "SET SideAPassword is accepted"))
		return false;
	if (!expect(join_code(*off, "wrong") == 19 && join_code(*off, "BLUE1") == 0,
	            "the admin's side A password: a wrong one is refused 19"))
		return false;
	auto on = host(game_type::kDeathmatch, true);
	if (!expect(join_code(*on, "wrong") == 0,
	            "a deathmatch host created with TeamChoose: the leg runs over a cleared side A"))
		return false;
	if (!expect(admin_set(*on, "SET SideAPassword blue1") && join_code(*on, "wrong") == 19,
	            "the admin's side A password is refused 19 for a wrong one"))
		return false;
	if (!expect(admin_set(*on, "SET ChangeTeam 0"), "SET ChangeTeam 0 is accepted")) return false;
	return expect(join_code(*on, "wrong") == 0,
	              "with no team test, SET ChangeTeam 0 drops the leg");
}

} // namespace

int main() {
	int failures = 0;
	if (!run_a_team_host_keeps_the_leg_after_change_team_0()) ++failures;
	if (!run_a_deathmatch_host_follows_the_cfg_word()) ++failures;
	if (failures != 0) return 1;
	std::printf("OK\n");
	return 0;
}
