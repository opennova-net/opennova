// The Problems core (ADR 0046 S11b): the query the window and the editor MCP share (each
// severity toggled, the text matched without case against each member it searches, the
// three scopes, the three groupings with their titles and counts, only the fixable
// findings, "shown of total", and the answer kept while the view and the query stand);
// the fixes for each finding that has them (with and without a factory, with and without
// the game data, several files to use, each with what its rename rewrites, a .png the game
// data has, no Create for a name that is taken or one the name rules refuse, each detail
// saying Undo cannot take it back) and none for the rest (no Rewrite for a file that does not
// serialize), kept while the view's revision stands; the bulk ones read without planning a
// rename (S11c: the window's Fix all); a Fix all merged from them; where a finding takes
// Problems (nowhere for a required file the project lacks, a catalog finding to its file and
// record, Files for a file the editor does not open); a missing texture's placeholder, applied
// over a real session (S11h); (S12) the places and fixes the findings that named no record
// or had no fix gained, applied over a real session; and (S13 V1) the findings by file and
// record (FindingsIndex), made again only when the view moves.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <editor/blank/blank_factory.h>
#include <editor/documents/document_types.h>
#include <editor/documents/mnu_document.h>
#include <editor/documents/texture_roles.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_queries.h>
#include <editor/graph/rename_transaction.h>
#include <editor/project/project_files.h>
#include <editor/project_build/build_plan.h>
#include <editor/project_build/build_run.h>
#include <editor/session/build_result.h>
#include <editor/session/finding_codes.h>
#include <editor/session/original_files.h>
#include <editor/session/view/findings_index.h>
#include <editor/session/preferences_store.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/problem_query.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/pff/pff.h>
#include <formats/rtxt/rtxt.h>
#include <runtime/renderer/texture_load_rules.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
namespace fs = std::filesystem;

namespace {

using editor_test::NoProcess;

// a.mnu: a button that opens screen B of b.mnu (an ACTION naming that file); b.mnu: that screen.
const char *const kMenuA =
        "<SCREEN>\r\n"
        "\t<NAME>A</NAME>\r\n"
        "\t<WINDOW type=\"window\" name=\"MAIN\">\r\n"
        "\t\t<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>\r\n"
        "\t\t<WINDOW type=\"button\" name=\"GO\">\r\n"
        "\t\t\t<ACTION type=\"SCREEN\" file=\"b.mnu\">B</ACTION>\r\n"
        "\t\t\t<APPEARANCE state=\"default\"></APPEARANCE>\r\n"
        "\t\t\t<POSITION><LEFT>10</LEFT><TOP>10</TOP></POSITION>\r\n"
        "\t\t</WINDOW>\r\n"
        "\t</WINDOW>\r\n"
        "</SCREEN>\r\n";
const char *const kMenuB =
        "<SCREEN>\r\n"
        "\t<NAME>B</NAME>\r\n"
        "\t<WINDOW type=\"window\" name=\"MAIN\">\r\n"
        "\t\t<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>\r\n"
        "\t</WINDOW>\r\n"
        "</SCREEN>\r\n";

// What the detail of every fix says (each acts on the files, which Undo does not reach).
const char *const kNotUndoable = "cannot be undone with Undo";

Diagnostic finding(DiagnosticSeverity severity, const char *code, const char *message, const char *asset = "",
                   const char *record = "", const char *field = "") {
	Diagnostic d = editor_test::finding_of(severity, code, message, asset, field);
	d.record = record;
	return d;
}

std::vector<size_t> rows_of(const ProblemQuery &query, const SessionView &view) {
	return answer_problems(query, view).rows;
}

std::vector<std::string> labels_of(const std::vector<ProblemFix> &fixes) {
	std::vector<std::string> labels;
	for (const ProblemFix &fix : fixes) labels.push_back(fix.label);
	return labels;
}

// The session's finding of `code` about the requirement `role`.
const Diagnostic *requirement_finding(const SessionView &view, const char *code, const char *role) {
	for (const Diagnostic &d : view.findings.diagnostics)
		if (d.code() == code && editor_test::requirement_of(d).role == role) return &d;
	return nullptr;
}

std::shared_ptr<const Document> menu_document(const editor_test::TempProjectDir &dir, const char *file, const char *relative) {
	auto document = std::make_shared<MnuDocument>();
	Diagnostic error;
	if (!editor_test::write_text(dir.file(file), kMenuB) ||
	    !document->load(dir.file(file), relative, AssetKind::Menu, "jo", error))
		return nullptr;
	return document;
}

} // namespace

// The query over a view of six findings in two open menus, a stylesheet and a catalog.
static int test_query() {
	editor_test::TempProjectDir dir("opennova_editor_problems_query");
	SessionView view;
	view.project.open = true;
	const auto a = menu_document(dir, "a.mnu", "menus/a.mnu");
	const auto b = menu_document(dir, "b.mnu", "menus/b.mnu");
	TEST_EXPECT(a && b);
	view.documents.open = {a, b};
	view.documents.active = "menus/a.mnu";
	Diagnostic font = finding(DiagnosticSeverity::Error, "reference.missing", "'MAIN' in b.mnu names the font 'nofont.fnt'.",
	                          "menus/b.mnu", "B/MAIN", "font.name");
	font.subject = ReferenceSubject{ReferenceKind::Font, "nofont.fnt"};
	Diagnostic required = finding(DiagnosticSeverity::Error, "requirement.missing", "Missing required file gametext.bin.");
	required.subject = RequirementSubject{"gametext", "gametext.bin"};
	view.findings.diagnostics = {
		finding(DiagnosticSeverity::Warning, "menu.duplicate_window", "Two windows are named GO.", "menus/a.mnu", "A/MAIN/GO", "name"),
		required,
		finding(DiagnosticSeverity::Info, "style.unused", "Nothing uses it.", "menus/menu_style.mns", "DEF_ONLY"),
		font,
		finding(DiagnosticSeverity::Error, "style.line_ending", "Line 3 ends LF.", "menus/menu_style.mns"),
		finding(DiagnosticSeverity::Warning, "catalog.ignored_input", "An unknown key.", "defs/items.def", "Marker", "subtype"),
	};
	// Every finding: errors, then warnings, then notes, each in the order reported.
	const ProblemAnswer all = answer_problems(ProblemQuery(), view);
	TEST_EXPECT(all.rows == std::vector<size_t>({1, 3, 4, 0, 5, 2}));
	TEST_EXPECT(all.errors == 3 && all.warnings == 2 && all.infos == 1 && all.total() == 6);
	TEST_EXPECT(!all.grouped && all.groups.empty());
	// Each severity off: the others shown, the counts still every finding's ("3 of 6").
	ProblemQuery query;
	query.errors = false;
	const ProblemAnswer no_errors = answer_problems(query, view);
	TEST_EXPECT(no_errors.rows == std::vector<size_t>({0, 5, 2}) && no_errors.total() == 6 && no_errors.errors == 3);
	query = ProblemQuery();
	query.warnings = false;
	TEST_EXPECT(rows_of(query, view) == std::vector<size_t>({1, 3, 4, 2}));
	query = ProblemQuery();
	query.infos = false;
	TEST_EXPECT(rows_of(query, view) == std::vector<size_t>({1, 3, 4, 0, 5}));
	// The text, without case, over the message, the file, the record, the field and the code.
	query = ProblemQuery();
	query.text = "TWO WINDOWS";
	TEST_EXPECT(rows_of(query, view) == std::vector<size_t>({0}));
	query.text = "Items.DEF";
	TEST_EXPECT(rows_of(query, view) == std::vector<size_t>({5}));
	query.text = "def_only";
	TEST_EXPECT(rows_of(query, view) == std::vector<size_t>({2}));
	query.text = "Font.Name";
	TEST_EXPECT(rows_of(query, view) == std::vector<size_t>({3}));
	query.text = "line_ENDING";
	TEST_EXPECT(rows_of(query, view) == std::vector<size_t>({4}));
	query.text = "nothing matches this";
	const ProblemAnswer none = answer_problems(query, view);
	TEST_EXPECT(none.rows.empty() && none.total() == 6);
	// The scopes: the active document's findings, the open documents', the project's.
	query = ProblemQuery();
	query.scope = ProblemScope::ActiveFile;
	TEST_EXPECT(rows_of(query, view) == std::vector<size_t>({0}));
	query.scope = ProblemScope::OpenFiles;
	TEST_EXPECT(rows_of(query, view) == std::vector<size_t>({3, 0}));
	query.scope = ProblemScope::Project;
	TEST_EXPECT(rows_of(query, view) == all.rows);
	// Only the fixable: the missing font (Create), the stylesheet (Rewrite); the requirement has
	// no row in this view to fix, and the catalog's ignored input none (a save keeps it).
	query.fixable = true;
	TEST_EXPECT(rows_of(query, view) == std::vector<size_t>({3, 4}));

	// By file: a group per file (the project's own findings first here), in the order its
	// first finding shows, the rows group after group.
	query = ProblemQuery();
	query.grouping = ProblemGrouping::File;
	const ProblemAnswer files = answer_problems(query, view);
	TEST_EXPECT(files.grouped && files.groups.size() == 5);
	if (files.groups.size() != 5) return 1;
	TEST_EXPECT(files.groups[0].key.empty() && files.groups[0].title == "Project" &&
	            files.groups[0].rows == std::vector<size_t>({1}));
	TEST_EXPECT(files.groups[1].key == "menus/b.mnu" && files.groups[1].title == "menus/b.mnu");
	TEST_EXPECT(files.groups[2].key == "menus/menu_style.mns" && files.groups[2].rows == std::vector<size_t>({4, 2}) &&
	            files.groups[2].errors == 1 && files.groups[2].warnings == 0 && files.groups[2].infos == 1);
	TEST_EXPECT(files.groups[3].key == "menus/a.mnu" && files.groups[4].key == "defs/items.def");
	TEST_EXPECT(files.rows == std::vector<size_t>({1, 3, 4, 2, 0, 5}));
	// By kind: the code's family, titled in plain words (a family the table lacks, its own name).
	view.findings.diagnostics.push_back(finding(DiagnosticSeverity::Warning, "rename.exists", "The project already has x.tga."));
	view.revisions.touch(ViewConcern::Findings);
	query.grouping = ProblemGrouping::Kind;
	const ProblemAnswer kinds = answer_problems(query, view);
	std::vector<std::string> titles;
	for (const ProblemGroup &group : kinds.groups) titles.push_back(group.title);
	TEST_EXPECT(titles == std::vector<std::string>({"Required files", "Missing references", "Stylesheets", "Menus",
	                                                "Catalogs", "Renames"}));
	TEST_EXPECT(kinds.groups[2].key == "style" && kinds.groups[2].rows.size() == 2 && kinds.groups[5].key == "rename");
	TEST_EXPECT(kinds.rows == std::vector<size_t>({1, 3, 4, 2, 0, 5, 6}) && kinds.total() == 7);
	// A group's title is its row's group's (the families the old table did not name have one too).
	TEST_EXPECT(std::string(finding_group_title(finding_row("reference.missing")->group)) == "Missing references" &&
	            std::string(finding_group_title(finding_row("animation_map.row")->group)) == "Animation maps" &&
	            std::string(finding_group_title(finding_row("rename.site")->group)) == "Renames");
	// Grouped and filtered: the groups hold what is shown ("4 of 7").
	query.errors = false;
	const ProblemAnswer filtered = answer_problems(query, view);
	TEST_EXPECT(filtered.rows.size() == 4 && filtered.total() == 7 && filtered.groups.size() == 4);

	// The answer kept while what it reads (the findings) and the query stand (a change no counter
	// marks is not seen), asked again when either moves; each one made counted (its generation,
	// which a reader of the answer follows).
	ProblemQueryCache cache;
	TEST_EXPECT(cache.answer(ProblemQuery(), view).rows.size() == 7);
	const uint64_t made = cache.generation();
	view.findings.diagnostics.pop_back();
	TEST_EXPECT(cache.answer(ProblemQuery(), view).rows.size() == 7 && cache.generation() == made);
	view.revisions.touch(ViewConcern::Findings);
	TEST_EXPECT(cache.answer(ProblemQuery(), view).rows.size() == 6 &&
	            cache.generation() == made + 1);
	TEST_EXPECT(cache.answer(query, view).rows.size() == 3 && cache.generation() == made + 2);
	return 0;
}

