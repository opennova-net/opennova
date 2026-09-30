// The stylesheet document (ADR 0046 S9i) over the neutral core: a stylesheet loads as
// one row per line and an untouched save rewrites the same bytes (the blank style, the
// synthetic forms, an empty file, the shipped menu_style.mns); an edit changes its own
// line and undoes byte for byte; adds land where they are put, CRLF; the changes the
// game would read otherwise are refused before they commit, leaving the history and
// last_added as they were; a variable's value is the game's, and the document agrees
// with the game's own read; the validator reports what the game does with each odd line
// and with the definitions it reads (on the blank project only the starter variables no
// blank menu names, style.unused).
#include <editor/documents/document_types.h>
#include <editor/documents/mns_document.h>
#include <editor/graph/reference_queries.h>
#include <editor/project/project_files.h>
#include <editor/project_build/build_run.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/mns/mns_document.h>

#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace opennova::editor;

namespace {

constexpr NodeKind kVariable = node_kind(StyleKind::Variable);
constexpr NodeKind kComment = node_kind(StyleKind::Comment);
constexpr NodeKind kBlank = node_kind(StyleKind::Blank);
constexpr NodeKind kConditional = node_kind(StyleKind::Conditional);
constexpr NodeKind kInactive = node_kind(StyleKind::Inactive);

using editor_test::NoProcess;

// A stylesheet of `text` loaded as a document, the file written under `dir`.
bool load(MnsDocument &document, const editor_test::TempProjectDir &dir, const std::string &text,
          const char *name = "menu_style.mns") {
	if (!editor_test::write_text(dir.file(name), text)) return false;
	Diagnostic error;
	return document.load(dir.file(name), name, AssetKind::MenuStyle, "jo", error);
}

std::string saved(const Document &document) { return document.serialize().text; }

NodeAddress row_at(const Document &document, size_t index) {
	const auto &row = document.rows()[index];
	return {row->id, row->kind, 0};
}

Edit set(NodeAddress address, const char *field, Value value) {
	Edit edit;
	edit.address = address;
	edit.field = field;
	edit.value = std::move(value);
	return edit;
}

Edit structural(EditOperation operation, NodeAddress address, size_t position = SIZE_MAX) {
	Edit edit;
	edit.operation = operation;
	edit.address = address;
	edit.position = position;
	return edit;
}

std::string text(const Document &document, NodeAddress address, const char *field) {
	Value value;
	if (!document.get(address, field, value)) return "<none>";
	return std::get<std::string>(value);
}

bool has_code(const std::vector<Diagnostic> &diagnostics, const std::string &code) {
	for (const Diagnostic &d : diagnostics)
		if (d.code == code) return true;
	return false;
}

} // namespace

static int test_byte_identity() {
	editor_test::TempProjectDir dir("opennova_styles_identity");
	const char *cases[] = {
		"FOO bar",
		"FOO bar\r\nBAZ qux\r\n",
		"// header\r\n\r\nFOO\t\tbar\t// c\r\n",
		"FOO\\\r\nvalue\r\n",
		"FOO bar \\\r\n  baz\r\nQUX 1\r\n",
		"#if 0\r\nOFF 1\r\n#else\r\nON 2\r\n#endif\r\n",
		"\xEF\xBB\xBF" "FOO bar\r\n",
	};
	for (const char *src : cases) {
		MnsDocument document;
		TEST_EXPECT(load(document, dir, src));
		if (saved(document) != src) {
			std::fprintf(stderr, "round-trip failed for: %s\n", src);
			return 1;
		}
	}
	// An empty file: no rows, no bytes.
	{
		MnsDocument document;
		TEST_EXPECT(load(document, dir, ""));
		TEST_EXPECT(document.rows().empty() && saved(document).empty());
	}
	// Rows: one per line, a variable's continuation lines in its one row.
	{
		MnsDocument document;
		TEST_EXPECT(load(document, dir, "// c\r\n\r\nFOO a \\\r\nb\r\n#if 0\r\nOFF 1\r\n#endif\r\n"));
		const auto &rows = document.rows();
		TEST_EXPECT(rows.size() == 6);
		TEST_EXPECT(rows[0]->kind == kComment && rows[1]->kind == kBlank && rows[2]->kind == kVariable);
		TEST_EXPECT(rows[3]->kind == kConditional && rows[4]->kind == kInactive && rows[5]->kind == kConditional);
		TEST_EXPECT(document.line_of(rows[3]->id) == 5 && document.row_at_line(4) == rows[2]->id);
		TEST_EXPECT(text(document, row_at(document, 2), "value") == "a b");
		TEST_EXPECT(document.issues().empty() && !document.blocked());
	}
	// A file with LF line ends: the writer ends every line CRLF.
	{
		MnsDocument document;
		TEST_EXPECT(load(document, dir, "A 1\nB 2\n"));
		TEST_EXPECT(saved(document) == "A 1\r\nB 2\r\n");
	}
	std::printf("test_byte_identity passed\n");
	return 0;
}

