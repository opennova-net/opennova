// D-NET-964: the session title g_SessionTitle. Every mission start's settings apply copies
// the cfg's server name into it, then zeroes it and fills it with the [TITLE] entry the
// mission-title table holds under the map name cut at its last '.' (up to a CR, at most 31
// bytes), else the map file's first 31 bytes; that table is never loaded, so only the mount's
// text-override table answers. SetServerName overwrites it. The Co-op S2C 0x7B advertises it, as
// the stock Co-op MISSIONNAME does.
// [orig: Game_ApplySessionSettingsToGlobals @0x551FDA, @0x552112..0x55219E;
//  g_MissionTitleTextResource @0x2550194 (no writer); CNapiGameSession_HandleServerCommand
//  @0x4D2D0D; NapiNPMsg_0x7B_BuildPayload @0x507810..0x507824; Game_SerializeMissionInfoToDataStream
//  @0x52373B..0x52375A]

#include <base/gameprofile/game_type.h>
#include <formats/rtxt/rtxt.h>
#include <net/npwire/session_vars.h>
#include <net/protocol/protocol.h>
#include <runtime/inmatch/game_config.h>
#include <runtime/inmatch/host_settings.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_admin_command.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/server_session.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "../common/title_fixture.h"

using namespace opennova;

namespace {

int failures = 0;
#define CHECK(c)                                                                                   \
	do {                                                                                           \
		if (!(c)) {                                                                                \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                               \
			++failures;                                                                            \
		}                                                                                          \
	} while (0)

// The S2C 0x7B body's fourth C string: the advertised mission.
std::string advertised_mission(const std::vector<uint8_t> &body) {
	size_t at = 0;
	for (int skip = 0; skip < 3; ++skip) {
		while (at < body.size() && body[at] != 0) ++at;
		++at;
	}
	std::string out;
	while (at < body.size() && body[at] != 0) out.push_back(static_cast<char>(body[at++]));
	return out;
}

void test_title_rule() {
	// No override table: the map file's first 31 bytes.
	CHECK(inmatch::mission_session_title(nullptr, "CO01.BMS") == "CO01.BMS");
	const std::string long_map = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.BMS";
	CHECK(inmatch::mission_session_title(nullptr, long_map) == long_map.substr(0, 31));
	// An override [TITLE] entry under the name cut at its last '.' answers: its text up to the
	// first CR, at most 31 bytes.
	rtxt::File override_table;
	override_table.sections.push_back(rtxt::Section{"TITLE", 2});
	rtxt::Entry desert;
	desert.key = "CO01";
	desert.text = "Desert Storm\r\nsecond line";
	rtxt::Entry longer;
	longer.key = "CO02.v2";
	longer.text = "A title well past thirty-one bytes long";
	override_table.entries = {desert, longer};
	CHECK(inmatch::mission_session_title(&override_table, "CO01.BMS") == "Desert Storm");
	CHECK(inmatch::mission_session_title(&override_table, "CO02.v2.BMS") ==
	      std::string("A title well past thirty-one bytes long").substr(0, 31));
	// No entry: the map file; a name with no '.' looks nothing up.
	CHECK(inmatch::mission_session_title(&override_table, "TDM01.BMS") == "TDM01.BMS");
	rtxt::File bare;
	bare.sections.push_back(rtxt::Section{"TITLE", 1});
	rtxt::Entry plain;
	plain.key = "PLAIN";
	plain.text = "Never";
	bare.entries = {plain};
	CHECK(inmatch::mission_session_title(&bare, "PLAIN") == "PLAIN");
}

void test_coop_advertises_the_session_title() {
	inmatch::NapiNPServerCtx ctx;
	test_titles::jo_config(ctx.config);
	ctx.np_protocol.identity = title_protocol("jo");
	// The hosting gate every ServerCommand verb but SetMPReset tests (D-NET-383).
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.config.game_type = game_type::kCoop;
	ctx.config.mission_file = "CO01.BMS";
	ctx.config.mission_name = "Header Name";
	ctx.config.server_name = "Cfg Host";
	ctx.session_title = inmatch::mission_session_title(nullptr, ctx.config.mission_file);
	inmatch::NapiNPConnection conn;
	conn.player_name = "Joiner";
	CHECK(advertised_mission(inmatch::build_tag7b_session_summary(ctx.config, ctx.mission_text_title,
	                                                              ctx.session_title, conn)) == "CO01.BMS");
	CHECK(inmatch::host_session_vars(ctx.config, ctx.mission_text_title, ctx.session_title).mission_name ==
	      "CO01.BMS");
	// SetServerName writes the session title: the Co-op 0x7B and MISSIONNAME advertise the name.
	const inmatch::ServerCommandOutcome outcome =
			inmatch::Server_ExecuteServerCommand(ctx, nullptr, "SetServerName", "", {"Night Ops"});
	CHECK(outcome.handled && ctx.session_title == "Night Ops");
	CHECK(advertised_mission(inmatch::build_tag7b_session_summary(ctx.config, ctx.mission_text_title,
	                                                              ctx.session_title, conn)) == "Night Ops");
	CHECK(inmatch::host_session_vars(ctx.config, ctx.mission_text_title, ctx.session_title).mission_name ==
	      "Night Ops");
	// Outside the waypoint family the 0x7B advertises the mission text's title, else the
	// header's name, whatever the session title.
	ctx.config.game_type = game_type::kTeamDeathmatch;
	CHECK(advertised_mission(inmatch::build_tag7b_session_summary(ctx.config, ctx.mission_text_title,
	                                                              ctx.session_title, conn)) == "Header Name");
}

// The mission-data block (the S2C 0x64 transfer) carries the session title at +0x74, set by
// the mission start's settings apply ahead of the block's build, at the create and at every
// map change's continue.
// [orig: CNapiGameSession_InitRandomSeedOrRequest @0x51E8F0 - g_MapFileName into +0x54
//  @0x51E973, g_SessionTitle into +0x74 @0x51E983 (Napi_CopyString(.., 32)); Game_StartMission
//  @0x524662 (the settings apply) ahead of @0x5248A7 (the block)]
std::string block_string(const inmatch::MissionMetadataBlob &blob, size_t offset) {
	std::string out;
	for (size_t i = offset; i < offset + 32 && blob[i] != 0; ++i) out.push_back(static_cast<char>(blob[i]));
	return out;
}

void test_the_mission_block_carries_the_session_title() {
	rtxt::File override_table;
	override_table.sections.push_back(rtxt::Section{"TITLE", 1});
	rtxt::Entry desert;
	desert.key = "CO01";
	desert.text = "Desert Storm";
	override_table.entries = {desert};
	inmatch::GameConfig config;
	test_titles::jo_config(config);
	config.game_type = game_type::kCoop;
	config.mission_file = "CO01.BMS";
	config.server_name = "Cfg Host";
	inmatch::SessionStartup startup;
	startup.protocol = title_protocol("jo");
	startup.text_override = &override_table;
	inmatch::NapiNPServerCtx ctx;
	test_titles::jo_config(ctx.config);
	// Not the authority, so the create's start_server installs no identity: the map change's
	// block reads the context's, which names JO's.
	ctx.np_protocol.identity = startup.protocol;
	CHECK(inmatch::create_session(ctx, config, startup) == inmatch::CreateSessionResult::Created);
	CHECK(ctx.session_title == "Desert Storm");
	CHECK(block_string(ctx.mission_metadata_blob, 84) == "CO01.BMS");
	CHECK(block_string(ctx.mission_metadata_blob, 116) == "Desert Storm");
	// A map change's next mission with no [TITLE] entry: the map file in both slots, over a
	// SetServerName's title.
	ctx.session_title = "Night Ops";
	config.mission_file = "CO02.BMS";
	inmatch::continue_session(ctx, config, &override_table);
	CHECK(ctx.session_title == "CO02.BMS");
	CHECK(block_string(ctx.mission_metadata_blob, 116) == "CO02.BMS");
	// Without a table the title is the map file's.
	config.mission_file = "CO01.BMS";
	inmatch::continue_session(ctx, config, nullptr);
	CHECK(block_string(ctx.mission_metadata_blob, 116) == "CO01.BMS");
}

} // namespace

int main() {
	test_title_rule();
	test_coop_advertises_the_session_title();
	test_the_mission_block_carries_the_session_title();
	if (failures != 0) {
		std::printf("npruntime_session_title: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("npruntime_session_title: ok\n");
	return 0;
}
