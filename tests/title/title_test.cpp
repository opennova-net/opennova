/* The title registry (engine/base/title/title.h) over the installed games (games::install, linked
   by every test's opennova_test_games): its lookups, the default title and each title's labels. */
#include <stdio.h>
#include <string.h>

#include <base/title/title.h>
#include <formats/gamecfg/game_cfg.h>
#include <formats/pff/pff.h>

using namespace opennova;

static int passed = 0;
static int failed = 0;

#define RUN_TEST(fn)                                                                               \
	do {                                                                                           \
		printf("Running %s... ", #fn);                                                             \
		if (fn()) {                                                                                \
			printf("PASS\n");                                                                      \
			++passed;                                                                              \
		} else {                                                                                   \
			printf("FAIL\n");                                                                      \
			++failed;                                                                              \
		}                                                                                          \
	} while (0)

#define CHECK(cond, msg)                                                                           \
	do {                                                                                           \
		if (!(cond)) {                                                                             \
			fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__);                              \
			return 0;                                                                              \
		}                                                                                          \
	} while (0)

static int test_count(void) {
	CHECK(title_count() == 5, "five titles: jo, dfx, dfx2, bhd, acidgambit");
	CHECK(title_at(title_count()) == NULL, "past-end index -> NULL");
	CHECK(title_at(0) != NULL && strcmp(title_at(0)->code, "jo") == 0, "index 0 is JO");
	return 1;
}

static int test_labels(void) {
	size_t i;
	for (i = 0; i < title_count(); ++i) {
		const Title *t = title_at(i);
		CHECK(t != NULL, "title not null");
		CHECK(t->display_name != NULL && t->display_name[0] != '\0', "display name non-empty");
		CHECK(t->code != NULL && t->code[0] != '\0', "code non-empty");
		size_t j;
		for (j = i + 1; j < title_count(); ++j)
			CHECK(strcmp(t->code, title_at(j)->code) != 0, "codes are unique");
	}
	CHECK(strcmp(title_by_code("jo")->display_name, "Joint Operations") == 0,
			"JO's shipping label");
	CHECK(strcmp(title_by_code("dfx2")->display_name, "Delta Force Xtreme 2") == 0, "DFX2's label");
	CHECK(strcmp(title_by_code("acidgambit")->display_name, "OP: Acid Gambit") == 0,
			"Acid Gambit's label");
	/* Each install's program, as it ships beside the archives; BHD's unread, OP: Acid Gambit none.
	 */
	CHECK(strcmp(title_by_code("jo")->executable, "Jointops.exe") == 0, "JO's program");
	CHECK(strcmp(title_by_code("dfx2")->executable, "dfx2.exe") == 0, "DFX2's program");
	CHECK(title_by_code("bhd")->executable == NULL, "BHD's program unread");
	CHECK(title_by_code("acidgambit")->executable == NULL, "Acid Gambit has no program of its own");
	/* A new archive of any title is PFF3 (ADR 0008's default). */
	CHECK(kNewArchivePffFormat == pff::PFF_FORMAT_PFF3, "a new archive is PFF3");
	return 1;
}

/* title_by_code and the default title. */
static int test_by_code(void) {
	const Title *jo = title_by_code("jo");
	CHECK(jo != NULL && strcmp(jo->code, "jo") == 0, "by_code jo -> JO");
	CHECK(title_by_code("JO") == jo, "by_code is case-insensitive");
	CHECK(title_by_code("nope") == NULL, "unknown code -> NULL");
	CHECK(title_by_code("") == NULL, "empty code -> NULL");
	CHECK(strcmp(default_title().code, "acidgambit") == 0,
			"the default title is OpenNova's own game's");
	return 1;
}

/* The stock wizards' command files (each title's command_file, compared without case), the cfg
   table each title's program walks, and its player caps (DFX2-8: the most a session takes, the host
   screen's cap; its rules are games_test's). */
static int test_command_files_tables_and_ids(void) {
	CHECK(title_by_command_file("JOPTR.CMD") == title_by_code("jo"),
			"JO:CA's wizard names JOPTR.CMD");
	CHECK(title_by_command_file("dfx2.cmd") == title_by_code("dfx2"),
			"the Steam DFX2's names DFX2.CMD");
	CHECK(title_by_command_file("ACIDGAMBIT.CMD") == title_by_code("acidgambit"),
			"Acid Gambit's own");
	CHECK(title_by_command_file("DFBHD.CMD") == NULL && title_by_command_file("") == NULL,
			"no other");
	CHECK(title_by_code("jo")->config_table == gamecfg::ConfigTable::Jo,
			"Jointops.exe's cfg table");
	CHECK(title_by_code("dfx2")->config_table == gamecfg::ConfigTable::Dfx2,
			"dfx2.exe's cfg table");
	CHECK(title_by_code("acidgambit")->config_table == gamecfg::ConfigTable::Jo,
			"Acid Gambit runs JO's");
	CHECK(title_by_code("jo")->max_players == 64 && title_by_code("jo")->max_players_host_screen == 64,
			"Jointops.exe's caps");
	CHECK(title_by_code("dfx2")->max_players == 250 &&
					title_by_code("dfx2")->max_players_host_screen == 100,
			"dfx2.exe's caps");
	CHECK(title_by_code("acidgambit")->max_players == 64 &&
					title_by_code("acidgambit")->max_players_host_screen == 64,
			"Acid Gambit takes JO's caps");
	return 1;
}

int main(void) {
	RUN_TEST(test_count);
	RUN_TEST(test_labels);
	RUN_TEST(test_by_code);
	RUN_TEST(test_command_files_tables_and_ids);

	printf("\n%d passed, %d failed\n", passed, failed);
	return failed > 0 ? 1 : 0;
}
