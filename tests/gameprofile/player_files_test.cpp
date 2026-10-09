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
	TEST_EXPECT(rows.size() >= 18);
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
	// A resource the game ships is none.
	TEST_EXPECT(!is_player_file("items.def") && !is_player_file("cc.bin") && !is_player_file("gametext.bin"));
	TEST_EXPECT(player_file_words("items.def").empty());
	return 0;
}

int main() {
	int failures = 0;
	failures += test_table();
	failures += test_names();
	if (failures == 0) std::printf("player_files: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