// The fixes over a real session: a project with two spare string tables and two menus (b.mnu
// named by a.mnu), a fake game install that has some of what the project lacks.
static int test_fixes() {
	editor_test::TempProjectDir dir("opennova_editor_problems_fixes");
	// The install: the boot table's resource.pff.
	const std::string install = dir.file("install");
	std::vector<uint8_t> table;
	Diagnostic error;
	BlankRequest blank;
	blank.logical_name = "gameerr.bin";
	TEST_EXPECT(make_blank(blank, AssetKind::Strings, table, error));
	const uint8_t bytes[] = {'d', 'a', 't', 'a'};
	const opennova::pff::PffWriteEntry entries[] = {
		{"gameerr.bin", table.data(), uint32_t(table.size()), 0, 0, 0},
		{"brand.mns", bytes, sizeof(bytes), 0, 0, 0},
		{"Custom.fnt", bytes, sizeof(bytes), 0, 0, 0},
		{"logo.dds", bytes, sizeof(bytes), 0, 0, 0},
		{"badge.png", bytes, sizeof(bytes), 0, 0, 0},
	};
	TEST_EXPECT(editor_test::write_text(install + "/readme.txt", "an install"));
	TEST_EXPECT(opennova::pff::pff_write_archive((install + "/resource.pff").c_str(), opennova::pff::PFF_FORMAT_PFF3, entries, 5) ==
	            opennova::pff::PFF_WRITE_OK);

	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Fixes"));
	const SessionView &v = session.view();
	const std::string root = v.project.root;
	blank.logical_name = "spare.bin";
	TEST_EXPECT(make_blank(blank, AssetKind::Strings, table, error));
	TEST_EXPECT(editor_test::write_bytes(root + "/strings/spare.bin", table) && editor_test::write_bytes(root + "/strings/other.bin", table));
	TEST_EXPECT(editor_test::write_text(root + "/menus/a.mnu", kMenuA) && editor_test::write_text(root + "/menus/b.mnu", kMenuB));
	TEST_EXPECT(editor_test::write_text(root + "/art/splash.tga", "tga") && editor_test::write_text(root + "/art/splash.pcx", "pcx"));
	TEST_EXPECT(editor_test::write_text(root + "/foo.bin", "raw bytes")); // a .bin that is no string table
	editor_test::handle_to_end(session, request::rescan());

	// A required file with a factory, and no game data: Create it (placeholder content, with
	// the others in a Fix all), or Use a string table of the project as it (never in bulk),
	// each naming what its rename does. main.mnu and the other required files are not offered.
	const Diagnostic *gameerr = requirement_finding(v, "requirement.missing", "gameerr");
	TEST_EXPECT(gameerr != nullptr);
	if (!gameerr) return 1;
	std::vector<ProblemFix> fixes = fixes_for(*gameerr, v);
	TEST_EXPECT(labels_of(fixes) ==
	            std::vector<std::string>({"Create gameerr.bin", "Use other.bin as gameerr.bin", "Use spare.bin as gameerr.bin"}));
	if (fixes.size() != 3) return 1;
	TEST_EXPECT(fixes[0].bulk && fixes[0].request.kind == EditorRequestKind::CreateMissing &&
	            fixes[0].request.roles == std::vector<std::string>({"gameerr"}));
	// Every fix acts on the files: its detail says Undo cannot take it back.
	TEST_EXPECT(fixes[0].detail.find("placeholder") != std::string::npos &&
	            fixes[0].detail.find(kNotUndoable) != std::string::npos);
	TEST_EXPECT(!fixes[1].bulk && fixes[1].request.kind == EditorRequestKind::AssignRequirement &&
	            fixes[1].request.role == "gameerr" && fixes[1].request.path == "strings/other.bin");
	TEST_EXPECT(fixes[1].detail == "Renames other.bin to gameerr.bin; nothing refers to it. It cannot be undone with Undo.");
	TEST_EXPECT(has_fixes(*gameerr, v));
	// With the game data it has, the game's own copy comes first (the import dialog on that one file,
	// planned with the files it needs as the editor's setting says): Fix all and the summary take a
	// finding's first fix, and a placeholder makes a game that starts empty. The placeholder is the
	// second, said as such.
	editor_test::set_game_install(session, install);
	gameerr = requirement_finding(v, "requirement.missing", "gameerr");
	TEST_EXPECT(gameerr != nullptr);
	if (!gameerr) return 1;
	fixes = fixes_for(*gameerr, v);
	TEST_EXPECT(labels_of(fixes) == std::vector<std::string>({"Import gameerr.bin from the game data...",
	                                                          "Create a placeholder gameerr.bin",
	                                                          "Use other.bin as gameerr.bin", "Use spare.bin as gameerr.bin"}));
	if (fixes.size() != 4) return 1;
	TEST_EXPECT(fixes[0].bulk && fixes[0].request.kind == EditorRequestKind::PreviewInstallImport &&
	            fixes[0].request.names == std::vector<std::string>({"gameerr.bin"}) && fixes[0].request.with_dependencies);
	TEST_EXPECT(fixes[0].detail.find(kNotUndoable) != std::string::npos);
	TEST_EXPECT(fixes[1].bulk && fixes[1].request.kind == EditorRequestKind::CreateMissing &&
	            fixes[1].detail.rfind("Instead of the game's own: ", 0) == 0);
	// A mission's required menu the game data lacks (missions on: cmap.mnu): Create it, or use a
	// menu of the project as it, each saying what its rename rewrites (b.mnu: the ACTION of a.mnu
	// that names it).
	editor_test::set_missions(session, true);
	const Diagnostic *cmap = requirement_finding(v, "requirement.missing", "cmap_menu");
	TEST_EXPECT(cmap != nullptr);
	if (!cmap) return 1;
	fixes = fixes_for(*cmap, v);
	TEST_EXPECT(labels_of(fixes) ==
	            std::vector<std::string>({"Create cmap.mnu", "Use a.mnu as cmap.mnu", "Use b.mnu as cmap.mnu"}));
	if (fixes.size() != 3) return 1;
	TEST_EXPECT(fixes[0].bulk && fixes[0].request.kind == EditorRequestKind::CreateMissing &&
	            fixes[0].request.roles == std::vector<std::string>({"cmap_menu"}));
	TEST_EXPECT(fixes[1].detail == "Renames a.mnu to cmap.mnu; nothing refers to it. It cannot be undone with Undo.");
	TEST_EXPECT(fixes[2].detail ==
	            "Renames b.mnu to cmap.mnu and rewrites 1 reference in 1 file. It cannot be undone with Undo.");
	editor_test::set_missions(session, false);
	// An optional file the project lacks: its note offers the same (a factory, the game data).
	const Diagnostic *brand = requirement_finding(v, "requirement.optional_missing", "brand_style");
	TEST_EXPECT(brand != nullptr && brand->severity == DiagnosticSeverity::Info);
	if (!brand) return 1;
	TEST_EXPECT(labels_of(fixes_for(*brand, v)) ==
	            std::vector<std::string>({"Import brand.mns from the game data...", "Create a placeholder brand.mns"}));
	// S11e: an optional file is made or imported, never taken from another file: no Use for
	// loading.pcx though the project has a splash.pcx (and no factory or game data has one).
	const Diagnostic *loading = requirement_finding(v, "requirement.optional_missing", "loading_pcx");
	TEST_EXPECT(loading && fixes_for(*loading, v).empty() && !has_fixes(*loading, v));
	// The game's boot report of a required file: the fixes of its requirement; none once the
	// project has the file (the row is then only a place to look).
	Diagnostic boot = editor_test::finding_of(DiagnosticSeverity::Error, "play.boot_missing", "The game could not find gameerr.bin.");
	boot.subject = RequirementSubject{"gameerr", "gameerr.bin"};
	gameerr = requirement_finding(v, "requirement.missing", "gameerr");
	TEST_EXPECT(gameerr && labels_of(fixes_for(boot, v)) == labels_of(fixes_for(*gameerr, v)));
	EditorRequest create = request::create_missing({"gameerr"});
	editor_test::handle_to_end(session, create);
	TEST_EXPECT(session.outcome().done() && fixes_for(boot, v).empty() && !has_fixes(boot, v));
	Diagnostic unknown = boot;
	editor_test::own_requirement(unknown).role.clear();
	TEST_EXPECT(fixes_for(unknown, v).empty());

	// A missing reference to a file: Import a name its loader reads from the game data, then
	// Create it blank when its kind has a free-form factory (not in bulk).
	Diagnostic font = editor_test::finding_of(DiagnosticSeverity::Error, "reference.missing", "The font is missing.", "menus/a.mnu",
	                                          "font.name");
	font.subject = ReferenceSubject{ReferenceKind::Font, "Custom.fnt"};
	fixes = fixes_for(font, v);
	TEST_EXPECT(labels_of(fixes) == std::vector<std::string>({"Import Custom.fnt from the game data...", "Create Custom.fnt"}));
	if (fixes.size() != 2) return 1;
	TEST_EXPECT(fixes[0].bulk && fixes[0].request.names == std::vector<std::string>({"Custom.fnt"}));
	TEST_EXPECT(!fixes[1].bulk && fixes[1].request.kind == EditorRequestKind::CreateFile &&
	            fixes[1].request.path == "Custom.fnt" && fixes[1].request.file_kind == "font");
	TEST_EXPECT(fixes[1].detail.find(kNotUndoable) != std::string::npos);
	// A menu texture: the .dds its .tga falls back to, then (S11h) a placeholder as its .tga.
	Diagnostic texture = font;
	texture.subject = ReferenceSubject{ReferenceKind::MenuTexture, "logo.tga"};
	TEST_EXPECT(labels_of(fixes_for(texture, v)) ==
	            std::vector<std::string>({"Import logo.dds from the game data...", "Create a placeholder logo.tga"}));
	// A .png the menu names: a texture as the scan lists a PNG with no import record, so the
	// game data's is offered; no placeholder is a .png.
	editor_test::own_reference(texture).target = "badge.png";
	TEST_EXPECT(labels_of(fixes_for(texture, v)) == std::vector<std::string>({"Import badge.png from the game data..."}));
	// A model's texture row (S11f): the file its type's loader reads from what the game data
	// has, a diffuse row's .dds sibling; a plain row (type 1) reads its own name alone, which
	// the game data lacks; so does a texture whose use's loader is not witnessed (ADR 0046 S18: no
	// twin of another extension), and one of a role by its loader (a sky map's archive loader, the
	// .dds beside it first); each takes a placeholder as the name it opens once that is there (S11h).
	Diagnostic skin = font;
	skin.subject = ReferenceSubject{ReferenceKind::Texture, "logo.tga", std::string(), 0};
	TEST_EXPECT(labels_of(fixes_for(skin, v)) ==
	            std::vector<std::string>({"Import logo.dds from the game data...", "Create a placeholder logo.tga"}));
	editor_test::own_reference(skin).loader_arg = 1;
	TEST_EXPECT(labels_of(fixes_for(skin, v)) == std::vector<std::string>({"Create a placeholder logo.tga"}));
	editor_test::own_reference(skin).loader_arg = -1;
	TEST_EXPECT(labels_of(fixes_for(skin, v)) == std::vector<std::string>({"Create a placeholder logo.tga"}));
	editor_test::own_reference(skin).loader_arg = texture_role_arg(TextureRoleId::SkyCloud);
	TEST_EXPECT(labels_of(fixes_for(skin, v)) ==
	            std::vector<std::string>({"Import logo.dds from the game data...", "Create a placeholder logo.tga"}));
	editor_test::own_reference(skin).loader_arg = -1;
	TEST_EXPECT(labels_of(fixes_for(skin, v)) == std::vector<std::string>({"Create a placeholder logo.tga"}));
	// A name the game data lacks with no factory, and a symbol: nothing to do but look.
	Diagnostic model = font;
	model.subject = ReferenceSubject{ReferenceKind::Model, "tank"};
	Diagnostic text_id = font;
	text_id.subject = ReferenceSubject{ReferenceKind::TextId, "NO_SUCH_ID"};
	TEST_EXPECT(fixes_for(model, v).empty() && fixes_for(text_id, v).empty() && !has_fixes(text_id, v));
	// No Create for a name another kind of file holds (it would only open, the reference still
	// missing), nor for one the project's name rules refuse (past the archive's 16 bytes).
	Diagnostic table_ref = font;
	table_ref.subject = ReferenceSubject{ReferenceKind::TextTable, "foo.bin"};
	TEST_EXPECT(v.project.scan->find("foo.bin") &&
			v.project.scan->find("foo.bin")->kind == AssetKind::RawBin);
	TEST_EXPECT(fixes_for(table_ref, v).empty() && !has_fixes(table_ref, v));
	editor_test::own_reference(table_ref).target = "freshtable.bin";
	TEST_EXPECT(labels_of(fixes_for(table_ref, v)) == std::vector<std::string>({"Create freshtable.bin"}));
	Diagnostic long_menu = font;
	long_menu.subject = ReferenceSubject{ReferenceKind::Menu, "averyveryverylongname.mnu"};
	TEST_EXPECT(fixes_for(long_menu, v).empty());

	// A missing import output: import its source again.
	const Diagnostic output = editor_test::finding_of(DiagnosticSeverity::Warning, "import.output_missing", "logo.pcx has not been made.",
	                                                  "art/logo.png");
	fixes = fixes_for(output, v);
	TEST_EXPECT(fixes.size() == 1 && fixes[0].label == "Import logo.png again" && fixes[0].bulk);
	if (fixes.size() != 1) return 1;
	TEST_EXPECT(fixes[0].request.kind == EditorRequestKind::Reimport && fixes[0].request.path == "art/logo.png" &&
	            fixes[0].request.force && fixes[0].detail.find(kNotUndoable) != std::string::npos);
	// Input a rewrite drops or normalizes: Rewrite the file (a Save of it), saying what goes. Every
	// Rewrite row of the tables, the editor's and the types' (S13 A6: the five the fixes named
	// before, each saying what its row's rewrite_does says); none while a finding of the file
	// says it does not serialize (a blocks_save row's).
	std::vector<const FindingCodeRow *> rewrites, blockers;
	for (const NamedFindingTable &table : finding_tables())
		for (const FindingCodeRow &row : table.rows) {
			if (row.fixes == FindingFix::Rewrite) rewrites.push_back(&row);
			if (row.blocks_save) blockers.push_back(&row);
		}
	TEST_EXPECT(blockers.size() == 15);
	std::vector<std::string> rewrite_tokens;
	for (const FindingCodeRow *row : rewrites) rewrite_tokens.push_back(row->token);
	std::sort(rewrite_tokens.begin(), rewrite_tokens.end());
	TEST_EXPECT(rewrite_tokens == std::vector<std::string>({"animation_map.ignored_input",
	                                                        "credits.line_ending", "hud_layout.line_ending", "menu.ignored_input",
	                                                        "mission.event_order", "mission.rewrite_differs",
	                                                        "script.line_ending", "shader.form",
	                                                        "sound_bank.ignored_input",
	                                                        "strings.regrouped", "style.line_ending"}));
	for (const FindingCodeRow *row : rewrites) {
		const Diagnostic rewrite = make_finding(*row, DiagnosticSeverity::Warning, "A rewrite fixes this.", "menus/a.mnu");
		fixes = fixes_for(rewrite, v);
		TEST_EXPECT(fixes.size() == 1 && fixes[0].label == "Rewrite a.mnu" && fixes[0].bulk);
		if (fixes.size() != 1) return 1;
		TEST_EXPECT(fixes[0].request.kind == EditorRequestKind::Save && fixes[0].request.path == "menus/a.mnu");
		TEST_EXPECT(fixes[0].detail == std::string("Writes menus/a.mnu again ") + row->rewrite_does + "." +
		                                       " It cannot be undone with Undo." &&
		            fixes[0].detail.find(kNotUndoable) != std::string::npos);
		// None while any finding of the file says it does not serialize (every blocks_save row's).
		for (const FindingCodeRow *blocker : blockers) {
			SessionView blocked = v;
			blocked.findings.diagnostics.push_back(
			        make_finding(*blocker, DiagnosticSeverity::Error, "It cannot be written.", "menus/a.mnu"));
			blocked.revisions.touch(ViewConcern::Findings);
			TEST_EXPECT(fixes_for(rewrite, blocked).empty() && !has_fixes(rewrite, blocked));
		}
	}
	// Open with unsaved edits, the rewrite saves them too, and says so.
	editor_test::handle_to_end(session, request::open_document("menus/a.mnu"));
	Document *menu = session.document_for("menus/a.mnu");
	NodeAddress go;
	TEST_EXPECT(menu && find_definition(AssetGraph(), *menu, "GO", go));
	if (!menu) return 1;
	EditorRequest edit = request::edit_record(menu->path(), Edit());
	edit.edits[0].address = go;
	edit.edits[0].field = "position.left";
	edit.edits[0].value = int64_t(20);
	editor_test::handle_to_end(session, edit);
	TEST_EXPECT(menu->dirty());
	const Diagnostic ending = editor_test::finding_of(DiagnosticSeverity::Error, "style.line_ending", "Line 3 ends LF.", "menus/a.mnu");
	TEST_EXPECT(fixes_for(ending, v).front().detail.find("unsaved edits") != std::string::npos);
	// Anything else has none; has_fixes agrees with fixes_for on every finding of the session,
	// and bulk_fixes_for (which plans no rename) gives fixes_for's bulk ones, in order.
	const Diagnostic other = editor_test::finding_of(DiagnosticSeverity::Error, "menu.duplicate_window", "Two windows.", "menus/a.mnu");
	TEST_EXPECT(fixes_for(other, v).empty() && bulk_fixes_for(other, v).empty());
	size_t bulk_seen = 0;
	for (const Diagnostic &d : v.findings.diagnostics) {
		TEST_EXPECT(has_fixes(d, v) == !fixes_for(d, v).empty());
		std::vector<std::string> bulk;
		for (const ProblemFix &fix : fixes_for(d, v))
			if (fix.bulk) bulk.push_back(fix.label + "\n" + fix.detail);
		std::vector<std::string> cheap;
		for (const ProblemFix &fix : bulk_fixes_for(d, v)) cheap.push_back(fix.label + "\n" + fix.detail);
		TEST_EXPECT(cheap == bulk);
		bulk_seen += bulk.size();
	}
	TEST_EXPECT(bulk_seen > 0);
	// No project, no fixes.
	editor_test::handle_to_end(session, request::undo(menu->path()));
	editor_test::handle_to_end(session, request::close_project());
	TEST_EXPECT(fixes_for(font, v).empty() && fixes_for(boot, v).empty());
	return 0;
}

