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
#include <string>
#include <utility>
#include <vector>

#include <editor/blank/blank_factory.h>
#include <editor/documents/mnu_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_queries.h>
#include <editor/graph/rename_transaction.h>
#include <editor/project/project_files.h>
#include <editor/project_build/build_plan.h>
#include <editor/session/view/findings_index.h>
#include <editor/session/preferences_store.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/problem_query.h>
#include <editor/session/project_session.h>
#include <editor/session/view/session_view.h>
#include <formats/pff/pff.h>
#include <formats/rtxt/rtxt.h>

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
	Diagnostic d = make_diagnostic(severity, code, message, asset, field);
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
		if (d.code == code && d.role == role) return &d;
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
	font.reference = ReferenceKind::Font;
	font.target = "nofont.fnt";
	Diagnostic required = finding(DiagnosticSeverity::Error, "requirement.missing", "Missing required file gametext.bin.");
	required.role = "gametext";
	required.target = "gametext.bin";
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
	// Only the fixable: the missing font (Create), the stylesheet and the catalog (Rewrite);
	// the requirement has no row in this view to fix.
	query.fixable = true;
	TEST_EXPECT(rows_of(query, view) == std::vector<size_t>({3, 4, 5}));

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
	                                                "Catalogs", "rename"}));
	TEST_EXPECT(kinds.groups[2].key == "style" && kinds.groups[2].rows.size() == 2 && kinds.groups[5].key == "rename");
	TEST_EXPECT(kinds.rows == std::vector<size_t>({1, 3, 4, 2, 0, 5, 6}) && kinds.total() == 7);
	TEST_EXPECT(problem_family_title("reference.missing") == "Missing references" &&
	            problem_family_title("animation_map.row") == "Animation maps" && problem_family_title("odd") == "odd");
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
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Fixes"));
	const SessionView &v = session.view();
	const std::string root = v.project.root;
	blank.logical_name = "spare.bin";
	TEST_EXPECT(make_blank(blank, AssetKind::Strings, table, error));
	TEST_EXPECT(editor_test::write_bytes(root + "/strings/spare.bin", table) && editor_test::write_bytes(root + "/strings/other.bin", table));
	TEST_EXPECT(editor_test::write_text(root + "/menus/a.mnu", kMenuA) && editor_test::write_text(root + "/menus/b.mnu", kMenuB));
	TEST_EXPECT(editor_test::write_text(root + "/art/splash.tga", "tga") && editor_test::write_text(root + "/art/splash.pcx", "pcx"));
	TEST_EXPECT(editor_test::write_text(root + "/foo.bin", "raw bytes")); // a .bin that is no string table
	session.handle(make_request(EditorRequestKind::Rescan));

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
	            fixes[0].request.names == std::vector<std::string>({"gameerr"}));
	// Every fix acts on the files: its detail says Undo cannot take it back.
	TEST_EXPECT(fixes[0].detail.find("placeholder") != std::string::npos &&
	            fixes[0].detail.find(kNotUndoable) != std::string::npos);
	TEST_EXPECT(!fixes[1].bulk && fixes[1].request.kind == EditorRequestKind::AssignRequirement &&
	            fixes[1].request.text == "gameerr" && fixes[1].request.path == "strings/other.bin");
	TEST_EXPECT(fixes[1].detail == "Renames other.bin to gameerr.bin; nothing refers to it. It cannot be undone with Undo.");
	TEST_EXPECT(has_fixes(*gameerr, v));
	// With the game data it has, Import from it too (the import dialog on that one file, planned
	// with the files it needs as the editor's setting says).
	editor_test::set_retail_directory(session, install);
	gameerr = requirement_finding(v, "requirement.missing", "gameerr");
	TEST_EXPECT(gameerr != nullptr);
	if (!gameerr) return 1;
	fixes = fixes_for(*gameerr, v);
	TEST_EXPECT(labels_of(fixes) == std::vector<std::string>({"Create gameerr.bin", "Import gameerr.bin from the game data...",
	                                                          "Use other.bin as gameerr.bin", "Use spare.bin as gameerr.bin"}));
	if (fixes.size() != 4) return 1;
	TEST_EXPECT(fixes[1].bulk && fixes[1].request.kind == EditorRequestKind::PreviewRetailImport &&
	            fixes[1].request.names == std::vector<std::string>({"gameerr.bin"}) && fixes[1].request.flag);
	TEST_EXPECT(fixes[1].detail.find(kNotUndoable) != std::string::npos);
	// A required file without a factory (missions on: cmap.mnu): no Create; each menu to use
	// says what its rename rewrites (b.mnu: the ACTION of a.mnu that names it).
	editor_test::set_missions(session, true);
	const Diagnostic *cmap = requirement_finding(v, "requirement.missing", "cmap_menu");
	TEST_EXPECT(cmap != nullptr);
	if (!cmap) return 1;
	fixes = fixes_for(*cmap, v);
	TEST_EXPECT(labels_of(fixes) == std::vector<std::string>({"Use a.mnu as cmap.mnu", "Use b.mnu as cmap.mnu"}));
	if (fixes.size() != 2) return 1;
	TEST_EXPECT(fixes[0].detail == "Renames a.mnu to cmap.mnu; nothing refers to it. It cannot be undone with Undo.");
	TEST_EXPECT(fixes[1].detail ==
	            "Renames b.mnu to cmap.mnu and rewrites 1 reference in 1 file. It cannot be undone with Undo.");
	editor_test::set_missions(session, false);
	// An optional file the project lacks: its note offers the same (a factory, the game data).
	const Diagnostic *brand = requirement_finding(v, "requirement.optional_missing", "brand_style");
	TEST_EXPECT(brand != nullptr && brand->severity == DiagnosticSeverity::Info);
	if (!brand) return 1;
	TEST_EXPECT(labels_of(fixes_for(*brand, v)) ==
	            std::vector<std::string>({"Create brand.mns", "Import brand.mns from the game data..."}));
	// S11e: an optional file is made or imported, never taken from another file: no Use for
	// loading.pcx though the project has a splash.pcx (and no factory or game data has one).
	const Diagnostic *loading = requirement_finding(v, "requirement.optional_missing", "loading_pcx");
	TEST_EXPECT(loading && fixes_for(*loading, v).empty() && !has_fixes(*loading, v));
	// The game's boot report of a required file: the fixes of its requirement; none once the
	// project has the file (the row is then only a place to look).
	Diagnostic boot = make_diagnostic(DiagnosticSeverity::Error, "play.boot_missing", "The game could not find gameerr.bin.");
	boot.role = "gameerr";
	boot.target = "gameerr.bin";
	gameerr = requirement_finding(v, "requirement.missing", "gameerr");
	TEST_EXPECT(gameerr && labels_of(fixes_for(boot, v)) == labels_of(fixes_for(*gameerr, v)));
	EditorRequest create = make_request(EditorRequestKind::CreateMissing);
	create.names = {"gameerr"};
	session.handle(create);
	TEST_EXPECT(session.outcome().done() && fixes_for(boot, v).empty() && !has_fixes(boot, v));
	Diagnostic unknown = boot;
	unknown.role.clear();
	TEST_EXPECT(fixes_for(unknown, v).empty());

	// A missing reference to a file: Import a name its loader reads from the game data, then
	// Create it blank when its kind has a free-form factory (not in bulk).
	Diagnostic font = make_diagnostic(DiagnosticSeverity::Error, "reference.missing", "The font is missing.", "menus/a.mnu",
	                                  "font.name");
	font.reference = ReferenceKind::Font;
	font.target = "Custom.fnt";
	fixes = fixes_for(font, v);
	TEST_EXPECT(labels_of(fixes) == std::vector<std::string>({"Import Custom.fnt from the game data...", "Create Custom.fnt"}));
	if (fixes.size() != 2) return 1;
	TEST_EXPECT(fixes[0].bulk && fixes[0].request.names == std::vector<std::string>({"Custom.fnt"}));
	TEST_EXPECT(!fixes[1].bulk && fixes[1].request.kind == EditorRequestKind::CreateFile &&
	            fixes[1].request.path == "Custom.fnt" && fixes[1].request.text == "font");
	TEST_EXPECT(fixes[1].detail.find(kNotUndoable) != std::string::npos);
	// A menu texture: the .dds its .tga falls back to, then (S11h) a placeholder as its .tga.
	Diagnostic texture = font;
	texture.reference = ReferenceKind::MenuTexture;
	texture.target = "logo.tga";
	TEST_EXPECT(labels_of(fixes_for(texture, v)) ==
	            std::vector<std::string>({"Import logo.dds from the game data...", "Create a placeholder logo.tga"}));
	// A .png the menu names: a texture as the scan lists a PNG with no import record, so the
	// game data's is offered; no placeholder is a .png.
	texture.target = "badge.png";
	TEST_EXPECT(labels_of(fixes_for(texture, v)) == std::vector<std::string>({"Import badge.png from the game data..."}));
	// A model's texture row (S11f): the file its type's loader reads from what the game data
	// has, a diffuse row's .dds sibling; a plain row (type 1) reads its own name alone, which
	// the game data lacks; a texture of no model reads as the runtime's texture lookup, its
	// stem's .dds among the names (S11h); each takes a placeholder as the name it opens once
	// that is there (S11h).
	Diagnostic skin = font;
	skin.reference = ReferenceKind::Texture;
	skin.target = "logo.tga";
	skin.loader_arg = 0;
	TEST_EXPECT(labels_of(fixes_for(skin, v)) ==
	            std::vector<std::string>({"Import logo.dds from the game data...", "Create a placeholder logo.tga"}));
	skin.loader_arg = 1;
	TEST_EXPECT(labels_of(fixes_for(skin, v)) == std::vector<std::string>({"Create a placeholder logo.tga"}));
	skin.loader_arg = -1;
	TEST_EXPECT(labels_of(fixes_for(skin, v)) ==
	            std::vector<std::string>({"Import logo.dds from the game data...", "Create a placeholder logo.tga"}));
	// A name the game data lacks with no factory, and a symbol: nothing to do but look.
	Diagnostic model = font;
	model.reference = ReferenceKind::Model;
	model.target = "tank";
	Diagnostic text_id = font;
	text_id.reference = ReferenceKind::TextId;
	text_id.target = "NO_SUCH_ID";
	TEST_EXPECT(fixes_for(model, v).empty() && fixes_for(text_id, v).empty() && !has_fixes(text_id, v));
	// No Create for a name another kind of file holds (it would only open, the reference still
	// missing), nor for one the project's name rules refuse (past the archive's 16 bytes).
	Diagnostic table_ref = font;
	table_ref.reference = ReferenceKind::TextTable;
	table_ref.target = "foo.bin";
	TEST_EXPECT(v.project.scan->find("foo.bin") && v.project.scan->find("foo.bin")->kind == AssetKind::RawBin);
	TEST_EXPECT(fixes_for(table_ref, v).empty() && !has_fixes(table_ref, v));
	table_ref.target = "freshtable.bin";
	TEST_EXPECT(labels_of(fixes_for(table_ref, v)) == std::vector<std::string>({"Create freshtable.bin"}));
	Diagnostic long_menu = font;
	long_menu.reference = ReferenceKind::Menu;
	long_menu.target = "averyveryverylongname.mnu";
	TEST_EXPECT(fixes_for(long_menu, v).empty());

	// A missing import output: import its source again.
	const Diagnostic output = make_diagnostic(DiagnosticSeverity::Warning, "import.output_missing", "logo.pcx has not been made.",
	                                          "art/logo.png");
	fixes = fixes_for(output, v);
	TEST_EXPECT(fixes.size() == 1 && fixes[0].label == "Import logo.png again" && fixes[0].bulk);
	if (fixes.size() != 1) return 1;
	TEST_EXPECT(fixes[0].request.kind == EditorRequestKind::Reimport && fixes[0].request.path == "art/logo.png" &&
	            fixes[0].request.flag && fixes[0].detail.find(kNotUndoable) != std::string::npos);
	// Input a rewrite drops or normalizes: Rewrite the file (a Save of it), saying what goes.
	for (const char *code :
	     {"style.line_ending", "catalog.ignored_input", "menu.ignored_input", "animation_map.ignored_input", "strings.regrouped"}) {
		const Diagnostic rewrite = make_diagnostic(DiagnosticSeverity::Warning, code, "A rewrite fixes this.", "menus/a.mnu");
		fixes = fixes_for(rewrite, v);
		TEST_EXPECT(fixes.size() == 1 && fixes[0].label == "Rewrite a.mnu" && fixes[0].bulk);
		if (fixes.size() != 1) return 1;
		TEST_EXPECT(fixes[0].request.kind == EditorRequestKind::Save && fixes[0].request.path == "menus/a.mnu");
		TEST_EXPECT(fixes[0].detail.find("Writes menus/a.mnu again") == 0 && fixes[0].detail.find("unsaved") == std::string::npos &&
		            fixes[0].detail.find(kNotUndoable) != std::string::npos);
	}
	// Open with unsaved edits, the rewrite saves them too, and says so.
	session.handle(make_request(EditorRequestKind::OpenDocument, "menus/a.mnu"));
	Document *menu = session.document_for("menus/a.mnu");
	NodeAddress go;
	TEST_EXPECT(menu && find_definition(AssetGraph(), *menu, "GO", go));
	if (!menu) return 1;
	EditorRequest edit = make_request(EditorRequestKind::EditRecord, menu->path());
	edit.edit.address = go;
	edit.edit.field = "position.left";
	edit.edit.value = int64_t(20);
	session.handle(edit);
	TEST_EXPECT(menu->dirty());
	const Diagnostic ending = make_diagnostic(DiagnosticSeverity::Error, "style.line_ending", "Line 3 ends LF.", "menus/a.mnu");
	TEST_EXPECT(fixes_for(ending, v).front().detail.find("unsaved edits") != std::string::npos);
	// Anything else has none; has_fixes agrees with fixes_for on every finding of the session,
	// and bulk_fixes_for (which plans no rename) gives fixes_for's bulk ones, in order.
	const Diagnostic other = make_diagnostic(DiagnosticSeverity::Error, "menu.duplicate_window", "Two windows.", "menus/a.mnu");
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
	session.handle(make_request(EditorRequestKind::Undo, menu->path()));
	session.handle(make_request(EditorRequestKind::CloseProject));
	TEST_EXPECT(fixes_for(font, v).empty() && fixes_for(boot, v).empty());
	return 0;
}

