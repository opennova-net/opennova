// Pins a project's expansion (ADR 0046 S16): the names the game takes for one (expansion_name.h, each
// refusal the game's own: docs/vfs/vfs-pff-mount-re.md § Expansions), the expansion weighed against
// an install's, and the files its name forms (expansion_files.h).
#include <cstdio>
#include <string>
#include <vector>

#include <base/gameprofile/required_resources.h>
#include <editor/project/expansion_files.h>
#include <editor/project/expansion_name.h>

#include "common/test_expect.h"

using namespace opennova::editor;

namespace {

bool takes(const std::string &name, ExpansionNameUse use = ExpansionNameUse::Own) {
	return expansion_name_problem(name, use).empty();
}

bool refuses(const std::string &name, const char *words, ExpansionNameUse use = ExpansionNameUse::Own) {
	const std::string problem = expansion_name_problem(name, use);
	return !problem.empty() && problem.find(words) != std::string::npos;
}

} // namespace

static int test_name_rule() {
	TEST_EXPECT(takes("jxm") && takes("jox01") && takes("x") && takes("a.b") && takes("Mod-2_x"));
	TEST_EXPECT(refuses("", "needs a name"));
	// 31 characters the game mounts [orig: strncpy(g_ExpansionName, token, 32) @ 0x4a76ca]; the
	// project's own takes 11, M<n>.bin and <n>L.lwf fitting the archives' 16-byte names.
	const std::string eleven(11, 'a'), twelve(12, 'a'), thirty_one(31, 'a'), thirty_two(32, 'a');
	TEST_EXPECT(takes(eleven) && refuses(twelve, "holds 11") && refuses(twelve, "Maaaaaaaaaaaa.bin"));
	TEST_EXPECT(takes(twelve, ExpansionNameUse::BuildsOn) && takes(thirty_one, ExpansionNameUse::BuildsOn));
	TEST_EXPECT(refuses(thirty_two, "holds an expansion's name in 31", ExpansionNameUse::BuildsOn));
	TEST_EXPECT(refuses(thirty_two, "holds an expansion's name in 31"));
	// One /exp token [orig: Terrain_TokenizeConfigLine @ 0x53cb60].
	for (const char *name : { "my mod", "my\tmod", "my,mod", "my\"mod", "my;mod", "trailing " })
		TEST_EXPECT(refuses(name, "one word") && refuses(name, "one word", ExpansionNameUse::BuildsOn));
	// Printable ASCII, a folder Windows can make.
	TEST_EXPECT(refuses("caf\xc3\xa9", "printable ASCII") && refuses("a\x01", "printable ASCII"));
	for (const char *name : { "a\\b", "a/b", "a:b", "a*b", "a?b", "a<b", "a>b", "a|b" })
		TEST_EXPECT(refuses(name, "no folder's name can hold"));
	TEST_EXPECT(refuses("mod.", "ends with a dot"));
	for (const char *name : { "con", "NUL", "Prn", "aux", "com1", "LPT9", "con.x" })
		TEST_EXPECT(refuses(name, "keeps for a device", ExpansionNameUse::BuildsOn));
	TEST_EXPECT(takes("com0") && takes("console") && takes("lpt10", ExpansionNameUse::BuildsOn));
	Diagnostic error;
	TEST_EXPECT(check_expansion_name("jxm", ExpansionNameUse::Own, error));
	TEST_EXPECT(!check_expansion_name("my mod", ExpansionNameUse::Own, error) && error.code() == "project.field.invalid" &&
	            error.severity == DiagnosticSeverity::Error);
	return 0;
}

static int test_project_expansion() {
	Diagnostic error;
	TEST_EXPECT(check_project_expansion("jo", ProjectExpansion(), error));
	TEST_EXPECT(check_project_expansion("dfx", ProjectExpansion(), error)); // standalone: any game
	TEST_EXPECT(check_project_expansion("jo", ProjectExpansion{ "jxm", "" }, error));
	TEST_EXPECT(check_project_expansion("JO", ProjectExpansion{ "jxm", "jox01" }, error));
	TEST_EXPECT(!check_project_expansion("dfx2", ProjectExpansion{ "jxm", "" }, error) &&
	            error.code() == "project.expansion.unsupported");
	TEST_EXPECT(!check_project_expansion("jo", ProjectExpansion{ "", "jox01" }, error) &&
	            error.code() == "project.field.invalid" && error.message.find("/exp") != std::string::npos);
	TEST_EXPECT(!check_project_expansion("jo", ProjectExpansion{ "jxm", "a b" }, error) &&
	            error.code() == "project.field.invalid");
	return 0;
}