// No Rewrite for a file whose own finding says it does not serialize (its Save is refused):
// fixes_for, has_fixes, bulk_fixes_for and the query's fixable filter agree; another file's
// input that a rewrite drops keeps its Rewrite.
static int test_rewrite_unserializable() {
	SessionView view;
	view.project.open = true;
	view.findings.diagnostics = {finding(DiagnosticSeverity::Warning, "menu.ignored_input", "A key the game ignores.", "menus/a.mnu"),
	                    finding(DiagnosticSeverity::Error, "menu.unserializable", "It cannot be written.", "menus/a.mnu"),
	                    finding(DiagnosticSeverity::Warning, "menu.ignored_input", "A key the game ignores.", "menus/b.mnu"),
	                    finding(DiagnosticSeverity::Warning, "strings.regrouped", "A section read twice.", "strings/menu.bin"),
	                    finding(DiagnosticSeverity::Error, "strings.invalid_input", "A string it cannot hold.", "strings/menu.bin")};
	for (const size_t blocked : {size_t(0), size_t(3)}) {
		const Diagnostic &d = view.findings.diagnostics[blocked];
		TEST_EXPECT(fixes_for(d, view).empty() && !has_fixes(d, view) && bulk_fixes_for(d, view).empty());
	}
	TEST_EXPECT(labels_of(fixes_for(view.findings.diagnostics[2], view)) == std::vector<std::string>({"Rewrite b.mnu"}));
	TEST_EXPECT(bulk_fixes_for(view.findings.diagnostics[2], view).size() == 1 && has_fixes(view.findings.diagnostics[2], view));
	ProblemQuery fixable;
	fixable.fixable = true;
	const ProblemAnswer answer = answer_problems(fixable, view);
	TEST_EXPECT(answer.rows == std::vector<size_t>({2}) && answer.total() == 5);
	return 0;
}

// The fixes kept while what they read stands (a Use fix plans a rename; the window and the
// editor MCP ask again and again): asked again, the same answer, even when the view changed
// without a counter moving; asked after the findings' counter moves, the fixes of now, the
// cache's generation (which a reader of the fixes follows) moving once.
static int test_fix_cache() {
	SessionView view;
	view.project.open = true;
	view.findings.diagnostics = {finding(DiagnosticSeverity::Error, "style.line_ending", "Line 3 ends LF.", "menus/menu_style.mns"),
	                    finding(DiagnosticSeverity::Warning, "menu.duplicate_window", "Two windows.", "menus/a.mnu")};
	ProblemFixCache cache;
	const uint64_t started = cache.generation(view);
	const std::vector<ProblemFix> *first = &cache.fixes(view, 0);
	TEST_EXPECT(first->size() == 1 && (*first)[0].label == "Rewrite menu_style.mns");
	TEST_EXPECT(cache.fixes(view, 1).empty() && cache.fixes(view, 7).empty());
	TEST_EXPECT(&cache.fixes(view, 0) == first);
	// The finding changed in place, unmarked: still the answer kept.
	const Diagnostic was = view.findings.diagnostics[0];
	view.findings.diagnostics[0] =
	        make_finding(finding_code(MenuFinding::DuplicateScreen), was.severity, was.message, was.asset, was.field);
	TEST_EXPECT(cache.fixes(view, 0).size() == 1 && cache.generation(view) == started);
	view.revisions.touch(ViewConcern::Findings);
	TEST_EXPECT(cache.fixes(view, 0).empty() && cache.generation(view) == started + 1);
	return 0;
}

// A Fix all: the bulk fixes merged, in the order they come: one CreateMissing naming each
// role once, one game-data list naming each file once, every other request once; a fix
// that never runs in bulk is left out.
static int test_merge() {
	const auto fix = [](EditorRequest request, bool bulk) { return ProblemFix{"", "", std::move(request), bulk}; };
	const EditorRequest menu = request::create_missing({"main_menu"});
	const EditorRequest text = request::create_missing({"gametext", "main_menu"});
	const EditorRequest listed = request::preview_install_import({"MAIN.MNU"});
	const EditorRequest fonts = request::preview_install_import({"Arial14b.fnt"});
	const EditorRequest again = request::reimport("art/logo.png", true);
	const std::vector<ProblemFix> fixes = {
		fix(request::save("defs/items.def"), true),
		fix(menu, true),
		fix(listed, true),
		fix(request::assign_requirement("main_menu", "menus/a.mnu"), false),
		fix(text, true),
		fix(request::save("defs/items.def"), true),
		fix(again, true),
		fix(fonts, true),
		fix(request::save("menus/menu_style.mns"), true),
		fix(request::create_file("Custom.fnt", "font"), false),
		fix(again, true),
	};
	const std::vector<EditorRequest> merged = merge_fixes(fixes);
	TEST_EXPECT(merged.size() == 5);
	if (merged.size() != 5) return 1;
	TEST_EXPECT(merged[0].kind == EditorRequestKind::Save && merged[0].path == "defs/items.def");
	TEST_EXPECT(merged[1].kind == EditorRequestKind::CreateMissing &&
	            merged[1].roles == std::vector<std::string>({"main_menu", "gametext"}));
	// The import's preview last: an operation over the project's files, which a request after it would find busy.
	TEST_EXPECT(merged[2].kind == EditorRequestKind::Reimport && merged[2].path == "art/logo.png" && merged[2].force);
	TEST_EXPECT(merged[3].kind == EditorRequestKind::Save && merged[3].path == "menus/menu_style.mns");
	TEST_EXPECT(merged[4].kind == EditorRequestKind::PreviewInstallImport &&
	            merged[4].names == std::vector<std::string>({"MAIN.MNU", "Arial14b.fnt"}));
	TEST_EXPECT(merge_fixes({}).empty() && merge_fixes({fix(menu, false)}).empty());
	return 0;
}

// Where a finding takes Problems: a file of the project the editor opens, at the record and
// field the finding names; Files for a file of a kind with no editor and for a file's name or
// place (S12); nowhere for a required file the project lacks, or a name the scan does not
// list as a path.
static int test_location() {
	SessionView view;
	view.project.open = true;
	const auto entry = [](const char *name, const char *path, AssetKind kind) {
		AssetEntry asset;
		asset.logical_name = name;
		asset.relative_path = path;
		asset.kind = kind;
		return asset;
	};
	editor_test::own(view.project.scan).entries = {entry("items.def", "defs/items.def", AssetKind::ItemDefs), entry("logo.png", "art/logo.png", AssetKind::ImportSource),
	                     entry("Arial14b.fnt", "fonts/Arial14b.fnt", AssetKind::Font)};
	editor_test::own(view.project.scan).index();
	Diagnostic required = editor_test::finding_of(DiagnosticSeverity::Error, "requirement.missing", "Missing required file main.mnu.");
	required.subject = RequirementSubject{"main_menu", "main.mnu"};
	TEST_EXPECT(problem_location(required, view).empty());
	Diagnostic catalog = editor_test::finding_of(DiagnosticSeverity::Error, "catalog.item_type", "Choose an item type.", "defs/items.def", "type");
	catalog.record = "Marker";
	catalog.row_id = 4;
	catalog.record_kind = 2;
	const ProblemLocation opened = problem_location(catalog, view);
	TEST_EXPECT(opened.path == "defs/items.def" && opened.record == (NodeAddress{4, 2, 0}) && opened.field == "type" &&
	            !opened.in_files);
	const EditorRequest open = opened.request();
	TEST_EXPECT(open.kind == EditorRequestKind::OpenDocument && open.path == "defs/items.def" &&
	            open.address == (NodeAddress{4, 2, 0}) && open.field == "type" && open.locator.empty());
	const ProblemLocation file = problem_location(editor_test::finding_of(DiagnosticSeverity::Warning, "catalog.ignored_input",
	                                                                      "An unknown key.", "defs/items.def", "subtype"),
	                                              view);
	TEST_EXPECT(file.path == "defs/items.def" && file.record == NodeAddress() && file.field.empty());
	// S12: a file of a kind the editor does not open is shown in Files (ShowInFiles), as is a
	// file whose name or place is the finding, one the editor opens too; a name the scan does
	// not list as a path goes nowhere.
	for (const char *asset : {"fonts/Arial14b.fnt", "art/logo.png"}) {
		const ProblemLocation shown = problem_location(editor_test::finding_of(DiagnosticSeverity::Warning, "graph.unreadable", "A finding.", asset), view);
		TEST_EXPECT(shown.path == asset && shown.in_files && shown.request().kind == EditorRequestKind::ShowInFiles &&
		            shown.request().path == asset && !shown.request().ask_name);
	}
	// The rows about the file as a whole (FindingPlace::File: S13 A6, the four the location named
	// before; S16 a root-only file an expansion's build leaves out, a file its expansion setting leaves
	// unread, a mission the game's list shows untitled or twice, a NovaWorld screen the game never reads).
	std::vector<const FindingCodeRow *> file_rows;
	std::vector<std::string> about_files;
	for (const NamedFindingTable &table : finding_tables())
		for (const FindingCodeRow &row : table.rows)
			if (row.place == FindingPlace::File) {
				file_rows.push_back(&row);
				about_files.push_back(row.token);
			}
	TEST_EXPECT(about_files == std::vector<std::string>({"asset.name.duplicate", "asset.name.empty", "asset.name.too_long",
	                                                     "build.archive_in_project", "build.expansion.mission_twice",
	                                                     "build.expansion.mission_untitled", "build.expansion.root_only",
	                                                     "build.name_unstorable", "build.unread", "expansion.file.unread"}));
	for (const FindingCodeRow *row : file_rows) {
		Diagnostic named = make_finding(*row, catalog.severity, catalog.message, catalog.asset, catalog.field);
		named.record = catalog.record;
		named.row_id = catalog.row_id;
		named.record_kind = catalog.record_kind;
		const ProblemLocation shown = problem_location(named, view);
		TEST_EXPECT(shown.path == "defs/items.def" && shown.in_files && shown.record == NodeAddress() &&
		            shown.request().kind == EditorRequestKind::ShowInFiles);
	}
	for (const char *asset : {"defs/weapon.def", "items.def"})
		TEST_EXPECT(problem_location(editor_test::finding_of(DiagnosticSeverity::Error, "graph.unreadable", "A finding.", asset), view).empty());
	return 0;
}

