// The stylesheet document (ADR 0046 S9i, as the S26 ruling made it) over the neutral core: a stylesheet loads as one
// row per variable of the list the game's reader makes of the file (its directives decided, its comments and
// switched-off lines read as nothing, the first spelling and the last value of a name), the same list the game reads
// (the synthetic forms, an empty file, the shipped menu_style.mns); a save writes the list alone, `name<TAB>value`
// and CR LF a line, which reads back as the list, a file of that form byte for byte; an edit changes its own line and
// undoes; the changes the game would read otherwise are refused before they commit; the validator reports what the
// stored file holds that a save leaves out (its line ends, its comments and directives, where the game hangs on it)
// until a save writes it, and on the blank project only the starter variables no blank menu names (style.unused).
#include <editor/documents/document_types.h>
#include <editor/documents/mns_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_kinds.h>
#include <editor/graph/reference_queries.h>
#include <editor/graph/style_value_use.h>
#include <editor/project/project_files.h>
#include <editor/project_build/build_plan.h>
#include <editor/project_build/build_run.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/mns/mns.h>

#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace opennova::editor;

namespace {

constexpr NodeKind kVariable = node_kind(StyleKind::Variable);

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

using editor_test::set_edit;

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

// The list the game's reader makes of `text` [orig: NapiConfigMap_ParseKeyValueBuffer @ 0x639870].
opennova::mns::KeyValueList game_read(const std::string &text) {
	opennova::mns::KeyValueList list;
	opennova::mns::parse_key_value_buffer(text.data(), text.size(), list);
	return list;
}

bool same_list(const opennova::mns::KeyValueList &a, const opennova::mns::KeyValueList &b) {
	if (a.nodes.size() != b.nodes.size()) return false;
	for (size_t i = 0; i < a.nodes.size(); ++i)
		if (a.nodes[i].name != b.nodes[i].name || a.nodes[i].value != b.nodes[i].value) return false;
	return true;
}

// The stylesheet findings of `document` with `code`.
size_t stored_findings(const MnsDocument &document, const char *code) {
	size_t n = 0;
	for (const Diagnostic &d : validate_styles_file(document)) n += d.code() == code;
	return n;
}

using editor_test::has_code;

} // namespace

