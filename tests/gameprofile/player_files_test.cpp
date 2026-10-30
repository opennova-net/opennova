// The player's and this machine's own files (base/gameprofile/player_files.h): the manifest's
// RES_F_PLAYER_FILE rows, then the files the game writes as it runs, each with its words and its
// witness; a save and a screenshot by their names' forms.
#include <cstdio>
#include <string>

#include <base/gameprofile/player_files.h>
#include <base/gameprofile/required_resources.h>

#include "common/test_expect.h"

using namespace opennova::gameprofile;

static int test_table() {
	const std::vector<PlayerFile> &rows = player_files();
	TEST_EXPECT(rows.size() >= 17);
	int manifest = 0;
	for (int i = 0; i < gameprofile_required_resource_count(); ++i)
		manifest += (gameprofile_required_resource_at(i)->flags & RES_F_PLAYER_FILE) != 0;
	TEST_EXPECT(manifest > 0 && rows.size() > static_cast<size_t>(manifest));
	// The manifest's rows first, in its order, each by its own name and witness.
	TEST_EXPECT(rows.front().name == "game.cfg" && rows.front().what == "this machine's configuration of the game");
	for (const PlayerFile &row : rows) {
		TEST_EXPECT(!row.name.empty() && !row.what.empty() && row.orig.find("[orig: ") == 0);
		TEST_EXPECT(is_player_file(row.name) && player_file_words(row.name) == row.what);
	}
	TEST_EXPECT(player_file_words("player.sav") == "the player's saved profiles");
	TEST_EXPECT(player_file_words("epass.bin") == "the player's stored NovaWorld credentials");
	TEST_EXPECT(player_file_words("_errlog.txt") == "the game's error log");
	// The NovaWorld cookie jar: the game rewrites it with the player's cookies as the menu closes, so a
	// release never ships it [orig: CUIScene_SaveCookieJarFile @ 0x63BA60, "wb" @ 0x63BA9A].
	TEST_EXPECT(is_player_file("nw_cdata.coo") && is_player_file("NW_CDATA.COO"));
	TEST_EXPECT(player_file_words("nw_cdata.coo") == "the player's NovaWorld cookies");
	return 0;
}

static int test_names() {
	// Of any folder, in any case, its name's trailing spaces aside (the archives' names compare so).
	TEST_EXPECT(is_player_file("PLAYER.SAV") && is_player_file("dir/SS12345.bmp") && is_player_file("dir\\_ERRLOG.TXT"));
	TEST_EXPECT(is_player_file("sysdump.txt ") && is_player_file("cache/mru.txt"));
	// Any save, and a screenshot: "SS", five digits, a dot, any extension.
	TEST_EXPECT(is_player_file("extra.sav") && player_file_words("extra.sav") == "the player's saved profiles");
	TEST_EXPECT(is_player_file("SS00001.tga") && player_file_words("ss99999.jpg") == "a screenshot the game took");
	TEST_EXPECT(!is_player_file("SS1234.tga") && !is_player_file("SS123456.tga") && !is_player_file("SS1234a.tga"));
	// A resource the game ships is none, the gate-tag override among them (a game's own data, which the game
	// only reads [orig: Mission_LoadEncryptedConfig @0x4cdcd0]).
	TEST_EXPECT(!is_player_file("items.def") && !is_player_file("cc.bin") && !is_player_file("gametext.bin"));
	TEST_EXPECT(!is_player_file("gt.ssc") && !is_player_file("expansion/x1/GT.SSC"));
	TEST_EXPECT(player_file_words("items.def").empty() && player_file_words("gt.ssc").empty());
	return 0;
}

// The Screen Shot action's file: the first SS%0.5d of 0..99998 the directory lacks, .jpg (the
// device's JPEG), .tga under /sstga; none once every index is taken.
// [orig: Screenshot_CaptureToFile @0x5221A0 - "jpg" @0x7D0224, sprintf("SS%0.5d.%s") @0x5222B2,
//  File_CheckExists @0x5222BC, the stop at 99999 @0x5222D1]
static int test_screenshot_file_name() {
	std::vector<std::string> asked;
	const auto none = [&](const std::string &name) {
		asked.push_back(name);
		return false;
	};
	TEST_EXPECT(screenshot_file_name(none, false) == "SS00000.jpg" && asked.size() == 1);
	TEST_EXPECT(screenshot_file_name(none, true) == "SS00000.tga");
	const auto first_three = [](const std::string &name) {
		return name == "SS00000.jpg" || name == "SS00001.jpg" || name == "SS00002.jpg";
	};
	TEST_EXPECT(screenshot_file_name(first_three, false) == "SS00003.jpg");
	// A gap is filled first: the search starts at 0 every time.
	const auto all_but_one = [](const std::string &name) { return name != "SS00001.jpg"; };
	TEST_EXPECT(screenshot_file_name(all_but_one, false) == "SS00001.jpg");
	const auto all = [](const std::string &) { return true; };
	TEST_EXPECT(screenshot_file_name(all, false).empty());
	const auto up_to_last = [](const std::string &name) { return name != "SS99998.jpg"; };
	TEST_EXPECT(screenshot_file_name(up_to_last, false) == "SS99998.jpg");
	TEST_EXPECT(is_player_file(screenshot_file_name(none, false)));
	return 0;
}

int main() {
	int failures = 0;
	failures += test_table();
	failures += test_names();
	failures += test_screenshot_file_name();
	if (failures == 0) std::printf("player_files: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