static int test_the_shipped_sheet(const std::string &fixture) {
	std::string src, message;
	TEST_EXPECT(read_file_text(fixture, src, message));
	editor_test::TempProjectDir dir("opennova_styles_retail");
	MnsDocument document;
	TEST_EXPECT(load(document, dir, src));
	TEST_EXPECT(saved(document) == src);
	// The document and the game read it alike.
	const opennova::mns::EvaluationResult evaluated = document.native().evaluate();
	TEST_EXPECT(evaluated.success && evaluated.sheet.variables.size() == 12);
	TEST_EXPECT(evaluated.sheet.variables == opennova::mns::Document::parse(src).evaluate().sheet.variables);
	NodeAddress fg;
	TEST_EXPECT(find_definition(AssetGraph(), document, "%def_text_fg%", fg) &&
			text(document, fg, "value") == "FFFFFFFF");
	// A value edit changes one line; its undo gives the file back.
	Diagnostic error;
	TEST_EXPECT(document.apply(set(fg, "value", std::string("11223344")), error));
	const std::string edited = saved(document);
	TEST_EXPECT(edited.size() == src.size());
	size_t differing = 0;
	for (size_t i = 0; i < src.size(); ++i) differing += src[i] != edited[i];
	TEST_EXPECT(differing == 8);
	document.undo();
	TEST_EXPECT(saved(document) == src);
	std::printf("test_the_shipped_sheet passed\n");
	return 0;
}

