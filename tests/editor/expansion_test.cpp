// Pins a project's expansion (ADR 0046 S16): the names the game takes for one (expansion_name.h, each
// refusal the game's own: docs/vfs/vfs-pff-mount-re.md § Expansions), the expansion weighed against
// an install's, the files its name forms (expansion_files.h), and the install as the game serves it to
// such a project (install_view.h) over a synthetic install written with the PFF writer, and a session's
// expansion project over it: made, checked against the install, renamed with its files.
#include <algorithm>
#include <chrono>
#include <cstdint>
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
#include <editor/assets/asset_import.h>
#include <editor/assets/asset_registry.h>
#include <editor/assets/install_view.h>
#include <editor/assets/project_asset_source.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/code_text_keys.h>
#include <editor/import/import_plan.h>
#include <editor/model/document.h>
#include <editor/project/base_project.h>
#include <editor/project/expansion_files.h>
#include <editor/project/expansion_name.h>
#include <editor/project/project_document.h>
#include <editor/project_build/export_build.h>
#include <editor/requirements/requirements.h>
#include <editor/session/original_files.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/base_layer_build.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/session/view_json.h>
#include <formats/pff/pff.h>
#include <formats/rtxt/rtxt.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
using opennova::pff::normalized_logical_name;

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
	if (table != TablePlace::Archive && !test_io::write_text(x1 + "/x1.bin", "x1 loose table")) return false;
	return write_archive(root + "/resource.pff", { { "shared.txt", "base" }, { "baseonly.txt", "base only" },
	                                               { "01TR.bms", "a mission" }, { "menumus.bin", "base menu script" },
	                                               { "gamemus.bin", "base game script" } }) &&
	       write_archive(x1 + "/x1.pff", { { "shared.txt", "expansion" }, { "x1only.txt", "x1 only" },
	                                       { "version.txt", "an archived version" }, { "gt.ssc", "archived config" } }) &&
	       write_archive(x1 + "/x1L.pff", language) &&
	       test_io::write_text(x1 + "/trailer.bik", "a video the game never reads") &&
	       test_io::write_text(x1 + "/gt.ssc", "loose config") &&
	       test_io::write_text(root + "/MENUMUS.SBF", "base music") &&
	       test_io::write_text(root + "/header.bik", "base header") &&
	       test_io::write_text(root + "/main.bik", "base main") && test_io::write_text(x1 + "/Mx1.sbf", "x1 music") &&
	       test_io::write_text(x1 + "/header.bik", "x1 header") &&
	       test_io::write_text(x1 + "/version.txt", "x1 version") &&
	       test_io::write_text(x1 + "/weapon.sav", "a player's");
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
	// One /exp token [orig: Config_TokenizeLine @ 0x53cb60] for the project's own; an installed
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
	for (const char *name : { "con", "NUL", "Prn", "aux", "com0", "com1", "LPT9", "con.x" })
		TEST_EXPECT(refuses(name, "keeps for a device"));
	TEST_EXPECT(takes("console") && takes("lpt10"));
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
	// T5: a base game's project, for an expansion alone and never beside an installed one.
	TEST_EXPECT(check_project_expansion("jo", ProjectExpansion{ "onx", "", "../base" }, error));
	TEST_EXPECT(!check_project_expansion("jo", ProjectExpansion{ "", "", "../base" }, error) &&
	            error.code() == "project.field.invalid" && error.message.find("only as an expansion") != std::string::npos);
	TEST_EXPECT(!check_project_expansion("jo", ProjectExpansion{ "onx", "jox01", "../base" }, error) &&
	            error.code() == "project.field.invalid" && error.message.find("never both") != std::string::npos);
	TEST_EXPECT(!check_project_expansion("dfx", ProjectExpansion{ "onx", "", "../base" }, error) &&
	            error.code() == "project.expansion.unsupported");
	// The project file keeps it, and a project on the install's base game writes no key for it (S16's form).
	ProjectDocument doc;
	doc.project_id = "id";
	doc.title = "Mod";
	doc.expansion = ProjectExpansion{ "onx", "", "../../assets" };
	ProjectDocument read;
	TEST_EXPECT(project_document_from_json(project_document_to_json(doc), read, error) &&
	            read.expansion == doc.expansion && read.expansion.on_base_project());
	const opennova::io::JsonValue on_base = project_document_to_json(doc);
	const opennova::io::JsonValue *written = on_base.get("expansion");
	TEST_EXPECT(written && written->get_string("base_project", "") == "../../assets");
	doc.expansion.base_project.clear();
	const opennova::io::JsonValue on_install = project_document_to_json(doc);
	written = on_install.get("expansion");
	TEST_EXPECT(written && !written->get("base_project"));
	// An object naming only a base game's project is refused as an expansion without a name.
	opennova::io::JsonValue json = project_document_to_json(doc);
	opennova::io::JsonValue unnamed = opennova::io::JsonValue::make_object();
	unnamed.set("base_project", opennova::io::JsonValue::make_string("../base"));
	json.set("expansion", std::move(unnamed));
	TEST_EXPECT(!project_document_from_json(json, read, error) && error.code() == "project.field.invalid");
	return 0;
}

// The base game's project an expansion names (T5, base_project.h): found from the project's folder when
// relative, its export folder the base game; refused (project.base_project) where no project is, where it
// builds as an expansion itself, or where it is another game's.
static int test_base_project_dir() {
	test_temp::TempDir dir("opennova_editor_base_project_dir");
	// A folder named whole is kept from the project's folder; one named from it already stays as it is.
	TEST_EXPECT(base_project_from(dir.file("game/expansions/onx"), dir.file("game/assets")) == "../../assets");
	TEST_EXPECT(base_project_from(dir.file("game/expansions/onx"), "../../assets") == "../../assets");
	// The features a game's files need: Missions for a mission, Multiplayer for the multiplayer screens.
	TEST_EXPECT(features_files_need({ "main.mnu", "01TR.bms" }).mission && !features_files_need({ "01TR.bms" }).multiplayer);
	TEST_EXPECT(features_files_need({ "MP.MNU" }).multiplayer && features_files_need({ "pre.mnu" }).multiplayer &&
	            !features_files_need({ "main.mnu", "gametext.bin" }).mission);
	const std::string mod = dir.file("game/expansions/onx");
	ProjectDocument made;
	Diagnostic error;
	TEST_EXPECT(create_project(dir.file("game/assets"), "Base", "jo", made, error));
	TEST_EXPECT(base_project_root(mod, ProjectExpansion{ "onx", "", "../../assets" }) == dir.file("game/assets"));
	TEST_EXPECT(base_project_root(mod, ProjectExpansion{ "onx", "", "../../assets/" }) == dir.file("game/assets"));
	TEST_EXPECT(base_project_root(mod, ProjectExpansion{ "onx" }).empty());
	std::string out;
	TEST_EXPECT(base_project_game_dir(mod, ProjectExpansion{ "onx", "", "../../assets" }, "jo", out, error) &&
	            out == dir.file("game/assets/build/export") && !opennova::vfs_has_boot_archive(out));
	std::error_code made_dir;
	std::filesystem::create_directories(opennova::io::os_path(out), made_dir);
	TEST_EXPECT(write_archive(out + "/resource.pff", { { "baseonly.txt", "base" } }) && opennova::vfs_has_boot_archive(out));
	TEST_EXPECT(!base_project_game_dir(mod, ProjectExpansion{ "onx", "", "../../nowhere" }, "jo", out, error) &&
	            error.code() == "project.base_project" && error.message.find("does not open") != std::string::npos &&
	            out.empty());
	TEST_EXPECT(create_project(dir.file("game/other"), "Other", "jo", made, error, ProjectExpansion{ "oth" }));
	TEST_EXPECT(!base_project_game_dir(mod, ProjectExpansion{ "onx", "", "../../other" }, "jo", out, error) &&
	            error.code() == "project.base_project" && error.message.find("builds as the expansion oth") != std::string::npos);
	TEST_EXPECT(create_project(dir.file("game/dfx"), "Dfx", "dfx", made, error));
	TEST_EXPECT(!base_project_game_dir(mod, ProjectExpansion{ "onx", "", "../../dfx" }, "jo", out, error) &&
	            error.code() == "project.base_project" && error.message.find("\"dfx\"") != std::string::npos);
	return 0;
}