// S11e: Use (a file of the project renamed into place) is for a file the game cannot start
// without: a required row offers a file of the kind and the name's extension (a rename keeps
// it, so a .tga is no .pcx), an optional one none, the same file there.
static int test_use_required_only() {
	SessionView view;
	view.project.open = true;
	const auto row = [](const char *role, const char *name, bool required) {
		RequirementRow out;
		out.role = role;
		out.name = name;
		out.required = required;
		out.expected_kind = AssetKind::Texture;
		out.state = RequirementState::Missing;
		return out;
	};
	editor_test::own(view.project.requirements).rows = {row("test_screen", "screen.pcx", true), row("test_splash", "splash.pcx", false)};
	for (const char *name : {"art.tga", "art.pcx"}) {
		AssetEntry entry;
		entry.logical_name = name;
		entry.relative_path = std::string("art/") + name;
		entry.kind = AssetKind::Texture;
		editor_test::own(view.project.scan).entries.push_back(entry);
	}
	editor_test::own(view.project.scan).index();
	Diagnostic required = finding(DiagnosticSeverity::Error, "requirement.missing", "Missing required file screen.pcx.");
	required.subject = RequirementSubject{"test_screen", "screen.pcx"};
	Diagnostic optional = finding(DiagnosticSeverity::Info, "requirement.optional_missing", "Optional file splash.pcx.");
	optional.subject = RequirementSubject{"test_splash", "splash.pcx"};
	view.findings.diagnostics = {required, optional};
	TEST_EXPECT(labels_of(fixes_for(required, view)) == std::vector<std::string>({"Use art.pcx as screen.pcx"}));
	TEST_EXPECT(fixes_for(optional, view).empty() && !has_fixes(optional, view) && bulk_fixes_for(optional, view).empty());
	return 0;
}

// S11e: what the fixes read of all the findings (the files that do not serialize) is found
// once per view and handed to each ask: the answers are the ones an ask finds alone, the
// fix cache's and the query's (only the fixable) among them, over a view of thousands of
// findings, one of whose files does not serialize.
static int test_fix_index() {
	SessionView view;
	view.project.open = true;
	for (size_t i = 0; i < 3000; ++i) {
		const std::string file = "menus/f" + std::to_string(i % 30) + ".mnu";
		view.findings.diagnostics.push_back(finding(DiagnosticSeverity::Warning, "menu.ignored_input", "An unknown key.", file.c_str()));
	}
	view.findings.diagnostics.push_back(finding(DiagnosticSeverity::Error, "menu.unserializable", "It cannot be written.", "menus/f7.mnu"));
	const ProblemFixIndex index(view);
	TEST_EXPECT(index.unserializable.size() == 1 && index.unserializable.count("menus/f7.mnu"));
	ProblemFixCache cache;
	for (size_t i : {size_t(0), size_t(7), size_t(8), size_t(37), size_t(2999), size_t(3000)}) {
		const Diagnostic &d = view.findings.diagnostics[i];
		const bool fixable = d.asset != "menus/f7.mnu" && d.code() == "menu.ignored_input";
		TEST_EXPECT(has_fixes(d, view) == fixable && has_fixes(d, view, &index) == fixable);
		TEST_EXPECT(labels_of(fixes_for(d, view, &index)) == labels_of(fixes_for(d, view)));
		TEST_EXPECT(labels_of(cache.fixes(view, i)) == labels_of(fixes_for(d, view)));
		TEST_EXPECT(labels_of(cache.bulk(view, i)) == labels_of(bulk_fixes_for(d, view)));
	}
	ProblemQuery query;
	query.fixable = true;
	TEST_EXPECT(answer_problems(query, view).rows.size() == 3000 - 100);
	return 0;
}

// S11h: a texture the project lacks takes a placeholder, the game's own missing-texture
// checkerboard, as the file the reference's loader opens once it is there: a model's diffuse row
// naming a .tga its own name (no DDS sibling there), a menu's image its .tga (though the loader
// takes the .dds a missing .tga leaves), a particle's graphic the name as written (its atlas's
// TGA, ADR 0046 S18); each in bulk, a Fix all making
// every one; none for a model's chunk row, a name no placeholder is made for, a particle's name
// whose own extension the factory cannot write, or a model row whose reader would read the
// placeholder in another format; Import first when the game install has the file. Applied, the
// file is made at the project's root, its finding goes, the graph resolves the reference to it
// and the Build packs it.
static int test_placeholders() {
	editor_test::TempProjectDir dir("opennova_editor_problems_placeholders");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Placeholders"));
	const SessionView &v = session.view();
	const std::string root = v.project.root;
	// armory.3di names armry.tga on a diffuse row (type 0); c.mnu an image, fx.ptl a graphic.
	const fs::path model = fs::path(__FILE__).parent_path().parent_path().parent_path() / "fixtures" / "threedi" / "synth" / "armory.3di";
	std::error_code ec;
	fs::create_directories(fs::path(root) / "models", ec);
	fs::copy_file(model, fs::path(root) / "models" / "armory.3di", ec);
	TEST_EXPECT(!ec);
	TEST_EXPECT(editor_test::write_text(root + "/menus/c.mnu",
	                                    "<SCREEN>\r\n\t<NAME>C</NAME>\r\n\t<WINDOW type=\"window\" name=\"MAIN\">\r\n"
	                                    "\t\t<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>\r\n"
	                                    "\t\t<APPEARANCE type=\"image\" state=\"default\">logo.tga</APPEARANCE>\r\n"
	                                    "\t</WINDOW>\r\n</SCREEN>\r\n"));
	TEST_EXPECT(editor_test::write_text(root + "/fx.ptl", "[effectdef]\n{\n\tid = BOOM;\n\tpdefs = puff;\n}\n\n[particledef]\n{\n"
	                                                      "\tid = puff;\n\tgraphic1 = puff.tga, additive;\n}\n"));
	editor_test::handle_to_end(session, request::rescan());
	const auto missing = [&v](ReferenceKind kind, const char *target) -> const Diagnostic * {
		for (const Diagnostic &d : v.findings.diagnostics)
			if (d.code() == "reference.missing" && editor_test::reference_of(d).kind == kind && subject_target(d) == target) return &d;
		return nullptr;
	};
	const Diagnostic *skin = missing(ReferenceKind::Texture, "armry.tga");
	const Diagnostic *logo = missing(ReferenceKind::MenuTexture, "logo.tga");
	const Diagnostic *puff = missing(ReferenceKind::Texture, "puff.tga");
	TEST_EXPECT(skin && editor_test::reference_of(*skin).loader_arg == 0 && logo && puff &&
	            editor_test::reference_of(*puff).loader_arg == texture_role_arg(TextureRoleId::ParticleGraphic));
	if (!skin || !logo || !puff) return 1;
	std::vector<ProblemFix> firsts;
	for (const auto &expected : {std::make_pair(skin, "armry.tga"), std::make_pair(logo, "logo.tga"), std::make_pair(puff, "puff.tga")}) {
		const std::vector<ProblemFix> fixes = fixes_for(*expected.first, v);
		TEST_EXPECT(labels_of(fixes) == std::vector<std::string>({std::string("Create a placeholder ") + expected.second}));
		if (fixes.size() != 1) return 1;
		TEST_EXPECT(fixes[0].bulk && fixes[0].request.kind == EditorRequestKind::CreateFile &&
		            fixes[0].request.path == expected.second && fixes[0].request.file_kind == "texture");
		TEST_EXPECT(fixes[0].detail.find("the checkerboard the game draws for a missing texture") != std::string::npos &&
		            fixes[0].detail.find(kNotUndoable) != std::string::npos);
		TEST_EXPECT(labels_of(bulk_fixes_for(*expected.first, v)) == labels_of(fixes) && has_fixes(*expected.first, v));
		firsts.push_back(fixes[0]);
	}
	// A Fix all: every placeholder, each its own CreateFile.
	const std::vector<EditorRequest> merged = merge_fixes(firsts);
	TEST_EXPECT(merged.size() == 3);
	if (merged.size() != 3) return 1;
	for (size_t i = 0; i < 3; ++i)
		TEST_EXPECT(merged[i].kind == EditorRequestKind::CreateFile && merged[i].path == firsts[i].request.path);
	// None for a model's chunk row (it reads a chunk container), nor for a name no placeholder
	// is made for (a menu's .png).
	Diagnostic chunk = *skin;
	editor_test::own_reference(chunk).loader_arg = 16;
	Diagnostic png = *logo;
	editor_test::own_reference(png).target = "badge.png";
	TEST_EXPECT(fixes_for(chunk, v).empty() && fixes_for(png, v).empty() && !has_fixes(png, v));
	// The pointer's name (a CURSOR's texture) makes the pointer, never the checkerboard.
	Diagnostic pointer = *logo;
	editor_test::own_reference(pointer).target = "newarow1.tga";
	const std::vector<ProblemFix> pointer_fixes = fixes_for(pointer, v);
	TEST_EXPECT(labels_of(pointer_fixes) == std::vector<std::string>({"Create newarow1.tga"}));
	if (pointer_fixes.size() == 1)
		TEST_EXPECT(pointer_fixes[0].bulk && pointer_fixes[0].request.kind == EditorRequestKind::CreateFile &&
		            pointer_fixes[0].request.path == "newarow1.tga" &&
		            pointer_fixes[0].detail.find("the game's mouse pointer") != std::string::npos &&
		            pointer_fixes[0].detail.find("checkerboard") == std::string::npos);
	// A particle's graphic, as its atlas reads it (ADR 0046 S18): the name alone, so one whose
	// extension the factory cannot write takes none, and neither does a name with no extension (no
	// loader of the game adds one).
	Diagnostic particle = *puff;
	editor_test::own_reference(particle).target = "puff.png";
	TEST_EXPECT(fixes_for(particle, v).empty() && !has_fixes(particle, v));
	editor_test::own_reference(particle).target = "foo";
	TEST_EXPECT(fixes_for(particle, v).empty() && !has_fixes(particle, v));
	// A model row takes a placeholder only in the format its reader reads the name as: a plain
	// row reads a.tga.pcx through its TGA reader (the factory would write a PCX) but a.pcx.tga
	// as the TGA it is; a diffuse row reads a.tga.pcx as its query, a.tga; a normal map reads no
	// PCX at all, a height producer no .mdt (renderer::material_texture_source).
	Diagnostic compound = *skin;
	editor_test::own_reference(compound).loader_arg = 1;
	editor_test::own_reference(compound).target = "a.tga.pcx";
	TEST_EXPECT(fixes_for(compound, v).empty());
	editor_test::own_reference(compound).target = "a.pcx.tga";
	TEST_EXPECT(labels_of(fixes_for(compound, v)) == std::vector<std::string>({"Create a placeholder a.pcx.tga"}));
	editor_test::own_reference(compound).loader_arg = 0;
	editor_test::own_reference(compound).target = "a.tga.pcx";
	TEST_EXPECT(labels_of(fixes_for(compound, v)) == std::vector<std::string>({"Create a placeholder a.tga"}));
	editor_test::own_reference(compound).loader_arg = 4;
	editor_test::own_reference(compound).target = "bump.pcx";
	TEST_EXPECT(fixes_for(compound, v).empty());
	editor_test::own_reference(compound).target = "bump.tga";
	TEST_EXPECT(labels_of(fixes_for(compound, v)) == std::vector<std::string>({"Create a placeholder bump.tga"}));
	editor_test::own_reference(compound).target = "bump.mdt";
	TEST_EXPECT(labels_of(fixes_for(compound, v)) == std::vector<std::string>({"Create a placeholder bump.mdt"}));
	editor_test::own_reference(compound).loader_arg = 6;
	TEST_EXPECT(fixes_for(compound, v).empty());
	// With the game install holding the menu texture's .dds: Import it first, the placeholder after.
	const std::string install = dir.file("install");
	const uint8_t bytes[] = {'d', 'a', 't', 'a'};
	const opennova::pff::PffWriteEntry entries[] = {{"logo.dds", bytes, sizeof(bytes), 0, 0, 0}};
	TEST_EXPECT(editor_test::write_text(install + "/readme.txt", "an install"));
	TEST_EXPECT(opennova::pff::pff_write_archive((install + "/resource.pff").c_str(), opennova::pff::PFF_FORMAT_PFF3, entries, 1) ==
	            opennova::pff::PFF_WRITE_OK);
	editor_test::set_game_install(session, install);
	logo = missing(ReferenceKind::MenuTexture, "logo.tga");
	TEST_EXPECT(logo && labels_of(fixes_for(*logo, v)) ==
	                            std::vector<std::string>({"Import logo.dds from the game data...", "Create a placeholder logo.tga"}));
	// Applied: the checkerboard written in the textures' folder (the UX round's project lane), the finding
	// gone, the reference found.
	skin = missing(ReferenceKind::Texture, "armry.tga");
	TEST_EXPECT(skin != nullptr);
	if (!skin) return 1;
	editor_test::handle_to_end(session, fixes_for(*skin, v).front().request);
	TEST_EXPECT(session.outcome().done());
	const AssetEntry *made = v.project.scan->find("armry.tga");
	TEST_EXPECT(made && made->kind == AssetKind::Texture && made->relative_path == "textures/armry.tga");
	BlankRequest blank;
	blank.logical_name = "armry.tga";
	std::vector<uint8_t> checkerboard;
	Diagnostic error;
	TEST_EXPECT(make_blank(blank, AssetKind::Texture, checkerboard, error) &&
	            test_io::read_file(root + "/textures/armry.tga") == checkerboard);
	TEST_EXPECT(missing(ReferenceKind::Texture, "armry.tga") == nullptr);
	bool found = false;
	for (const GraphEdge *edge : v.findings.graph->references_of("models/armory.3di")) {
		if (edge->kind != ReferenceKind::Texture || edge->value != "armry.tga") continue;
		std::string file;
		TEST_EXPECT(v.findings.graph->resolve(*edge, &file) == ReferenceStatus::Present && file == "textures/armry.tga");
		found = true;
	}
	TEST_EXPECT(found);
	// The Build packs it into an archive under its name.
	bool packed = false;
	for (const BuildArchive &archive : plan_build(ProjectPaths::for_root(root), *v.project.scan, *v.project.requirements, {}).archives)
		for (const BuildEntry &entry : archive.entries) packed = packed || entry.logical_name == "armry.tga";
	TEST_EXPECT(packed);
	return 0;
}

