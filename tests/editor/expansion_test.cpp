// Pins a project's expansion (ADR 0046 S16): the names the game takes for one (expansion_name.h, each
// refusal the game's own: docs/vfs/vfs-pff-mount-re.md § Expansions), the expansion weighed against
// an install's, the files its name forms (expansion_files.h), and the install as the game serves it to
// such a project (install_view.h) over a synthetic install written with the PFF writer.
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <base/gameprofile/required_resources.h>
#include <editor/assets/asset_registry.h>
#include <editor/assets/install_view.h>
#include <editor/project/expansion_files.h>
#include <editor/project/expansion_name.h>
#include <editor/project/project_document.h>
#include <editor/session/original_files.h>
#include <formats/pff/pff.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"

using namespace opennova::editor;

namespace {

bool takes(const std::string &name, ExpansionNameUse use = ExpansionNameUse::Own) {
	return expansion_name_problem(name, use).empty();
}

bool refuses(const std::string &name, const char *words, ExpansionNameUse use = ExpansionNameUse::Own) {
	const std::string problem = expansion_name_problem(name, use);
	return !problem.empty() && problem.find(words) != std::string::npos;
}

bool write_archive(const std::string &path, const std::vector<std::pair<std::string, std::string>> &files) {
	std::vector<opennova::pff::PffWriteEntry> entries;
	for (const auto &file : files)
		entries.push_back({ file.first.c_str(), reinterpret_cast<const uint8_t *>(file.second.data()),
		                    uint32_t(file.second.size()), 0, 1, 0 });
	return opennova::pff::pff_write_archive(path.c_str(), opennova::pff::PFF_FORMAT_PFF3, entries.data(),
	                                        uint32_t(entries.size())) == opennova::pff::PFF_WRITE_OK;
}

// A synthetic install: the base's archive and loose files, an expansion x1 with its two archives, its
// music bank, a video, its version text and a player's weapon.sav.
bool make_install(const std::string &root) {
	const std::string x1 = root + "/expansion/x1";
	std::error_code ec;
	std::filesystem::create_directories(x1, ec);
	return write_archive(root + "/resource.pff", { { "shared.txt", "base" }, { "baseonly.txt", "base only" },
	                                               { "menumus.bin", "base menu script" },
	                                               { "gamemus.bin", "base game script" } }) &&
	       write_archive(x1 + "/x1.pff", { { "shared.txt", "expansion" }, { "x1only.txt", "x1 only" } }) &&
	       write_archive(x1 + "/x1L.pff", { { "x1.bin", "x1 table" }, { "Mx1.bin", "x1 menu script" },
	                                        { "x1L.lwf", "x1 bank" } }) &&
	       editor_test::write_text(root + "/MENUMUS.SBF", "base music") &&
	       editor_test::write_text(root + "/header.bik", "base header") &&
	       editor_test::write_text(root + "/main.bik", "base main") && editor_test::write_text(x1 + "/Mx1.sbf", "x1 music") &&
	       editor_test::write_text(x1 + "/header.bik", "x1 header") &&
	       editor_test::write_text(x1 + "/version.txt", "x1 version") &&
	       editor_test::write_text(x1 + "/weapon.sav", "a player's");
}

std::string text_of(const InstallView &view, const std::string &name) {
	const InstallFile *file = view.find(name);
	std::vector<uint8_t> bytes;
	if (!file || !view.read(*file, bytes)) return "<none>";
	return std::string(bytes.begin(), bytes.end());
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

// The install as the game serves it (install_view.h): the base game's view; /exp x1 for a project that
// builds as jxm on it; the base game for one that builds as jxm on it; an expansion the install lacks.
static int test_install_view() {
	editor_test::TempProjectDir dir("opennova_editor_expansion_install");
	const std::string root = dir.file("install");
	TEST_EXPECT(make_install(root));
	ProjectDocument document;
	std::string error;
	// The base game: no expansion's file, the root's loose ones.
	InstallView base;
	TEST_EXPECT(base.open(base_install_spec(root, document), error));
	TEST_EXPECT(text_of(base, "shared.txt") == "base" && text_of(base, "x1only.txt") == "<none>" &&
	            text_of(base, "header.bik") == "base header" && text_of(base, "MENUMUS.SBF") == "base music" &&
	            text_of(base, "menumus.bin") == "base menu script");
	for (const InstallFile &file : base.files())
		TEST_EXPECT(file.layer == InstallFile::Layer::Base && file.name == file.member);
	// /exp x1, the project building as jxm: the expansion's copy of a shared name, its folder's video and
	// bank, the root's other video; the expansion's own names under the project's; the base's music pairs,
	// the version text and the player's file not listed.
	document.expansion = { "jxm", "x1" };
	InstallView origin;
	TEST_EXPECT(origin.open(install_spec(root, document), error));
	TEST_EXPECT(text_of(origin, "shared.txt") == "expansion" && text_of(origin, "baseonly.txt") == "base only" &&
	            text_of(origin, "x1only.txt") == "x1 only");
	TEST_EXPECT(origin.find("shared.txt")->layer == InstallFile::Layer::Expansion &&
	            origin.find("baseonly.txt")->layer == InstallFile::Layer::Base);
	TEST_EXPECT(text_of(origin, "header.bik") == "x1 header" && text_of(origin, "main.bik") == "base main");
	TEST_EXPECT(text_of(origin, "jxm.bin") == "x1 table" && text_of(origin, "Mjxm.bin") == "x1 menu script" &&
	            text_of(origin, "Mjxm.sbf") == "x1 music" && text_of(origin, "jxmL.lwf") == "x1 bank");
	TEST_EXPECT(!origin.find("x1.bin") && !origin.find("Mx1.sbf") && !origin.find("MENUMUS.SBF") &&
	            !origin.find("menumus.bin") && !origin.find("gamemus.bin") && !origin.find("version.txt") &&
	            !origin.find("weapon.sav"));
	const InstallFile *bank = origin.find("Mjxm.sbf");
	TEST_EXPECT(bank && bank->member == "Mx1.sbf" && !bank->loose_path.empty() &&
	            bank->layer == InstallFile::Layer::Expansion);
	const ImportChoice choice = install_choice(root, *bank);
	TEST_EXPECT(choice.install && choice.entry == "Mx1.sbf" && choice.as == "Mjxm.sbf" && choice.name() == "Mjxm.sbf");
	const ImportChoice plain = install_choice(root, *origin.find("shared.txt"));
	TEST_EXPECT(plain.as.empty() && plain.name() == "shared.txt");
	// On the base game, building as jxm: its music pairs under the project's names.
	document.expansion = { "jxm", "" };
	InstallView own;
	TEST_EXPECT(own.open(install_spec(root, document), error));
	TEST_EXPECT(text_of(own, "Mjxm.sbf") == "base music" && text_of(own, "Mjxm.bin") == "base menu script" &&
	            text_of(own, "Gjxm.bin") == "base game script" && !own.find("menumus.bin") &&
	            !own.find("MENUMUS.SBF") && text_of(own, "shared.txt") == "base");
	// An expansion the install lacks is refused, never the base game.
	document.expansion = { "jxm", "x2" };
	InstallView missing;
	TEST_EXPECT(!missing.open(install_spec(root, document), error) && error.find("'x2'") != std::string::npos &&
	            !missing.is_open() && missing.files().empty());
	return 0;
}

// The fold of the game's own data (S15) compares a project's file with the install's copy as the
// project imports it, under the project's name, and forgets what it found when the project's expansion
// moves.
static int test_fold_follows_the_view() {
	editor_test::TempProjectDir dir("opennova_editor_expansion_fold");
	const std::string install = dir.file("install");
	TEST_EXPECT(make_install(install));
	const std::string root = dir.file("project");
	ProjectDocument created;
	Diagnostic error;
	TEST_EXPECT(create_project(root, "Mod", "jo", created, error, ProjectExpansion{ "jxm", "x1" }));
	TEST_EXPECT(editor_test::write_text(root + "/jxm.bin", "x1 table"));
	const ProjectPaths paths = ProjectPaths::for_root(root);
	const AssetScan scan = scan_project_assets(paths, created);
	const std::vector<Diagnostic> findings{ make_finding(CoreFinding::ReferenceMissing, DiagnosticSeverity::Warning,
	                                                     "about jxm.bin", "jxm.bin") };
	OriginalFiles fold;
	const auto settle = [&](const std::shared_ptr<const ProjectDocument> &document) {
		fold.want(install, document, root, scan, findings);
		while (!fold.step(1 << 20)) {
		}
		return fold.files()->count("jxm.bin") == 1;
	};
	TEST_EXPECT(settle(std::make_shared<const ProjectDocument>(created)));
	// The base game holds no jxm.bin of its own: what it knew is forgotten and the file is the modder's.
	ProjectDocument on_base = created;
	on_base.expansion = { "jxm", "" };
	TEST_EXPECT(!settle(std::make_shared<const ProjectDocument>(on_base)));
	return 0;
}

int main() {
	int failures = 0;
	failures += test_name_rule();
	failures += test_project_expansion();
	failures += test_install_findings();
	failures += test_files_table();
	failures += test_name_forms_no_game_file();
	failures += test_install_view();
	failures += test_fold_follows_the_view();
	if (failures == 0) std::printf("editor_expansion: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