static int test_edits() {
	editor_test::TempProjectDir dir("opennova_styles_edits");
	Diagnostic error;
	// A value edit changes one line; undo returns the same bytes; a rename keeps the gap.
	{
		const std::string src = "// Colors\r\nDEF_TEXT_FG\t\tFFFFFFFF\r\nTRIM\tFF808080\r\n";
		MnsDocument document;
		TEST_EXPECT(load(document, dir, src));
		const NodeAddress fg = row_at(document, 1);
		TEST_EXPECT(document.apply(set(fg, "value", std::string("FF102030")), error));
		TEST_EXPECT(saved(document) == "// Colors\r\nDEF_TEXT_FG\t\tFF102030\r\nTRIM\tFF808080\r\n");
		document.undo();
		TEST_EXPECT(saved(document) == src && !document.dirty());
		TEST_EXPECT(document.apply(set(fg, "name", std::string("TEXT_COLOR")), error));
		TEST_EXPECT(saved(document) == "// Colors\r\nTEXT_COLOR\t\tFFFFFFFF\r\nTRIM\tFF808080\r\n");
		// The comment edits in place, "//" added when left out; the value is refused a
		// lone backslash (the game joins there), '#' (it stops responding), nothing.
		TEST_EXPECT(document.apply(set(row_at(document, 0), "text", std::string("Text colours")), error));
		TEST_EXPECT(saved(document).rfind("// Text colours\r\n", 0) == 0);
		TEST_EXPECT(!document.apply(set(fg, "value", std::string("a\\b")), error));
		TEST_EXPECT(!document.apply(set(fg, "value", std::string("#FF0000")), error));
		TEST_EXPECT(!document.apply(set(fg, "value", std::string("")), error));
		TEST_EXPECT(document.apply(set(fg, "value", std::string("c:\\\\x")), error)); // a pair stays doubled
		TEST_EXPECT(text(document, fg, "value") == "c:\\\\x");
	}
	// An add at the end of a file with no final newline: that line gains CRLF.
	{
		MnsDocument document;
		TEST_EXPECT(load(document, dir, "FOO bar"));
		TEST_EXPECT(document.apply(structural(EditOperation::Add, {0, kVariable, 0}), error));
		const NodeAddress added = document.address_of(document.last_added());
		TEST_EXPECT(document.apply(set(added, "name", std::string("NEW")), error));
		TEST_EXPECT(document.apply(set(added, "value", std::string("value")), error));
		TEST_EXPECT(saved(document) == "FOO bar\r\nNEW\tvalue\r\n");
		// A comment and a blank line where they are put.
		TEST_EXPECT(document.apply(structural(EditOperation::Add, {0, kBlank, 0}, 1), error));
		TEST_EXPECT(document.apply(structural(EditOperation::Add, {0, kComment, 0}, 2), error));
		TEST_EXPECT(saved(document) == "FOO bar\r\n\r\n// \r\nNEW\tvalue\r\n");
	}
	// Removing a variable leaves the comments above it; a duplicate name is two rows, and
	// the earlier one edits too.
	{
		MnsDocument document;
		TEST_EXPECT(load(document, dir, "// c\r\nFOO a\r\nBAR 1\r\nfoo b\r\n"));
		NodeAddress found;
		TEST_EXPECT(find_definition(AssetGraph(), document, "FOO", found) &&
				found.row == document.rows()[3]->id); // the one the game reads
		Value overridden;
		TEST_EXPECT(document.get(row_at(document, 1), "overridden", overridden) && std::get<int64_t>(overridden) == 1);
		TEST_EXPECT(document.apply(set(row_at(document, 1), "value", std::string("z")), error));
		TEST_EXPECT(saved(document) == "// c\r\nFOO z\r\nBAR 1\r\nfoo b\r\n");
		TEST_EXPECT(document.apply(structural(EditOperation::Remove, row_at(document, 2)), error));
		TEST_EXPECT(saved(document) == "// c\r\nFOO z\r\nfoo b\r\n");
	}
	// The save-conflict check: a file changed outside the editor is not overwritten.
	{
		MnsDocument document;
		TEST_EXPECT(load(document, dir, "FOO a\r\n", "conflict.mns"));
		TEST_EXPECT(document.apply(set(row_at(document, 0), "value", std::string("b")), error));
		TEST_EXPECT(editor_test::write_text(dir.file("conflict.mns"), "FOO other\r\n"));
		TEST_EXPECT(!document.save(error) && error.code == "document.conflict");
	}
	std::printf("test_edits passed\n");
	return 0;
}