static int test_install_findings() {
	const std::vector<std::string> installed{ "jox01", "Other" };
	std::vector<Diagnostic> out;
	expansion_install_findings(ProjectExpansion(), installed, DiagnosticSeverity::Error, out);
	TEST_EXPECT(out.empty());
	expansion_install_findings(ProjectExpansion{ "jxm", "jox01" }, installed, DiagnosticSeverity::Error, out);
	TEST_EXPECT(out.empty());
	// Compared as the file system does, case-insensitively.
	expansion_install_findings(ProjectExpansion{ "JOX01", "" }, installed, DiagnosticSeverity::Warning, out);
	TEST_EXPECT(out.size() == 1 && out[0].code() == "project.expansion.name_taken" &&
	            out[0].severity == DiagnosticSeverity::Warning && !out[0].row()->gates_build);
	out.clear();
	expansion_install_findings(ProjectExpansion{ "jxm", "jox02" }, installed, DiagnosticSeverity::Error, out);
	TEST_EXPECT(out.size() == 1 && out[0].code() == "project.expansion.not_installed" &&
	            out[0].severity == DiagnosticSeverity::Error && !out[0].row()->gates_build &&
	            out[0].message.find("'jox02'") != std::string::npos);
	out.clear();
	expansion_install_findings(ProjectExpansion{ "other", "jox02" }, {}, DiagnosticSeverity::Info, out);
	TEST_EXPECT(out.size() == 1 && out[0].code() == "project.expansion.not_installed");
	return 0;
}

// The files an expansion's name forms [orig: Expansion_LoadAssets @ 0x4a4730]: one row per role, each
// with its manifest row, named as the game names it; the music pairs by the runtime's own naming.
static int test_files_table() {
	const std::vector<ExpansionFile> files = expansion_files("jxm");
	std::vector<std::string> names;
	for (const ExpansionFile &file : files) names.push_back(file.name);
	TEST_EXPECT((names == std::vector<std::string>{ "jxm.bin", "version.txt", "Mjxm.sbf", "Mjxm.bin", "Gjxm.sbf",
	                                                  "Gjxm.bin", "jxmL.lwf", "jxm.lwf" }));
	TEST_EXPECT(expansion_files("").empty());
	for (const ExpansionFile &file : files) {
		const opennova::gameprofile::RequiredResource *row =
				opennova::gameprofile::gameprofile_required_resource_by_role(file.row->manifest_role);
		TEST_EXPECT(row && (row->flags & opennova::gameprofile::RES_F_EXPANSION));
		TEST_EXPECT(expansion_file_row_for_manifest_role(file.row->manifest_role) == file.row);
		TEST_EXPECT(file.row->orig && *file.row->orig && file.row->what && *file.row->what);
	}
	// Loose in the expansion's folder: the table, the version text, the music banks; the rest by kind.
	const auto placed = [](ExpansionFileRole role) { return expansion_file_row(role).placement; };
	TEST_EXPECT(placed(ExpansionFileRole::Table) == ExpansionPlacement::Folder &&
	            placed(ExpansionFileRole::Version) == ExpansionPlacement::Folder &&
	            placed(ExpansionFileRole::MenuMusicBank) == ExpansionPlacement::Folder &&
	            placed(ExpansionFileRole::GameMusicBank) == ExpansionPlacement::Folder &&
	            placed(ExpansionFileRole::MenuMusicScript) == ExpansionPlacement::ByKind &&
	            placed(ExpansionFileRole::LocalBank) == ExpansionPlacement::ByKind);
	TEST_EXPECT(std::string(expansion_file_row(ExpansionFileRole::MenuMusicBank).replaces) == "MENUMUS.SBF" &&
	            !expansion_file_row(ExpansionFileRole::Table).replaces);
	// Looked up without case, as the game compares names.
	TEST_EXPECT(expansion_file_for("jxm", "MJXM.SBF") == &expansion_file_row(ExpansionFileRole::MenuMusicBank));
	TEST_EXPECT(expansion_file_for("jxm", "VERSION.TXT") == &expansion_file_row(ExpansionFileRole::Version));
	TEST_EXPECT(!expansion_file_for("jxm", "jox01.bin") && !expansion_file_for("", "version.txt"));
	return 0;
}

// A name whose files would be names the game reads as files of its own (the editor holds one file of a
// name): game.bin, the menu's table; MENUMUS.SBF, the base game's menu music.
static int test_name_forms_no_game_file() {
	TEST_EXPECT(refuses("game", "game.bin") && refuses("GAME", "GAME.bin") && refuses("ENUMUS", "MENUMUS.sbf"));
	TEST_EXPECT(refuses("gametext", "gametext.bin"));
	TEST_EXPECT(takes("game", ExpansionNameUse::BuildsOn)); // an installed one forms no file of the project's
	return 0;
}

int main() {
	int failures = 0;
	failures += test_name_rule();
	failures += test_project_expansion();
	failures += test_install_findings();
	failures += test_files_table();
	failures += test_name_forms_no_game_file();
	if (failures == 0) std::printf("editor_expansion: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
