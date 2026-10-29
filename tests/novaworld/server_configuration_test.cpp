#include "hosted/model/console_configuration.h"
#include "server_completion.h"
#include <base/io/json.h>
#include <formats/gamecfg/game_cfg.h>
#include <runtime/inmatch/host_config.h>
#include "common/test_expect.h"
#include <set>

using namespace opennova;
namespace hosted = novaworld_server::hosted;

int main() {
	const gamecfg::WeaponRoster weapons = { { "None", 0 }, { "Rifle", 1 }, { "Grenade", 1 } };
	io::JsonValue patch;
	std::string error, field;
	TEST_EXPECT(
			io::json_parse(R"({"server_message":"Welcome","replay":false,"side_a_password":"blue",
        "spectator_slots":-1,"vote_percent":0.625,"auto_balance":true,"flag_return_time":999,
        "flag_reset_time":777,"classes":{"sniper":false},"weapons":{"Rifle":"armory"}})",
					patch, error));
	auto cfg = gamecfg::defaults();
	TEST_EXPECT(gamecfg::apply_server_settings(cfg, patch, true, weapons, field, error));
	TEST_EXPECT(
			cfg.replay == 0 && cfg.mp_num_spectators_max == -1 && cfg.mpvoting_percent == 0.625f);
	TEST_EXPECT(cfg.class_availability[6] == 0 && cfg.weapon_availability[1] == 2);
	auto dfx = gamecfg::defaults({}, 0, 0, gamecfg::ConfigTable::Dfx2);
	TEST_EXPECT(gamecfg::apply_server_settings(dfx, patch, true, weapons, field, error));
	TEST_EXPECT(dfx.weapon_availability[1] == 3);
	const auto previous = cfg.servermsg;
	io::JsonValue bad;
	TEST_EXPECT(io::json_parse(R"({"server_message":"bad","remote_admin_port":1234})", bad, error));
	TEST_EXPECT(!gamecfg::apply_server_settings(cfg, bad, true, weapons, field, error));
	TEST_EXPECT(cfg.servermsg == previous); // validation is atomic
	TEST_EXPECT(io::json_parse(R"({"max_players":65})", bad, error));
	TEST_EXPECT(!gamecfg::apply_server_settings(cfg, bad, true, weapons, field, error));
	TEST_EXPECT(gamecfg::apply_server_settings(dfx, bad, true, weapons, field, error));

	hosted::HostedSettings settings;
	settings.game_name = "Configuration test";
	settings.missions = { "a.bms", "a.bms", "b.bms" };
	settings.mission_options = { 0, 1, 0 };
	settings.options = patch;
	hosted::HostedSettings copy;
	TEST_EXPECT(hosted::read_settings_document(
			hosted::settings_document(settings), copy, field, error));
	TEST_EXPECT(
			copy.missions == settings.missions && copy.mission_options == settings.mission_options);
	TEST_EXPECT(io::json_parse(R"({"options":{"classes":{"medic":false}}})", bad, error));
	TEST_EXPECT(hosted::read_settings_document(bad, copy, field, error));
	TEST_EXPECT(!copy.options.get("classes")->get_bool("sniper", true) &&
			!copy.options.get("classes")->get_bool("medic", true));
	const auto block = hosted::settings_block(copy, "jo", weapons);
	TEST_EXPECT(block.flag_return_time == 999 && block.flag_reset_time == 777 && block.replay == 0);
	TEST_EXPECT(hosted::render_host_file(copy, 1).find("Replay 0") != std::string::npos);

	auto command = hosted::console_settings_patch("SET ServerName A new name", weapons);
	TEST_EXPECT(command.get_string("game_name", "") == "A new name ");
	command = hosted::console_settings_patch("SET ServerPassword", weapons);
	TEST_EXPECT(command.get_string("game_password", "wrong").empty());
	command = hosted::console_settings_patch("SET KillLimit 500", weapons);
	TEST_EXPECT(command.get_int("kill_limit", 0) == 500);
	command = hosted::console_settings_patch("SET AutoBalanceOnRecycle 1", weapons);
	TEST_EXPECT(command.get("options")->get_bool("auto_balance", false));
	command = hosted::console_settings_patch("WEAPON SET ALL ARMORY", weapons);
	TEST_EXPECT(command.get("options")->get("weapons")->get_string("Grenade", "") == "armory");
	TEST_EXPECT(
			hosted::console_log_text("SET SideAPassword Secret") == "SET SideAPassword [redacted]");
	TEST_EXPECT(
			hosted::console_log_text("ServerPassword = Secret\nKillLimit = 10\n").find("Secret") ==
			std::string::npos);
	TEST_EXPECT(!hosted::console_reply_succeeded({ "OK - Setting Changed.", "ERROR - Failed" }));
	TEST_EXPECT(hosted::console_changes_rotation("MISSION ADD a.bms 1 0 ONESHOT"));
	TEST_EXPECT(!hosted::console_changes_rotation("MISSION CYCLE"));

	for (auto table : { gamecfg::ConfigTable::Jo, gamecfg::ConfigTable::Dfx2 }) {
		const auto schema = gamecfg::server_settings_schema(table, true);
		auto defaults = gamecfg::defaults({}, 0, 0, table);
		std::set<std::string> keys;
		for (const auto &row : schema.array) {
			const auto key = row.get_string("key", "");
			TEST_EXPECT(keys.insert(key).second);
			TEST_EXPECT(gamecfg::set_server_setting(
					defaults, *gamecfg::server_setting(key), *row.get("default"), error));
		}
	}
	TEST_EXPECT(server_completed(0, kRotationCompleted) && server_completed(0, kAdminSessionEnded));
	TEST_EXPECT(!server_completed(1, kRotationCompleted) && !server_completed(0, ""));
	return 0;
}