// No Rewrite for a file whose own finding says it does not serialize (its Save is refused):
// fixes_for, has_fixes, bulk_fixes_for and the query's fixable filter agree; another file's
// input that a rewrite drops keeps its Rewrite.
static int test_rewrite_unserializable() {
	SessionView view;
	view.project.open = true;
	view.findings.diagnostics = {finding(DiagnosticSeverity::Warning, "catalog.ignored_input", "A key the game ignores.", "defs/weapon.def"),
	                    finding(DiagnosticSeverity::Error, "catalog.unserializable", "It cannot be written.", "defs/weapon.def"),
	                    finding(DiagnosticSeverity::Warning, "catalog.ignored_input", "A key the game ignores.", "defs/ammo.def"),
	                    finding(DiagnosticSeverity::Warning, "strings.regrouped", "A section read twice.", "strings/menu.bin"),
	                    finding(DiagnosticSeverity::Error, "strings.invalid_input", "A string it cannot hold.", "strings/menu.bin")};
	for (const size_t blocked : {size_t(0), size_t(3)}) {
		const Diagnostic &d = view.findings.diagnostics[blocked];
		TEST_EXPECT(fixes_for(d, view).empty() && !has_fixes(d, view) && bulk_fixes_for(d, view).empty());
	}
	TEST_EXPECT(labels_of(fixes_for(view.findings.diagnostics[2], view)) == std::vector<std::string>({"Rewrite ammo.def"}));
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
	view.findings.diagnostics[0].code = "menu.test"; // unmarked: still the answer kept
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
	EditorRequest menu = make_request(EditorRequestKind::CreateMissing);
	menu.names = {"main_menu"};
	EditorRequest text = make_request(EditorRequestKind::CreateMissing);
	text.names = {"gametext", "main_menu"};
	EditorRequest listed = make_request(EditorRequestKind::PreviewRetailImport);
	listed.names = {"MAIN.MNU"};
	EditorRequest fonts = make_request(EditorRequestKind::PreviewRetailImport);
	fonts.names = {"Arial14b.fnt"};
	EditorRequest again = make_request(EditorRequestKind::Reimport, "art/logo.png");
	again.flag = true;
	const std::vector<ProblemFix> fixes = {
		fix(make_request(EditorRequestKind::Save, "defs/items.def"), true),
		fix(menu, true),
		fix(listed, true),
		fix(make_request(EditorRequestKind::AssignRequirement, "menus/a.mnu", "main_menu"), false),
		fix(text, true),
		fix(make_request(EditorRequestKind::Save, "defs/items.def"), true),
		fix(again, true),
		fix(fonts, true),
		fix(make_request(EditorRequestKind::Save, "menus/menu_style.mns"), true),
		fix(make_request(EditorRequestKind::CreateFile, "Custom.fnt", "font"), false),
		fix(again, true),
	};
	const std::vector<EditorRequest> merged = merge_fixes(fixes);
	TEST_EXPECT(merged.size() == 5);
	if (merged.size() != 5) return 1;
	TEST_EXPECT(merged[0].kind == EditorRequestKind::Save && merged[0].path == "defs/items.def");
	TEST_EXPECT(merged[1].kind == EditorRequestKind::CreateMissing &&
	            merged[1].names == std::vector<std::string>({"main_menu", "gametext"}));
	TEST_EXPECT(merged[2].kind == EditorRequestKind::PreviewRetailImport &&
	            merged[2].names == std::vector<std::string>({"MAIN.MNU", "Arial14b.fnt"}));
	TEST_EXPECT(merged[3].kind == EditorRequestKind::Reimport && merged[3].path == "art/logo.png" && merged[3].flag);
	TEST_EXPECT(merged[4].kind == EditorRequestKind::Save && merged[4].path == "menus/menu_style.mns");
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
	editor_test::own(view.project.scan).entries = {entry("items.def", "defs/items.def", AssetKind::ItemDefs), entry("logo.png", "art/logo.png", AssetKind::ImageSource),
	                     entry("Arial14b.fnt", "fonts/Arial14b.fnt", AssetKind::Font)};
	editor_test::own(view.project.scan).index();
	Diagnostic required = make_diagnostic(DiagnosticSeverity::Error, "requirement.missing", "Missing required file main.mnu.");
	required.role = "main_menu";
	required.target = "main.mnu";
	TEST_EXPECT(problem_location(required, view).empty());
	Diagnostic catalog = make_diagnostic(DiagnosticSeverity::Error, "catalog.item_type", "Choose an item type.", "defs/items.def", "type");
	catalog.record = "Marker";
	catalog.row_id = 4;
	catalog.record_kind = 2;
	const ProblemLocation opened = problem_location(catalog, view);
	TEST_EXPECT(opened.path == "defs/items.def" && opened.record == (NodeAddress{4, 2, 0}) && opened.field == "type" &&
	            !opened.in_files);
	const EditorRequest open = opened.request();
	TEST_EXPECT(open.kind == EditorRequestKind::OpenDocument && open.path == "defs/items.def" &&
	            open.edit.address == (NodeAddress{4, 2, 0}) && open.edit.field == "type");
	const ProblemLocation file = problem_location(make_diagnostic(DiagnosticSeverity::Warning, "catalog.ignored_input",
	                                                              "An unknown key.", "defs/items.def", "subtype"),
	                                              view);
	TEST_EXPECT(file.path == "defs/items.def" && file.record == NodeAddress() && file.field.empty());
	// S12: a file of a kind the editor does not open is shown in Files (ShowInFiles), as is a
	// file whose name or place is the finding, one the editor opens too; a name the scan does
	// not list as a path goes nowhere.
	for (const char *asset : {"fonts/Arial14b.fnt", "art/logo.png"}) {
		const ProblemLocation shown = problem_location(make_diagnostic(DiagnosticSeverity::Warning, "graph.unreadable", "A finding.", asset), view);
		TEST_EXPECT(shown.path == asset && shown.in_files && shown.request().kind == EditorRequestKind::ShowInFiles &&
		            shown.request().path == asset && !shown.request().flag);
	}
	for (const char *code : {"asset.name.too_long", "asset.name.duplicate", "build.name_unstorable", "build.archive_in_project"}) {
		Diagnostic named = catalog;
		named.code = code;
		const ProblemLocation shown = problem_location(named, view);
		TEST_EXPECT(shown.path == "defs/items.def" && shown.in_files && shown.record == NodeAddress() &&
		            shown.request().kind == EditorRequestKind::ShowInFiles);
	}
	for (const char *asset : {"defs/weapon.def", "items.def"})
		TEST_EXPECT(problem_location(make_diagnostic(DiagnosticSeverity::Error, "x.y", "A finding.", asset), view).empty());
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
	required.role = "test_screen";
	required.target = "screen.pcx";
	Diagnostic optional = finding(DiagnosticSeverity::Info, "requirement.optional_missing", "Optional file splash.pcx.");
	optional.role = "test_splash";
	optional.target = "splash.pcx";
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
		const std::string file = "defs/f" + std::to_string(i % 30) + ".def";
		view.findings.diagnostics.push_back(finding(DiagnosticSeverity::Warning, "catalog.ignored_input", "An unknown key.", file.c_str()));
	}
	view.findings.diagnostics.push_back(finding(DiagnosticSeverity::Error, "catalog.unserializable", "It cannot be written.", "defs/f7.def"));
	const ProblemFixIndex index(view);
	TEST_EXPECT(index.unserializable.size() == 1 && index.unserializable.count("defs/f7.def"));
	ProblemFixCache cache;
	for (size_t i : {size_t(0), size_t(7), size_t(8), size_t(37), size_t(2999), size_t(3000)}) {
		const Diagnostic &d = view.findings.diagnostics[i];
		const bool fixable = d.asset != "defs/f7.def" && d.code == "catalog.ignored_input";
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
// takes the .dds a missing .tga leaves), a particle's graphic the name as written (or, with no
// extension, the runtime's lookup's first name the factory makes); each in bulk, a Fix all making
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
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Placeholders"));
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
	session.handle(make_request(EditorRequestKind::Rescan));
	const auto missing = [&v](ReferenceKind kind, const char *target) -> const Diagnostic * {
		for (const Diagnostic &d : v.findings.diagnostics)
			if (d.code == "reference.missing" && d.reference == kind && d.target == target) return &d;
		return nullptr;
	};
	const Diagnostic *skin = missing(ReferenceKind::Texture, "armry.tga");
	const Diagnostic *logo = missing(ReferenceKind::MenuTexture, "logo.tga");
	const Diagnostic *puff = missing(ReferenceKind::Texture, "puff.tga");
	TEST_EXPECT(skin && skin->loader_arg == 0 && logo && puff && puff->loader_arg == -1);
	if (!skin || !logo || !puff) return 1;
	std::vector<ProblemFix> firsts;
	for (const auto &expected : {std::make_pair(skin, "armry.tga"), std::make_pair(logo, "logo.tga"), std::make_pair(puff, "puff.tga")}) {
		const std::vector<ProblemFix> fixes = fixes_for(*expected.first, v);
		TEST_EXPECT(labels_of(fixes) == std::vector<std::string>({std::string("Create a placeholder ") + expected.second}));
		if (fixes.size() != 1) return 1;
		TEST_EXPECT(fixes[0].bulk && fixes[0].request.kind == EditorRequestKind::CreateFile &&
		            fixes[0].request.path == expected.second && fixes[0].request.text == "texture");
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
	chunk.loader_arg = 16;
	Diagnostic png = *logo;
	png.target = "badge.png";
	TEST_EXPECT(fixes_for(chunk, v).empty() && fixes_for(png, v).empty() && !has_fixes(png, v));
	// A particle's graphic, as the runtime's texture lookup reads it: that lookup reads a name
	// with an extension first, so one the factory cannot write takes none; a name with none
	// takes the lookup's first name the factory makes, its .tga.
	Diagnostic particle = *puff;
	particle.target = "puff.png";
	TEST_EXPECT(fixes_for(particle, v).empty() && !has_fixes(particle, v));
	particle.target = "foo";
	TEST_EXPECT(labels_of(fixes_for(particle, v)) == std::vector<std::string>({"Create a placeholder foo.tga"}));
	// A model row takes a placeholder only in the format its reader reads the name as: a plain
	// row reads a.tga.pcx through its TGA reader (the factory would write a PCX) but a.pcx.tga
	// as the TGA it is; a diffuse row reads a.tga.pcx as its query, a.tga; a normal map reads no
	// PCX at all, a height producer no .mdt (renderer::material_texture_source).
	Diagnostic compound = *skin;
	compound.loader_arg = 1;
	compound.target = "a.tga.pcx";
	TEST_EXPECT(fixes_for(compound, v).empty());
	compound.target = "a.pcx.tga";
	TEST_EXPECT(labels_of(fixes_for(compound, v)) == std::vector<std::string>({"Create a placeholder a.pcx.tga"}));
	compound.loader_arg = 0;
	compound.target = "a.tga.pcx";
	TEST_EXPECT(labels_of(fixes_for(compound, v)) == std::vector<std::string>({"Create a placeholder a.tga"}));
	compound.loader_arg = 4;
	compound.target = "bump.pcx";
	TEST_EXPECT(fixes_for(compound, v).empty());
	compound.target = "bump.tga";
	TEST_EXPECT(labels_of(fixes_for(compound, v)) == std::vector<std::string>({"Create a placeholder bump.tga"}));
	compound.target = "bump.mdt";
	TEST_EXPECT(labels_of(fixes_for(compound, v)) == std::vector<std::string>({"Create a placeholder bump.mdt"}));
	compound.loader_arg = 6;
	TEST_EXPECT(fixes_for(compound, v).empty());
	// With the game install holding the menu texture's .dds: Import it first, the placeholder after.
	const std::string install = dir.file("install");
	const uint8_t bytes[] = {'d', 'a', 't', 'a'};
	const opennova::pff::PffWriteEntry entries[] = {{"logo.dds", bytes, sizeof(bytes), 0, 0, 0}};
	TEST_EXPECT(editor_test::write_text(install + "/readme.txt", "an install"));
	TEST_EXPECT(opennova::pff::pff_write_archive((install + "/resource.pff").c_str(), opennova::pff::PFF_FORMAT_PFF3, entries, 1) ==
	            opennova::pff::PFF_WRITE_OK);
	editor_test::set_retail_directory(session, install);
	logo = missing(ReferenceKind::MenuTexture, "logo.tga");
	TEST_EXPECT(logo && labels_of(fixes_for(*logo, v)) ==
	                            std::vector<std::string>({"Import logo.dds from the game data...", "Create a placeholder logo.tga"}));
	// Applied: the checkerboard written at the root, the finding gone, the reference found.
	skin = missing(ReferenceKind::Texture, "armry.tga");
	TEST_EXPECT(skin != nullptr);
	if (!skin) return 1;
	session.handle(fixes_for(*skin, v).front().request);
	TEST_EXPECT(session.outcome().done());
	const AssetEntry *made = v.project.scan->find("armry.tga");
	TEST_EXPECT(made && made->kind == AssetKind::Texture && made->relative_path == "armry.tga");
	BlankRequest blank;
	blank.logical_name = "armry.tga";
	std::vector<uint8_t> checkerboard;
	Diagnostic error;
	TEST_EXPECT(make_blank(blank, AssetKind::Texture, checkerboard, error) && test_io::read_file(root + "/armry.tga") == checkerboard);
	TEST_EXPECT(missing(ReferenceKind::Texture, "armry.tga") == nullptr);
	bool found = false;
	for (const GraphEdge *edge : v.findings.graph->references_of("models/armory.3di")) {
		if (edge->kind != ReferenceKind::Texture || edge->value != "armry.tga") continue;
		std::string file;
		TEST_EXPECT(v.findings.graph->resolve(*edge, &file) == ReferenceStatus::Present && file == "armry.tga");
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
	TEST_EXPECT(problem_family_title("requirement.optional_missing") == "Optional files" &&
	            problem_family_title("requirement.missing") == "Required files");
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
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Places"));
	editor_test::create_missing_files(session);
	editor_test::set_retail_directory(session, install);
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
	session.handle(make_request(EditorRequestKind::Rescan));
	const auto finding_in = [&v](const char *code, const std::string &asset) -> const Diagnostic * {
		for (const Diagnostic &d : v.findings.diagnostics)
			if (d.code == code && d.asset == asset) return &d;
		return nullptr;
	};

	// A menu's input retail's reader leaves out: on the window the reader found it in (the
	// second of two named GO), which a click selects.
	const Diagnostic *ignored = finding_in("menu.ignored_input", "menus/a.mnu");
	TEST_EXPECT(ignored && ignored->row_id && ignored->record == "A/MAIN/GO");
	if (!ignored) return 1;
	session.handle(problem_location(*ignored, v).request());
	const Document *menu = session.document_for("menus/a.mnu");
	NodeAddress main_window, second_go;
	TEST_EXPECT(menu && find_definition(AssetGraph(), *menu, "MAIN", main_window));
	if (!menu) return 1;
	for (const Document::Collection &collection : menu->collections_of(main_window))
		if (std::string(menu->kind_token(collection.spec.kind)) == "window" && collection.ids.size() == 2)
			second_go = {main_window.row, collection.spec.kind, collection.ids[1]};
	TEST_EXPECT(second_go.child && menu->record_name(second_go) == "GO" && v.documents.selection == second_go);

	// An ungrouped table: on the first string the game reads under another section.
	const Diagnostic *regrouped = finding_in("strings.regrouped", "strings/ungrouped.bin");
	TEST_EXPECT(regrouped && regrouped->row_id && regrouped->record == "Menu");
	if (!regrouped) return 1;
	session.handle(problem_location(*regrouped, v).request());
	const Document *strings = session.document_for("strings/ungrouped.bin");
	TEST_EXPECT(strings && v.documents.active == strings->path() && strings->record_name(v.documents.selection) == "MM_Cafe");

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
	session.handle(fixes[0].request);
	Document *adm = session.document_for("anims/noreset.adm");
	TEST_EXPECT(session.outcome().done() && adm && adm->dirty() && adm->rows().size() == 2);
	if (!adm) return 1;
	TEST_EXPECT(adm->record_name(v.documents.selection) == "anim_reset" && !finding_in("animation_map.no_reset", adm->path()));
	session.handle(make_request(EditorRequestKind::Undo, adm->path()));
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
		catalog_findings += d.asset == items_path && (d.code == "catalog.name_duplicate" || d.code == "catalog.item_identity");
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
	TEST_EXPECT(fixes[0].request.kind == EditorRequestKind::ShowInFiles && fixes[0].request.flag &&
	            fixes[0].request.path == long_path && !fixes[0].bulk);
	// Each ask one RevealFile event, the same file asked again another: Files shows it again.
	const uint64_t before = v.events.next_seq() - 1;
	session.handle(fixes[0].request);
	std::vector<ViewEvent> shown = editor_test::events_after(v, before, ViewEventKind::RevealFile);
	TEST_EXPECT(session.outcome().done() && shown.size() == 1 && shown[0].path == long_path && shown[0].flag &&
	            !shown[0].address.row && shown[0].field.empty() && shown[0].tag == 0);
	session.handle(make_request(EditorRequestKind::ShowInFiles, "a_name_too_long_for_archives.tga"));
	shown = editor_test::events_after(v, before, ViewEventKind::RevealFile);
	TEST_EXPECT(shown.size() == 2 && shown[1].path == long_path && !shown[1].flag && shown[1].seq == shown[0].seq + 1);
	session.handle(make_request(EditorRequestKind::ShowInFiles, "nowhere.tga"));
	TEST_EXPECT(!session.outcome().done() && editor_test::events_after(v, before, ViewEventKind::RevealFile).size() == 2);
	const Diagnostic unstorable =
	        make_diagnostic(DiagnosticSeverity::Error, "build.name_unstorable", "The name is too long.", long_path);
	TEST_EXPECT(labels_of(fixes_for(unstorable, v)) == std::vector<std::string>({"Rename a_name_too_long_for_archives.tga..."}));
	// Two files of one name: the fix renames the one the finding is on (other/twin.tga, the
	// second), and the rename takes that one, not the other of its name. c.mnu's reference
	// reaches the first, which the game finds: renaming the second rewrites nothing, and c.mnu
	// still reaches the first.
	std::string twin;
	for (const Diagnostic &d : v.findings.diagnostics)
		if (d.code == "asset.name.duplicate") twin = d.asset;
	TEST_EXPECT(twin == "other/twin.tga");
	Diagnostic duplicate = make_diagnostic(DiagnosticSeverity::Error, "asset.name.duplicate", "Two files.", twin);
	fixes = fixes_for(duplicate, v);
	TEST_EXPECT(fixes.size() == 1 && fixes[0].request.path == twin);
	const ProjectPaths paths = ProjectPaths::for_root(root);
	const RenamePlan first_plan = plan_rename(paths, *v.project.scan, *v.findings.graph, "art/twin.tga", "twin3.tga");
	TEST_EXPECT(first_plan.ok() && first_plan.sites.size() == 1 && first_plan.sites[0].file == "menus/c.mnu");
	const RenamePlan second_plan = plan_rename(paths, *v.project.scan, *v.findings.graph, twin, "twin2.tga");
	TEST_EXPECT(second_plan.ok() && second_plan.path == twin && second_plan.sites.empty());
	session.handle(make_request(EditorRequestKind::RenameAsset, twin, "twin2.tga"));
	const std::string moved = (fs::path(twin).parent_path() / "twin2.tga").generic_string();
	TEST_EXPECT(session.outcome().done() && fs::exists(root + "/" + moved) && !fs::exists(root + "/" + twin));
	TEST_EXPECT(fs::exists(root + "/art/twin.tga") &&
	            std::none_of(v.findings.diagnostics.begin(), v.findings.diagnostics.end(),
	                         [](const Diagnostic &d) { return d.code == "rename.partial"; }));
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
	            fixes[1].request.flag);

	// A missing symbol: the file where it belongs, opened (a style variable's stylesheet, the
	// menu a screen is looked up in, the table a string id's scope names); none for a string
	// id whose window names no table.
	Diagnostic style = make_diagnostic(DiagnosticSeverity::Warning, "reference.missing", "A variable.", "menus/a.mnu", "font.name");
	style.reference = ReferenceKind::StyleVar;
	style.target = "%NOPE%";
	const AssetEntry *stylesheet = v.project.scan->find("menu_style.mns");
	fixes = fixes_for(style, v);
	TEST_EXPECT(stylesheet && labels_of(fixes) == std::vector<std::string>({"Open menu_style.mns"}));
	if (!stylesheet || fixes.size() != 1) return 1;
	TEST_EXPECT(fixes[0].request.kind == EditorRequestKind::OpenDocument && fixes[0].request.path == stylesheet->relative_path &&
	            !fixes[0].bulk && fixes[0].detail.find(kNotUndoable) == std::string::npos);
	Diagnostic screen = style;
	screen.reference = ReferenceKind::MenuScreen;
	screen.target = "B";
	screen.scope = "A.MNU";
	TEST_EXPECT(labels_of(fixes_for(screen, v)) == std::vector<std::string>({"Open a.mnu"}));
	Diagnostic text_id = style;
	text_id.reference = ReferenceKind::TextId;
	text_id.target = "NO_ID";
	text_id.scope = "UNGROUPED.BIN/Menu";
	TEST_EXPECT(labels_of(fixes_for(text_id, v)) == std::vector<std::string>({"Open ungrouped.bin"}));
	text_id.scope = "/menu";
	TEST_EXPECT(fixes_for(text_id, v).empty());
	// A table of its kind that defines nothing yet: the one the game reads (a weapon table and
	// a stylesheet emptied).
	const AssetEntry *weapons = v.project.scan->find("weapon.def");
	TEST_EXPECT(weapons != nullptr);
	if (!weapons) return 1;
	const std::string weapons_path = weapons->relative_path, style_path = stylesheet->relative_path;
	TEST_EXPECT(editor_test::write_text(root + "/" + weapons_path, "") && editor_test::write_text(root + "/" + style_path, ""));
	session.handle(make_request(EditorRequestKind::Rescan));
	bool any = false;
	v.findings.graph->for_each_symbol([&any](const GraphSymbol &symbol) {
		any = any ||
				(symbol.kind == ReferenceKind::Weapon || symbol.kind == ReferenceKind::StyleVar);
	});
	TEST_EXPECT(!any);
	Diagnostic weapon = style;
	weapon.reference = ReferenceKind::Weapon;
	weapon.target = "NOPE";
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
	const auto on = [](const char *file, NodeId row, NodeId child, const char *code) {
		Diagnostic d = make_diagnostic(DiagnosticSeverity::Error, code, code, file);
		d.row_id = row;
		d.child_id = child;
		return d;
	};
	v.findings.diagnostics = {make_diagnostic(DiagnosticSeverity::Error, "requirement.missing", "project"),
	                 on("a.mnu", 5, 0, "row"), on("a.mnu", 5, 7, "child"),
	                 on("b.mnu", 5, 0, "other file"), on("a.mnu", 0, 0, "the file"),
	                 on("a.mnu", 6, 7, "another row")};
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

int main() {
	int failures = 0;
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
	if (failures == 0) std::printf("editor_problems: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