// The rows are the game's list: what it reads of each form, which a save writes alone and which reads back as the
// list; a file in the writer's form saves byte for byte, every other form what the game reads of it.
static int test_the_list() {
	editor_test::TempProjectDir dir("opennova_styles_list");
	struct Case {
		const char *src;
		const char *written;
		bool dropped;
	};
	const Case cases[] = {
		{"FOO bar", "FOO\tbar\r\n", false},
		{"FOO\tbar\r\nBAZ\tqux\r\n", "FOO\tbar\r\nBAZ\tqux\r\n", false},
		{"// header\r\n\r\nFOO\t\tbar\t// c\r\n", "FOO\tbar\r\n", false},
		{"FOO a \\\r\nb\r\nQUX 1\r\n", "FOO\ta b\r\nQUX\t1\r\n", true},
		{"#if 0\r\nOFF 1\r\n#else\r\nON 2\r\n#endif\r\n", "ON\t2\r\n", true},
		{"\xEF\xBB\xBF" "FOO bar\r\n", "FOO\tbar\r\n", false},
		// A name defined again: its first spelling, its last value.
		{"FOO a\r\nBAR 1\r\nfoo b\r\n", "FOO\tb\r\nBAR\t1\r\n", true},
		// An LF line end at the end of a value with nothing after: read on; a save ends the line CR LF (the line end
		// alone is style.line_ending's).
		{"A\t1\r\nB\t2\n", "A\t1\r\nB\t2\r\n", false},
	};
	for (const Case &c : cases) {
		MnsDocument document;
		TEST_EXPECT(load(document, dir, c.src));
		const std::string written = saved(document);
		if (written != c.written || !same_list(document.list(), game_read(c.src)) ||
		    !same_list(game_read(written), document.list())) {
			std::fprintf(stderr, "the list of: %s\n  writes: %s\n", c.src, written.c_str());
			return 1;
		}
		TEST_EXPECT(stored_findings(document, "style.ignored_input") == (c.dropped ? 1u : 0u));
		TEST_EXPECT(document.issues().empty() && !document.blocked());
	}
	// An empty file: no rows, no bytes.
	{
		MnsDocument document;
		TEST_EXPECT(load(document, dir, ""));
		TEST_EXPECT(document.rows().empty() && saved(document).empty() && validate_styles_file(document).empty());
	}
	// A row's line is its place in the file a save writes.
	{
		MnsDocument document;
		TEST_EXPECT(load(document, dir, "// c\r\n\r\nFOO a \\\r\nb\r\n#if 0\r\nOFF 1\r\n#endif\r\nON 2\r\n"));
		const auto &rows = document.rows();
		TEST_EXPECT(rows.size() == 2 && rows[0]->kind == kVariable && rows[0]->name() == "FOO" && rows[1]->name() == "ON");
		TEST_EXPECT(document.line_of(rows[1]->id) == 2 && document.row_at_line(1) == rows[0]->id &&
		            document.row_at_line(3) == 0);
		TEST_EXPECT(text(document, row_at(document, 0), "value") == "a b");
		Value line;
		TEST_EXPECT(document.get(row_at(document, 1), "line", line) && std::get<int64_t>(line) == 2);
		TEST_EXPECT(document.winning_row("%on%") == rows[1]->id && document.winning_row("OFF") == 0);
	}
	std::printf("test_the_list passed\n");
	return 0;
}

static int test_the_shipped_sheet(const std::string &fixture) {
	std::string src, message;
	TEST_EXPECT(opennova::io::read_file_text(fixture, src, message));
	editor_test::TempProjectDir dir("opennova_styles_retail");
	MnsDocument document;
	TEST_EXPECT(load(document, dir, src));
	// The document and the game read it alike.
	TEST_EXPECT(same_list(document.list(), game_read(src)) && document.list().sheet().variables.size() == 12);
	opennova::mns::StyleSheet sheet;
	TEST_EXPECT(opennova::mns::parse(src.data(), src.size(), sheet, message) &&
	            sheet.variables == document.list().sheet().variables);
	const std::string written = saved(document);
	TEST_EXPECT(same_list(game_read(written), document.list()));
	NodeAddress fg;
	TEST_EXPECT(find_definition(AssetGraph(), document, "%def_text_fg%", fg) &&
			text(document, fg, "value") == "FFFFFFFF");
	// A value edit changes its own line; its undo gives the text back.
	Diagnostic error;
	TEST_EXPECT(document.apply(set_edit(fg, "value", std::string("11223344")), error));
	const std::string edited = saved(document);
	TEST_EXPECT(edited.size() == written.size());
	size_t differing = 0;
	for (size_t i = 0; i < written.size() && i < edited.size(); ++i) differing += written[i] != edited[i];
	TEST_EXPECT(differing == 8);
	document.undo();
	TEST_EXPECT(saved(document) == written);
	std::printf("test_the_shipped_sheet passed\n");
	return 0;
}