// S11e: an optional file the project lacks is a note grouped apart from the required files
// (by kind: "Optional files"), so a group of notes alone can fold away from the errors.
static int test_optional_group() {
	SessionView view;
	view.project.open = true;
	Diagnostic required = finding(DiagnosticSeverity::Error, "requirement.missing", "Missing required file gametext.bin.");
	Diagnostic optional = finding(DiagnosticSeverity::Info, "requirement.optional_missing", "Optional file brand.mns.");
	Diagnostic wrong = finding(DiagnosticSeverity::Error, "requirement.wrong_kind", "main.mnu is not a menu.", "menus/main.mnu");
	view.findings.diagnostics = {optional, required, wrong};
	ProblemQuery query;
	query.grouping = ProblemGrouping::Kind;
	const ProblemAnswer kinds = answer_problems(query, view);
	TEST_EXPECT(kinds.groups.size() == 2);
	if (kinds.groups.size() != 2) return 1;
	TEST_EXPECT(kinds.groups[0].key == "requirement" && kinds.groups[0].title == "Required files" &&
	            kinds.groups[0].rows == std::vector<size_t>({1, 2}));
	TEST_EXPECT(kinds.groups[1].key == "requirement.optional_missing" && kinds.groups[1].title == "Optional files" &&
	            kinds.groups[1].infos == 1 && kinds.groups[1].errors == 0);
	TEST_EXPECT(std::string(finding_group_title(finding_code(CoreFinding::RequirementOptionalMissing).group)) ==
	                    "Optional files" &&
	            std::string(finding_group_title(finding_code(CoreFinding::RequirementMissing).group)) == "Required files");
	return 0;
}

// S12 C3: where the findings that named no record take Problems now, and the fixes they
// gained: a menu's ignored input on the window its key names; an ungrouped table on the first
// string the game reads under another section; a table with no anim_reset row on its first
// row, with an Add anim_reset row fix that opens the table and adds the row in one undoable
// step; a catalog's name finding on the field that names the record; a file name the archives
// cannot take, or another file has, shown in Files, whose Rename... the fix opens; a required
// name another kind of file holds: Import the game's own, or Rename that file; a missing
// symbol: Open the file where it belongs.
static int test_locations_and_fixes() {
	editor_test::TempProjectDir dir("opennova_editor_problems_places");
	const std::string install = dir.file("install");
	std::vector<uint8_t> table;
	Diagnostic error;
	BlankRequest blank;
	blank.logical_name = "gameerr.bin";
	TEST_EXPECT(make_blank(blank, AssetKind::Strings, table, error));
	const opennova::pff::PffWriteEntry entries[] = {{"gameerr.bin", table.data(), uint32_t(table.size()), 0, 0, 0}};
	TEST_EXPECT(editor_test::write_text(install + "/readme.txt", "an install"));
	TEST_EXPECT(opennova::pff::pff_write_archive((install + "/resource.pff").c_str(), opennova::pff::PFF_FORMAT_PFF3, entries, 1) ==
	            opennova::pff::PFF_WRITE_OK);
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Places"));
	editor_test::create_missing_files(session);
	editor_test::set_game_install(session, install);
	const SessionView &v = session.view();
	const std::string root = v.project.root;
	const AssetEntry *items_entry = v.project.scan->find("items.def");
	const AssetEntry *gameerr_entry = v.project.scan->find("gameerr.bin");
	TEST_EXPECT(items_entry && gameerr_entry);
	if (!items_entry || !gameerr_entry) return 1;
	const std::string items_path = items_entry->relative_path, gameerr_path = gameerr_entry->relative_path;
	// a.mnu with a second window named GO beside the first, holding input the reader leaves out.
	std::string bogus = kMenuA;
	bogus.insert(bogus.rfind("\t</WINDOW>\r\n</SCREEN>"),
	             "\t\t<WINDOW type=\"button\" name=\"GO\">\r\n"
	             "\t\t\t<BOGUS>x</BOGUS>\r\n"
	             "\t\t\t<APPEARANCE state=\"default\"></APPEARANCE>\r\n"
	             "\t\t\t<POSITION><LEFT>10</LEFT><TOP>40</TOP></POSITION>\r\n"
	             "\t\t</WINDOW>\r\n");
	TEST_EXPECT(editor_test::write_text(root + "/menus/a.mnu", bogus));
	// c.mnu names twin.tga, of which the project has two (art/ and other/).
	TEST_EXPECT(editor_test::write_text(root + "/menus/c.mnu",
	                                    "<SCREEN>\r\n<NAME>C</NAME>\r\n<WINDOW TYPE=\"STATIC\" NAME=\"PIC\">\r\n"
	                                    "<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>9</RIGHT><BOTTOM>9</BOTTOM></POSITION>\r\n"
	                                    "<APPEARANCE STATE=\"DEFAULT\" TYPE=\"IMAGE\">twin.tga</APPEARANCE>\r\n"
	                                    "</WINDOW>\r\n</SCREEN>\r\n"));
	opennova::rtxt::File strings_file;
	strings_file.sections = {{"Menu", 1}, {"WepDes", 2}};
	opennova::rtxt::Entry exit_entry, cafe_entry, weapon_entry;
	exit_entry.key = "MM_Exit";
	exit_entry.section_index = 0;
	cafe_entry.key = "MM_Cafe";
	cafe_entry.section_index = 0;
	weapon_entry.key = "WPN_ONE";
	weapon_entry.section_index = 1;
	strings_file.entries = {exit_entry, weapon_entry, cafe_entry}; // MM_Cafe is read as WepDes's
	std::vector<uint8_t> ungrouped;
	std::string io_error;
	TEST_EXPECT(opennova::rtxt::write(strings_file, ungrouped, io_error) &&
	            editor_test::write_bytes(root + "/strings/ungrouped.bin", ungrouped));
	TEST_EXPECT(editor_test::write_text(root + "/anims/noreset.adm", "anim_idle\t\"idle.bad\"\r\n"));
	TEST_EXPECT(editor_test::write_text(root + "/" + items_path,
	                                    "begin \"Marker\"\nid 100001\ntype marker\nhp 10\nend\n"
	                                    "begin \"MARKER\"\nid 100002\ntype marker\nhp 10\nend\n"
	                                    "begin \"Crate\"\nid 100001\ntype marker\nhp 10\nend\n"));
	TEST_EXPECT(editor_test::write_text(root + "/art/a_name_too_long_for_archives.tga", "tga"));
	TEST_EXPECT(editor_test::write_text(root + "/art/twin.tga", "tga") && editor_test::write_text(root + "/other/twin.tga", "tga"));
	TEST_EXPECT(editor_test::write_text(root + "/" + gameerr_path, "raw bytes")); // no string table: the wrong kind
	editor_test::handle_to_end(session, request::rescan());
	const auto finding_in = [&v](const char *code, const std::string &asset) -> const Diagnostic * {
		for (const Diagnostic &d : v.findings.diagnostics)
			if (d.code() == code && d.asset == asset) return &d;
		return nullptr;
	};

	// A menu's input retail's reader leaves out: on the window the reader found it in (the
	// second of two named GO), which a click selects.
	const Diagnostic *ignored = finding_in("menu.ignored_input", "menus/a.mnu");
	TEST_EXPECT(ignored && ignored->row_id && ignored->record == "A/MAIN/GO");
	if (!ignored) return 1;
	editor_test::handle_to_end(session, problem_location(*ignored, v).request());
	const Document *menu = session.document_for("menus/a.mnu");
	NodeAddress main_window, second_go;
	TEST_EXPECT(menu && find_definition(AssetGraph(), *menu, "MAIN", main_window));
	if (!menu) return 1;
	for (const Document::Collection &collection : menu->collections_of(main_window))
		if (std::string(menu->kind_token(collection.spec.kind)) == "window" && collection.ids.size() == 2)
			second_go = {main_window.row, collection.spec.kind, collection.ids[1]};
	TEST_EXPECT(second_go.child && menu->record_name(second_go) == "GO" && v.documents.selection.primary == second_go);

	// An ungrouped table: on the first string the game reads under another section.
	const Diagnostic *regrouped = finding_in("strings.regrouped", "strings/ungrouped.bin");
	TEST_EXPECT(regrouped && regrouped->row_id && regrouped->record == "Menu");
	if (!regrouped) return 1;
	editor_test::handle_to_end(session, problem_location(*regrouped, v).request());
	const Document *strings = session.document_for("strings/ungrouped.bin");
	TEST_EXPECT(strings && v.documents.active == strings->path() && strings->record_name(v.documents.selection.primary) == "MM_Cafe");

	// A table with no anim_reset row: on its first row's key; the fix opens the table and adds
	// the row, one step Undo takes back.
	const Diagnostic *no_reset = finding_in("animation_map.no_reset", "anims/noreset.adm");
	TEST_EXPECT(no_reset && no_reset->row_id && no_reset->record == "anim_idle" && no_reset->field == "key");
	if (!no_reset) return 1;
	std::vector<ProblemFix> fixes = fixes_for(*no_reset, v);
	TEST_EXPECT(labels_of(fixes) == std::vector<std::string>({"Add anim_reset row"}));
	if (fixes.size() != 1) return 1;
	TEST_EXPECT(!fixes[0].bulk && fixes[0].detail.find(kNotUndoable) == std::string::npos);
	TEST_EXPECT(session.document_for("anims/noreset.adm") == nullptr);
	editor_test::handle_to_end(session, fixes[0].request);
	Document *adm = session.document_for("anims/noreset.adm");
	TEST_EXPECT(session.outcome().done() && adm && adm->dirty() && adm->rows().size() == 2);
	if (!adm) return 1;
	TEST_EXPECT(adm->record_name(v.documents.selection.primary) == "anim_reset" && !finding_in("animation_map.no_reset", adm->path()));
	editor_test::handle_to_end(session, request::undo(adm->path()));
	TEST_EXPECT(!adm->dirty() && finding_in("animation_map.no_reset", adm->path()));

	// A catalog's name finding: on the field that names the later item (the name compared
	// without case, as the game's lookup compares it). The game loads both, so it is a warning
	// naming the one a lookup finds; the same for an id two items share.
	const Diagnostic *twice = finding_in("catalog.name_duplicate", items_path);
	TEST_EXPECT(twice && twice->field == "display_name" && twice->row_id && twice->record == "MARKER" &&
	            twice->severity == DiagnosticSeverity::Warning && twice->message.find("id 100001") != std::string::npos);
	const Diagnostic *same_id = finding_in("catalog.item_identity", items_path);
	TEST_EXPECT(same_id && same_id->field == "id" && same_id->record == "Crate" &&
	            same_id->severity == DiagnosticSeverity::Warning && same_id->message.find("\"Marker\"") != std::string::npos);
	size_t catalog_findings = 0;
	for (const Diagnostic &d : v.findings.diagnostics)
		catalog_findings += d.asset == items_path && (d.code() == "catalog.name_duplicate" || d.code() == "catalog.item_identity");
	TEST_EXPECT(catalog_findings == 2); // on the later record alone

	// A name the archives cannot take: shown in Files, whose Rename... the fix opens; the same
	// for one the build cannot store. ShowInFiles takes a logical name too, and refuses a file
	// the project does not have.
	const std::string long_path = "art/a_name_too_long_for_archives.tga";
	const Diagnostic *too_long = finding_in("asset.name.too_long", long_path);
	TEST_EXPECT(too_long && problem_location(*too_long, v).in_files);
	if (!too_long) return 1;
	fixes = fixes_for(*too_long, v);
	TEST_EXPECT(labels_of(fixes) == std::vector<std::string>({"Rename a_name_too_long_for_archives.tga..."}));
	if (fixes.size() != 1) return 1;
	TEST_EXPECT(fixes[0].request.kind == EditorRequestKind::ShowInFiles && fixes[0].request.ask_name &&
	            fixes[0].request.path == long_path && !fixes[0].bulk);
	// Each ask one RevealFile event, the same file asked again another: Files shows it again.
	const uint64_t before = v.events.next_seq() - 1;
	editor_test::handle_to_end(session, fixes[0].request);
	std::vector<ViewEvent> shown = editor_test::events_after(v, before, ViewEventKind::RevealFile);
	TEST_EXPECT(session.outcome().done() && shown.size() == 1 && shown[0].path == long_path &&
			shown[0].flag && !shown[0].address.row && shown[0].field.empty() && shown[0].tag == 0);
	editor_test::handle_to_end(session, request::show_in_files("a_name_too_long_for_archives.tga"));
	shown = editor_test::events_after(v, before, ViewEventKind::RevealFile);
	TEST_EXPECT(shown.size() == 2 && shown[1].path == long_path && !shown[1].flag &&
			shown[1].seq == shown[0].seq + 1);
	editor_test::handle_to_end(session, request::show_in_files("nowhere.tga"));
	TEST_EXPECT(!session.outcome().done() &&
			editor_test::events_after(v, before, ViewEventKind::RevealFile).size() == 2);
	const Diagnostic unstorable =
	        editor_test::finding_of(DiagnosticSeverity::Error, "build.name_unstorable", "The name is too long.", long_path);
	TEST_EXPECT(labels_of(fixes_for(unstorable, v)) == std::vector<std::string>({"Rename a_name_too_long_for_archives.tga..."}));
	// Two files of one name: the fix renames the one the finding is on (other/twin.tga, the
	// second), and the rename takes that one, not the other of its name. c.mnu's reference
	// reaches the first, which the game finds: renaming the second rewrites nothing, and c.mnu
	// still reaches the first.
	std::string twin;
	for (const Diagnostic &d : v.findings.diagnostics)
		if (d.code() == "asset.name.duplicate") twin = d.asset;
	TEST_EXPECT(twin == "other/twin.tga");
	Diagnostic duplicate = editor_test::finding_of(DiagnosticSeverity::Error, "asset.name.duplicate", "Two files.", twin);
	fixes = fixes_for(duplicate, v);
	TEST_EXPECT(fixes.size() == 1 && fixes[0].request.path == twin);
	const ProjectPaths paths = ProjectPaths::for_root(root);
	const RenamePlan first_plan =
			plan_rename(paths, *v.project.scan, *v.findings.graph, "art/twin.tga", "twin3.tga");
	TEST_EXPECT(first_plan.ok() && first_plan.sites.size() == 1 && first_plan.sites[0].file == "menus/c.mnu");
	const RenamePlan second_plan =
			plan_rename(paths, *v.project.scan, *v.findings.graph, twin, "twin2.tga");
	TEST_EXPECT(second_plan.ok() && second_plan.path == twin && second_plan.sites.empty());
	editor_test::handle_to_end(session, request::rename_asset(twin, "twin2.tga"));
	const std::string moved = (fs::path(twin).parent_path() / "twin2.tga").generic_string();
	TEST_EXPECT(session.outcome().done() && fs::exists(root + "/" + moved) && !fs::exists(root + "/" + twin));
	TEST_EXPECT(fs::exists(root + "/art/twin.tga") &&
	            std::none_of(v.findings.diagnostics.begin(), v.findings.diagnostics.end(),
	                         [](const Diagnostic &d) { return d.code() == "rename.partial"; }));
	std::string c_menu, read_error;
	TEST_EXPECT(read_file_text(root + "/menus/c.mnu", c_menu, read_error) && c_menu.find(">twin.tga<") != std::string::npos);
	TEST_EXPECT(v.project.scan->find("twin.tga") && !finding_in("asset.name.duplicate", twin) && !finding_in("reference.missing", "menus/c.mnu"));

	// A required name another kind of file holds: Import the game's own, or Rename that file
	// (no Create or Use while the name is taken).
	const Diagnostic *wrong = requirement_finding(v, "requirement.wrong_kind", "gameerr");
	TEST_EXPECT(wrong != nullptr);
	if (!wrong) return 1;
	fixes = fixes_for(*wrong, v);
	TEST_EXPECT(labels_of(fixes) == std::vector<std::string>({"Import gameerr.bin from the game data...", "Rename gameerr.bin..."}));
	if (fixes.size() != 2) return 1;
	TEST_EXPECT(fixes[1].request.kind == EditorRequestKind::ShowInFiles && fixes[1].request.path == gameerr_path &&
	            fixes[1].request.ask_name);

	// A missing symbol: the file where it belongs, opened (a style variable's stylesheet, the
	// menu a screen is looked up in, the table a string id's scope names); none for a string
	// id whose window names no table.
	Diagnostic style = editor_test::finding_of(DiagnosticSeverity::Warning, "reference.missing", "A variable.", "menus/a.mnu", "font.name");
	style.subject = ReferenceSubject{ReferenceKind::StyleVar, "%NOPE%"};
	const AssetEntry *stylesheet = v.project.scan->find("menu_style.mns");
	fixes = fixes_for(style, v);
	TEST_EXPECT(stylesheet && labels_of(fixes) == std::vector<std::string>({"Open menu_style.mns"}));
	if (!stylesheet || fixes.size() != 1) return 1;
	TEST_EXPECT(fixes[0].request.kind == EditorRequestKind::OpenDocument && fixes[0].request.path == stylesheet->relative_path &&
	            !fixes[0].bulk && fixes[0].detail.find(kNotUndoable) == std::string::npos);
	Diagnostic screen = style;
	screen.subject = ReferenceSubject{ReferenceKind::MenuScreen, "B", "A.MNU"};
	TEST_EXPECT(labels_of(fixes_for(screen, v)) == std::vector<std::string>({"Open a.mnu"}));
	Diagnostic text_id = style;
	text_id.subject = ReferenceSubject{ReferenceKind::TextId, "NO_ID", "UNGROUPED.BIN/Menu"};
	TEST_EXPECT(labels_of(fixes_for(text_id, v)) == std::vector<std::string>({"Open ungrouped.bin"}));
	editor_test::own_reference(text_id).scope = "/menu";
	TEST_EXPECT(fixes_for(text_id, v).empty());
	// A table of its kind that defines nothing yet: the one the game reads (a weapon table and
	// a stylesheet emptied).
	const AssetEntry *weapons = v.project.scan->find("weapon.def");
	TEST_EXPECT(weapons != nullptr);
	if (!weapons) return 1;
	const std::string weapons_path = weapons->relative_path, style_path = stylesheet->relative_path;
	TEST_EXPECT(editor_test::write_text(root + "/" + weapons_path, "") && editor_test::write_text(root + "/" + style_path, ""));
	editor_test::handle_to_end(session, request::rescan());
	bool any = false;
	v.findings.graph->for_each_symbol([&any](const GraphSymbol &symbol) {
		any = any ||
				(symbol.kind == ReferenceKind::Weapon || symbol.kind == ReferenceKind::StyleVar);
	});
	TEST_EXPECT(!any);
	Diagnostic weapon = style;
	weapon.subject = ReferenceSubject{ReferenceKind::Weapon, "NOPE"};
	fixes = fixes_for(weapon, v);
	TEST_EXPECT(labels_of(fixes) == std::vector<std::string>({"Open weapon.def"}) && fixes[0].request.path == weapons_path);
	fixes = fixes_for(style, v);
	TEST_EXPECT(labels_of(fixes) == std::vector<std::string>({"Open menu_style.mns"}) && fixes[0].request.path == style_path);
	return 0;
}

