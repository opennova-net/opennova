// The session's two flag words and the S2C 0x08's live BuildFlags (D-NET-NEW-L1-40).
//
// Retail's 0x08 record calls CNapiServerConfig_BuildFlags each time it is written, so a later
// joiner's tail follows a remote admin's SET after the create; the protocol's advertised words
// (the server hello's P1 / P2) are the create's, recomputed only by an admin password SET, and a
// map change writes neither. BuildFlags' first TeamChoose test reads the cfg block's live
// mp-attribute word (which the admin SET's tail rewrites), its team test the create's
// game_settings copies of the game type and that word (which nothing after the create writes):
//   - a deathmatch host's `SET ChangeTeam 1` sets the 0x08 tail's bit 4; P2 keeps the create's;
//   - a team host created with TeamChoose keeps bit 4 after `SET ChangeTeam 0` (the create's
//     copy), a deathmatch host's drops it;
//   - a map change to a team mission keeps the create's type in the team test (a side password
//     arms no 0x20) and leaves P1 / P2 as created.
// [orig: ServerConfig_SerializeToPacket @0x505BD0 - CNapiServerConfig_BuildFlags @0x505D29,
//  stored @0x505D3B; CNapiServerConfig_BuildFlags @0x4C4DC0 - the cfg word's bit 4 @0x4C4DD2,
//  `test [ecx+0F34h], 10000h` @0x4C4E30, `test [ecx+0F38h], al` @0x4C4E3C, side A @0x4C4E5F;
//  CNapiGameSession_BuildAndCreateSession @0x5694D0 - the settings +0xCC @0x569678, +0xD0
//  @0x56967F; CNapiGameSession_CreateSession @0x4C97C0 - P1 @0x4C9A09, P2 @0x4C9A1A;
//  CAdminServer_HandleSetCommand @0x405A60 - ChangeTeam @0x405C80 / @0x405C89, the cfg word
//  @0x406190; NapiNPProtocol_SendServerInfoPacket - P1 @0x6208A8, P2 @0x6208CF]

#include "host_test_setup.h"

#include <base/gameprofile/game_type.h>
#include <base/io/le.h>
#include <formats/admincfg/admin_cfg.h>
#include <formats/gamecfg/game_cfg.h>
#include <net/admin/admin_server.h>
#include <runtime/inmatch/admin_console.h>
#include <runtime/inmatch/game_config.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_initial_state.h>
#include <runtime/inmatch/server_session.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace inmatch = opennova::inmatch;
namespace game_type = opennova::game_type;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

constexpr uint32_t kTeamChoose = inmatch::GameConfig::kMpAttribTeamChoose;

// A LAN Serve Only host of `type` whose cfg mp-attribute word carries TeamChoose or not.
std::unique_ptr<inmatch::NapiNPServerCtx> host(uint32_t type, bool team_choose,
		const std::string &side_a_password = std::string()) {
	auto ctx = std::make_unique<inmatch::NapiNPServerCtx>();
	inmatch::GameConfig config;
	config.max_players = 4;
	config.game_type = type;
	config.mp_attributes = team_choose ? (config.mp_attributes | kTeamChoose)
	                                   : (config.mp_attributes & ~kTeamChoose);
	config.side_a_password = side_a_password;
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

uint32_t record_tail(const inmatch::NapiNPServerCtx &ctx) {
	const std::vector<uint8_t> record = inmatch::serialize_server_config(ctx);
	return record.size() == 51 ? opennova::io::read_u32_le(record.data() + 47) : 0xFFFFFFFFu;
}

bool run_an_admin_change_team_reaches_a_later_record() {
	auto ctx = host(game_type::kDeathmatch, false);
	const uint32_t p2 = ctx->np_protocol.build_flags;
	if (!expect((p2 & 0x904u) == 0x900u && record_tail(*ctx) == p2,
	            "a LAN deathmatch host without TeamChoose: 0x900 set, bit 4 clear, the tail the same"))
		return false;
	if (!expect(admin_set(*ctx, "SET ChangeTeam 1"), "SET ChangeTeam 1 is accepted")) return false;
	return expect(record_tail(*ctx) == (p2 | 0x4u) && ctx->np_protocol.build_flags == p2,
	              "the next 0x08 tail carries bit 4; P2 keeps the create's word");
}

bool run_the_team_test_reads_the_creates_copy() {
	auto team = host(game_type::kTeamDeathmatch, true);
	const uint32_t team_p2 = team->np_protocol.build_flags;
	if (!expect((team_p2 & 0x4u) != 0, "a team host created with TeamChoose advertises bit 4"))
		return false;
	if (!expect(admin_set(*team, "SET ChangeTeam 0"), "SET ChangeTeam 0 is accepted")) return false;
	if (!expect(record_tail(*team) == team_p2,
	            "the team test's create copy keeps bit 4 in the tail after ChangeTeam 0"))
		return false;
	auto dm = host(game_type::kDeathmatch, true);
	const uint32_t dm_p2 = dm->np_protocol.build_flags;
	if (!expect((dm_p2 & 0x4u) != 0 && admin_set(*dm, "SET ChangeTeam 0"),
	            "a deathmatch host created with TeamChoose, then ChangeTeam 0"))
		return false;
	return expect(record_tail(*dm) == (dm_p2 & ~0x4u) && dm->np_protocol.build_flags == dm_p2,
	              "with no team test the tail drops bit 4; P2 keeps it");
}

bool run_a_map_change_keeps_the_creates_words() {
	auto ctx = host(game_type::kDeathmatch, false, "blue1");
	const uint32_t p1 = ctx->np_protocol.server_flags;
	const uint32_t p2 = ctx->np_protocol.build_flags;
	if (!expect(p1 == game_type::kDeathmatch && (p2 & 0x30u) == 0,
	            "a deathmatch create: P1 its type, no side-password bit"))
		return false;
	inmatch::GameConfig next = ctx->config;
	next.game_type = game_type::kTeamDeathmatch; // the rotation row's type
	inmatch::continue_session(*ctx, next, nullptr);
	return expect(ctx->config.game_type == game_type::kTeamDeathmatch &&
	                      ctx->np_protocol.server_flags == p1 &&
	                      ctx->np_protocol.build_flags == p2 && record_tail(*ctx) == p2,
	              "after a map change to a team mission: P1 / P2 the create's, no 0x20 in the tail");
}

} // namespace

int main() {
	int failures = 0;
	if (!run_an_admin_change_team_reaches_a_later_record()) ++failures;
	if (!run_the_team_test_reads_the_creates_copy()) ++failures;
	if (!run_a_map_change_keeps_the_creates_words()) ++failures;
	if (failures != 0) return 1;
	std::printf("OK\n");
	return 0;
}