// An expansion's base project's export as the expansion weighs it (base_project.h's base_export_of): none
// there (not current, may be made); one whose record keeps the stamp of the base's files as they stand
// (export_build.h's project_files_stamp: current), which a changed file or a new one makes older than the
// project, the base's own `.opennova/` and its export folder never moving it; another project's export (never
// replaced) and a record keeping no stamp, neither current; a base that does not open, refused.
static int test_base_export_weighed() {
	test_temp::TempDir dir("opennova_editor_base_export_weighed");
	const std::string mod = dir.file("game/expansions/onx"), root = dir.file("game/assets");
	ProjectDocument base;
	Diagnostic error;
	TEST_EXPECT(create_project(root, "Base", "jo", base, error));
	TEST_EXPECT(test_io::write_text(root + "/menus/main.mnu", "<SCREEN>\r\n</SCREEN>\r\n"));
	const ProjectExpansion on_base{ "onx", "", "../../assets" };
	BaseExport weighed = base_export_of(mod, on_base, "jo");
	TEST_EXPECT(weighed.resolved() && weighed.root == root && weighed.export_dir == root + "/build/export" &&
	            !weighed.current && weighed.replaceable);
	const std::string exported = weighed.export_dir;
	const auto record = [&exported](const std::string &project_id, const std::string &sources) {
		return test_io::write_text(exported + "/export.json", "{\"schema_version\": 2, \"project_id\": \"" + project_id +
		                                                          "\", \"sources\": \"" + sources + "\", \"files\": []}");
	};
	TEST_EXPECT(record(base.project_id, project_files_stamp(root, exported)));
	weighed = base_export_of(mod, on_base, "jo");
	TEST_EXPECT(weighed.current && weighed.replaceable);
	// The machine's own folder, the export folder and its staging folder hold no file of the base's.
	TEST_EXPECT(test_io::write_text(root + "/.opennova/local.json", "{}") && test_io::write_text(exported + "/resource.pff", "x") &&
	            test_io::write_text(exported + ".tmp/resource.pff", "x"));
	TEST_EXPECT(base_export_of(mod, on_base, "jo").current);
	// A file changed, a file added: the export is older than the project.
	TEST_EXPECT(test_io::write_text(root + "/menus/main.mnu", "<SCREEN>\r\n<NAME>STARTUP</NAME>\r\n</SCREEN>\r\n"));
	TEST_EXPECT(!base_export_of(mod, on_base, "jo").current);
	TEST_EXPECT(record(base.project_id, project_files_stamp(root, exported)) && base_export_of(mod, on_base, "jo").current);
	TEST_EXPECT(test_io::write_text(root + "/notes.md", "a note"));
	TEST_EXPECT(!base_export_of(mod, on_base, "jo").current);
	// Another project's export: not current, never replaced; a record keeping no stamp: not current.
	TEST_EXPECT(record("another", project_files_stamp(root, exported)));
	weighed = base_export_of(mod, on_base, "jo");
	TEST_EXPECT(!weighed.current && !weighed.replaceable);
	TEST_EXPECT(record(base.project_id, ""));
	weighed = base_export_of(mod, on_base, "jo");
	TEST_EXPECT(!weighed.current && weighed.replaceable);
	weighed = base_export_of(mod, ProjectExpansion{ "onx", "", "../../nowhere" }, "jo");
	TEST_EXPECT(!weighed.resolved() && weighed.refusal.code() == "project.base_project" && !weighed.current);
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

// The editor's rows of the files an expansion's name forms (the manifest's RES_F_EXPANSION rows name
// them; tests/gameprofile/required_resources_test pins the names): one row per role and per manifest
// row, in the roles' order, each placed where its manifest row's path says (loose in the folder for a
// pattern under expansion\<n>\, by kind otherwise), its name, fixedness and replaced file the
// manifest's.
static int test_files_table() {
	const std::vector<ExpansionFile> files = expansion_files("jxm");
	std::vector<std::string> names;
	for (const ExpansionFile &file : files) names.push_back(file.name);
	TEST_EXPECT((names == std::vector<std::string>{ "jxm.bin", "version.txt", "Mjxm.sbf", "Mjxm.bin", "Gjxm.sbf",
	                                                  "Gjxm.bin", "jxmL.lwf", "jxm.lwf" }));
	TEST_EXPECT(expansion_files("").empty());
	int manifest_rows = 0;
	for (int i = 0; i < opennova::gameprofile::gameprofile_required_resource_count(); ++i)
		manifest_rows += (opennova::gameprofile::gameprofile_required_resource_at(i)->flags &
		                  opennova::gameprofile::RES_F_EXPANSION) != 0;
	TEST_EXPECT(files.size() == static_cast<size_t>(manifest_rows));
	for (const ExpansionFile &file : files) {
		const opennova::gameprofile::RequiredResource *row = file.row->resource();
		TEST_EXPECT(row && (row->flags & opennova::gameprofile::RES_F_EXPANSION) &&
		            row == opennova::gameprofile::gameprofile_required_resource_by_role(file.row->manifest_role));
		TEST_EXPECT(expansion_file_row_for_manifest_role(file.row->manifest_role) == file.row);
		TEST_EXPECT(file.row->what && *file.row->what);
		const bool in_folder = std::string(row->name).rfind("expansion\\<n>\\", 0) == 0;
		TEST_EXPECT((file.row->placement == ExpansionPlacement::Folder) == in_folder);
		TEST_EXPECT(file.row->fixed() == !opennova::gameprofile::gameprofile_expansion_file_formed(row) &&
		            file.row->replaces() == row->replaces);
	}
	TEST_EXPECT(expansion_file_row(ExpansionFileRole::Version).fixed() && !expansion_file_row(ExpansionFileRole::Table).fixed());
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

// The install as the game serves it (install_view.h): the base game's view; a standalone project's, which is
// the base game, its version text too; /exp x1 for a project that builds as jxm on it; the base game for one
// that builds as jxm on it; an expansion the install lacks.
static int test_install_view() {
	test_temp::TempDir dir("opennova_editor_expansion_install");
	const std::string root = dir.file("install");
	TEST_EXPECT(make_install(root));
	// The base game's version text, loose in the root, which the updater alone reads [orig:
	// Cupdate2Dlg_ReportResult @ 0x405140 (UPDATE.EXE)].
	TEST_EXPECT(test_io::write_text(root + "/version.txt", "base version"));
	ProjectDocument document;
	std::string error;
	// The base game under an expansion project (base_install_spec): no expansion's file; the root's loose ones,
	// not its version text, which the game never reads.
	InstallView base;
	TEST_EXPECT(base.open(base_install_spec(root, document), error));
	TEST_EXPECT(text_of(base, "shared.txt") == "base" && text_of(base, "x1only.txt") == "<none>" &&
	            text_of(base, "header.bik") == "base header" && text_of(base, "MENUMUS.SBF") == "base music" &&
	            text_of(base, "menumus.bin") == "base menu script" && !base.find("version.txt"));
	for (const InstallFile &file : base.files())
		TEST_EXPECT(file.layer == InstallFile::Layer::Base && file.name == file.member);
	// A standalone project's view: the base game's files, its version text among them, loose from the root.
	InstallView standalone;
	TEST_EXPECT(standalone.open(install_spec(root, document), error));
	TEST_EXPECT(standalone.files().size() == base.files().size() + 1);
	const InstallFile *version = standalone.find("VERSION.TXT");
	TEST_EXPECT(version && version->name == "version.txt" && version->member == "version.txt" &&
	            version->layer == InstallFile::Layer::Base && version->archive == ArchiveSlot::None &&
	            !version->loose_path.empty() && text_of(standalone, "version.txt") == "base version");
	TEST_EXPECT(standalone.files().back().name == "version.txt");
	const ImportChoice versioned = install_choice(root, *version);
	TEST_EXPECT(versioned.install && versioned.entry == "version.txt" && versioned.as.empty() && versioned.expansion.empty());
	TEST_EXPECT(install_spec(root, document) != base_install_spec(root, document));
	// /exp x1, the project building as jxm: the expansion's copy of a shared name, its folder's video and
	// bank, the root's other video; the expansion's own names under the project's; the base's music pairs,
	// either version text and the player's file not listed.
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
	// On the base game, building as jxm: its music pairs under the project's names; the base game's version text
	// not listed (the project makes its own).
	document.expansion = { "jxm", "" };
	InstallView own;
	TEST_EXPECT(own.open(install_spec(root, document), error));
	TEST_EXPECT(text_of(own, "Mjxm.sbf") == "base music" && text_of(own, "Mjxm.bin") == "base menu script" &&
	            text_of(own, "Gjxm.bin") == "base game script" && !own.find("menumus.bin") &&
	            !own.find("MENUMUS.SBF") && text_of(own, "shared.txt") == "base" && !own.find("version.txt"));
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
	test_temp::TempDir dir("opennova_editor_expansion_fold");
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
		fold.want(install_spec(install, *document), document, &scan, {normalized_logical_name("jxm.bin")});
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

} // namespace

// A session's expansion project (ADR 0046 S16) over the synthetic install: a name the install has or
// an expansion it lacks refused before anything is made; one made on the base game holds its version
// text and its text table; a music file the setting leaves unread said; the name changed in the
// settings renames the project's files of the old name; the expansion it builds on checked against the
// install, and listed when the install changes under it.
static int test_session() {
	test_temp::TempDir dir("opennova_editor_expansion_session");
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
	TEST_EXPECT(version && table && test_io::read_file_text(root + "/" + version->relative_path) == "Own\r\n");
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
	TEST_EXPECT(test_io::write_text(root + "/MENUMUS.SBF", "music") &&
	            test_io::write_text(root + "/menumus.bin", "SCR0....")); // a script by its bytes, as the scan types a .bin
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
	TEST_EXPECT(test_io::write_text(jxq, "a stale table"));
	editor_test::handle_to_end(session, request::rescan());
	ProjectSettingsChange onto_stale;
	onto_stale.expansion = "jxq";
	editor_test::apply_settings(session, onto_stale);
	TEST_EXPECT(count_code(view.project.settings_result.failures, "rename.exists") == 1 &&
	            view.project.document->expansion.name == "jxk" && std::filesystem::exists(opennova::io::os_path(jxk)) &&
	            test_io::read_file_text(jxq) == "a stale table");
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

// A game you own (new_project's all) in a session, over a game install with an expansion: refused with no install
// to make it from; the base game's project made standalone on the install, every file of it imported as it opens
// (the operation that follows the open: the import's outcome follows it), the install's root version text among
// them, the features its files need on (a mission: Missions), no placeholder made; its expansion refused with no
// base game's project named (nothing made)
// and for an expansion the install lacks, then made on the base game's project, whose export the session makes
// first (the base has none: session/base_export.h), the import never refused for it but following it, the
// installed expansion's own files imported (its version text the install's, never a placeholder's).
static int test_session_stock() {
	test_temp::TempDir dir("opennova_editor_expansion_stock");
	const std::string install = dir.file("install");
	TEST_EXPECT(make_install(install) && write_archive(install + "/localres.pff", { { "local.txt", "localres" } }) &&
	            write_archive(install + "/language.pff", { { "words.txt", "language" } }) &&
	            test_io::write_text(install + "/version.txt", "base version"));
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	const SessionView &view = session.view();
	const std::string base = dir.file("work/JO"), expansion = dir.file("work/JOX01");
	std::string text, message;
	const auto text_in = [&](const char *name) {
		const AssetEntry *entry = view.project.scan->find(name);
		return entry && opennova::io::read_file_text(view.project.root + "/" + entry->relative_path, text, message) ? text : "<none>";
	};
	ActionOutcome outcome = editor_test::handle_to_end(session, request::new_stock_project(base, "Joint Operations", ""));
	TEST_EXPECT(outcome.refused && count_code(outcome.findings, "project.install.invalid") == 1 && !view.project.open &&
	            !std::filesystem::exists(opennova::io::os_path(base)));
	outcome = editor_test::handle_to_end(session, request::new_stock_project(base, "Joint Operations", install));
	TEST_EXPECT(!outcome.refused && view.project.open && view.project.document->expansion.standalone());
	const OperationOutcome &imported = view.activity.last_operation;
	TEST_EXPECT(imported.kind == OperationKind::ImportApply && imported.end == OperationEnd::Done && imported.follows != 0 &&
	            imported.follows == outcome.operation && imported.id == outcome.operation + 1 && !imported.imported.empty());
	TEST_EXPECT(view.project.scan->find("01TR.bms") && view.project.scan->find("baseonly.txt") &&
	            view.project.scan->find("local.txt") && view.project.scan->find("words.txt") && view.project.scan->find("MENUMUS.SBF"));
	// The base game's version text, the updater's, as the install has it: the build ships it loose at the root.
	TEST_EXPECT(text_in("version.txt") == "base version");
	TEST_EXPECT(view.project.document->features.mission && !view.project.document->features.multiplayer);
	TEST_EXPECT(count_code(view.project.requirements->diagnostics, "project.mission.feature_off") == 0);
	// Its expansion: on the base game's project, which must be named.
	outcome = editor_test::handle_to_end(session, request::new_stock_project(expansion, "Escalation", install, "x1"));
	TEST_EXPECT(outcome.refused && count_code(outcome.findings, "project.base_project", "make Joint Operations first") == 1 &&
	            !std::filesystem::exists(opennova::io::os_path(expansion)));
	outcome = editor_test::handle_to_end(session, request::new_stock_project(expansion, "Escalation", install, "x2", "../JO"));
	TEST_EXPECT(outcome.refused && count_code(outcome.findings, "project.field.invalid", "no expansion named x2") == 1);
	outcome = editor_test::handle_to_end(session, request::new_stock_project(expansion, "Escalation", install, "x1", "../JO"));
	TEST_EXPECT(!outcome.refused && view.project.open &&
	            view.project.document->expansion == (ProjectExpansion{ "x1", "", "../JO" }));
	// The base game's project has no export: the session exports it first, then the import goes on (the operation
	// between them the export), never refused for the slot the export held.
	TEST_EXPECT(view.activity.last_operation.kind == OperationKind::ImportApply &&
	            view.activity.last_operation.end == OperationEnd::Done &&
	            view.activity.last_operation.follows == outcome.operation &&
	            view.activity.last_operation.id == outcome.operation + 2 && count_code(view.findings.diagnostics, "operation.busy") == 0);
	TEST_EXPECT(text_in("version.txt") == "x1 version" && text_in("x1.bin") == "x1 table" && text_in("x1only.txt") == "x1 only");
	return 0;
}

// An expansion of a project's base game (T5) in a session, over a game install that has an expansion of its
// own: new_project makes it on the base project's folder (refused, nothing made, where no project is there).
// The session exports the base project itself where its export is missing or older than the project
// (session/base_export.h): a base that does not build is the one finding, nothing validated over a base not
// read and no required file said missing or offered to create, and a Build exports it again first; a base that
// builds is exported before the Build it waited for, which goes on under the same operation, and the base game
// the expansion reads is that export (its names, its expansions: none), never the install's. Opened again with
// the base unchanged, nothing is exported; with a file of the base changed, the base is exported again first.
static int test_session_on_base_project() {
	test_temp::TempDir dir("opennova_editor_expansion_base_project");
	const std::string install = dir.file("install");
	TEST_EXPECT(make_install(install));
	Preferences chosen;
	chosen.game_install = install;
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences(chosen);
	ProjectSession session(platform, preferences);
	const SessionView &view = session.view();
	ProjectDocument base;
	Diagnostic error;
	TEST_EXPECT(create_project(dir.file("game/assets"), "Base", "jo", base, error));
	const std::string mod = dir.file("game/expansions/onx");
	ActionOutcome outcome =
	        editor_test::handle_to_end(session, request::new_expansion_project(dir.file("game/expansions/bad"), "Bad", "onx",
	                                                                           "", true, "../../nowhere"));
	TEST_EXPECT(outcome.refused && count_code(outcome.findings, "project.base_project") == 1 &&
	            !std::filesystem::exists(opennova::io::os_path(dir.file("game/expansions/bad"))) && !view.project.open);
	outcome = editor_test::handle_to_end(session,
	                                     request::new_expansion_project(mod, "Night", "onx", "", true, "../../assets"));
	TEST_EXPECT(!outcome.refused && view.project.open &&
	            view.project.document->expansion == (ProjectExpansion{ "onx", "", "../../assets" }));
	TEST_EXPECT(test_io::read_file_text(mod + "/project.opennova").find("\"base_project\": \"../../assets\"") != std::string::npos);
	TEST_EXPECT(view.project.scan->find("onx.bin") && view.project.scan->find("version.txt"));
	{
		const opennova::io::JsonValue project = view_section_to_json(view, ViewSection::Project);
		const opennova::io::JsonValue *expansion = project.get("expansion");
		TEST_EXPECT(expansion && expansion->get_string("base_project", "") == "../../assets");
	}
	// The base project holds none of the game's required files, so its export is refused: said once, nothing
	// validated over a base not read, nothing said of what the expansion lacks (no required file missing, none
	// to create).
	const std::string exported = dir.file("game/assets/build/export");
	const auto no_false_errors = [&view]() {
		size_t errors = 0;
		for (const Diagnostic &d : view.findings.diagnostics) errors += d.severity == DiagnosticSeverity::Error ? 1 : 0;
		return errors == 0 && count_code(view.project.requirements->diagnostics, "requirement.missing") == 0 &&
		       unmet_required_roles(*view.project.requirements).empty() && !view.activity.validation.read;
	};
	TEST_EXPECT(view.activity.last_operation.kind == OperationKind::BaseExport &&
	            view.activity.last_operation.end == OperationEnd::Failed);
	TEST_EXPECT(count_code(view.project.requirements->diagnostics, "project.base_project", "could not be exported") == 1 &&
	            count_code(view.findings.diagnostics, "project.base_project") == 1);
	TEST_EXPECT(no_false_errors());
	TEST_EXPECT(view.project.base_files.empty() && view.project.install_expansions.empty() &&
	            !opennova::vfs_has_boot_archive(exported));
	// A Build exports the base again first, refused again: no build.
	editor_test::handle_to_end(session, request::build());
	TEST_EXPECT(view.activity.last_operation.kind == OperationKind::BaseExport && !view.activity.has_build &&
	            count_code(view.findings.diagnostics, "build.expansion.base_missing") == 0 && no_false_errors());
	// The base made whole (its required files created, as Create all makes them): the next Build exports it,
	// then builds the expansion over it under the operation the Build named.
	{
		MemoryPreferencesStore base_preferences(chosen);
		ProjectSession base_session(platform, base_preferences);
		editor_test::handle_to_end(base_session, request::open_project(dir.file("game/assets")));
		TEST_EXPECT(base_session.view().project.open);
		editor_test::handle_to_end(base_session,
		                           request::create_missing(unmet_required_roles(*base_session.view().project.requirements)));
		TEST_EXPECT(unmet_required_roles(*base_session.view().project.requirements).empty());
	}
	session.handle(request::build());
	const uint64_t asked = session.outcome().operation;
	TEST_EXPECT(asked != 0 && view.activity.operation.running() && view.activity.operation.kind == OperationKind::BaseExport);
	session.run_operations();
	TEST_EXPECT(view.activity.last_operation.id == asked && view.activity.last_operation.kind == OperationKind::Build &&
	            view.activity.has_build && view.activity.last_build->ok);
	if (!view.activity.last_build->ok)
		for (const Diagnostic &d : view.findings.diagnostics) std::fprintf(stderr, "%s: %s\n", d.code().c_str(), d.message.c_str());
	TEST_EXPECT(opennova::vfs_has_boot_archive(exported) &&
	            test_io::read_file_text(exported + "/export.json").find("\"sources\"") != std::string::npos);
	TEST_EXPECT(count_code(view.project.requirements->diagnostics, "project.base_project") == 0 &&
	            view.activity.validation.read);
	const auto has = [&view](const char *name) {
		return std::any_of(view.project.base_files.begin(), view.project.base_files.end(),
		                   [name](const std::string &held) { return opennova::strutil::iequals(held, name); });
	};
	TEST_EXPECT(has("main.mnu") && !has("x1only.txt") && !has("01TR.bms"));
	TEST_EXPECT(view.project.install_expansions.empty());
	// The game install's own still listed apart: the installed expansion x1 Import offers the project.
	TEST_EXPECT(view.project.game_install_expansions.size() == 1 && view.project.game_install_expansions[0].name == "x1");
	// Opened again over the base as it was exported: nothing exported.
	const std::string record = test_io::read_file_text(exported + "/export.json");
	outcome = editor_test::handle_to_end(session, request::open_project(mod));
	TEST_EXPECT(!outcome.refused && view.project.open && view.activity.last_operation.kind == OperationKind::Open &&
	            test_io::read_file_text(exported + "/export.json") == record && has("main.mnu"));
	// A file of the base changed since (its last write moved on): exported again as the expansion opens.
	{
		MemoryPreferencesStore base_preferences(chosen);
		ProjectSession base_session(platform, base_preferences);
		editor_test::handle_to_end(base_session, request::open_project(dir.file("game/assets")));
		const AssetEntry *menu = base_session.view().project.scan->find("main.mnu");
		TEST_EXPECT(menu != nullptr);
		if (!menu) return 1;
		const std::filesystem::path path = opennova::io::os_path(dir.file("game/assets") + "/" + menu->relative_path);
		std::error_code ec;
		const std::filesystem::file_time_type written = std::filesystem::last_write_time(path, ec);
		std::filesystem::last_write_time(path, written + std::chrono::seconds(2), ec);
		TEST_EXPECT(!ec);
	}
	outcome = editor_test::handle_to_end(session, request::open_project(mod));
	TEST_EXPECT(!outcome.refused && view.project.open && view.activity.last_operation.kind == OperationKind::BaseExport &&
	            view.activity.last_operation.end == OperationEnd::Done && has("main.mnu") && view.activity.validation.read);
	// Settings: the base game's project and an installed expansion together refused, nothing written.
	ProjectSettingsChange both;
	both.builds_on = std::string("x1");
	editor_test::apply_settings(session, both);
	TEST_EXPECT(count_code(view.project.settings_result.failures, "project.field.invalid", "never both") == 1 &&
	            view.project.document->expansion.base_project == "../../assets");
	// Moved back to the install's base game: the install's names again.
	ProjectSettingsChange install_base;
	install_base.base_project = std::string();
	editor_test::apply_settings(session, install_base);
	TEST_EXPECT(view.project.document->expansion == (ProjectExpansion{ "onx", "" }) && has("01TR.bms") &&
	            !has("x1only.txt"));
	TEST_EXPECT(test_io::read_file_text(mod + "/project.opennova").find("base_project") == std::string::npos);
	return 0;
}

// The base game under an expansion's graph (the base layer, base_layer_build.h): the names its files
// define resolve the project's references as the game resolves them through the archives below the
// expansion's pair, so a menu naming the base's style variable, the font it names and a texture it ships
// leaves no reference.missing row; a required file the base serves is a note, never an error. Built a
// step at a time within a budget, the layer is the one a single call builds; a standalone project, or a
// base that does not mount, builds none.
static int test_base_layer_session() {
	test_temp::TempDir dir("opennova_editor_expansion_base_layer");
	const std::string exported = dir.file("game/assets/build/export");
	std::error_code made_dir;
	std::filesystem::create_directories(opennova::io::os_path(exported), made_dir);
	TEST_EXPECT(write_archive(exported + "/localres.pff",
	                          { { "menu_style.mns", "DEF_FONTNAME_LG basefont.fnt\r\n" },
	                            { "basefont.fnt", "font" },
	                            { "onlybase.tga", "tga" },
	                            { "main.mnu", "<SCREEN>\r\n<NAME>STARTUP</NAME>\r\n</SCREEN>\r\n" } }) &&
	            write_archive(exported + "/resource.pff", { { "baseonly.txt", "base" } }));
	ProjectDocument base;
	Diagnostic error;
	TEST_EXPECT(create_project(dir.file("game/assets"), "Base", "jo", base, error));
	// Stepped, the layer the one call builds.
	ProjectDocument mod;
	mod.target_game = "jo";
	mod.expansion = ProjectExpansion{ "onx", "", "../../assets" };
	BaseLayerBuild stepped(exported, mod);
	size_t steps = 0;
	while (!stepped.step(4)) ++steps;
	const std::shared_ptr<const GraphLayer> layer = stepped.take();
	const std::shared_ptr<const GraphLayer> whole = build_base_layer(exported, mod);
	TEST_EXPECT(steps > 2 && layer && whole && layer->file_count() == whole->file_count() &&
	            layer->symbol_count() == whole->symbol_count() && layer->file_named("onlybase.tga") &&
	            layer->symbol_count() > 0);
	TEST_EXPECT(!build_base_layer(exported, ProjectDocument()) &&
	            !build_base_layer(dir.file("nowhere"), mod)); // standalone; a base that does not mount

	Preferences chosen;
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences(chosen);
	ProjectSession session(platform, preferences);
	const SessionView &view = session.view();
	const std::string root = dir.file("game/expansions/onx");
	ActionOutcome outcome =
	        editor_test::handle_to_end(session, request::new_expansion_project(root, "Night", "onx", "", true, "../../assets"));
	TEST_EXPECT(!outcome.refused && view.project.open);
	TEST_EXPECT(test_io::write_text(root + "/menus/night.mnu",
	                                    "<SCREEN>\r\n<NAME>NIGHT</NAME>\r\n"
	                                    "<WINDOW TYPE=\"STATIC\" NAME=\"W1\">\r\n"
	                                    "<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>100</RIGHT><BOTTOM>20</BOTTOM></POSITION>\r\n"
	                                    "<FONT><NAME>%DEF_FONTNAME_LG%</NAME></FONT>\r\n"
	                                    "<APPEARANCE STATE=\"DEFAULT\" TYPE=\"IMAGE\">onlybase.tga</APPEARANCE>\r\n"
	                                    "<APPEARANCE STATE=\"MOUSEOVER\" TYPE=\"IMAGE\">nowhere.tga</APPEARANCE>\r\n"
	                                    "</WINDOW>\r\n</SCREEN>\r\n"));
	editor_test::handle_to_end(session, request::open_project(root));
	TEST_EXPECT(view.project.open && view.project.scan->find("night.mnu"));
	const std::vector<Diagnostic> &rows = view.findings.diagnostics;
	// The base's names resolve; the one name neither has is the only one missing.
	TEST_EXPECT(count_code(rows, "reference.missing") == 1 && count_code(rows, "reference.missing", "nowhere.tga") == 1);
	// main.mnu, which the base serves: a note.
	size_t served = 0;
	for (const Diagnostic &d : rows)
		if (d.code() == "requirement.missing" && d.message.find("main.mnu") == 0) {
			++served;
			TEST_EXPECT(d.severity == DiagnosticSeverity::Info &&
			            d.message.find("The game reads the base game's") != std::string::npos);
		}
	TEST_EXPECT(served == 1);
	// The checklist counts it served, not missing: the game reads the base's, and Create all leaves it.
	const RequirementReport &report = *view.project.requirements;
	const RequirementRow *menu = nullptr;
	for (const RequirementRow &row : report.rows)
		if (row.name == "main.mnu") menu = &row;
	TEST_EXPECT(menu && menu->state == RequirementState::Served && report.required_served >= 1);
	const std::vector<std::string> unmet = unmet_required_roles(report);
	TEST_EXPECT(menu && std::find(unmet.begin(), unmet.end(), menu->role) == unmet.end());
	TEST_EXPECT(view_section_to_json(view, ViewSection::Requirements).get_number("served", 0) == double(report.required_served));
	return 0;
}

namespace {

// A string table's bytes: each section by its name, its keys in order.
std::vector<uint8_t> table_bytes(const std::vector<std::pair<std::string, std::vector<std::string>>> &sections) {
	opennova::rtxt::File table;
	for (const auto &section : sections) {
		table.sections.push_back({ section.first, uint32_t(section.second.size()) });
		for (const std::string &key : section.second) {
			opennova::rtxt::Entry entry;
			entry.key = key;
			entry.text = key + " text";
			entry.section_index = uint32_t(table.sections.size() - 1);
			table.entries.push_back(entry);
		}
	}
	std::vector<uint8_t> bytes;
	std::string error;
	return opennova::rtxt::write(table, bytes, error) ? bytes : std::vector<uint8_t>();
}

} // namespace

// An expansion's mission on a terrain its base game ships (a project of the base game's export): the project's files
// read over the base's as the game mounts them below the expansion's pair (ProjectAssetSource::set_base): the
// terrain query samples the base's ground under the mission, a file of the project's own name is the project's, and
// the brush's planes name the base's files as no file of the project (copy one in first), so nothing writes them. A
// standalone project reads no base.
static int test_base_files_under_the_project() {
	using opennova::io::JsonValue;
	test_temp::TempDir dir("opennova_editor_expansion_base_files");
	const std::string exported = dir.file("game/assets/build/export");
	std::error_code made_dir;
	std::filesystem::create_directories(opennova::io::os_path(exported), made_dir);
	const std::string repo = test_paths_repo_root(__FILE__);
	const auto fixture = [&](const char *name) {
		const std::vector<uint8_t> bytes = test_io::read_file(repo + "/fixtures/terrain/tmap/" + name);
		return std::string(bytes.begin(), bytes.end());
	};
	const std::vector<uint8_t> env_bytes = test_io::read_file(repo + "/fixtures/env/synth_full.env");
	const std::string environment(env_bytes.begin(), env_bytes.end());
	TEST_EXPECT(write_archive(exported + "/resource.pff",
	                          { { "Tmap.trn", fixture("Tmap.trn") }, { "Tmap.cpt", fixture("Tmap.cpt") },
	                            { "Tmap_m.pcx", fixture("Tmap_m.pcx") }, { "synth_full.env", environment },
	                            { "shadowed.env", "the base's" } }));
	ProjectDocument base;
	Diagnostic error;
	TEST_EXPECT(create_project(dir.file("game/assets"), "Base", "jo", base, error));
	Preferences chosen;
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences(chosen);
	ProjectSession session(platform, preferences);
	const SessionView &view = session.view();
	const std::string root = dir.file("game/expansions/onx");
	ActionOutcome outcome =
	        editor_test::handle_to_end(session, request::new_expansion_project(root, "Night", "onx", "", true, "../../assets"));
	TEST_EXPECT(!outcome.refused && view.project.open && view.findings.assets && view.findings.assets->base());
	if (!view.findings.assets) return 1;
	// The expansion's mission (the minted one, on Tmap under synth_full.env, both the base's alone).
	TEST_EXPECT(test_io::write_text(root + "/shadowed.env", "the project's") &&
	            test_io::write_file(root + "/missions/m.bms", test_io::read_file(repo + "/fixtures/bms/synth_logic.bms")));
	editor_test::handle_to_end(session, request::rescan());
	editor_test::set_missions(session, true);
	const AssetEntry *mission = view.project.scan->find("m.bms");
	TEST_EXPECT(mission != nullptr);
	if (!mission) return 1;
	// The base's files read by name where the project lacks them; the project's own over the base's of its name.
	std::vector<uint8_t> bytes;
	TEST_EXPECT(view.findings.assets->read("Tmap.trn", bytes) && !bytes.empty() &&
	            view.findings.assets->stamp("Tmap.trn") != 0 && view.findings.assets->path_of("Tmap.trn").empty());
	TEST_EXPECT(view.findings.assets->read("shadowed.env", bytes) &&
	            std::string(bytes.begin(), bytes.end()).rfind("the project's", 0) == 0 &&
	            view.findings.assets->path_of("shadowed.env") == "shadowed.env");
	TEST_EXPECT(!view.findings.assets->read("nowhere.trn", bytes) && view.findings.assets->stamp("nowhere.trn") == 0);
	// The terrain query samples the base's ground under the expansion's mission.
	session.handle(request::open_document(mission->relative_path));
	JsonValue args = JsonValue::make_object();
	args.set("path", opennova::io::json_string(mission->relative_path));
	args.set("op", opennova::io::json_string("sample"));
	args.set("x", opennova::io::json_number(-256.0));
	args.set("y", opennova::io::json_number(256.0));
	std::string asked;
	const JsonValue sample = session.query("terrain", args, asked);
	TEST_EXPECT(asked.empty() && sample.get("height") && sample.get("height")->is_number() && sample.get("ground"));
	if (!asked.empty()) std::fprintf(stderr, "sample: %s\n", asked.c_str());
	// The brush reads the base's terrain and writes none of it: each plane's file is the game's own here.
	args.set("op", opennova::io::json_string("family"));
	const JsonValue family = session.query("terrain", args, asked);
	const JsonValue *planes = family.get("planes");
	TEST_EXPECT(asked.empty() && planes && planes->get("heights") && planes->get("heights")->get_string("path", "x").empty() &&
	            planes->get("heights")->get_string("refusal", "").find("copy it into the project") != std::string::npos);
	// Closed: no base under nothing.
	editor_test::handle_to_end(session, request::close_project());
	TEST_EXPECT(!view.findings.assets || !view.findings.assets->base());
	return 0;
}

// The base game's string tables under an expansion's graph (base_layer_build.h): a `.bin` of the base typed by its
// content as the project scan types the project's own, so the base's menutxt.bin (RTXT) is a string table and its
// fgn2.bin (no magic) a raw table. An expansion's menu naming menutxt.BIN as its TEXT_RSRC then reads the base's
// table, as the game does through the archives below the expansion's pair [orig: CUIStringTable_LookupString @
// 0x6527c0; PFF_OpenAllArchives @ 0x4a4310, slots 2..4]: no reference.wrong_kind (Escalation's menus over the JO
// project's export read "the project's menutxt.bin is Binary table", 2026-10-26), a string id the base's Menu
// section defines resolved, one it does not still missing.
static int test_base_string_tables() {
	test_temp::TempDir dir("opennova_editor_expansion_base_strings");
	const std::string exported = dir.file("game/assets/build/export");
	std::error_code made_dir;
	std::filesystem::create_directories(opennova::io::os_path(exported), made_dir);
	const std::vector<uint8_t> menutxt = table_bytes({ { "Menu", { "MM_HELLO" } } });
	TEST_EXPECT(!menutxt.empty() &&
	            write_archive(exported + "/language.pff", { { "menutxt.bin", std::string(menutxt.begin(), menutxt.end()) },
	                                                        { "fgn2.bin", "a marker" } }));
	ProjectDocument base;
	Diagnostic error;
	TEST_EXPECT(create_project(dir.file("game/assets"), "Base", "jo", base, error));
	ProjectDocument mod;
	mod.target_game = "jo";
	mod.expansion = ProjectExpansion{ "onx", "", "../../assets" };
	// Typed by content in the layer, stepped or in one call alike.
	const std::shared_ptr<const GraphLayer> whole = build_base_layer(exported, mod);
	BaseLayerBuild stepped(exported, mod);
	while (!stepped.step(4)) {
	}
	const std::shared_ptr<const GraphLayer> layer = stepped.take();
	for (const std::shared_ptr<const GraphLayer> &built : { whole, layer }) {
		const GraphSlot *table = built ? built->file_named("menutxt.bin") : nullptr;
		const GraphSlot *marker = built ? built->file_named("fgn2.bin") : nullptr;
		TEST_EXPECT(table && table->kind == AssetKind::Strings && table->read && table->ok && !table->symbols.empty());
		TEST_EXPECT(marker && marker->kind == AssetKind::RawBin);
	}
	TEST_EXPECT(whole && layer && whole->symbol_count() == layer->symbol_count());

	Preferences chosen;
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences(chosen);
	ProjectSession session(platform, preferences);
	const SessionView &view = session.view();
	const std::string root = dir.file("game/expansions/onx");
	const ActionOutcome outcome =
	        editor_test::handle_to_end(session, request::new_expansion_project(root, "Night", "onx", "", true, "../../assets"));
	TEST_EXPECT(!outcome.refused && view.project.open);
	const std::string position = "<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>100</RIGHT><BOTTOM>20</BOTTOM></POSITION>\r\n";
	TEST_EXPECT(test_io::write_text(root + "/menus/night.mnu",
	                                    "<SCREEN>\r\n<NAME>NIGHT</NAME>\r\n<WINDOW TYPE=\"STATIC\" NAME=\"MAIN\">\r\n" + position +
	                                            "<TEXT_RSRC>menutxt.BIN</TEXT_RSRC>\r\n"
	                                            "<WINDOW TYPE=\"STATIC\" NAME=\"HELLO\">\r\n" + position +
	                                            "<STRING type=\"id\">MM_HELLO</STRING>\r\n</WINDOW>\r\n"
	                                            "<WINDOW TYPE=\"STATIC\" NAME=\"GONE\">\r\n" + position +
	                                            "<STRING type=\"id\">MM_GONE</STRING>\r\n</WINDOW>\r\n"
	                                            "</WINDOW>\r\n</SCREEN>\r\n"));
	editor_test::handle_to_end(session, request::open_project(root));
	TEST_EXPECT(view.project.open && view.project.scan->find("night.mnu") && view.findings.graph);
	if (!view.findings.graph) return 1;
	const std::vector<Diagnostic> &rows = view.findings.diagnostics;
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::TextTable, "menutxt.BIN") == ReferenceStatus::Present);
	TEST_EXPECT(count_code(rows, "reference.wrong_kind") == 0 && count_code(rows, "reference.missing", "menutxt.BIN") == 0);
	TEST_EXPECT(count_code(rows, "reference.missing", "MM_HELLO") == 0 && count_code(rows, "reference.missing", "MM_GONE") == 1);
	return 0;
}

// An expansion's terrain naming a detail map its base game serves: the game makes the detail coefficient of the
// base's file by its name, read through the archives below the expansion's pair [orig: PFF_OpenAllArchives @
// 0x4a4310, slots 2..4], so no texture.wrong_reader says "the project lacks it" (Escalation's G11.trn, g14.trn and
// I7.trn over the JO project's Det_d1.tga and Det_m2.tga, 2026-10-26); a name neither has, whose .dds alone the
// project holds, still does.
static int test_base_detail_map() {
	test_temp::TempDir dir("opennova_editor_expansion_base_detail");
	const std::string exported = dir.file("game/assets/build/export");
	std::error_code made_dir;
	std::filesystem::create_directories(opennova::io::os_path(exported), made_dir);
	TEST_EXPECT(write_archive(exported + "/resource.pff", { { "det_base.tga", "the base's detail map" } }));
	ProjectDocument base;
	Diagnostic error;
	TEST_EXPECT(create_project(dir.file("game/assets"), "Base", "jo", base, error));
	Preferences chosen;
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences(chosen);
	ProjectSession session(platform, preferences);
	const SessionView &view = session.view();
	const std::string root = dir.file("game/expansions/onx");
	const ActionOutcome outcome =
	        editor_test::handle_to_end(session, request::new_expansion_project(root, "Night", "onx", "", true, "../../assets"));
	TEST_EXPECT(!outcome.refused && view.project.open);
	const auto terrain = [](const char *detail) {
		return std::string("polytrn_detailmap ") + detail +
		       "\npolytrn_polydata isle.cpt\npolytrn_sectorcount 1\npolytrn_sectors 0\npolytrn_colormap colour.tga\n";
	};
	TEST_EXPECT(test_io::write_text(root + "/terrain/served.trn", terrain("det_base.tga")) &&
	            test_io::write_text(root + "/terrain/lacking.trn", terrain("det_own.tga")) &&
	            test_io::write_text(root + "/textures/det_own.dds", "the near texture alone"));
	editor_test::handle_to_end(session, request::open_project(root));
	TEST_EXPECT(view.project.open && view.project.scan->find("served.trn") && view.project.scan->find("lacking.trn"));
	size_t served = 0, lacking = 0;
	for (const Diagnostic &d : view.findings.diagnostics) {
		if (d.code() != "texture.wrong_reader") continue;
		if (d.asset == "terrain/served.trn") ++served;
		if (d.asset == "terrain/lacking.trn" && d.message.find("the project lacks it") != std::string::npos) ++lacking;
	}
	TEST_EXPECT(served == 0 && lacking == 1);
	return 0;
}

// The particle token slots over an expansion (graph/use_checks.cpp check_particle_token_slots): the game parses the
// expansion's archive before its base game's [orig: CEffectSystem_Init @ 0x5F6070, the slots ascending;
// PFF_OpenAllArchives @ 0x4a4310], so an expansion's flags list read past its line faults the game though the base's
// table (aaa.ptl, its name sorting first) would fill the slot: the build gate's error on the expansion's file, where
// the check had skipped every expansion. The expansion's own table first, none. The base's bbb.ptl, the same flags
// list after aaa.ptl's table, faults nowhere for the base alone; once the expansion has its own aaa.ptl of no table
// (and no other file), the walk reaches the base's aaa.ptl entry and parses the expansion's copy in its place, by name
// through the front door [orig: File_ParseASCIIFile by name @ 0x5f6545], so bbb.ptl faults only under the expansion:
// the project's error, said of the project, naming the base's file and line and the copy.
static int test_base_particle_walk() {
	test_temp::TempDir dir("opennova_editor_expansion_particles");
	const std::string exported = dir.file("game/assets/build/export");
	std::error_code made_dir;
	std::filesystem::create_directories(opennova::io::os_path(exported), made_dir);
	std::string table = "[tabledef]\r\n{\r\n\tid = grow;\r\n";
	for (int row = 1; row <= 32; ++row) table += "\ttl" + std::to_string(row) + " = 1, 2, 3, 4, 5, 6, 7, 8;\r\n";
	table += "}\r\n";
	const std::string flags = "[particledef]\r\n{\r\n\tid = Dot;\r\n\tflags\t=  EMITVECTOR AMBIENTCOLOR ;\r\n}\r\n";
	TEST_EXPECT(write_archive(exported + "/resource.pff", { { "aaa.ptl", table }, { "bbb.ptl", flags } }));
	ProjectDocument base;
	Diagnostic error;
	TEST_EXPECT(create_project(dir.file("game/assets"), "Base", "jo", base, error));
	Preferences chosen;
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences(chosen);
	ProjectSession session(platform, preferences);
	const SessionView &view = session.view();
	const std::string root = dir.file("game/expansions/onx");
	const ActionOutcome outcome =
	        editor_test::handle_to_end(session, request::new_expansion_project(root, "Night", "onx", "", true, "../../assets"));
	TEST_EXPECT(!outcome.refused && view.project.open);
	TEST_EXPECT(test_io::write_text(root + "/particles/effects.ptl", flags));
	editor_test::handle_to_end(session, request::open_project(root));
	const auto faults = [&]() {
		std::vector<Diagnostic> out;
		for (const Diagnostic &d : view.findings.diagnostics)
			if (d.code() == "particle.unfilled_token_slot") out.push_back(d);
		return out;
	};
	const std::vector<Diagnostic> found = faults();
	TEST_EXPECT(found.size() == 1 && found[0].asset == "particles/effects.ptl" && found[0].line == 4 &&
	            found[0].row() && found[0].row()->gates_build);
	TEST_EXPECT(test_io::write_text(root + "/particles/atable.ptl", table));
	editor_test::handle_to_end(session, request::rescan());
	session.run_operations();
	TEST_EXPECT(faults().empty());
	std::error_code gone;
	std::filesystem::remove(opennova::io::os_path(root + "/particles/atable.ptl"), gone);
	std::filesystem::remove(opennova::io::os_path(root + "/particles/effects.ptl"), gone);
	TEST_EXPECT(test_io::write_text(root + "/particles/aaa.ptl", "[particledef]\r\n{\r\n\tid = Other;\r\n}\r\n"));
	editor_test::handle_to_end(session, request::rescan());
	session.run_operations();
	const std::vector<Diagnostic> copied = faults();
	TEST_EXPECT(copied.size() == 1 && copied[0].asset.empty() && copied[0].row() && copied[0].row()->gates_build &&
	            copied[0].message.find("The base game's bbb.ptl, line 4:") == 0 &&
	            copied[0].message.find("the expansion's own aaa.ptl parsing in the base's files' places") !=
	                    std::string::npos);
	return 0;
}

// The expansion's text table, which every string lookup reads before the table it names (AssetGraph::
// text_override) [orig: TextResource_FindEntryBySectionAndKey @ 0x75D27B, TextResource_FindEntryByKey @
// 0x75D473]: a key of the section the lookup names, of any section for a flat lookup, found there first, even
// where the project lacks the table the lookup names; the table follows the expansion's name.
static int test_override_table() {
	test_temp::TempDir dir("opennova_editor_expansion_override");
	const std::string install = dir.file("install");
	TEST_EXPECT(make_install(install));
	Preferences chosen;
	chosen.game_install = install;
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences(chosen);
	ProjectSession session(platform, preferences);
	const SessionView &view = session.view();
	const std::string root = dir.file("own");
	const ActionOutcome outcome = editor_test::handle_to_end(session, request::new_expansion_project(root, "Own", "jxm"));
	TEST_EXPECT(!outcome.refused && view.project.open);
	const AssetEntry *made = view.project.scan->find("jxm.bin");
	TEST_EXPECT(made != nullptr);
	if (!made || !view.findings.graph) return 1;
	TEST_EXPECT(test_io::write_file(root + "/" + made->relative_path,
	                                     table_bytes({ { "WepDes", { "WEP_OVER" } }, { "Overlays", { "OVL_FLAT" } },
	                                                   { "Client", { "STRCLI01" } } })) &&
	            test_io::write_file(root + "/gametext.bin",
	                                     table_bytes({ { "WepDes", { "WEP_OVER", "WEP_BASE" } },
	                                                   { "Client", { "STRCLI01", "STRCLI04" } },
	                                                   { "Keys", { "Ctrl-" } } })) &&
	            test_io::write_file(root + "/keyhelp.bin",
	                                     table_bytes({ { "Keys", { "Ctrl-" } }, { "Text", { "CAMERA" } } })));
	editor_test::handle_to_end(session, request::rescan());
	const auto found_in = [&](const char *key, const char *scope, const char *table) {
		std::string file;
		return view.findings.graph->resolve(ReferenceKind::TextId, key, scope, &file) == ReferenceStatus::Present &&
		       opennova::strutil::iequals(std::filesystem::path(file).filename().string(), table);
	};
	TEST_EXPECT(view.findings.graph->text_override() == "JXM.BIN");
	TEST_EXPECT(found_in("WEP_OVER", "GAMETEXT.BIN/WepDes", "jxm.bin"));
	TEST_EXPECT(found_in("WEP_BASE", "GAMETEXT.BIN/WepDes", "gametext.bin"));
	TEST_EXPECT(found_in("OVL_FLAT", "GAMETEXT.BIN", "jxm.bin"));
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::TextId, "OVL_FLAT", "GAMETEXT.BIN/WepDes") ==
	            ReferenceStatus::Missing);
	TEST_EXPECT(found_in("WEP_OVER", "MEDMSSN.BIN/WepDes", "jxm.bin"));
	// The game's own code reads Client/STRCLI01 and Client/STRCLI04 by name (code_text_keys): the reads reach the
	// expansion's string where it defines one, gametext.bin's where it does not.
	const auto code_reads = [&](const char *key, const char *table) {
		for (const GraphSymbol *symbol : view.findings.graph->symbols_named(ReferenceKind::TextId, key))
			if (opennova::strutil::iequals(std::filesystem::path(symbol->file).filename().string(), table))
				return code_reads_of(*view.findings.graph, *symbol).size();
		return size_t(SIZE_MAX);
	};
	TEST_EXPECT(code_reads("STRCLI01", "jxm.bin") > 0 && code_reads("STRCLI01", "gametext.bin") == 0);
	TEST_EXPECT(code_reads("STRCLI04", "gametext.bin") > 0);
	TEST_EXPECT(code_reads("WEP_BASE", "gametext.bin") == 0);
	// The table as read from the binary, pinned: 557 rows of a function and a key (642 calls of the getters with
	// written keys), among them the yes/no prompt's key, an item's fallback text and Game.bin's class names.
	TEST_EXPECT(code_text_keys().size() == 557);
	const auto reads = [](const char *table, const char *section, const char *key) {
		return std::count_if(code_text_keys().begin(), code_text_keys().end(), [&](const CodeTextKey &row) {
			return std::string(row.table) == table && opennova::strutil::iequals(row.section, section) &&
			       opennova::strutil::iequals(row.key, key);
		});
	};
	TEST_EXPECT(reads("GAMETEXT.BIN", "KeyPress", "STRKEYPRESS_YES") == 1 && reads("GAMETEXT.BIN", "item", "text_default") == 1 &&
	            reads("GAME.BIN", "Menu", "CHARCLASS_ENGINEER") >= 1 && reads("GAMEERR.BIN", "MP Errors", "ERR5") >= 1);
	// The key-help getter reads keyhelp.bin, gating on gametext.bin's table alone [orig: KeyHelp_GetStringWithFallback
	// @ 0x51ED40, g_TextKeyHelp @ 0x51ED65]: its Keys and Text reads are keyhelp.bin's, never gametext.bin's, and a
	// string of the name in gametext.bin is read by none of them.
	TEST_EXPECT(reads("KEYHELP.BIN", "Keys", "Ctrl-") == 2 && reads("KEYHELP.BIN", "Text", "CAMERA") == 1 &&
	            reads("GAMETEXT.BIN", "Keys", "Ctrl-") == 0 && reads("GAMETEXT.BIN", "Text", "CAMERA") == 0);
	TEST_EXPECT(code_reads("Ctrl-", "keyhelp.bin") == 2 && code_reads("CAMERA", "keyhelp.bin") == 1 &&
	            code_reads("Ctrl-", "gametext.bin") == 0);
	// The name changed: the table of the new name is the one read first.
	ProjectSettingsChange renamed;
	renamed.expansion = "jxk";
	editor_test::apply_settings(session, renamed);
	TEST_EXPECT(view.project.document->expansion.name == "jxk" && view.findings.graph->text_override() == "JXK.BIN");
	TEST_EXPECT(found_in("WEP_OVER", "GAMETEXT.BIN/WepDes", "jxk.bin"));
	return 0;
}