// A view's findings by file and by record: a row's are its own and every record's it holds, a
// nested record's its own; a file with none has none; made again when the findings' counter
// moves, the findings are another count or elsewhere in memory, and for another view (a
// finding changed in place, unmarked, is not seen); asked while the findings are fewer than it
// names, the ones past their end are left out.
static int test_findings_index() {
	SessionView v;
	const auto on = [](const char *file, NodeId row, NodeId child, const char *what) {
		Diagnostic d = editor_test::finding_of(DiagnosticSeverity::Error, "menu.duplicate_window", what, file);
		d.row_id = row;
		d.child_id = child;
		return d;
	};
	v.findings.diagnostics = { editor_test::finding_of(
									   DiagnosticSeverity::Error, "requirement.missing", "project"),
		on("a.mnu", 5, 0, "row"), on("a.mnu", 5, 7, "child"), on("b.mnu", 5, 0, "other file"),
		on("a.mnu", 0, 0, "the file"), on("a.mnu", 6, 7, "another row") };
	FindingsIndex index;
	index.follow(v);
	TEST_EXPECT(index.of_file("a.mnu") == std::vector<size_t>({1, 2, 4, 5}));
	TEST_EXPECT(index.of_file("b.mnu") == std::vector<size_t>({3}));
	TEST_EXPECT(index.of_file("c.mnu").empty());
	TEST_EXPECT(index.of_record("a.mnu", 5, 0) == std::vector<size_t>({1, 2}));
	TEST_EXPECT(index.of_record("a.mnu", 5, 7) == std::vector<size_t>({2}));
	TEST_EXPECT(index.of_record("a.mnu", 6, 0) == std::vector<size_t>({5}));
	TEST_EXPECT(index.of_record("a.mnu", 9, 0).empty());
	// Kept while the view and its findings stand; made again once the findings' counter moves,
	// they are another count or elsewhere in memory, or for another view.
	v.findings.diagnostics[3].asset = "c.mnu"; // in place, unmarked
	index.follow(v);
	TEST_EXPECT(index.of_file("b.mnu") == std::vector<size_t>({3}) &&
	            index.of_file("c.mnu").empty());
	v.findings.diagnostics[3].asset = "b.mnu";
	v.findings.diagnostics.push_back(on("c.mnu", 1, 0, "later")); // unmarked, but one more
	index.follow(v);
	TEST_EXPECT(index.of_file("c.mnu") == std::vector<size_t>({6}));
	v.revisions.touch(ViewConcern::Findings);
	index.follow(v);
	TEST_EXPECT(index.of_file("c.mnu") == std::vector<size_t>({6}));
	// Two findings left, asked before the index follows: what it names past them is left out.
	v.findings.diagnostics.resize(2);
	TEST_EXPECT(index.of_file("a.mnu") == std::vector<size_t>({1}) &&
	            index.of_file("c.mnu").empty());
	TEST_EXPECT(index.of_record("a.mnu", 5, 0) == std::vector<size_t>({1}) &&
	            index.of_record("a.mnu", 5, 7).empty() && index.of_record("a.mnu", 6, 0).empty());
	index.follow(v);
	TEST_EXPECT(index.of_file("a.mnu") == std::vector<size_t>({1}) &&
	            index.of_file("b.mnu").empty());
	SessionView other = v;
	other.findings.diagnostics.clear();
	index.follow(other);
	TEST_EXPECT(index.of_file("a.mnu").empty());
	return 0;
}

