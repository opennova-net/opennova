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
//     arms no 0x20) and leaves P1 / P2 as created;
//   - a map change keeps game_settings' server and side passwords, the create's or an admin
//     SET's, whatever the next config carries (D-NET-NEW-L1-42);
//   - the hello's SN / SF read the protocol's copies, which the create and the admin SET write
//     and a map change does not (D-NET-NEW-L1-43);
//   - the game shell's listen host applies the cfg block onto its session config at a map change,
//     so an admin SET's block words reach the next mission (D-NET-NEW-L1-42's shell half).
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
#include <net/npwire/session_hello.h> // make_title_client_hello, ServerHello
#include <net/title/title_identity.h>
#include <runtime/inmatch/admin_console.h>
#include <runtime/inmatch/game_config.h>
#include <runtime/inmatch/host_config.h> // host_screen_from_block, apply_mission_start_session_config
#include <runtime/inmatch/napi_np_protocol.h> // host_server_hello
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_initial_state.h>
#include <runtime/inmatch/server_session.h>

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

// A remote admin's SET line over the host's console (the SET right alone), into `block` (null:
// a scratch block).
bool admin_set(inmatch::NapiNPServerCtx &ctx, const char *line,
		opennova::gamecfg::GameCfg *block = nullptr) {
	opennova::gamecfg::GameCfg scratch;
	inmatch::AdminConsole::Seams seams;
	seams.config_block = block != nullptr ? block : &scratch;
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

// D-NET-NEW-L1-42: game_settings' passwords are the create's or an admin SET's; a map change
// writes none of them, whatever the next config carries.
// [orig: CNapiGameSession_CreateSession @0x4C97C0 - the settings copy @0x4C9826;
//  CAdminServer_HandleSetCommand - CAdminServer_SetSidePassword (+0x60) @0x406284]
bool run_a_map_change_keeps_the_settings_passwords() {
	auto ctx = std::make_unique<inmatch::NapiNPServerCtx>();
	inmatch::GameConfig config;
	config.max_players = 4;
	config.game_type = game_type::kTeamDeathmatch;
	config.server_password = "sesame";
	config.side_a_password = "blue1";
	inmatch::test::bring_up_host(*ctx, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan,
			/*host_key=*/0, nullptr, config);
	if (!expect(admin_set(*ctx, "SET SideBPassword bravo"), "SET SideBPassword is accepted"))
		return false;
	inmatch::GameConfig next = config; // the embedder's next config: other passwords
	next.server_password.clear();
	next.side_a_password = "other";
	next.side_b_password.clear();
	inmatch::continue_session(*ctx, next, nullptr);
	return expect(ctx->config.server_password == "sesame" && ctx->config.side_a_password == "blue1" &&
	                      ctx->config.side_b_password == "bravo" &&
	                      (record_tail(*ctx) & 0x38u) == 0x38u,
	              "after a map change the passwords are the create's and the admin's");
}

// D-NET-NEW-L1-43: the hello's SN and SF and the 0x42's PW gate read the protocol's copies of
// the name (+0x288, "Untitled" for an empty one) and the password (+0x300): the create writes
// them, then only the admin SET's ServerName / ServerPassword arms, so a SET ServerName reaches
// the next hello at once and a map change leaves both as they were.
// [orig: CNapiGameSession_CreateSession @0x4C97C0 - the default @0x4C986F..0x4C9885, +0x288
//  @0x4C9985, +0x300 @0x4C99F3; NapiNPProtocol_SendServerInfoPacket @0x6204B0 - SN
//  @0x6207B2..0x6207DA, SF @0x620872..0x620898; CAdminServer_HandleSetCommand - the name into
//  +0x288 @0x406179, the lone ServerPassword's +0x300 @0x405AD6]
opennova::ServerHello hello_of(const inmatch::NapiNPServerCtx &ctx) {
	const opennova::TitleIdentity &jo = *opennova::title_identity(opennova::gameprofile::GAME_JO);
	return inmatch::host_server_hello(ctx, opennova::make_title_client_hello(jo, 1),
			opennova::PeerAddr{0x0100007Fu, 32110});
}

bool run_the_hello_reads_the_protocols_copies() {
	auto ctx = std::make_unique<inmatch::NapiNPServerCtx>();
	inmatch::GameConfig config;
	config.max_players = 4;
	config.server_name = "Named Host";
	config.server_password = "sesame";
	inmatch::test::bring_up_host(*ctx, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan,
			/*host_key=*/0, nullptr, config);
	opennova::ServerHello hello = hello_of(*ctx);
	if (!expect(hello.sn == "Named Host" && hello.sf == 1u &&
	                    ctx->np_protocol.server_password == "sesame",
	            "the create's name and password in the hello"))
		return false;
	inmatch::GameConfig next = config; // a map change's next config, another name
	next.server_name = "Next Config Name";
	inmatch::continue_session(*ctx, next, nullptr);
	hello = hello_of(*ctx);
	if (!expect(hello.sn == "Named Host" && hello.sf == 1u,
	            "a map change rewrites neither protocol copy"))
		return false;
	if (!expect(admin_set(*ctx, "SET ServerName Admin Renamed"), "SET ServerName is accepted"))
		return false;
	if (!expect(hello_of(*ctx).sn == "Admin Renamed ", "the SET's name reaches the next hello"))
		return false;
	if (!expect(admin_set(*ctx, "SET ServerPassword") && hello_of(*ctx).sf == 0u &&
	                    ctx->np_protocol.server_password.empty(),
	            "a lone SET ServerPassword clears the protocol's copy"))
		return false;
	auto untitled = std::make_unique<inmatch::NapiNPServerCtx>();
	inmatch::GameConfig unnamed;
	unnamed.max_players = 4;
	unnamed.server_name.clear();
	inmatch::test::bring_up_host(*untitled, inmatch::ConnectionMode::HostOnly,
			inmatch::SocketMode::Lan, /*host_key=*/0, nullptr, unnamed);
	return expect(hello_of(*untitled).sn == "Untitled" && untitled->config.server_name.empty(),
	              "an empty name: the protocol's copy is \"Untitled\", the config's stays empty");
}

// D-NET-NEW-L1-42's shell half: the game shell's listen host carries its session config from
// mission to mission and, at a map change, applies the process's cfg block onto it again
// (apply_block_session_settings), as every first-load start applies the block: what a remote
// admin's SET wrote into the block (the attribute word, the rules) reaches the next mission, and
// the words the block does not hold (the callsign, the channel, the rotation's mode) stay the
// session's.
// [orig: Game_StartMission @0x524360 - Game_ApplySessionSettingsToGlobals @0x524662 under the
//  first-load flag @0x52465E; CAdminServer_HandleSetCommand @0x405A60 - the cfg word @0x406190]
bool run_the_shell_host_applies_the_block_at_a_map_change() {
	opennova::gamecfg::GameCfg block;
	block.mp_max_players = 4;
	block.time_limit = 15;
	inmatch::GameConfig session = inmatch::host_screen_from_block(block).config;
	session.game_type = game_type::kTeamDeathmatch;
	session.player_name = "Callsign";
	session.session_channel = inmatch::GameSessionChannel::Lan;
	auto ctx = std::make_unique<inmatch::NapiNPServerCtx>();
	inmatch::test::bring_up_host(*ctx, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan,
			/*host_key=*/0, nullptr, session);
	if (!expect((ctx->config.mp_attributes & kTeamChoose) == 0 &&
	                    ctx->config.round_time_limit_minutes == 15,
	            "the create: the block's word without TeamChoose, its 15 minutes"))
		return false;
	if (!expect(admin_set(*ctx, "SET ChangeTeam 1", &block) &&
	                    admin_set(*ctx, "SET GameTime 30", &block),
	            "SET ChangeTeam 1 and SET GameTime 30 are accepted"))
		return false;
	// The shell's session config, the next map staged on it as every host's mission start
	// stages it: the block, then the rotation's map and mode and the header's name.
	inmatch::MissionRotation rotation;
	rotation.map_file = "next.bms";
	rotation.map_game_type = game_type::kTeamDeathmatch;
	inmatch::GameConfig next = session;
	inmatch::apply_mission_start_session_config(next, &block, rotation, "Next Map");
	inmatch::continue_session(*ctx, next, nullptr);
	return expect((ctx->config.mp_attributes & kTeamChoose) != 0 &&
	                      ctx->config.round_time_limit_minutes == 30 &&
	                      ctx->config.player_name == "Callsign" &&
	                      ctx->config.session_channel == inmatch::GameSessionChannel::Lan &&
	                      ctx->config.game_type == game_type::kTeamDeathmatch &&
	                      next.mission_file == "next.bms" && next.mission_name == "Next Map",
	              "the next mission takes the SET's block words, the rotation's map and mode, and "
	              "keeps the session's own");
}

} // namespace

int main() {
	int failures = 0;
	if (!run_an_admin_change_team_reaches_a_later_record()) ++failures;
	if (!run_the_team_test_reads_the_creates_copy()) ++failures;
	if (!run_a_map_change_keeps_the_creates_words()) ++failures;
	if (!run_a_map_change_keeps_the_settings_passwords()) ++failures;
	if (!run_the_hello_reads_the_protocols_copies()) ++failures;
	if (!run_the_shell_host_applies_the_block_at_a_map_change()) ++failures;
	if (failures != 0) return 1;
	std::printf("OK\n");
	return 0;
}