static int test_edits() {
	editor_test::TempProjectDir dir("opennova_styles_edits");
	Diagnostic error;
	// A value edit changes one line; undo returns the same bytes; a rename.
	{
		const std::string src = "DEF_TEXT_FG\tFFFFFFFF\r\nTRIM\tFF808080\r\n";
		MnsDocument document;
		TEST_EXPECT(load(document, dir, src));
		const NodeAddress fg = row_at(document, 0);
		TEST_EXPECT(document.apply(set_edit(fg, "value", std::string("FF102030")), error));
		TEST_EXPECT(saved(document) == "DEF_TEXT_FG\tFF102030\r\nTRIM\tFF808080\r\n");
		document.undo();
		TEST_EXPECT(saved(document) == src && !document.dirty());
		TEST_EXPECT(document.apply(set_edit(fg, "name", std::string("TEXT_COLOR")), error));
		TEST_EXPECT(saved(document) == "TEXT_COLOR\tFFFFFFFF\r\nTRIM\tFF808080\r\n");
		// The value is refused a lone backslash (the game joins there), '#' (it stops responding), a "//" (a comment
		// starts there), nothing; a doubled one stays doubled.
		TEST_EXPECT(!document.apply(set_edit(fg, "value", std::string("a\\b")), error));
		TEST_EXPECT(!document.apply(set_edit(fg, "value", std::string("#FF0000")), error));
		TEST_EXPECT(!document.apply(set_edit(fg, "value", std::string("a // b")), error));
		TEST_EXPECT(!document.apply(set_edit(fg, "value", std::string("")), error));
		TEST_EXPECT(document.apply(set_edit(fg, "value", std::string("c:\\\\x")), error));
		TEST_EXPECT(text(document, fg, "value") == "c:\\\\x");
		// A name is one word the reader reads whole.
		TEST_EXPECT(!document.apply(set_edit(fg, "name", std::string("TWO WORDS")), error));
		TEST_EXPECT(!document.apply(set_edit(fg, "name", std::string("A%B")), error));
		TEST_EXPECT(!document.apply(set_edit(fg, "line", std::string("3")), error));
	}
	// An add: a variable at the end, named anew and white; its name and value set.
	{
		MnsDocument document;
		TEST_EXPECT(load(document, dir, "FOO bar"));
		TEST_EXPECT(document.apply(structural(EditOperation::Add, {0, kVariable, 0}), error));
		const NodeAddress added = document.address_of(document.last_added());
		TEST_EXPECT(text(document, added, "value") == "FFFFFFFF" && document.line_of(added.row) == 2);
		TEST_EXPECT(document.apply(set_edit(added, "name", std::string("NEW")), error));
		TEST_EXPECT(document.apply(set_edit(added, "value", std::string("value")), error));
		TEST_EXPECT(saved(document) == "FOO\tbar\r\nNEW\tvalue\r\n");
		// Moved first: its line first.
		TEST_EXPECT(document.apply(structural(EditOperation::Move, added, 0), error));
		TEST_EXPECT(saved(document) == "NEW\tvalue\r\nFOO\tbar\r\n");
	}
	// A name the reader takes for another variable's (compared without case) is refused, the history as it was;
	// a removal.
	{
		MnsDocument document;
		TEST_EXPECT(load(document, dir, "FOO a\r\nBAR 1\r\nBAZ 2\r\n"));
		const uint64_t revision = document.revision();
		TEST_EXPECT(!document.apply(set_edit(row_at(document, 1), "name", std::string("foo")), error));
		TEST_EXPECT(document.revision() == revision && !document.can_undo());
		TEST_EXPECT(document.apply(structural(EditOperation::Remove, row_at(document, 1)), error));
		TEST_EXPECT(saved(document) == "FOO\ta\r\nBAZ\t2\r\n");
		NodeAddress found;
		TEST_EXPECT(find_definition(AssetGraph(), document, "%baz%", found) && found == row_at(document, 1));
	}
	// The save-conflict check: a file changed outside the editor is not overwritten.
	{
		MnsDocument document;
		TEST_EXPECT(load(document, dir, "FOO a\r\n", "conflict.mns"));
		TEST_EXPECT(document.apply(set_edit(row_at(document, 0), "value", std::string("b")), error));
		TEST_EXPECT(editor_test::write_text(dir.file("conflict.mns"), "FOO other\r\n"));
		TEST_EXPECT(!document.save(error) && error.code() == "document.conflict");
	}
	std::printf("test_edits passed\n");
	return 0;
}