// The changes the game would read otherwise, refused before they commit.
static int test_refusals() {
	editor_test::TempProjectDir dir("opennova_styles_refusals");
	Diagnostic error;
	// Into a switched-off #if block: an Add and a Move.
	{
		const std::string src = "#if 0\r\nOFF 1\r\n#endif\r\nON 2\r\n";
		MnsDocument document;
		TEST_EXPECT(load(document, dir, src));
		const uint64_t revision = document.revision();
		TEST_EXPECT(!document.apply(structural(EditOperation::Add, {0, kVariable, 0}, 1), error));
		TEST_EXPECT(error.code == "document.structure" && error.message.find("switched-off") != std::string::npos);
		TEST_EXPECT(!document.apply(structural(EditOperation::Move, row_at(document, 3), 1), error));
		// The directive rows and the lines they switch off stay put: Remove, Move, Duplicate.
		TEST_EXPECT(!document.apply(structural(EditOperation::Remove, row_at(document, 0)), error));
		TEST_EXPECT(!document.apply(structural(EditOperation::Move, row_at(document, 2), 0), error));
		TEST_EXPECT(!document.apply(structural(EditOperation::Duplicate, row_at(document, 0), 1), error));
		TEST_EXPECT(!document.apply(structural(EditOperation::Remove, row_at(document, 1)), error));
		TEST_EXPECT(document.frozen(*document.rows()[0]) && document.frozen(*document.rows()[1]));
		TEST_EXPECT(saved(document) == src && document.revision() == revision && !document.can_undo());
	}
	// After a value that continues at the end of the file: the new line would join it.
	{
		const std::string src = "FOO bar \\";
		MnsDocument document;
		TEST_EXPECT(load(document, dir, src));
		TEST_EXPECT(!document.apply(structural(EditOperation::Add, {0, kVariable, 0}), error));
		TEST_EXPECT(error.message.find("continues") != std::string::npos);
		TEST_EXPECT(saved(document) == src && !document.can_undo());
	}
	// A variable whose value crosses other lines stays where it is; its value still edits.
	{
		const std::string src = "A 1\r\nFOO a \\\r\n#if 0\r\nx\r\n#endif\r\nb\r\n";
		MnsDocument document;
		TEST_EXPECT(load(document, dir, src));
		const NodeAddress foo = row_at(document, 1);
		TEST_EXPECT(document.frozen(*document.rows()[1]));
		TEST_EXPECT(!document.apply(structural(EditOperation::Remove, foo), error));
		TEST_EXPECT(!document.apply(structural(EditOperation::Move, foo, 0), error));
		TEST_EXPECT(document.apply(set(foo, "value", std::string("new")), error));
		TEST_EXPECT(text(document, foo, "value") == "new");
		TEST_EXPECT(document.native().evaluate().sheet.get("FOO") == "new");
	}
	// A refused change keeps last_added and the history as they were.
	{
		MnsDocument document;
		TEST_EXPECT(load(document, dir, "#if 0\r\nOFF 1\r\n#endif\r\n"));
		TEST_EXPECT(document.apply(structural(EditOperation::Add, {0, kVariable, 0}), error));
		const NodeId added = document.last_added();
		const uint64_t revision = document.revision();
		TEST_EXPECT(added && !document.apply(structural(EditOperation::Add, {0, kVariable, 0}, 1), error));
		TEST_EXPECT(document.last_added() == added && document.revision() == revision);
		document.undo();
		TEST_EXPECT(!document.can_undo() && document.rows().size() == 3);
	}
	std::printf("test_refusals passed\n");
	return 0;
}

