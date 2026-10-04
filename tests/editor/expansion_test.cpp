// Pins a project's expansion (ADR 0046 S16): the names the game takes for one (expansion_name.h, each
// refusal the game's own: docs/vfs/vfs-pff-mount-re.md § Expansions), the expansion weighed against
// an install's, the files its name forms (expansion_files.h), and the install as the game serves it to
// such a project (install_view.h) over a synthetic install written with the PFF writer, and a session's
// expansion project over it: made, checked against the install, renamed with its files.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <base/gameprofile/required_resources.h>
#include <base/io/json.h>
#include <base/io/os_path.h>
#include <base/io/strutil.h>
#include <editor/assets/asset_registry.h>
#include <editor/assets/install_view.h>
#include <editor/import/import_plan.h>
#include <editor/model/document.h>
#include <editor/project/expansion_files.h>
#include <editor/project/expansion_name.h>
#include <editor/project/project_document.h>
#include <editor/requirements/requirements.h>
#include <editor/session/original_files.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/session/view_json.h>
#include <formats/pff/pff.h>
#include <formats/rtxt/rtxt.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

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

// Where a synthetic install's expansion keeps its text table x1.bin: in x1L.pff alone (as JO:CA's
// jox01 does), loose in its folder alone (as every expansion the editor builds), or both.
enum class TablePlace { Archive, Loose, Both };