// S11a: a value edit is a change and reverts; a line's number, read off the whole document, never is, though a
// variable added above moves every line.
static int test_changes_since_save() {
	editor_test::TempProjectDir dir("opennova_styles_changes");
	MnsDocument document;
	TEST_EXPECT(load(document, dir, "FOO a\r\nBAR 1\r\n"));
	using Change = Document::RecordChange;
	Diagnostic error;
	const NodeAddress foo = row_at(document, 0), bar = row_at(document, 1);
	TEST_EXPECT(document.apply(set_edit(bar, "value", std::string("2")), error));
	TEST_EXPECT(document.field_changed(bar, "value") && document.record_change(bar) == Change::Changed);
	TEST_EXPECT(document.apply(document.revert_edits(bar, "value"), error) && !document.field_changed(bar, "value"));
	TEST_EXPECT(text(document, bar, "value") == "1" && document.record_change(bar) == Change::Unchanged);
	TEST_EXPECT(document.apply(structural(EditOperation::Add, {0, kVariable, 0}, 0), error));
	Value line;
	TEST_EXPECT(document.get(foo, "line", line) && std::get<int64_t>(line) == 2);
	TEST_EXPECT(!document.field_changed(foo, "line"));
	TEST_EXPECT(document.record_change(foo) == Change::Unchanged && document.record_change(bar) == Change::Unchanged);
	TEST_EXPECT(document.record_change(row_at(document, 0)) == Change::Added);
	std::printf("test_changes_since_save passed\n");
	return 0;
}