// The validator: the format's findings on their rows, a stylesheet the game does not
// read, and on a blank project nothing but the starter variables no blank menu names
// (style.unused, info, S9l), which leave once a menu names one.
static int test_validation() {
	editor_test::TempProjectDir dir("opennova_styles_validation");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Styles"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const auto unused = [&view]() {
		std::vector<std::string> names;
		for (const Diagnostic &d : view.findings.diagnostics)
			if (d.code == "style.unused" && d.severity == DiagnosticSeverity::Info && d.field == "value")
				names.push_back(d.record);
		std::sort(names.begin(), names.end());
		return names;
	};
	for (const Diagnostic &d : view.findings.diagnostics)
		if (d.code.rfind("style.", 0) == 0 && d.code != "style.unused") {
			std::fprintf(stderr, "blank project: %s %s\n", d.code.c_str(), d.message.c_str());
			return 1;
		}
	// The blank menus name the large font and the four text colours.
	TEST_EXPECT(unused() == std::vector<std::string>({"COLOR_BLACK", "DEF_FONTNAME", "IMPACT_FONTNAME", "ITEM_SELECTED_BG",
	                                                  "SEMIOPAQUE_BLACK", "TRIM_COLOR"}));
	session.handle(request::open_document("main.mnu"));
	const Document *menu = session.document_for("main.mnu");
	NodeAddress title;
	TEST_EXPECT(menu && find_definition(AssetGraph(), *menu, "TITLE", title));
	if (!menu) return 1;
	EditorRequest name_it = request::edit_record(
			menu->path(), set(title, "font.default_bg", std::string("%TRIM_COLOR%")));
	session.handle(name_it);
	const std::vector<std::string> left = unused();
	TEST_EXPECT(left.size() == 5 && std::find(left.begin(), left.end(), "TRIM_COLOR") == left.end());
	session.handle(request::undo(menu->path()));
	session.handle(request::close_document(menu->path()));
	const AssetEntry *style = view.project.scan->find("menu_style.mns");
	TEST_EXPECT(style != nullptr);
	const std::string style_dir = (dir.path / "project" / style->relative_path).parent_path().generic_string();
	// LF line ends, a lone backslash, a stray #else, and a stylesheet by another name.
	TEST_EXPECT(editor_test::write_text(style_dir + "/brand.mns", "A x\\ y\nB 2\n"));
	TEST_EXPECT(editor_test::write_text(style_dir + "/other.mns", "#else\r\nC 3\r\n"));
	session.handle(request::rescan());
	bool line_ending = false;
	for (const Diagnostic &d : view.findings.diagnostics) {
		if (d.code == "style.line_ending") {
			line_ending = true;
			TEST_EXPECT(d.severity == DiagnosticSeverity::Error && d.line == 1 && d.record == "A");
		}
	}
	TEST_EXPECT(line_ending);
	TEST_EXPECT(has_code(view.findings.diagnostics, "style.lone_backslash"));
	TEST_EXPECT(has_code(view.findings.diagnostics, "style.unbalanced_else"));
	TEST_EXPECT(has_code(view.findings.diagnostics, "style.not_loaded"));
	// The LF sheet opens with CR LF rows: the game's reading of what Save writes. The file
	// keeps its LF ends (and the finding) until Save writes it; then the build takes it.
	const AssetEntry *brand = view.project.scan->find("brand.mns");
	TEST_EXPECT(brand != nullptr);
	if (!brand) return 1;
	const std::string brand_path = brand->relative_path;
	session.handle(request::open_document(brand_path));
	const auto *styles = dynamic_cast<const MnsDocument *>(session.document_for(brand_path));
	TEST_EXPECT(styles != nullptr);
	if (!styles) return 1;
	TEST_EXPECT(styles->native().evaluate().success && styles->native().evaluate().sheet.get("B") == "2");
	NodeAddress b;
	TEST_EXPECT(find_definition(AssetGraph(), *styles, "B", b));
	EditorRequest edit = request::edit_record(brand_path, set(b, "value", std::string("3")));
	session.handle(edit);
	TEST_EXPECT(styles->dirty() && has_code(view.findings.diagnostics, "style.line_ending"));
	session.handle(request::save_all());
	TEST_EXPECT(!styles->dirty() && styles->wrote_file());
	TEST_EXPECT(!has_code(view.findings.diagnostics, "style.line_ending"));
	std::string written, message;
	TEST_EXPECT(read_file_text(dir.file("project") + "/" + brand_path, written, message) && written == "A x\\ y\r\nB 3\r\n");
	const opennova::mns::EvaluationResult evaluated = styles->native().evaluate();
	TEST_EXPECT(evaluated.success && !evaluated.hangs && evaluated.sheet.get("B") == "3");
	TEST_EXPECT(opennova::mns::Document::parse(written).evaluate().sheet.variables == evaluated.sheet.variables);
	session.handle(request::build());
	session.run_operations();
	for (const Diagnostic &d : view.activity.last_build->diagnostics)
		if (d.severity == DiagnosticSeverity::Error)
			std::fprintf(stderr, "build: %s %s %s\n", d.code.c_str(), d.asset.c_str(), d.message.c_str());
	TEST_EXPECT(view.activity.last_build->ok);
	// A clean sheet with LF line ends: an explicit Save of it (no edit) rewrites it CR LF,
	// and its finding is gone.
	TEST_EXPECT(editor_test::write_text(style_dir + "/note.mns", "N 1\nM 2\n"));
	session.handle(request::rescan());
	const AssetEntry *note = view.project.scan->find("note.mns");
	TEST_EXPECT(note != nullptr);
	if (!note) return 1;
	const std::string note_path = note->relative_path;
	const auto line_ending_on = [&](const std::string &asset) {
		for (const Diagnostic &d : view.findings.diagnostics)
			if (d.code == "style.line_ending" && d.asset == asset) return true;
		return false;
	};
	TEST_EXPECT(line_ending_on(note_path));
	session.handle(request::open_document(note_path));
	TEST_EXPECT(session.document_for(note_path) && !session.document_for(note_path)->dirty());
	session.handle(request::save(note_path));
	TEST_EXPECT(session.outcome().done() && !line_ending_on(note_path));
	TEST_EXPECT(read_file_text(dir.file("project") + "/" + note_path, written, message) && written == "N 1\r\nM 2\r\n");
	session.handle(request::save(note_path));
	TEST_EXPECT(session.outcome().done() &&
			view.activity.status == note_path + " has no changes to save.");
	std::printf("test_validation passed\n");
	return 0;
}

