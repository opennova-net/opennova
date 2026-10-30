#include "hosted/model/console_configuration.h"
#include "server_completion.h"
#include <base/io/json.h>
#include <formats/gamecfg/game_cfg.h>
#include <runtime/inmatch/host_config.h>
#include <runtime/inmatch/host_settings.h>
#include "common/test_expect.h"
#include <set>
#include <string>
#include <vector>

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
	const gamecfg::ServerSettingsTitle jo = opennova::inmatch::server_settings_title("jo");
	const gamecfg::ServerSettingsTitle dfx2 = opennova::inmatch::server_settings_title("dfx2");
	TEST_EXPECT(jo.max_players == 64 && jo.armory_word == 2 && jo.mission_word == 3);
	TEST_EXPECT(dfx2.max_players == 100 && dfx2.armory_word == 3 && dfx2.mission_word == 2);
	auto cfg = gamecfg::defaults();
	TEST_EXPECT(gamecfg::apply_server_settings(cfg, patch, true, weapons, jo, field, error));
	TEST_EXPECT(
			cfg.replay == 0 && cfg.mp_num_spectators_max == -1 && cfg.mpvoting_percent == 0.625f);
	TEST_EXPECT(cfg.class_availability[6] == 0 && cfg.weapon_availability[1] == 2);
	auto dfx = gamecfg::defaults({}, 0, 0, gamecfg::ConfigTable::Dfx2);
	TEST_EXPECT(gamecfg::apply_server_settings(dfx, patch, true, weapons, dfx2, field, error));
	TEST_EXPECT(dfx.weapon_availability[1] == 3);
	const auto previous = cfg.servermsg;
	io::JsonValue bad;
	TEST_EXPECT(io::json_parse(R"({"server_message":"bad","remote_admin_port":1234})", bad, error));
	TEST_EXPECT(!gamecfg::apply_server_settings(cfg, bad, true, weapons, jo, field, error));
	TEST_EXPECT(cfg.servermsg == previous); // validation is atomic
	TEST_EXPECT(io::json_parse(R"({"max_players":65})", bad, error));
	TEST_EXPECT(!gamecfg::apply_server_settings(cfg, bad, true, weapons, jo, field, error));
	TEST_EXPECT(gamecfg::apply_server_settings(dfx, bad, true, weapons, dfx2, field, error));

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
	// A settings edit sends only what it changes from the saved configuration (a weapon left at
	// its saved availability keeps its word, the mission word among them), retail's WEAPON SET
	// keywords, one WEAPON SET ALL when every selectable weapon ends at one of them and ALL's
	// rows (the admin table's word) are the roster's, and a weapon set to "mission", which no
	// keyword reaches, at the next launch.
	const auto edit_of = [&](const char *text, const hosted::HostedSettings &saved,
								 const gamecfg::WeaponRoster &roster) -> hosted::ConfigurationEdit {
		io::JsonValue edit_patch;
		if (!io::json_parse(text, edit_patch, error))
			return { io::JsonValue::make_object(), { "unparsed" } };
		return hosted::configuration_edit(edit_patch, saved, "jo", roster);
	};
	// The admin table's word known, the roster's selectable rows and no other.
	const gamecfg::WeaponRoster known = { { "None", 0, "", 0 }, { "Rifle", 1, "", 1 },
		{ "Grenade", 1, "", 1 } };
	// A subclass row the admin table marks too (weapon.def's loadout_selectable on it), which the
	// roster leaves out: ALL would set it as well.
	gamecfg::WeaponRoster subclassed = known;
	subclassed.push_back({ "Rifle_sub", 0, "", 1 });
	hosted::HostedSettings saved;
	auto edit = edit_of(R"({"options":{"weapons":{"Rifle":"mission","Grenade":"never"}}})", saved, known);
	TEST_EXPECT(edit.commands == std::vector<std::string>{ "WEAPON SET 2 NEVER" } &&
			!edit.next_launch.get("options"));
	const char *both_always = R"({"options":{"weapons":{"Rifle":"always","Grenade":"always"}}})";
	edit = edit_of(both_always, saved, known);
	TEST_EXPECT(edit.commands == std::vector<std::string>{ "WEAPON SET ALL ALWAYS" });
	// ALL's rows differ from the roster's (a subclass row), or the admin word is not known (a
	// roster stored before it): each row its own command.
	const std::vector<std::string> each = { "WEAPON SET 1 ALWAYS", "WEAPON SET 2 ALWAYS" };
	TEST_EXPECT(edit_of(both_always, saved, subclassed).commands == each);
	TEST_EXPECT(edit_of(both_always, saved, weapons).commands == each);
	// A tree's stored roster keeps the admin word; one stored before it reads it as not known.
	gamecfg::WeaponRoster decoded;
	TEST_EXPECT(hosted::decode_tree_weapons(hosted::encode_tree_weapons(subclassed), decoded) &&
			decoded.size() == 4 && decoded[3].loadout_selectable == 0 &&
			decoded[3].admin_loadout_selectable == 1 && decoded[1].admin_loadout_selectable == 1);
	TEST_EXPECT(hosted::decode_tree_weapons(R"([{"name":"Rifle","selectable":true}])", decoded) &&
			decoded.size() == 1 && decoded[0].admin_loadout_selectable == -1);
	edit = edit_of(R"({"kill_limit":5,"options":{"weapons":{"Rifle":"armory"}}})", saved, known);
	TEST_EXPECT(edit.commands == (std::vector<std::string>{ "SET KillLimit 5", "WEAPON SET 1 ARMORY" }));
	// The SETs go in the schema's order whatever order the patch names them in (the queue keeps
	// the patch as jsonb, which reorders its keys), the weapons after them.
	edit = edit_of(R"({"options":{"weapons":{"Rifle":"armory"}},"max_score":12,"kill_limit":61,"time_limit":25})",
			saved, known);
	TEST_EXPECT(edit.commands == (std::vector<std::string>{ "SET GameTime 25", "SET KillLimit 61",
										 "SET MaxScore 12", "WEAPON SET 1 ARMORY" }));
	// A name SET ServerName carries as typed goes out live; one its rebuild would cut (a token
	// past the 32-character test, a name past 29, a run of blanks) waits for the next launch,
	// whose host file keeps it whole [orig: CAdminServer_HandleSetCommand @0x405A60 - @0x4060FF].
	edit = edit_of(R"({"game_name":"A new name"})", saved, known);
	TEST_EXPECT(edit.commands == std::vector<std::string>{ "SET ServerName A new name" } &&
			!edit.next_launch.get("game_name"));
	for (const char *uncarried : { R"({"game_name":"OpenNova Dedicated Server East1"})",
				 R"({"game_name":"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123"})", R"({"game_name":"Two  blanks"})" }) {
		io::JsonValue named;
		TEST_EXPECT(io::json_parse(uncarried, named, error));
		edit = edit_of(uncarried, saved, known);
		TEST_EXPECT(edit.commands.empty() &&
				edit.next_launch.get_string("game_name", "") == named.get_string("game_name", "?"));
	}
	TEST_EXPECT(io::json_parse(R"({"weapons":{"Rifle":"never"}})", saved.options, error));
	edit = edit_of(R"({"options":{"weapons":{"Rifle":"mission","Grenade":"mission"}}})", saved, known);
	TEST_EXPECT(edit.commands.empty() &&
			edit.next_launch.get("options")->get("weapons")->get_string("Rifle", "") == "mission" &&
			!edit.next_launch.get("options")->get("weapons")->get("Grenade"));
	command = hosted::console_settings_patch("WEAPON SET 1 MISSION", weapons);
	TEST_EXPECT(command.get("options")->get("weapons")->get_string("Rifle", "") == "always");
	TEST_EXPECT(!hosted::console_reply_succeeded({ "OK - Setting Changed.", "ERROR - Failed" }));
	TEST_EXPECT(hosted::console_changes_rotation("MISSION ADD a.bms 1 0 ONESHOT"));
	TEST_EXPECT(!hosted::console_changes_rotation("MISSION CYCLE"));

	for (const gamecfg::ServerSettingsTitle &title : { jo, dfx2 }) {
		const auto schema = gamecfg::server_settings_schema(title, true);
		auto defaults = gamecfg::defaults({}, 0, 0, title.table);
		std::set<std::string> keys;
		for (const auto &row : schema.array) {
			const auto key = row.get_string("key", "");
			TEST_EXPECT(keys.insert(key).second);
			TEST_EXPECT(gamecfg::set_server_setting(
					defaults, *gamecfg::server_setting(key), *row.get("default"), title, error));
			// A dependency names a setting the same schema offers.
			const auto parent = row.get_string("depends_on", "");
			TEST_EXPECT(parent.empty() || gamecfg::server_setting(parent) != nullptr);
		}
		// The hosted channel never reads lanmode; nothing reads wind (no reader, no schema row).
		TEST_EXPECT(
				!keys.count("lan_mode") && !keys.count("wind") && keys.count("max_packet_size"));
		TEST_EXPECT(
				gamecfg::server_settings_schema(title, false).array.size() > schema.array.size());
	}
	io::JsonValue unread;
	TEST_EXPECT(io::json_parse(R"({"wind":3})", unread, error));
	TEST_EXPECT(!gamecfg::apply_server_settings(cfg, unread, false, weapons, jo, field, error));
	TEST_EXPECT(io::json_parse(R"({"lan_mode":2})", unread, error));
	TEST_EXPECT(!gamecfg::apply_server_settings(cfg, unread, true, weapons, jo, field, error));
	TEST_EXPECT(gamecfg::apply_server_settings(cfg, unread, false, weapons, jo, field, error));
	for (const auto &row : gamecfg::server_settings_schema(jo, true).array)
		if (row.get_string("key", "") == "side_a_password")
			TEST_EXPECT(row.get_string("depends_on", "") == "team_choose");
	TEST_EXPECT(server_completed(0, kRotationCompleted) && server_completed(0, kAdminSessionEnded));
	TEST_EXPECT(!server_completed(1, kRotationCompleted) && !server_completed(0, ""));
	return 0;
}