// The validator: what the stored file holds that a save leaves out, until a save writes it; a stylesheet the game
// does not read; and on a blank project nothing but the starter variables no blank menu names (style.unused, info,
// S9l), which leave once a menu names one.
static int test_validation() {
	editor_test::TempProjectDir dir("opennova_styles_validation");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Styles"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const auto unused = [&view]() {
		std::vector<std::string> names;
		for (const Diagnostic &d : view.findings.diagnostics)
			if (d.code() == "style.unused" && d.severity == DiagnosticSeverity::Info && d.field == "value")
				names.push_back(d.record);
		std::sort(names.begin(), names.end());
		return names;
	};
	for (const Diagnostic &d : view.findings.diagnostics)
		if (d.code().rfind("style.", 0) == 0 && d.code() != "style.unused") {
			std::fprintf(stderr, "blank project: %s %s\n", d.code().c_str(), d.message.c_str());
			return 1;
		}
	// The blank menus name the large font and the four text colours.
	TEST_EXPECT(unused() == std::vector<std::string>({"COLOR_BLACK", "DEF_FONTNAME", "IMPACT_FONTNAME", "ITEM_SELECTED_BG",
	                                                  "SEMIOPAQUE_BLACK", "TRIM_COLOR"}));
	editor_test::handle_to_end(session, request::open_document("main.mnu"));
	const Document *menu = session.document_for("main.mnu");
	NodeAddress title;
	TEST_EXPECT(menu && find_definition(AssetGraph(), *menu, "TITLE", title));
	if (!menu) return 1;
	EditorRequest name_it = request::edit_record(
			menu->path(), set_edit(title, "font.default_bg", std::string("%TRIM_COLOR%")));
	editor_test::handle_to_end(session, name_it);
	const std::vector<std::string> left = unused();
	TEST_EXPECT(left.size() == 5 && std::find(left.begin(), left.end(), "TRIM_COLOR") == left.end());
	editor_test::handle_to_end(session, request::undo(menu->path()));
	editor_test::handle_to_end(session, request::close_document(menu->path()));
	const AssetEntry *style = view.project.scan->find("menu_style.mns");
	TEST_EXPECT(style != nullptr);
	const std::string style_dir = (dir.path / "project" / style->relative_path).parent_path().generic_string();
	// A comment ended by a lone LF, and a stylesheet by another name.
	TEST_EXPECT(editor_test::write_text(style_dir + "/brand.mns", "// brand\nA 1\r\nB 2\r\n"));
	TEST_EXPECT(editor_test::write_text(style_dir + "/other.mns", "#else\r\nC 3\r\n"));
	editor_test::handle_to_end(session, request::rescan());
	const AssetEntry *brand = view.project.scan->find("brand.mns");
	TEST_EXPECT(brand != nullptr);
	if (!brand) return 1;
	const std::string brand_path = brand->relative_path;
	const auto on = [&](const char *code, const std::string &asset) -> const Diagnostic * {
		for (const Diagnostic &d : view.findings.diagnostics)
			if (d.code() == code && d.asset == asset) return &d;
		return nullptr;
	};
	const Diagnostic *line_ending = on("style.line_ending", brand_path);
	TEST_EXPECT(line_ending && line_ending->severity == DiagnosticSeverity::Warning && line_ending->line == 1);
	// (A comment is layout, which a save drops as every writer does: no finding of its own.)
	TEST_EXPECT(!on("style.ignored_input", brand_path) && !on("style.hangs", brand_path));
	TEST_EXPECT(has_code(view.findings.diagnostics, "style.not_loaded"));
	// Opened, its rows are the game's list; an edit; the file keeps its findings until Save writes it, then the
	// build takes it.
	editor_test::handle_to_end(session, request::open_document(brand_path));
	const auto *styles = dynamic_cast<const MnsDocument *>(session.document_for(brand_path));
	TEST_EXPECT(styles != nullptr);
	if (!styles) return 1;
	TEST_EXPECT(styles->list().sheet().get("B") == "2");
	NodeAddress b;
	TEST_EXPECT(find_definition(AssetGraph(), *styles, "B", b));
	EditorRequest edit = request::edit_record(brand_path, set_edit(b, "value", std::string("3")));
	editor_test::handle_to_end(session, edit);
	TEST_EXPECT(styles->dirty() && on("style.line_ending", brand_path));
	editor_test::handle_to_end(session, request::save_all());
	TEST_EXPECT(!styles->dirty() && styles->wrote_file());
	TEST_EXPECT(!on("style.line_ending", brand_path) && !on("style.ignored_input", brand_path));
	std::string written, message;
	TEST_EXPECT(opennova::io::read_file_text(dir.file("project") + "/" + brand_path, written, message) &&
	            written == "A\t1\r\nB\t3\r\n");
	TEST_EXPECT(same_list(game_read(written), styles->list()));
	editor_test::handle_to_end(session, request::build());
	for (const Diagnostic &d : view.activity.last_build->diagnostics)
		if (d.severity == DiagnosticSeverity::Error)
			std::fprintf(stderr, "build: %s %s %s\n", d.code().c_str(), d.asset.c_str(), d.message.c_str());
	TEST_EXPECT(view.activity.last_build->ok);
	// A clean sheet with a line ended LF: an explicit Save of it (no edit) rewrites it CR LF, and its finding is gone.
	TEST_EXPECT(editor_test::write_text(style_dir + "/note.mns", "N 1\r\nM 2\n"));
	editor_test::handle_to_end(session, request::rescan());
	const AssetEntry *note = view.project.scan->find("note.mns");
	TEST_EXPECT(note != nullptr);
	if (!note) return 1;
	const std::string note_path = note->relative_path;
	// Nothing reads note.mns: what the game would do reading it is a warning, which gates no build (the build follows
	// retail, S14).
	const Diagnostic *note_ending = on("style.line_ending", note_path);
	TEST_EXPECT(note_ending && note_ending->severity == DiagnosticSeverity::Warning && note_ending->line == 2);
	editor_test::handle_to_end(session, request::open_document(note_path));
	TEST_EXPECT(session.document_for(note_path) && !session.document_for(note_path)->dirty());
	editor_test::handle_to_end(session, request::save(note_path));
	TEST_EXPECT(session.outcome().done() && !on("style.line_ending", note_path));
	TEST_EXPECT(opennova::io::read_file_text(dir.file("project") + "/" + note_path, written, message) &&
	            written == "N\t1\r\nM\t2\r\n");
	editor_test::handle_to_end(session, request::save(note_path));
	TEST_EXPECT(session.outcome().done() && view.activity.status == note_path + " has no changes to save.");
	std::printf("test_validation passed\n");
	return 0;
}