// S11a: a value edit is a change and reverts; a line's number and its overridden flag,
// read off the whole document, never are, though a line added above moves every line and
// a later definition's removal lifts the override.
static int test_changes_since_save() {
	editor_test::TempProjectDir dir("opennova_styles_changes");
	MnsDocument document;
	TEST_EXPECT(load(document, dir, "// c\r\nFOO a\r\nBAR 1\r\nfoo b\r\n"));
	using Change = Document::RecordChange;
	Diagnostic error;
	const NodeAddress foo = row_at(document, 1), bar = row_at(document, 2), later = row_at(document, 3);
	TEST_EXPECT(document.apply(set(bar, "value", std::string("2")), error));
	TEST_EXPECT(document.field_changed(bar, "value") && document.record_change(bar) == Change::Changed);
	TEST_EXPECT(document.apply(document.revert_edits(bar, "value"), error) && !document.field_changed(bar, "value"));
	TEST_EXPECT(text(document, bar, "value") == "1" && document.record_change(bar) == Change::Unchanged);
	TEST_EXPECT(document.apply(structural(EditOperation::Add, {0, kBlank, 0}, 0), error));
	TEST_EXPECT(document.apply(structural(EditOperation::Remove, later), error));
	Value line, overridden;
	TEST_EXPECT(document.get(foo, "line", line) && std::get<int64_t>(line) == 3);
	TEST_EXPECT(document.get(foo, "overridden", overridden) && std::get<int64_t>(overridden) == 0);
	TEST_EXPECT(!document.field_changed(foo, "line") && !document.field_changed(foo, "overridden"));
	TEST_EXPECT(document.record_change(foo) == Change::Unchanged && document.record_change(bar) == Change::Unchanged);
	TEST_EXPECT(document.record_change(row_at(document, 0)) == Change::Added);
	std::printf("test_changes_since_save passed\n");
	return 0;
}

// S13 D6: a load in place starts the revisions again at 0 and the rows' identities from 1, so the
// stylesheet's own memos (the sheet the game reads, what a line's value is used as) key on the load
// generation as well: FOO a colour, then the file loaded again in place with FOO a font's file, at
// the same revision and on the same row identity, answers from the new file.
static int test_load_in_place() {
	editor_test::TempProjectDir dir("opennova_styles_load_in_place");
	MnsDocument document;
	TEST_EXPECT(load(document, dir, "FOO FFFF0000\r\n"));
	const NodeAddress foo = row_at(document, 0);
	SymbolFacts colour;
	document.refine_symbol(foo, colour);
	TEST_EXPECT(colour.value == "FFFF0000" && !colour.inert &&
	            document.style_value_use(foo, nullptr, 0).colour);
	const uint64_t generation = document.load_generation();
	TEST_EXPECT(load(document, dir, "FOO arial.fnt\r\n"));
	TEST_EXPECT(document.revision() == 0 && document.load_generation() != generation &&
	            row_at(document, 0) == foo);
	SymbolFacts font;
	document.refine_symbol(foo, font);
	TEST_EXPECT(font.value == "arial.fnt" && !document.style_value_use(foo, nullptr, 0).colour);
	std::printf("test_load_in_place passed\n");
	return 0;
}

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failures = 0;
	failures += test_load_in_place();
	failures += test_byte_identity();
	failures += test_edits();
	failures += test_refusals();
	failures += test_changes_since_save();
	failures += test_validation();
	const std::string fixture = retail::reference_fixture("mns/menu_style.mns");
	if (!fixture.empty()) failures += test_the_shipped_sheet(fixture);
	if (failures == 0) std::printf("editor_styles: all tests passed\n");
	if (failures == 0 && fixture.empty())
		return retail::skip_leg("OPENNOVA_JO_ASSETS/fixtures/mns/menu_style.mns (the shipped style sheet)");
	return failures == 0 ? 0 : 1;
}
