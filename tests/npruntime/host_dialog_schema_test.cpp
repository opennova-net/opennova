// npruntime_host_dialog_schema: which server settings the retail host dialog owns
// (formats/gamecfg/server_settings.h `host_control`), held against the dialog's own read of its
// controls (runtime/inmatch/host_settings.h apply_host_dialog_control): a setting naming a
// control is one that control's read changes, and every setting a control's read changes names
// that control, so the flag is the dialog's read and no second list. The supplemental "All
// server options" editor shows exactly the settings naming none.
// [orig: HostDialog_ReadSettings @0x555940]
#include <runtime/inmatch/host_settings.h>
#include "common/test_expect.h"
#include <base/io/json.h>
#include <formats/gamecfg/game_cfg.h>
#include <formats/gamecfg/server_settings.h>
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <set>
#include <string>
#include <string_view>

using namespace opennova;
namespace gamecfg = opennova::gamecfg;

namespace {

// The settings a control's read changes from the defaults, over values that move every word the
// dialog reads: a text, the numbers 0, 1 and 7 (each attribute bit reads both ways across 0 and 1;
// ALLOW_SPECTATORS 0 and 1 move the spectator count either way), a country.
std::set<std::string> settings_moved_by(const Title &title, gamecfg::ConfigTable table,
		std::string_view control) {
	std::set<std::string> moved;
	for (const char *value : { "Abc", "0", "1", "7", "DE" }) {
		const gamecfg::GameCfg before = gamecfg::defaults({}, 0, 0, table);
		gamecfg::GameCfg after = before;
		if (!inmatch::apply_host_dialog_control(after, title, control, value)) continue;
		for (const gamecfg::ServerSetting &s : gamecfg::server_settings())
			if (io::json_write(gamecfg::server_setting_value(before, s)) !=
					io::json_write(gamecfg::server_setting_value(after, s)))
				moved.insert(s.key);
	}
	return moved;
}

int check_title(const char *code, gamecfg::ConfigTable table) {
	const Title &title = *title_by_code(code);
	const std::vector<std::string_view> &controls = inmatch::host_dialog_controls();
	std::set<std::string> owned;
	for (const std::string_view control : controls)
		for (const std::string &key : settings_moved_by(title, table, control)) {
			const gamecfg::ServerSetting *s = gamecfg::server_setting(key);
			TEST_EXPECT(s != nullptr);
			// The setting this control's read changes names this control.
			if (std::string_view(s->host_control) != control) {
				std::fprintf(stderr, "%s: %.*s moves %s, which names '%s'\n", code,
						int(control.size()), control.data(), key.c_str(), s->host_control);
				return 1;
			}
			owned.insert(key);
		}
	for (const gamecfg::ServerSetting &s : gamecfg::server_settings()) {
		if (*s.host_control == '\0') continue;
		// A named control is one the dialog reads, and its read changes the setting.
		TEST_EXPECT(std::find(controls.begin(), controls.end(), std::string_view(s.host_control)) !=
				controls.end());
		if (!owned.count(s.key)) {
			std::fprintf(stderr, "%s: %s names %s, whose read never changes it\n", code, s.key,
					s.host_control);
			return 1;
		}
	}
	// The dialog's 31 controls: 27 edit a setting, and SERVER_PUNKBUSTER, CONNECTIONSPEED,
	// ALLOW_AI and TOD_CONTINUITY store words no setting offers.
	TEST_EXPECT(controls.size() == 31 && owned.size() == 27);
	for (const char *word_only : { "SERVER_PUNKBUSTER", "CONNECTIONSPEED", "ALLOW_AI", "TOD_CONTINUITY" })
		TEST_EXPECT(settings_moved_by(title, table, word_only).empty());
	return 0;
}

// The schema carries the flag to its readers (the local editor's and the hosted one's), and the
// rows naming no control are the ones the supplemental editor shows.
int check_schema() {
	const io::JsonValue local = gamecfg::server_settings_schema(gamecfg::ConfigTable::Jo, false);
	size_t named = 0, unnamed = 0;
	for (const io::JsonValue &row : local.array) {
		const std::string key = row.get_string("key", "");
		const std::string control = row.get_string("host_control", "?");
		TEST_EXPECT(control == gamecfg::server_setting(key)->host_control);
		(control.empty() ? unnamed : named) += 1;
	}
	TEST_EXPECT(named == 27 && unnamed > 40);
	for (const io::JsonValue &row : local.array) {
		const std::string key = row.get_string("key", "");
		if (key == "game_name") TEST_EXPECT(row.get_string("host_control", "") == "GAME_NAME");
		if (key == "spectator_slots")
			TEST_EXPECT(row.get_string("host_control", "") == "ALLOW_SPECTATORS");
		if (key == "vote_percent") TEST_EXPECT(row.get_string("host_control", "?").empty());
	}
	// The hosted schema leaves the local-only rows out (SERVERTYPE's and SERVER_LANONLY's among
	// them) and keeps the flag on the rest.
	size_t hosted_named = 0;
	for (const io::JsonValue &row : gamecfg::server_settings_schema(gamecfg::ConfigTable::Jo, true).array)
		hosted_named += row.get_string("host_control", "").empty() ? 0 : 1;
	TEST_EXPECT(hosted_named == 25);
	return 0;
}

} // namespace

int main() {
	if (check_title("jo", gamecfg::ConfigTable::Jo) != 0) return 1;
	if (check_title("dfx2", gamecfg::ConfigTable::Dfx2) != 0) return 1;
	if (check_schema() != 0) return 1;
	return 0;
}