// A stylesheet the game reads whose stored text hangs its reader (S26 A2): a value ended by a lone LF with text right
// after it [orig: NapiConfigMap_ParseKeyValueBuffer @ 0x639c3b]. The list holds what the game read before it, so the
// hang is the stored file's: style.hangs, an error that gates the build with the game's refusal, until a Save writes
// the list; then it is gone and the build goes.
static int test_stored_hang() {
	editor_test::TempProjectDir dir("opennova_styles_stored_hang");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Styles"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const AssetEntry *style = view.project.scan->find("menu_style.mns");
	TEST_EXPECT(style != nullptr);
	if (!style) return 1;
	const std::string style_dir = (dir.path / "project" / style->relative_path).parent_path().generic_string();
	TEST_EXPECT(editor_test::write_text(style_dir + "/brand.mns", "A 1\nB 2\r\n"));
	editor_test::handle_to_end(session, request::rescan());
	const AssetEntry *brand = view.project.scan->find("brand.mns");
	TEST_EXPECT(brand != nullptr);
	if (!brand) return 1;
	const std::string brand_path = brand->relative_path;
	const auto hang = [&]() -> const Diagnostic * {
		for (const Diagnostic &d : view.findings.diagnostics)
			if (d.code() == "style.hangs" && d.asset == brand_path) return &d;
		return nullptr;
	};
	// The read stands still at B on line 2, after the value the lone LF ended.
	const Diagnostic *found = hang();
	TEST_EXPECT(found && found->severity == DiagnosticSeverity::Error && found->line == 2 && blocks_build(*found) &&
	            blocker_is_the_games(*found) &&
	            blocker_reason(*found).find("NapiConfigMap_ParseKeyValueBuffer") != std::string::npos);
	editor_test::handle_to_end(session, request::build());
	TEST_EXPECT(view.activity.last_build && !view.activity.last_build->ok);
	editor_test::handle_to_end(session, request::open_document(brand_path));
	editor_test::handle_to_end(session, request::save(brand_path));
	TEST_EXPECT(!hang());
	std::string written, message;
	TEST_EXPECT(opennova::io::read_file_text(dir.file("project") + "/" + brand_path, written, message) &&
	            written == "A\t1\r\n");
	editor_test::handle_to_end(session, request::build());
	TEST_EXPECT(view.activity.last_build && view.activity.last_build->ok);
	std::printf("test_stored_hang passed\n");
	return 0;
}