// ADR 0046 S15, decided per finding against the install as a whole (the UX round's problems lane). A fake
// install serving a menu whose window's appearance names a texture the install lacks; the project holds
// that menu byte for byte, and a menu of its own naming the same texture. Once the install is validated
// (session/original_files.h), the shipped menu's finding is the game's own and the modder's menu's is not;
// the answer lists the modder's first and the game's own under their group, last, counted apart. The
// shipped menu edited: a move of its window, an appearance inserted above the logo's, removed, the logo's
// moved among its siblings, the window renamed, saved, a byte changed on disk: the logo's finding stays
// the game's own, keyed on the record as itself; another texture named is the modder's. The install
// patched to serve the texture (its folder moved: validated again at the next rescan): the project's
// finding is the modder's (the install resolves it). With no install, none is the game's own.
static int test_original_data() {
	editor_test::TempProjectDir dir("opennova_editor_problems_original");
	const auto menu = [](const char *name) {
		return std::string("<SCREEN>\r\n\t<NAME>") + name + "</NAME>\r\n\t<WINDOW type=\"window\" name=\"MAIN\">\r\n"
		       "\t\t<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>\r\n"
		       "\t\t<APPEARANCE type=\"image\" state=\"default\">logo.tga</APPEARANCE>\r\n\t</WINDOW>\r\n</SCREEN>\r\n";
	};
	const std::string shipped = menu("SHIPPED");
	const std::string install = dir.file("install");
	const auto write_install = [&install](const std::vector<std::pair<std::string, std::string>> &files) {
		std::vector<opennova::pff::PffWriteEntry> entries;
		for (const auto &[name, bytes] : files)
			entries.push_back({name.c_str(), reinterpret_cast<const uint8_t *>(bytes.data()), uint32_t(bytes.size()), 0, 0, 0});
		return opennova::pff::pff_write_archive((install + "/resource.pff").c_str(), opennova::pff::PFF_FORMAT_PFF3, entries.data(),
		                                        uint32_t(entries.size())) == opennova::pff::PFF_WRITE_OK;
	};
	TEST_EXPECT(editor_test::write_text(install + "/readme.txt", "an install"));
	TEST_EXPECT(write_install({{"shipped.mnu", shipped}}));
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Original"));
	const SessionView &v = session.view();
	const std::string root = v.project.root;
	TEST_EXPECT(editor_test::write_text(root + "/menus/shipped.mnu", shipped));
	TEST_EXPECT(editor_test::write_text(root + "/menus/mine.mnu", menu("MINE")));
	editor_test::set_game_install(session, install);
	editor_test::handle_to_end(session, request::rescan());
	const auto logo_in = [&v](const char *path) {
		for (size_t i = 0; i < v.findings.diagnostics.size(); ++i) {
			const Diagnostic &d = v.findings.diagnostics[i];
			if (d.code() == "reference.missing" && d.asset == path && subject_target(d) == "logo.tga") return i;
		}
		return SIZE_MAX;
	};
	const size_t theirs = logo_in("menus/shipped.mnu"), mine = logo_in("menus/mine.mnu");
	TEST_EXPECT(theirs != SIZE_MAX && mine != SIZE_MAX);
	if (theirs == SIZE_MAX || mine == SIZE_MAX) return 1;
	TEST_EXPECT(v.findings.originals && v.findings.originals->ready && v.findings.originals->findings.count(normalized_logical_name("shipped.mnu")) == 1 &&
	            v.findings.originals->findings.count(normalized_logical_name("mine.mnu")) == 0);
	TEST_EXPECT(!v.findings.diagnostics[theirs].record_key.empty());
	TEST_EXPECT(in_original_data(theirs, v) && !in_original_data(mine, v));
	const OriginalFiles &baseline = session.originals();
	const size_t validated = baseline.validations();
	TEST_EXPECT(validated == 1 && baseline.files() >= 1);
	const auto group_of = [](const ProblemAnswer &answer, size_t finding) {
		for (size_t g = 0; g < answer.groups.size(); ++g)
			if (std::find(answer.groups[g].rows.begin(), answer.groups[g].rows.end(), finding) != answer.groups[g].rows.end())
				return g;
		return SIZE_MAX;
	};
	ProblemQuery query;
	ProblemAnswer answer = answer_problems(query, v);
	TEST_EXPECT(answer.grouped && answer.groups.size() == 2 && !answer.groups[0].header && answer.groups[1].original &&
	            answer.groups[1].key == kOriginalGroupKey && answer.groups[1].title == kOriginalGroupTitle);
	TEST_EXPECT(group_of(answer, mine) == 0 && group_of(answer, theirs) == 1 && answer.rows.back() == answer.groups[1].rows.back());
	TEST_EXPECT(answer.original() == answer.groups[1].rows.size() && answer.total() == v.findings.diagnostics.size());
	query.grouping = ProblemGrouping::File;
	answer = answer_problems(query, v);
	TEST_EXPECT(!answer.groups.empty() && answer.groups.back().original && group_of(answer, theirs) == answer.groups.size() - 1 &&
	            group_of(answer, mine) < answer.groups.size() - 1 && answer.groups[group_of(answer, mine)].header);
	// The counts every reader shares (Problems, the menu bar, problem_counts, the CLI's status) agree.
	ProblemCounts counts = count_problems(v);
	TEST_EXPECT(counts.original_errors + counts.original_warnings + counts.original_infos == answer.original() &&
	            counts.errors == answer.errors && counts.warnings == answer.warnings);
	// Edits of the shipped menu: the logo's finding stays the game's own through each.
	editor_test::handle_to_end(session, request::open_document("menus/shipped.mnu"));
	const Document *opened = records_of(*session.document_for("menus/shipped.mnu"));
	TEST_EXPECT(opened != nullptr);
	if (!opened) return 1;
	const Diagnostic logo = v.findings.diagnostics[logo_in("menus/shipped.mnu")];
	const NodeAddress appearance{logo.row_id, logo.record_kind, logo.child_id};
	const std::vector<NodeAddress> owners = opened->ancestors(appearance);
	TEST_EXPECT(!owners.empty() && !logo.field.empty());
	if (owners.empty()) return 1;
	const NodeAddress window = owners.back();
	const auto still_theirs = [&](const char *after) {
		const size_t row = logo_in("menus/shipped.mnu");
		const bool ok = row != SIZE_MAX && in_original_data(row, v);
		if (!ok) std::fprintf(stderr, "the logo's finding is not the game's own after %s\n", after);
		return ok;
	};
	// Its window moved.
	Edit moved;
	moved.address = window;
	moved.field = "position.left";
	moved.value = int64_t(10);
	editor_test::handle_to_end(session, request::edit_record("menus/shipped.mnu", moved));
	TEST_EXPECT(opened->dirty() && still_theirs("a move of its window"));
	// An appearance inserted above the logo's (its place among its siblings moves), then removed.
	Edit inserted;
	inserted.operation = EditOperation::Add;
	inserted.address = {appearance.row, appearance.kind, 0};
	inserted.parent = window.child;
	inserted.position = 0;
	editor_test::handle_to_end(session, request::edit_record("menus/shipped.mnu", inserted));
	NodeId added = 0;
	for (const Document::Collection &collection : opened->collections_of(window))
		if (collection.spec.kind == appearance.kind && collection.ids.size() == 2) added = collection.ids[0];
	TEST_EXPECT(added != 0 && added != appearance.child && still_theirs("an appearance inserted above it"));
	Edit removed;
	removed.operation = EditOperation::Remove;
	removed.address = {appearance.row, appearance.kind, added};
	editor_test::handle_to_end(session, request::edit_record("menus/shipped.mnu", removed));
	TEST_EXPECT(still_theirs("an appearance removed above it"));
	// The logo's moved among its siblings (to the end, after the one left).
	Edit reordered;
	reordered.operation = EditOperation::Move;
	reordered.address = appearance;
	reordered.position = SIZE_MAX;
	editor_test::handle_to_end(session, request::edit_record("menus/shipped.mnu", reordered));
	TEST_EXPECT(still_theirs("a move among its siblings"));
	// Its window renamed: an owner's name is no part of it.
	Edit renamed_window;
	renamed_window.address = window;
	renamed_window.field = "name";
	renamed_window.value = std::string("BACKDROP");
	editor_test::handle_to_end(session, request::edit_record("menus/shipped.mnu", renamed_window));
	TEST_EXPECT(still_theirs("its window renamed"));
	// The image changed to another the project lacks: that finding is the modder's, and the logo's is gone
	// with the name; Undo brings the logo's back as the game's own.
	Edit renamed;
	renamed.address = appearance;
	renamed.field = logo.field;
	renamed.value = std::string("other.tga");
	editor_test::handle_to_end(session, request::edit_record("menus/shipped.mnu", renamed));
	const auto other_in = [&v]() {
		for (size_t i = 0; i < v.findings.diagnostics.size(); ++i)
			if (v.findings.diagnostics[i].code() == "reference.missing" && subject_target(v.findings.diagnostics[i]) == "other.tga")
				return i;
		return SIZE_MAX;
	};
	const size_t other = other_in();
	TEST_EXPECT(other != SIZE_MAX && !in_original_data(other, v) && logo_in("menus/shipped.mnu") == SIZE_MAX);
	editor_test::handle_to_end(session, request::undo("menus/shipped.mnu"));
	TEST_EXPECT(other_in() == SIZE_MAX && still_theirs("an undo"));
	// Saved: the file differs from the install's on disk, and the logo is still the game's own (the audit's
	// case: one saved field made every original finding of the file the modder's).
	editor_test::handle_to_end(session, request::save("menus/shipped.mnu"));
	TEST_EXPECT(!opened->dirty() && still_theirs("a save"));
	editor_test::handle_to_end(session, request::close_document("menus/shipped.mnu"));
	// Changed by a byte on disk: the same; the modder's menu's is the modder's. The install was validated
	// once: nothing of the project's moves it.
	TEST_EXPECT(editor_test::write_text(root + "/menus/shipped.mnu", shipped + " "));
	editor_test::handle_to_end(session, request::rescan());
	TEST_EXPECT(still_theirs("a byte changed on disk") && !in_original_data(logo_in("menus/mine.mnu"), v));
	TEST_EXPECT(baseline.validations() == validated);
	answer = answer_problems(ProblemQuery(), v);
	TEST_EXPECT(answer.grouped && answer.original() >= 1);
	counts = count_problems(v);
	TEST_EXPECT(counts.original_errors + counts.original_warnings + counts.original_infos == answer.original());
	// The install patched to serve the texture: its folder moved, so the next rescan validates it again, and
	// the project's missing logo is the modder's own (the install has it; the modder's project lacks it).
	const std::string texture(18, '\0');
	TEST_EXPECT(write_install({{"shipped.mnu", shipped}, {"logo.tga", texture}}));
	editor_test::handle_to_end(session, request::rescan());
	TEST_EXPECT(baseline.validations() == validated + 1);
	TEST_EXPECT(logo_in("menus/shipped.mnu") != SIZE_MAX && !in_original_data(logo_in("menus/shipped.mnu"), v));
	// With no install, none is the original's.
	editor_test::set_game_install(session, std::string());
	editor_test::handle_to_end(session, request::rescan());
	TEST_EXPECT(v.findings.originals && v.findings.originals->findings.empty());
	TEST_EXPECT(!in_original_data(logo_in("menus/shipped.mnu"), v));
	std::printf("original data: per finding, against the install as a whole\n");
	return 0;
}

// The rows' marks over the install's findings by hand (the UX round's problems lane): each of the install's
// findings of a file taken by one row (an item and its duplicate name the same missing shadow, the install
// has it once: the first row's is the game's own, the duplicate's the modder's); a finding the install
// lacks is the modder's; a finding whose code gates never folds; nothing before the install is validated.
// The key: the record as itself over its path, the line left out, a record's or a message's numbers left
// out where it has no key. What blocks the build: the plan's refusals, else (no plan) the gating codes.
static int test_original_marks() {
	const auto shadow = [](const char *record) {
		Diagnostic d = editor_test::finding_of(DiagnosticSeverity::Error, "reference.missing", "No shadow.", "defs/items.def",
		                                       "shadow_texture");
		d.record = record;
		d.record_key = std::string("item:") + record;
		d.subject = ReferenceSubject{ReferenceKind::Texture, "shadow1.tga", std::string(), -1};
		return d;
	};
	std::vector<Diagnostic> rows = {shadow("Drivable Dune Buggy"), shadow("Drivable Dune Buggy")};
	Diagnostic identity = editor_test::finding_of(DiagnosticSeverity::Warning, "catalog.item_identity", "Same id.",
	                                              "defs/items.def", "id");
	identity.record = "Drivable Dune Buggy";
	identity.record_key = "item:Drivable Dune Buggy";
	rows.push_back(identity);
	Diagnostic logo = editor_test::finding_of(DiagnosticSeverity::Error, "reference.missing", "No logo.", "menus/shipped.mnu", "value");
	logo.subject = ReferenceSubject{ReferenceKind::MenuTexture, "logo.tga", std::string(), -1};
	rows.push_back(logo);
	rows.push_back(editor_test::finding_of(DiagnosticSeverity::Error, "document.parse", "Unreadable.", "menus/shipped.mnu"));
	OriginalData originals;
	originals.ready = true;
	originals.findings[normalized_logical_name("items.def")][original_finding_key(rows[0])] = 1;
	originals.findings[normalized_logical_name("shipped.mnu")][original_finding_key(rows[3])] = 1;
	originals.findings[normalized_logical_name("shipped.mnu")][original_finding_key(rows[4])] = 1;
	FindingMarks marks = mark_findings(rows, &originals, nullptr);
	TEST_EXPECT(marks.rows == rows.size() && marks.original == std::vector<uint8_t>({1, 0, 0, 1, 0}));
	TEST_EXPECT(marks.blocking == std::vector<uint8_t>({0, 0, 0, 0, 1}) && marks.blocking_count == 1);
	// Not validated yet: none is the game's own.
	OriginalData waiting = originals;
	waiting.ready = false;
	TEST_EXPECT(mark_findings(rows, &waiting, nullptr).original == std::vector<uint8_t>(rows.size(), 0));
	// The record as itself over its path: the same record anywhere (its owner renamed, a sibling inserted
	// before it) is the same key; another record is another.
	Diagnostic elsewhere = rows[0];
	elsewhere.record = "Vehicles/Drivable Dune Buggy";
	TEST_EXPECT(original_finding_key(elsewhere) == original_finding_key(rows[0]));
	elsewhere.record_key = "item:Drivable ATV";
	TEST_EXPECT(original_finding_key(elsewhere) != original_finding_key(rows[0]));
	// A line moved: the same key (an edit above a finding moves its line).
	Diagnostic moved = rows[0];
	moved.line = 40;
	TEST_EXPECT(original_finding_key(moved) == original_finding_key(rows[0]));
	// No key: the record's path, its numbers left out (an event's place among its siblings); a finding of no
	// record by its words, its numbers left out (a count, a place).
	Diagnostic event = editor_test::finding_of(DiagnosticSeverity::Warning, "reference.missing", "No sound.", "m.bms", "param1");
	event.record = "Event 3/Action 2";
	Diagnostic later = event;
	later.record = "Event 4/Action 1";
	TEST_EXPECT(original_finding_key(event) == original_finding_key(later));
	Diagnostic ends = editor_test::finding_of(DiagnosticSeverity::Warning, "document.parse", "3 line ends are so.", "a.wac");
	ends.line = 2;
	Diagnostic more = ends;
	more.message = "14 line ends are so.";
	more.line = 9;
	TEST_EXPECT(original_finding_key(ends) == original_finding_key(more));
	more.message = "14 line ends are not so.";
	TEST_EXPECT(original_finding_key(ends) != original_finding_key(more));
	// The plan's refusals alone block: a gating code the plan does not read (a project check's) blocks nothing.
	const std::vector<Diagnostic> blockers;
	marks = mark_findings(rows, &originals, &blockers);
	TEST_EXPECT(marks.blocking_count == 0 && marks.original[4] == 0);
	const std::vector<Diagnostic> refused = {rows[4]};
	marks = mark_findings(rows, &originals, &refused);
	TEST_EXPECT(marks.blocking_count == 1 && marks.blocking[4] == 1);
	// A view no session made works the marks out from what it holds; the session's are read while they are
	// the rows'.
	SessionView v;
	v.findings.diagnostics = rows;
	v.findings.originals = std::make_shared<const OriginalData>(originals);
	TEST_EXPECT(in_original_data(0, v) && in_original_data(3, v) && !in_original_data(2, v) && blocks_the_build(4, v));
	const ProblemCounts counts = count_problems(v);
	TEST_EXPECT(counts.original_errors == 2 && counts.errors == 2 && counts.warnings == 1 && counts.blocking == 1);
	FindingMarks session = mark_findings(rows, &originals, &blockers);
	v.findings.marks = std::make_shared<const FindingMarks>(session);
	TEST_EXPECT(!blocks_the_build(4, v) && count_problems(v).blocking == 0);
	std::printf("original marks: per finding, each of the install's taken once\n");
	return 0;
}