// An expansion project's import of an installed expansion (2026-10-24): the expansion's own files alone (its two
// archives' and its folder's: no base game's file, none the folder holds that the game never reads there; its
// gate-tag override, gt.ssc, the folder's copy over the archived one, as the game reads it), each choice naming the
// expansion; its names renamed to the project's expansion; its version
// text only where the project is that expansion, which keeps it as the install has it. The import records the
// archive of the pair each came from (ProjectDocument::archives).
static int test_installed_expansion_import() {
	test_temp::TempDir dir("opennova_editor_expansion_import");
	const std::string root = dir.file("install");
	TEST_EXPECT(make_install(root));
	ProjectDocument document;
	std::vector<Diagnostic> diagnostics;
	// A standalone project imports no expansion; an install without it is refused, never the base game.
	TEST_EXPECT(list_installed_expansion_choices(root, document, "x1", diagnostics).empty() && diagnostics.size() == 1 &&
	            diagnostics[0].code() == "import.install");
	document.expansion = { "x1", "" };
	diagnostics.clear();
	TEST_EXPECT(list_installed_expansion_choices(root, document, "x2", diagnostics).empty() && diagnostics.size() == 1);
	const auto names_of = [](const std::vector<ImportChoice> &choices) {
		std::vector<std::string> names;
		for (const ImportChoice &choice : choices) names.push_back(choice.name());
		std::sort(names.begin(), names.end());
		return names;
	};
	diagnostics.clear();
	const std::vector<ImportChoice> own = list_installed_expansion_choices(root, document, "x1", diagnostics);
	TEST_EXPECT(diagnostics.empty());
	TEST_EXPECT(names_of(own) == (std::vector<std::string>{ "Mx1.bin", "Mx1.sbf", "gt.ssc", "header.bik", "shared.txt",
	                                                        "version.txt", "x1.bin", "x1L.lwf", "x1only.txt" }));
	for (const ImportChoice &choice : own) TEST_EXPECT(choice.install && choice.path == root && choice.expansion == "x1");
	// Another expansion's project gets the expansion's names as its own, and makes its own version text.
	ProjectDocument other = document;
	other.expansion = { "jxm", "" };
	TEST_EXPECT(names_of(list_installed_expansion_choices(root, other, "x1", diagnostics)) ==
	            (std::vector<std::string>{ "Mjxm.bin", "Mjxm.sbf", "gt.ssc", "header.bik", "jxm.bin", "jxmL.lwf",
	                                       "shared.txt", "x1only.txt" }));
	// Imported: the expansion's bytes, its folder's version text as the install has it (not the archived
	// copy), and the archive of each archived file recorded.
	ProjectDocument made;
	Diagnostic error;
	const std::string project = dir.file("x1");
	TEST_EXPECT(create_project(project, "X1", "jo", made, error, ProjectExpansion{ "x1", "" }));
	const ProjectPaths paths = ProjectPaths::for_root(project);
	const ImportResult result = import_assets(own, paths, made, true);
	for (const Diagnostic &d : result.diagnostics) std::fprintf(stderr, "%s: %s\n", d.code().c_str(), d.message.c_str());
	TEST_EXPECT(result.not_imported.empty() && result.imported.size() == own.size());
	const AssetScan scan = scan_project_assets(paths, made);
	std::string text, message;
	const auto text_in = [&](const char *name) {
		const AssetEntry *entry = scan.find(name);
		return entry && opennova::io::read_file_text(paths.root + "/" + entry->relative_path, text, message) ? text : "<none>";
	};
	TEST_EXPECT(text_in("version.txt") == "x1 version" && text_in("shared.txt") == "expansion" && text_in("x1.bin") == "x1 table");
	TEST_EXPECT(text_in("gt.ssc") == "loose config");
	TEST_EXPECT(result.archives.size() == 5 && result.archives.at("X1.BIN") == ArchiveSlot::Language &&
	            result.archives.at("X1L.LWF") == ArchiveSlot::Language && result.archives.at("MX1.BIN") == ArchiveSlot::Language &&
	            result.archives.at("SHARED.TXT") == ArchiveSlot::Localres &&
	            result.archives.at("X1ONLY.TXT") == ArchiveSlot::Localres && !result.archives.count("VERSION.TXT") &&
	            !result.archives.count("GT.SSC"));
	return 0;
}

int main() {
	int failures = 0;
	failures += test_name_rule();
	failures += test_project_expansion();
	failures += test_base_project_dir();
	failures += test_base_export_weighed();
	failures += test_session_on_base_project();
	failures += test_session_stock();
	failures += test_base_layer_session();
	failures += test_base_files_under_the_project();
	failures += test_base_string_tables();
	failures += test_base_detail_map();
	failures += test_base_particle_walk();
	failures += test_install_findings();
	failures += test_files_table();
	failures += test_name_forms_no_game_file();
	failures += test_install_view();
	failures += test_installed_expansion_import();
	failures += test_fold_follows_the_view();
	failures += test_session();
	failures += test_override_table();
	if (failures == 0) std::printf("editor_expansion: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