// What a variable's value is used as (graph/style_value_use, the Styles view's cell): a
// colour use gives the value its swatch; a use as a string id or as a menu's text is none of a
// colour, a font and an image, so a value that reads as hex there ("ADD", "FACE") is no colour
// (S13 D4's second review); a value no menu uses that the game reads whole as hex still is.
static int test_style_value_use() {
	editor_test::TempProjectDir dir("opennova_styles_value_use");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Uses"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const AssetEntry *style = view.project.scan->find("menu_style.mns");
	TEST_EXPECT(style != nullptr);
	if (!style) return 1;
	const std::string path = style->relative_path;
	const std::string sheet = dir.file("project") + "/" + path;
	std::string text, message;
	TEST_EXPECT(opennova::io::read_file_text(sheet, text, message));
	TEST_EXPECT(editor_test::write_text(sheet, text + "ID_ONLY ADD\r\nTEXT_ONLY FACE\r\nPAINT FF102030\r\nSPARE 30\r\n"));
	const std::string window = "<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>90</RIGHT><BOTTOM>90</BOTTOM></POSITION>\r\n";
	TEST_EXPECT(editor_test::write_text(
	        (std::filesystem::path(sheet).parent_path() / "uses.mnu").generic_string(),
	        "<SCREEN>\r\n<NAME>USES</NAME>\r\n<WINDOW TYPE=\"STATIC\" NAME=\"A\">\r\n" + window +
	                "<STRING TYPE=\"ID\">%ID_ONLY%</STRING>\r\n</WINDOW>\r\n<WINDOW TYPE=\"STATIC\" NAME=\"B\">\r\n" +
	                window + "<STRING>%TEXT_ONLY%</STRING>\r\n</WINDOW>\r\n<WINDOW TYPE=\"STATIC\" NAME=\"C\">\r\n" +
	                window + "<APPEARANCE STATE=\"DEFAULT\" TYPE=\"COLOR\">%PAINT%</APPEARANCE>\r\n</WINDOW>\r\n</SCREEN>\r\n"));
	editor_test::handle_to_end(session, request::rescan());
	editor_test::handle_to_end(session, request::open_document(path));
	const auto *styles = dynamic_cast<const MnsDocument *>(session.document_for(path));
	const std::shared_ptr<const AssetGraph> &graph = view.findings.graph;
	TEST_EXPECT(styles != nullptr && graph != nullptr);
	if (!styles || !graph) return 1;
	const auto colour = [&](const char *name) {
		NodeAddress line;
		if (!find_definition(*graph, *styles, name, line)) return -1;
		return style_value_use(*styles, line, graph.get()).colour ? 1 : 0;
	};
	TEST_EXPECT(colour("ID_ONLY") == 0 && colour("TEXT_ONLY") == 0);
	TEST_EXPECT(colour("PAINT") == 1 && colour("SPARE") == 1);
	return 0;
}

// S13 D6: a load in place starts the revisions again at 0 and the rows' identities from 1, so the
// stylesheet's own memo (the sheet the game reads, which what a line's value is used as reads) keys
// on the load generation as well: FOO a colour, then the file loaded again in place with FOO a
// font's file, at the same revision and on the same row identity, answers from the new file.
static int test_load_in_place() {
	editor_test::TempProjectDir dir("opennova_styles_load_in_place");
	MnsDocument document;
	TEST_EXPECT(load(document, dir, "FOO FFFF0000\r\n"));
	const NodeAddress foo = row_at(document, 0);
	SymbolFacts colour;
	document.refine_symbol(foo, colour);
	TEST_EXPECT(colour.value == "FFFF0000" && !colour.inert &&
	            style_value_use(document, foo, nullptr).colour);
	const uint64_t generation = document.load_generation();
	TEST_EXPECT(load(document, dir, "FOO arial.fnt\r\n"));
	TEST_EXPECT(document.revision() == 0 && document.load_generation() != generation &&
	            row_at(document, 0) == foo);
	SymbolFacts font;
	document.refine_symbol(foo, font);
	TEST_EXPECT(font.value == "arial.fnt" && !style_value_use(document, foo, nullptr).colour);
	std::printf("test_load_in_place passed\n");
	return 0;
}

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failures = 0;
	failures += test_style_value_use();
	failures += test_load_in_place();
	failures += test_the_list();
	failures += test_edits();
	failures += test_changes_since_save();
	failures += test_validation();
	failures += test_stored_hang();
	const std::string fixture = retail::reference_fixture("mns/menu_style.mns");
	if (!fixture.empty()) failures += test_the_shipped_sheet(fixture);
	if (failures == 0) std::printf("editor_styles: all tests passed\n");
	if (failures == 0 && fixture.empty())
		return retail::skip_leg("OPENNOVA_JO_ASSETS/fixtures/mns/menu_style.mns (the shipped style sheet)");
	return failures == 0 ? 0 : 1;
}
