// The server-info update's session side (inmatch/server_info.h): the NovaWorld
// listing's columns read off the live session at each run (the server name and
// message, the game type's GateTypeAbbrev text, the mission text's title else
// the header's name, the header's climate, the published cap, the join lock,
// the skins and tracers rules, the password and the round clock), so a map
// change, a rename or a lock reaches the next update (D-NET-373); the session
// settings apply's allowcustomskins copy.
// [orig: Lobby_UpdateServerInfo @0x4FE8C0; Game_ApplySessionSettingsToGlobals
//  @0x551C22]
#include <base/gameprofile/game_type.h>
#include <formats/gamecfg/game_cfg.h>
#include <formats/mission/bms.h>
#include <net/novaworld/lobby_vars.h>
#include <runtime/inmatch/host_config.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_info.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

std::string value(const std::vector<ClientVar> &vars, const char *name) {
	for (const ClientVar &v : vars)
		if (v.fnum == 0 && v.name == name)
			return v.value;
	return "<absent>";
}

// A gametext carrying two GateTypeAbbrev rows.
std::string gametext(const char *section, const char *key, const char *fallback) {
	if (std::strcmp(section, "GateTypeAbbrev") == 0 && std::strcmp(key, "CTF") == 0)
		return "Flag";
	if (std::strcmp(section, "GateTypeAbbrev") == 0 && std::strcmp(key, "TDM") == 0)
		return "Team";
	return fallback;
}

void name_mission(bms::File &mission, const char *name, bms::ClimateType climate) {
	std::memset(mission.header.mission_name, 0, sizeof(mission.header.mission_name));
	std::snprintf(mission.header.mission_name, sizeof(mission.header.mission_name), "%s", name);
	mission.header.climate = climate;
}

// The columns follow the live session, not the registration the host request was built from;
// a map change's next mission and a rename reach the next read.
void test_columns_follow_the_session() {
	world::World world;
	world.match.set_remaining_ticks(3720 * 7 + 5);
	bms::File mission;
	name_mission(mission, "Header Name", bms::ClimateType::Snow);
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.mission = &mission;
	ctx.config.server_name = "Live Name";
	ctx.config.custom_text = "live message";
	ctx.config.server_password = "secret";
	ctx.config.game_type = game_type::kCaptureTheFlag;
	ctx.config.mp_attributes = inmatch::GameConfig::kMpAttribNoTracers;
	ctx.config.allow_custom_skins = 1;
	ctx.config.country = "DE";
	ctx.config.max_players = 10;
	ctx.config.dedicated_server = false;
	ctx.join_locked = true;

	// The host request's registration: the first map's columns.
	HostRegistration r;
	r.server_name = "Requested Name";
	r.mission_name = "First Map";
	r.game_type = "DM";
	r.region_index = 0;
	r.lobby_name = "jop_2_consumer";
	r.host_key = "KEY";
	inmatch::read_session_listing_columns(r, ctx, gametext);
	CHECK(r.server_name == "Live Name");
	CHECK(r.server_message == "live message");
	CHECK(r.game_type == "Flag");
	// No mission text title: the header's name.
	CHECK(r.mission_name == "Header Name");
	CHECK(r.region_index == 2);
	CHECK(r.published_cap == static_cast<int>(ctx.config.player_slot_limit()));
	CHECK(r.locked);
	CHECK(r.skins);
	CHECK(!r.tracers);
	CHECK(r.password);
	CHECK(r.country == "DE");
	CHECK(r.round_time_remaining_ticks == 3720 * 7 + 5);
	// The registration's own keys stay.
	CHECK(r.lobby_name == "jop_2_consumer" && r.host_key == "KEY");

	// Built into the full Host list.
	HostLobbyText text;
	text.yes = "Yes";
	text.no = "No";
	text.region = { "Desert", "Jungle", "Snow" };
	const std::vector<ClientVar> host = make_host_var_list(r, text, /*full=*/true);
	CHECK(value(host, "ServerName") == "Live Name");
	CHECK(value(host, "GameType") == "Flag");
	CHECK(value(host, "MissionName") == "Header Name");
	CHECK(value(host, "Region") == "Snow");
	CHECK(value(host, "Locked") == "Yes");
	CHECK(value(host, "Country") == "XX"); // a locked game advertises XX
	CHECK(value(host, "Skins") == "Yes");
	CHECK(value(host, "Tracers") == "No");
	CHECK(value(host, "TimeLeft") == "7");
	CHECK(value(host, "Password") == "Yes");
	CHECK(value(host, "Msg") == "live message");

	// The mission text's title wins over the header's name.
	ctx.mission_text_title = "Text Title";
	inmatch::read_session_listing_columns(r, ctx, gametext);
	CHECK(r.mission_name == "Text Title");

	// A map change: the next mission, its mode and the kept session's rename, unlocked, tracers on,
	// untimed, no skins, no password.
	bms::File next;
	name_mission(next, "Next Map", bms::ClimateType::Jungle);
	ctx.mission = &next;
	ctx.mission_text_title.clear();
	ctx.config.game_type = game_type::kTeamDeathmatch;
	ctx.config.server_name = "Renamed";
	ctx.config.mp_attributes = 0;
	ctx.config.allow_custom_skins = 0;
	ctx.config.server_password.clear();
	ctx.join_locked = false;
	world.match.set_remaining_ticks(-1);
	inmatch::read_session_listing_columns(r, ctx, gametext);
	CHECK(value(make_host_var_list(r, text, /*full=*/true), "Country") == "DE");
	CHECK(r.mission_name == "Next Map");
	CHECK(r.region_index == 1);
	CHECK(r.game_type == "Team");
	CHECK(r.server_name == "Renamed");
	CHECK(!r.locked && !r.skins && r.tracers && !r.password);
	CHECK(r.round_time_remaining_ticks == -1);

	// No gametext: the abbreviation reads empty, as a missing key does.
	inmatch::read_session_listing_columns(r, ctx, hud::GameTextLookup{});
	CHECK(r.game_type.empty());
}

// The session settings apply copies the cfg's allowcustomskins into the session's word.
void test_skins_from_the_cfg() {
	gamecfg::GameCfg cfg;
	cfg.allowcustomskins = 1;
	CHECK(inmatch::host_session_settings(cfg).config.allow_custom_skins == 1);
	cfg.allowcustomskins = 0;
	CHECK(inmatch::host_session_settings(cfg).config.allow_custom_skins == 0);
}

} // namespace

int main() {
	test_columns_follow_the_session();
	test_skins_from_the_cfg();
	if (failures == 0)
		std::printf("server_info: OK\n");
	return failures == 0 ? 0 : 1;
}