// What blocks the build (the UX round's problems lane): on a new project every required file is missing;
// the rows the gate refuses (the string tables the boot exits without, the main menu it dead-ends
// without) are marked, the rest are not; the query shows them alone; the refused build names them, its
// status line the first, each refusal said with why (the manifest's citation).
static int test_blocking() {
	editor_test::TempProjectDir dir("opennova_editor_problems_blocking");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Blocking"));
	const SessionView &v = session.view();
	std::set<std::string> blocking;
	for (size_t i = 0; i < v.findings.diagnostics.size(); ++i)
		if (blocks_the_build(i, v)) blocking.insert(subject_target(v.findings.diagnostics[i]));
	TEST_EXPECT(blocking == std::set<std::string>({"gametext.bin", "vmacros.bin", "keyhelp.bin", "main.mnu"}));
	TEST_EXPECT(count_problems(v).blocking == 4);
	ProblemQuery only;
	only.blocking = true;
	const ProblemAnswer answer = answer_problems(only, v);
	TEST_EXPECT(answer.rows.size() == 4 && answer.blocking == 4 && answer.total() == v.findings.diagnostics.size());
	for (const size_t row : answer.rows) {
		const Diagnostic &d = v.findings.diagnostics[row];
		TEST_EXPECT(blocker_reason(d).rfind("The game stops here as the original does: ", 0) == 0 &&
		            blocker_reason(d).find("[orig:") != std::string::npos);
	}
	editor_test::handle_to_end(session, request::build());
	TEST_EXPECT(v.activity.has_build && !v.activity.last_build->ok && v.activity.last_build->refused);
	TEST_EXPECT(v.activity.status ==
	            "Build refused: gametext.bin is missing: the game shows \"Unable to load game strings\" and exits (and 3 more). "
	            "See Problems.");
	const Diagnostic *refusal = nullptr;
	for (const Diagnostic &d : v.findings.diagnostics)
		if (d.code() == "build.blocked") refusal = &d;
	TEST_EXPECT(refusal && refusal->message.rfind("The build was refused: 4 problems stop it: gametext.bin is missing: ", 0) == 0 &&
	            refusal->message.find("; and 1 more. Problems marks them \"Blocks the build\".") != std::string::npos);
	std::printf("blocking: what a build is refused for, marked and named\n");
	return 0;
}

// A Fix all over the required files served whole (the review's M4): where the game install has some (here
// keyhelp.bin) and lacks the rest, the group's first fixes are an import of the one and placeholders of the
// others. Raised in one frame, as the Problems window's drain raises them: the placeholders first (made at
// once), then the import's preview (an operation over the project's files); neither is refused as busy.
static int test_fix_all_served_whole() {
	editor_test::TempProjectDir dir("opennova_editor_problems_fix_all");
	const std::string install = dir.file("install");
	const std::string table = "a table";
	const opennova::pff::PffWriteEntry entries[] = {
		{"keyhelp.bin", reinterpret_cast<const uint8_t *>(table.data()), uint32_t(table.size()), 0, 0, 0},
	};
	TEST_EXPECT(editor_test::write_text(install + "/readme.txt", "an install"));
	TEST_EXPECT(opennova::pff::pff_write_archive((install + "/resource.pff").c_str(), opennova::pff::PFF_FORMAT_PFF3, entries, 1) ==
	            opennova::pff::PFF_WRITE_OK);
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Fix all"));
	editor_test::set_game_install(session, install);
	editor_test::handle_to_end(session, request::rescan());
	const SessionView &v = session.view();
	std::vector<ProblemFix> firsts;
	for (const Diagnostic &d : v.findings.diagnostics) {
		if (d.code() != "requirement.missing") continue;
		const std::vector<ProblemFix> fixes = bulk_fixes_for(d, v);
		if (!fixes.empty()) firsts.push_back(fixes.front());
	}
	const std::vector<EditorRequest> merged = merge_fixes(firsts);
	TEST_EXPECT(merged.size() == 2);
	if (merged.size() != 2) return 1;
	TEST_EXPECT(merged[0].kind == EditorRequestKind::CreateMissing && merged[1].kind == EditorRequestKind::PreviewInstallImport &&
	            merged[1].names == std::vector<std::string>({"keyhelp.bin"}));
	// Raised in one frame: each served before any poll.
	session.handle(merged[0]);
	TEST_EXPECT(session.outcome().done());
	session.handle(merged[1]);
	TEST_EXPECT(session.outcome().operation != 0);
	session.run_operations();
	TEST_EXPECT(v.project.scan->find("gametext.bin") && v.project.scan->find("vmacros.bin") && !v.project.scan->find("keyhelp.bin"));
	TEST_EXPECT(v.dialogs.import_preview.open && v.dialogs.import_preview.plan);
	std::printf("fix all: placeholders made, then the import's preview\n");
	return 0;
}

// What only the build's plan refuses (the review's M3): an archive in the project is a Problems row before
// any build, marked "Blocks the build" like the gate's; the refusal's count, the rows' blocking count and
// Show them agree; a refused build adds no second row of it; the headline says whose refusals they are.
static int test_plan_refusals_are_rows() {
	editor_test::TempProjectDir dir("opennova_editor_problems_plan_rows");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Plan rows"));
	const SessionView &v = session.view();
	const opennova::pff::PffWriteEntry entries[] = {{"note.txt", reinterpret_cast<const uint8_t *>("x"), 1, 0, 0, 0}};
	TEST_EXPECT(opennova::pff::pff_write_archive((v.project.root + "/extra.pff").c_str(), opennova::pff::PFF_FORMAT_PFF3, entries, 1) ==
	            opennova::pff::PFF_WRITE_OK);
	editor_test::handle_to_end(session, request::rescan());
	const auto archive_rows = [&v]() {
		std::vector<size_t> rows;
		for (size_t i = 0; i < v.findings.diagnostics.size(); ++i)
			if (v.findings.diagnostics[i].code() == "build.archive_in_project") rows.push_back(i);
		return rows;
	};
	std::vector<size_t> rows = archive_rows();
	TEST_EXPECT(rows.size() == 1 && blocks_the_build(rows[0], v));
	TEST_EXPECT(count_problems(v).blocking == 5); // the four required files the boot stops without, and the archive
	ProblemQuery only;
	only.blocking = true;
	TEST_EXPECT(answer_problems(only, v).rows.size() == 5);
	editor_test::handle_to_end(session, request::build());
	TEST_EXPECT(v.activity.has_build && v.activity.last_build->refused);
	rows = archive_rows();
	TEST_EXPECT(rows.size() == 1 && blocks_the_build(rows[0], v) && count_problems(v).blocking == 5);
	const Diagnostic *refusal = nullptr;
	for (const Diagnostic &d : v.findings.diagnostics)
		if (d.code() == "build.blocked") refusal = &d;
	TEST_EXPECT(refusal && refusal->message.rfind("The build was refused: 5 problems stop it: ", 0) == 0);
	const BuildResult result = build_result(*v.activity.last_build, true);
	TEST_EXPECT(result.refusals.size() == 5 &&
	            result.headline == "Build refused: 5 problems stop it: the game would stop for some, and the editor does not pack the others.");
	TEST_EXPECT(std::find(result.refusals.begin(), result.refusals.end(),
	                      "extra.pff is an archive in the project: the build packs the project's files itself") != result.refusals.end());
	// The menu bar's words, from the build's own report whatever the status line says since (the review's L5).
	TEST_EXPECT(refused_words(*v.activity.last_build) ==
	            "Refused: " + result.refusals.front() + " (and 4 more).");
	// A name keeps its case in a refusal's words; a sentence's opener is lowered (the review's L6).
	Diagnostic upper = editor_test::finding_of(DiagnosticSeverity::Error, "build.archive_in_project", "x", "RESOURCE.PFF");
	TEST_EXPECT(blocker_words(upper) == "RESOURCE.PFF is an archive in the project: the build packs the project's files itself");
	Diagnostic named = editor_test::finding_of(DiagnosticSeverity::Error, "document.parse", "MAIN.MNU cannot be read.", "main.mnu");
	TEST_EXPECT(blocker_words(named) == "MAIN.MNU cannot be read");
	named.message = "The file cannot be read.";
	TEST_EXPECT(blocker_words(named) == "the file cannot be read");
	// A fatal table of another kind: read without a check, not refused with a message (the review's L1).
	Diagnostic wrong = editor_test::finding_of(DiagnosticSeverity::Error, "requirement.wrong_kind", "x", "strings/gametext.bin");
	wrong.subject = RequirementSubject{"gametext", "gametext.bin"};
	TEST_EXPECT(blocker_words(wrong) == "gametext.bin is not the kind of file the game reads there: the game reads it as one "
	                                   "without checking it, so it may crash or show garbage");
	TEST_EXPECT(blocker_reason(wrong).find("TextResource_FixupPointers @ 0x75d050") != std::string::npos &&
	            blocker_reason(wrong).find("Unable to load") == std::string::npos);
	std::printf("plan refusals: rows before any build, marked and counted alike\n");
	return 0;
}

// The demo round's bug 10: the required files landing take their "required file missing" rows out of
// Problems at once, while the validation the change left due has yet to run (an import's 27 such rows
// stood until the validation after it ended); the other rows wait for it, which then composes them.
static int test_requirements_at_once() {
	editor_test::TempProjectDir dir("opennova_editor_problems_requirements");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "At once"));
	const SessionView &v = session.view();
	const auto missing = [&v] {
		size_t n = 0;
		for (const Diagnostic &d : v.findings.diagnostics) n += d.code() == "requirement.missing" ? 1 : 0;
		return n;
	};
	TEST_EXPECT(missing() > 0);
	const size_t compositions = session.problems_compositions();
	session.handle(request::create_missing(unmet_required_roles(*v.project.requirements)));
	TEST_EXPECT(session.outcome().done() && v.activity.validation.running && missing() == 0 &&
	            session.problems_compositions() == compositions);
	session.run_operations();
	TEST_EXPECT(!v.activity.validation.running && missing() == 0 && session.problems_compositions() > compositions);
	std::printf("requirements: their rows follow the files at once\n");
	return 0;
}

int main() {
	int failures = 0;
	failures += test_requirements_at_once();
	failures += test_original_marks();
	failures += test_blocking();
	failures += test_fix_all_served_whole();
	failures += test_plan_refusals_are_rows();
	failures += test_findings_index();
	failures += test_locations_and_fixes();
	failures += test_query();
	failures += test_fixes();
	failures += test_rewrite_unserializable();
	failures += test_fix_cache();
	failures += test_merge();
	failures += test_location();
	failures += test_use_required_only();
	failures += test_fix_index();
	failures += test_optional_group();
	failures += test_placeholders();
	failures += test_original_data();
	if (failures == 0) std::printf("editor_problems: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