// A synthetic install: the base's archive and loose files, an expansion x1 with its two archives, its
// music bank, a video, its version text and a player's weapon.sav, its text table where `table` says.
bool make_install(const std::string &root, TablePlace table = TablePlace::Archive) {
	const std::string x1 = root + "/expansion/x1";
	std::error_code ec;
	std::filesystem::create_directories(x1, ec);
	std::vector<std::pair<std::string, std::string>> language{ { "Mx1.bin", "x1 menu script" }, { "x1L.lwf", "x1 bank" } };
	if (table != TablePlace::Loose) language.push_back({ "x1.bin", "x1 table" });
	if (table != TablePlace::Archive && !editor_test::write_text(x1 + "/x1.bin", "x1 loose table")) return false;
	return write_archive(root + "/resource.pff", { { "shared.txt", "base" }, { "baseonly.txt", "base only" },
	                                               { "01TR.bms", "a mission" }, { "menumus.bin", "base menu script" },
	                                               { "gamemus.bin", "base game script" } }) &&
	       write_archive(x1 + "/x1.pff", { { "shared.txt", "expansion" }, { "x1only.txt", "x1 only" },
	                                       { "version.txt", "an archived version" }, { "gt.ssc", "archived config" } }) &&
	       write_archive(x1 + "/x1L.pff", language) &&
	       editor_test::write_text(x1 + "/trailer.bik", "a video the game never reads") &&
	       editor_test::write_text(x1 + "/gt.ssc", "loose config") &&
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
	// One /exp token [orig: Terrain_TokenizeConfigLine @ 0x53cb60] for the project's own; an installed
	// one the game mounts by its folder, a quoted name with a space among them, takes any.
	for (const char *name : { "my mod", "my,mod", "my;mod", "trailing " })
		TEST_EXPECT(refuses(name, "one word") && takes(name, ExpansionNameUse::BuildsOn));
	for (const char *name : { "my\tmod", "my\"mod" }) // no folder's name holds these
		TEST_EXPECT(refuses(name, "one word") && !takes(name, ExpansionNameUse::BuildsOn));
	// Printable ASCII, a folder Windows can make.
	TEST_EXPECT(refuses("caf\xc3\xa9", "printable ASCII") && refuses("a\x01", "printable ASCII"));
	TEST_EXPECT(takes("caf\xc3\xa9", ExpansionNameUse::BuildsOn));
	for (const char *name : { "a\\b", "a/b", "a:b", "a*b", "a?b", "a<b", "a>b", "a|b" })
		TEST_EXPECT(refuses(name, "no folder's name can hold"));
	TEST_EXPECT(refuses("mod.", "ends with a dot"));
	for (const char *name : { "con", "NUL", "Prn", "aux", "com1", "LPT9", "con.x" })
		TEST_EXPECT(refuses(name, "keeps for a device"));
	TEST_EXPECT(takes("com0") && takes("console") && takes("lpt10"));
	// A leading dot: the Mods list never lists the folder [orig: Expansion_ScanAndRegister @ 0x4a444b].
	TEST_EXPECT(refuses(".mod", "starts with a dot") && refuses(".", "starts with a dot"));
	TEST_EXPECT(takes(".mod", ExpansionNameUse::BuildsOn));
	// An installed one's name is a folder's: none of the characters no folder's name holds, not a folder's
	// own links.
	for (const char *name : { "a/b", "a\\b", "a:b", "a\x01", ".", ".." })
		TEST_EXPECT(!takes(name, ExpansionNameUse::BuildsOn));
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
	// An installed expansion by any name its folder can have (a quoted /exp mounts one with a space).
	TEST_EXPECT(check_project_expansion("jo", ProjectExpansion{ "jxm", "a b" }, error));
	TEST_EXPECT(!check_project_expansion("jo", ProjectExpansion{ "jxm", "a/b" }, error) &&
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
	// Nor a file the game reads by a mission's name [orig: Game_StartMission @ 0x524360]: 01TR.bin is
	// JO's 01TR.bms's text, which would become the expansion's override table (compared without case).
	const std::vector<std::string> missions{ "01TR.bms", "C1.npz", "readme.txt", "02TR.til" };
	TEST_EXPECT(expansion_name_mission_problem("01TR", missions).find("01TR.bin") != std::string::npos &&
	            expansion_name_mission_problem("01tr", missions).find("01TR.bms") != std::string::npos);
	TEST_EXPECT(!expansion_name_mission_problem("C1", missions).empty()); // a .npz lists as a mission too
	TEST_EXPECT(expansion_name_mission_problem("jxm", missions).empty() &&
	            expansion_name_mission_problem("readme", missions).empty() &&
	            expansion_name_mission_problem("02TR", missions).empty()); // a .til is no mission
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
	// An import's rows say the layer a file is served from.
	{
		ImportOrigin from;
		std::string why;
		TEST_EXPECT(from.open(ImportOrigin::Kind::GameInstall, root, document, why));
		TEST_EXPECT(from.words("shared.txt") == "the game install's expansion x1" &&
		            from.words("baseonly.txt") == "the game install's base game" && from.words() == "the game install");
		ImportOrigin plain_game;
		TEST_EXPECT(plain_game.open(ImportOrigin::Kind::GameInstall, root, ProjectDocument(), why) &&
		            plain_game.words("shared.txt") == "the game install");
	}
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
	// A video in the folder the game never reads there is not offered; an archived version text never is;
	// the configuration read loose first [orig: Mission_LoadEncryptedConfig @ 0x4cdcf4] is the folder's.
	TEST_EXPECT(!origin.find("trailer.bik") && text_of(origin, "gt.ssc") == "loose config");
	// The text table the game reads from the folder alone [orig: TextResource_LoadOverrideTable @ 0x4a49de]:
	// a loose x1.bin is listed as the project's jxm.bin, from the folder, and stands over an archived copy.
	for (const TablePlace place : { TablePlace::Loose, TablePlace::Both }) {
		const std::string other = dir.file(place == TablePlace::Loose ? "loose" : "both");
		TEST_EXPECT(make_install(other, place));
		document.expansion = { "jxm", "x1" };
		InstallView view;
		TEST_EXPECT(view.open(install_spec(other, document), error));
		const InstallFile *table = view.find("jxm.bin");
		TEST_EXPECT(table && table->member == "x1.bin" && table->layer == InstallFile::Layer::Expansion &&
		            table->loose_path.find("expansion") != std::string::npos);
		TEST_EXPECT(text_of(view, "jxm.bin") == "x1 loose table" && !view.find("x1.bin"));
		TEST_EXPECT(text_of(view, "header.bik") == "x1 header" && !view.find("version.txt"));
	}
	return 0;
}

// The game's own data's baseline (S15) validates the install as the project imports it, its files under
// the project's names (x1's table as jxm.bin), and validates it again when the project's expansion moves
// (the base game holds no jxm.bin).
static int test_fold_follows_the_view() {
	editor_test::TempProjectDir dir("opennova_editor_expansion_fold");
	const std::string install = dir.file("install");
	TEST_EXPECT(make_install(install));
	// x1's table with a section of no name: a finding the install makes of its own file
	// (strings.section_empty).
	opennova::rtxt::File table;
	table.sections.push_back({ "", 1 });
	opennova::rtxt::Entry entry;
	entry.key = "KEY";
	entry.text = "text";
	table.entries.push_back(entry);
	std::vector<uint8_t> bytes;
	std::string written;
	TEST_EXPECT(opennova::rtxt::write(table, bytes, written));
	TEST_EXPECT(write_archive(install + "/expansion/x1/x1L.pff", { { "Mx1.bin", "x1 menu script" }, { "x1L.lwf", "x1 bank" },
	                                                              { "x1.bin", std::string(bytes.begin(), bytes.end()) } }));
	const std::string root = dir.file("project");
	ProjectDocument created;
	Diagnostic error;
	TEST_EXPECT(create_project(root, "Mod", "jo", created, error, ProjectExpansion{ "jxm", "x1" }));
	OriginalFiles fold;
	const int scan = 0; // the session's scan stands in: the same one throughout
	const auto settle = [&](const std::shared_ptr<const ProjectDocument> &document) {
		fold.want(install, document, &scan);
		while (!fold.step(1 << 20)) {
		}
		const OriginalData &data = *fold.data();
		const auto found = data.findings.find(normalized_logical_name("jxm.bin"));
		return data.ready && found != data.findings.end() && !found->second.empty();
	};
	TEST_EXPECT(settle(std::make_shared<const ProjectDocument>(created)));
	const size_t validated = fold.validations();
	ProjectDocument on_base = created;
	on_base.expansion = { "jxm", "" };
	TEST_EXPECT(!settle(std::make_shared<const ProjectDocument>(on_base)) && fold.validations() == validated + 1);
	return 0;
}

namespace {

size_t count_code(const std::vector<Diagnostic> &findings, const char *code, const std::string &about = std::string()) {
	size_t count = 0;
	for (const Diagnostic &d : findings)
		if (d.code() == code && (about.empty() || d.message.find(about) != std::string::npos)) ++count;
	return count;
}

std::string file_text(const std::string &path) {
	std::ifstream in(opennova::io::os_path(path), std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

// A session's expansion project (ADR 0046 S16) over the synthetic install: a name the install has or
// an expansion it lacks refused before anything is made; one made on the base game holds its version
// text and its text table; a music file the setting leaves unread said; the name changed in the
// settings renames the project's files of the old name; the expansion it builds on checked against the
// install, and listed when the install changes under it.
static int test_session() {
	editor_test::TempProjectDir dir("opennova_editor_expansion_session");
	const std::string install = dir.file("install");
	TEST_EXPECT(make_install(install));
	Preferences chosen;
	chosen.game_install = install; // the install a new project starts with
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences(chosen);
	ProjectSession session(platform, preferences);
	const SessionView &view = session.view();
	// The install's expansions, open or not.
	TEST_EXPECT(view.project.install_expansions.size() == 1 && view.project.install_expansions[0].name == "x1");
	{
		const opennova::io::JsonValue project = view_section_to_json(view, ViewSection::Project);
		const opennova::io::JsonValue *listed = project.get("install_expansions");
		TEST_EXPECT(listed && listed->array.size() == 1 && listed->array[0].get_string("name", "") == "x1" &&
		            !project.get("expansion"));
		// The install a new project opens with (the one last chosen), whatever an open project names.
		const opennova::io::JsonValue *for_new = project.get("new_project_expansions");
		TEST_EXPECT(for_new && for_new->array.size() == 1 && for_new->array[0].get_string("name", "") == "x1");
	}
	// Refused before anything is made: a name the install has (without case, as the file system
	// compares), an expansion to build on that it lacks.
	const std::string taken = dir.file("taken");
	ActionOutcome outcome = editor_test::handle_to_end(session, request::new_expansion_project(taken, "Taken", "X1"));
	TEST_EXPECT(outcome.refused && count_code(outcome.findings, "project.expansion.name_taken") == 1 &&
	            !std::filesystem::exists(opennova::io::os_path(taken)) && !view.project.open);
	const std::string lacks = dir.file("lacks");
	outcome = editor_test::handle_to_end(session, request::new_expansion_project(lacks, "Lacks", "jxm", "x2"));
	TEST_EXPECT(outcome.refused && count_code(outcome.findings, "project.expansion.not_installed", "'x2'") == 1 &&
	            !std::filesystem::exists(opennova::io::os_path(lacks)) && !view.project.open);
	// A name whose table is a mission's text (the install's 01TR.bms reads 01TR.bin).
	const std::string clash = dir.file("clash");
	outcome = editor_test::handle_to_end(session, request::new_expansion_project(clash, "Clash", "01tr"));
	TEST_EXPECT(outcome.refused && count_code(outcome.findings, "project.field.invalid", "01TR.bms") == 1 &&
	            !std::filesystem::exists(opennova::io::os_path(clash)) && !view.project.open);
	// On the base game: its version text, its table.
	const std::string root = dir.file("own");
	outcome = editor_test::handle_to_end(session, request::new_expansion_project(root, "Own", "jxm"));
	TEST_EXPECT(!outcome.refused && view.project.open &&
	            view.project.document->expansion == (ProjectExpansion{ "jxm", "" }));
	const AssetEntry *version = view.project.scan->find("version.txt");
	const AssetEntry *table = view.project.scan->find("jxm.bin");
	TEST_EXPECT(version && table && file_text(root + "/" + version->relative_path) == "Own\r\n");
	const std::string table_path = table ? root + "/" + table->relative_path : std::string(); // the scan moves on
	// The base game's music files are no rows of an expansion's checklist: M<n>.* and G<n>.* take their
	// place [orig: Expansion_LoadAssets @ 0x4a4906..0x4a494a].
	for (const RequirementRow &row : view.project.requirements->rows)
		for (const char *unread : { "MENUMUS.SBF", "MENUMUS.BIN", "GAMEMUS.SBF", "GAMEMUS.BIN" })
			TEST_EXPECT(!opennova::strutil::iequals(row.name, unread));
	{
		const opennova::io::JsonValue project = view_section_to_json(view, ViewSection::Project);
		const opennova::io::JsonValue *expansion = project.get("expansion");
		TEST_EXPECT(expansion && expansion->get_string("name", "-") == "jxm" &&
		            expansion->get_string("builds_on", "-").empty());
	}
	// The base game's music bank and script in an expansion's project: the game reads Mjxm.sbf and
	// Mjxm.bin in their place.
	TEST_EXPECT(editor_test::write_text(root + "/MENUMUS.SBF", "music") &&
	            editor_test::write_text(root + "/menumus.bin", "SCR0....")); // a script by its bytes, as the scan types a .bin
	editor_test::handle_to_end(session, request::rescan());
	const std::vector<Diagnostic> &listed = view.project.requirements->diagnostics;
	TEST_EXPECT(count_code(listed, "expansion.file.unread") == 2 &&
	            count_code(listed, "expansion.file.unread", "Mjxm.sbf") == 1 &&
	            count_code(listed, "expansion.file.unread", "Mjxm.bin") == 1);
	// The name changed: its files of the old name follow it, the version text keeps its own.
	ProjectSettingsChange renamed;
	renamed.expansion = "jxk";
	editor_test::apply_settings(session, renamed);
	TEST_EXPECT(view.project.settings_result.failures.empty() && view.project.document->expansion.name == "jxk");
	TEST_EXPECT(view.project.scan->find("jxk.bin") && !view.project.scan->find("jxm.bin") &&
	            view.project.scan->find("version.txt") &&
	            !table_path.empty() && !std::filesystem::exists(opennova::io::os_path(table_path)));
	TEST_EXPECT(count_code(view.project.requirements->diagnostics, "expansion.file.unread", "Mjxk.sbf") == 1);
	// Renamed onto a mission's name: refused, the name as it was.
	ProjectSettingsChange onto_mission;
	onto_mission.expansion = "01TR";
	editor_test::apply_settings(session, onto_mission);
	TEST_EXPECT(count_code(view.project.settings_result.failures, "project.field.invalid", "01TR.bin") == 1 &&
	            view.project.document->expansion.name == "jxk" && view.project.scan->find("jxk.bin"));
	// All or nothing: a target the project holds already refuses the whole change, nothing renamed and
	// nothing saved; so does a file of the old name open with unsaved edits.
	const AssetEntry *renamed_table = view.project.scan->find("jxk.bin");
	TEST_EXPECT(renamed_table != nullptr);
	if (!renamed_table) return 1;
	const std::string table_file = renamed_table->relative_path;
	const std::string jxk = root + "/" + table_file;
	const std::string jxq = std::filesystem::path(jxk).parent_path().generic_string() + "/jxq.bin";
	TEST_EXPECT(editor_test::write_text(jxq, "a stale table"));
	editor_test::handle_to_end(session, request::rescan());
	ProjectSettingsChange onto_stale;
	onto_stale.expansion = "jxq";
	editor_test::apply_settings(session, onto_stale);
	TEST_EXPECT(count_code(view.project.settings_result.failures, "rename.exists") == 1 &&
	            view.project.document->expansion.name == "jxk" && std::filesystem::exists(opennova::io::os_path(jxk)) &&
	            file_text(jxq) == "a stale table");
	ProjectDocument on_disk;
	Diagnostic unread;
	TEST_EXPECT(open_project(root, on_disk, unread) && on_disk.expansion.name == "jxk");
	std::error_code gone;
	std::filesystem::remove(opennova::io::os_path(jxq), gone);
	editor_test::handle_to_end(session, request::rescan());
	editor_test::handle_to_end(session, request::open_document(table_file));
	{
		const Document *held = records_of(*session.document_for(table_file));
		Edit retitled;
		if (held)
			for (const auto &row : held->rows()) {
				if (!row || !retitled.field.empty()) continue;
				for (const FieldSchema &field : held->fields(row->kind))
					if (field.type == FieldType::Text && !field.read_only && retitled.field.empty()) {
						retitled.address = { row->id, row->kind, 0 };
						retitled.field = field.id;
					}
			}
		retitled.value = std::string("EDITED");
		TEST_EXPECT(held && !retitled.field.empty());
		editor_test::handle_to_end(session, request::edit_record(table_file, retitled));
		TEST_EXPECT(held && held->dirty());
	}
	ProjectSettingsChange while_unsaved;
	while_unsaved.expansion = "jxq";
	editor_test::apply_settings(session, while_unsaved);
	TEST_EXPECT(count_code(view.project.settings_result.failures, "rename.conflict") == 1 &&
	            view.project.document->expansion.name == "jxk" && std::filesystem::exists(opennova::io::os_path(jxk)));
	editor_test::handle_to_end(session, request::undo(table_file));
	TEST_EXPECT(!session.document_for(table_file)->dirty());
	// Clean and open, the change goes through and the document follows its file.
	ProjectSettingsChange back;
	back.expansion = "jxq";
	editor_test::apply_settings(session, back);
	TEST_EXPECT(view.project.settings_result.failures.empty() && view.project.document->expansion.name == "jxq" &&
	            view.project.scan->find("jxq.bin") && !view.project.scan->find("jxk.bin") &&
	            session.document_for(view.project.scan->find("jxq.bin")->relative_path) != nullptr);
	ProjectSettingsChange restore;
	restore.expansion = "jxk";
	editor_test::apply_settings(session, restore);
	TEST_EXPECT(view.project.settings_result.failures.empty() && view.project.scan->find("jxk.bin"));
	// To build on an expansion the install lacks: refused, the expansion as it was; on one it has.
	ProjectSettingsChange elsewhere;
	elsewhere.builds_on = "x2";
	editor_test::apply_settings(session, elsewhere);
	TEST_EXPECT(count_code(view.project.settings_result.failures, "project.expansion.not_installed") == 1 &&
	            view.project.document->expansion == (ProjectExpansion{ "jxk", "" }));
	ProjectSettingsChange on_x1;
	on_x1.builds_on = "x1";
	editor_test::apply_settings(session, on_x1);
	TEST_EXPECT(view.project.settings_result.failures.empty() &&
	            view.project.document->expansion == (ProjectExpansion{ "jxk", "x1" }));
	ProjectDocument saved;
	Diagnostic error;
	TEST_EXPECT(open_project(root, saved, error) && saved.expansion == (ProjectExpansion{ "jxk", "x1" }));
	// The project's own export copied into an install: its name listed as taken, and a change of what it
	// builds on alone not refused for it; a change of the name weighed again.
	const std::string mine = dir.file("mine");
	std::error_code made_mine;
	std::filesystem::create_directories(opennova::io::os_path(mine + "/expansion/jxk"), made_mine);
	TEST_EXPECT(make_install(mine) && write_archive(mine + "/expansion/jxk/jxk.pff", { { "exported.txt", "x" } }));
	editor_test::set_game_install(session, mine);
	TEST_EXPECT(count_code(view.project.requirements->diagnostics, "project.expansion.name_taken", "'jxk'") == 1);
	ProjectSettingsChange onto_base;
	onto_base.builds_on = "";
	editor_test::apply_settings(session, onto_base);
	TEST_EXPECT(view.project.settings_result.failures.empty() &&
	            view.project.document->expansion == (ProjectExpansion{ "jxk", "" }));
	// A change of the name's case alone is a change of name (a host compares EXP as spelled [orig:
	// String_ExactMatch @ 0x5122f0]): weighed again, and the install's folder is the same to its file system.
	ProjectSettingsChange case_only;
	case_only.expansion = "JXK";
	editor_test::apply_settings(session, case_only);
	TEST_EXPECT(count_code(view.project.settings_result.failures, "project.expansion.name_taken") == 1 &&
	            view.project.document->expansion.name == "jxk");
	ProjectSettingsChange back_on_x1;
	back_on_x1.builds_on = "x1";
	editor_test::apply_settings(session, back_on_x1);
	TEST_EXPECT(view.project.settings_result.failures.empty() &&
	            view.project.document->expansion == (ProjectExpansion{ "jxk", "x1" }));
	// An install without x1: listed, never refused.
	const std::string bare = dir.file("bare");
	std::error_code made;
	std::filesystem::create_directories(opennova::io::os_path(bare), made);
	editor_test::set_game_install(session, bare);
	TEST_EXPECT(view.project.install_expansions.empty() &&
	            count_code(view.project.requirements->diagnostics, "project.expansion.not_installed", "'x1'") == 1);
	// A standalone project: none of its rows, none of its findings.
	ProjectSettingsChange standalone;
	standalone.expansion = "";
	standalone.builds_on = "";
	editor_test::apply_settings(session, standalone);
	TEST_EXPECT(view.project.document->expansion.standalone() &&
	            count_code(view.project.requirements->diagnostics, "project.expansion.not_installed") == 0 &&
	            count_code(view.project.requirements->diagnostics, "expansion.file.unread") == 0);
	// A new project is weighed against the install it opens with (the one last chosen), not the open
	// project's: open on the bare install for this session alone, one building on x1 is still made.
	editor_test::set_game_install(session, install);
	editor_test::handle_to_end(session, request::open_project(root, true, bare));
	TEST_EXPECT(view.project.open && view.project.install_expansions.empty() &&
	            view.project.new_project_expansions.size() == 1 && view.project.new_project_expansions[0].name == "x1");
	const std::string second = dir.file("second");
	outcome = editor_test::handle_to_end(session, request::new_expansion_project(second, "Second", "jxz", "x1"));
	TEST_EXPECT(!outcome.refused && view.project.open &&
	            view.project.document->expansion == (ProjectExpansion{ "jxz", "x1" }) &&
	            count_code(view.project.requirements->diagnostics, "project.expansion.not_installed") == 0);
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
	failures += test_session();
	if (failures == 0) std::printf("editor_expansion: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
