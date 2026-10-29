/* The title registry (engine/base/title/title.h) over the installed games (games::install, linked
   by every test's opennova_test_games): its lookups, the default title, the SCR policy seam and
   each title's labels. */
#include <stdio.h>
#include <string.h>

#include <base/title/title.h>
#include <base/vfs/vfs_decode.h>
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
	CHECK(title_count() == 6, "six titles: jo, jodemo, dfx, dfx2, bhd, acidgambit");
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

/* title_by_code + the game-to-policy seam used by native runtime loaders. */
static int test_by_code_and_policy(void) {
	const Title *demo = title_by_code("jodemo");
	CHECK(demo != NULL && strcmp(demo->code, "jodemo") == 0, "by_code jodemo -> demo");
	CHECK(title_by_code("JODEMO") == demo, "by_code is case-insensitive");
	CHECK(title_by_code("nope") == NULL, "unknown code -> NULL");
	CHECK(title_by_code("") == NULL, "empty code -> NULL");
	CHECK(strcmp(default_title().code, "jo") == 0, "the default title is JO's");

	CHECK(title_scr_policy_for_code("jodemo") == VFS_SCR_FORCE_DEFAULT, "jodemo forces DEFAULT");
	CHECK(title_scr_policy_for_code("jo") == VFS_SCR_VERSION_DETECT, "jo version-detect");
	CHECK(title_scr_policy_for_code(NULL) == VFS_SCR_VERSION_DETECT,
			"NULL -> version-detect (JO default)");
	CHECK(title_scr_policy_for_code("nope") == VFS_SCR_VERSION_DETECT, "unknown -> version-detect");
	return 1;
}

/* SCR keying is per-game. Retail titles key version-1 payloads with JO_DFX2 (which the version
   byte selects), so they version-detect. The JO Demo keys the same version byte with the DEFAULT
   key, so it must force that key. */
static int test_scr_policy_per_game(void) {
	CHECK(title_by_code("jodemo")->scr_policy == VFS_SCR_FORCE_DEFAULT,
			"JO Demo forces the DEFAULT key");
	CHECK(title_by_code("jo")->scr_policy == VFS_SCR_VERSION_DETECT, "retail JO version-detect");
	CHECK(title_by_code("dfx")->scr_policy == VFS_SCR_VERSION_DETECT, "DFX version-detect");
	CHECK(title_by_code("dfx2")->scr_policy == VFS_SCR_VERSION_DETECT, "DFX2 version-detect");
	CHECK(title_by_code("bhd")->scr_policy == VFS_SCR_VERSION_DETECT, "BHD version-detect");
	CHECK(title_by_code("acidgambit")->scr_policy == VFS_SCR_VERSION_DETECT,
			"Acid Gambit version-detect");
	return 1;
}

/* The stock wizards' command files (each title's command_file, compared without case), the cfg
   table each title's program walks, and the GameId the rules still branch on. */
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
	CHECK(title_game_id(*title_by_code("dfx2")) == gameprofile::GAME_DFX2, "DFX2's rules");
	CHECK(title_game_id(*title_by_code("jo")) == gameprofile::GAME_JO, "JO's rules");
	CHECK(title_game_id(*title_by_code("acidgambit")) == gameprofile::GAME_JO,
			"Acid Gambit runs JO's rules");
	return 1;
}

int main(void) {
	RUN_TEST(test_count);
	RUN_TEST(test_labels);
	RUN_TEST(test_scr_policy_per_game);
	RUN_TEST(test_by_code_and_policy);
	RUN_TEST(test_command_files_tables_and_ids);

	printf("\n%d passed, %d failed\n", passed, failed);
	return failed > 0 ? 1 : 0;
}
