// S13 V10 (ADR 0046 S13, "the script device"): the Control-flavour device's portable half. The kinds'
// table: ViewportKind::Script, the Main role of every text type, its token and the refusal that lists
// the kinds, and the row's canvas flag (the script's none, as its viewport makes none). The span diff
// (preview/shown_text): a control's text taken back to the document as the fewest characters
// replaced, byte-exact against the TextDocument (an insert, a delete, a replace across lines, a line
// end joined and typed, a paste of several lines, CR LF kept, a lone CR and a NUL kept where the
// control cannot show them, the caret picking among equal runs, a code-page character stored as its
// byte, one it has none for refused), each undone byte for byte; a change at several places that
// keeps the line count (an indent of lines 3 to 9 over a file whose line 5 ends with an LF alone and
// holds a NUL) a span for each line it changes, so no line end or hidden byte between is rewritten.
// The keystroke burst (preview/text_burst): one token a run of typing, an edit away from where the
// last left off ending it, a quiet second, one EndEdit only when an edit went out. The gutter marks
// from the findings at the document's places, column included (a file-wide row's and another file's
// none). The highlights: the WAC compiler's words (keywords, commands, operands) in the control's
// places. Through a real session over a project: the script's Main viewport made as it opens, its
// device pinned by a headless cache and rebuilt; a keystroke burst planned from the control's text,
// served as one undo step; an indent of lines a step of its own, its burst ended with it and one
// open before it ended first; the follow's actions (an edit, an undo and a reload an Update); a compile report a mark once the burst ends; a Go to's span selected
// (a reference's, a keyword's, a caret alone); a credits file held read only and the busy gate
// refusing the planner; the envelope; a SetViewport of its device's size; and what the editor MCP's
// ops say of a viewport with no canvas (a hit refused as such, a render, a drag, a command and a
// camera refused, its items the marks, its notes none, the clock alone set). The retail leg
// (OPENNOVA_JO_DIR): every shipped script's highlights tokens of their lines, its shown text's lines
// the document's.
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <base/io/json.h>
#include <base/vfs/vfs.h>
#include <editor/assets/asset_import.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/documents/document_types.h>
#include <editor/documents/script_type.h>
#include <editor/model/text_document.h>
#include <editor/preview/canvas_half.h>
#include <editor/preview/script_viewport.h>
#include <editor/preview/shown_text.h>
#include <editor/preview/text_burst.h>
#include <editor/preview/viewport_json.h>
#include <editor/preview/viewport_kinds.h>
#include <editor/preview/viewports.h>
#include <editor/session/finding_codes.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/rtxt/rtxt.h>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"
#include "editor/viewport_test_support.h"

using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

std::string repo_file(const std::string &relative) {
	const std::vector<uint8_t> bytes = test_io::read_file(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/" + relative);
	return std::string(bytes.begin(), bytes.end());
}

std::vector<uint8_t> bytes_of(const std::string &text) { return std::vector<uint8_t>(text.begin(), text.end()); }

JsonValue parse(const std::string &text) {
	JsonValue json;
	std::string error;
	opennova::io::json_parse(text, json, error);
	return json;
}

// The batch a plan makes, its spans in the order the plan gives them (the ones further on first).
std::vector<Edit> edits_of(const ShownTextEdit &planned, uint64_t gesture = 0) {
	std::vector<Edit> edits;
	for (const ShownTextSpan &each : planned.spans) edits.push_back(TextDocument::replace(each.span, each.text, false, gesture));
	return edits;
}

TextSpan span(size_t line, size_t column, size_t length) {
	TextSpan out;
	out.line = line;
	out.column = column;
	out.length = length;
	return out;
}

bool same_span(const TextSpan &a, const TextSpan &b) {
	return a.line == b.line && a.column == b.column && a.length == b.length;
}

// A document of `kind`'s type (its line ends its reader's) over `text`.
std::unique_ptr<DocumentBase> document_of(AssetKind kind, const std::string &text, const std::string &name) {
	std::unique_ptr<DocumentBase> made = document_type_for(kind)->make();
	Diagnostic error;
	made->load_bytes(bytes_of(text), name, kind, "jo", error);
	return made;
}

// --- the kinds' table --------------------------------------------------------------------------------

int test_kind_table() {
	TEST_EXPECT(kViewportKindCount == 4); // the menu's, the model's, the script's, the mission's (S14)
	TEST_EXPECT(std::string(viewport_kind_token(ViewportKind::Script)) == "script");
	ViewportKind named = ViewportKind::kCount;
	TEST_EXPECT(viewport_kind_from_token("script", named) && named == ViewportKind::Script);
	const ViewportKindRow &row = viewport_kind_row(ViewportKind::Script);
	TEST_EXPECT(row.kind == ViewportKind::Script && row.role == ViewportRole::Main && !row.as_saved && !row.part &&
	            row.feed_count == 5 && row.make && !row.canvas);
	// Whether a kind has a canvas is what its viewport makes of one: the script's none, the others' one.
	for (size_t i = 0; i < kViewportKindCount; ++i) {
		const ViewportKindRow &kind_row = viewport_kind_row(static_cast<ViewportKind>(i));
		TEST_EXPECT((kind_row.make("t")->make_canvas() != nullptr) == kind_row.canvas);
	}
	// Every text type shown by it, the Main role's one kind; no record type; no Preview window's kind for
	// a text.
	const DocumentTypeId texts[] = {DocumentTypeId::Script, DocumentTypeId::MusicScript, DocumentTypeId::Credits,
	                                DocumentTypeId::Shader, DocumentTypeId::Text};
	size_t shown = 0, mains = 0;
	for (const DocumentTypeId type : texts) {
		shown += viewport_kind_shows(ViewportKind::Script, type) ? 1 : 0;
		TEST_EXPECT(main_viewport_kind(type) == ViewportKind::Script && preview_kind_of(type) == ViewportKind::kCount);
	}
	for (size_t i = 1; i <= kDocumentTypeCount; ++i) mains += main_viewport_kind(static_cast<DocumentTypeId>(i)) != ViewportKind::kCount;
	for (size_t i = 0; i < kViewportKindCount; ++i)
		// The Main-role kinds: the script's and the mission's (S14).
		TEST_EXPECT((viewport_kind_row(static_cast<ViewportKind>(i)).role == ViewportRole::Main) ==
		            (i == size_t(ViewportKind::Script) || i == size_t(ViewportKind::Mission)));
	TEST_EXPECT(shown == 5 && mains == 6); // the five text types and the mission (S14) have a Main-role kind
	// Its viewport: no options, no camera, no canvas; the empty one's reason.
	std::unique_ptr<ViewportModel> made = row.make("t.wac");
	TEST_EXPECT(made && made->kind() == ViewportKind::Script && !made->make_canvas() && made->options_json().is_object() &&
	            made->options_json().object.empty() && made->camera_json().is_null() &&
	            std::string(made->reason()) == "no_text" && made->status() == ViewportStatus::Empty);
	std::printf("kinds: %zu, the Main role's one shows %zu text types\n", kViewportKindCount, shown);
	return 0;
}

// --- the span diff -----------------------------------------------------------------------------------

struct DiffCase {
	const char *name;
	AssetKind kind;
	std::string text;
	std::u32string control;
	size_t caret;
	TextSpan span;
	std::string inserted;
	std::string after;
};

int test_span_diff() {
	// A text of code points beyond ASCII, written as numbers (MSVC with no /utf-8 widens a \u escape of a
	// char32_t literal through the narrow code page).
	const std::u32string accented{U'a', char32_t(0xE9), char32_t(0x20AC), U'b', U'\n'};
	const DiffCase cases[] = {
		{"an insert", AssetKind::Script, "ab\r\ncd\r\n", U"abX\ncd\n", 3, span(1, 3, 0), "X", "abX\r\ncd\r\n"},
		{"a delete", AssetKind::Script, "ab\r\ncd\r\n", U"a\ncd\n", 1, span(1, 2, 1), "", "a\r\ncd\r\n"},
		{"a replace across lines", AssetKind::Script, "ab\r\ncd\r\nef", U"aXf", 2, span(1, 2, 8), "X", "aXf"},
		{"two lines joined, the CR LF going whole", AssetKind::Script, "ab\r\ncd\r\n", U"abcd\n", 2, span(1, 3, 2), "",
		 "abcd\r\n"},
		{"a line end typed in a script, CR LF", AssetKind::Script, "ab\r\ncd\r\n", U"ab\nc\nd\n", 5, span(2, 2, 0), "\r\n",
		 "ab\r\nc\r\nd\r\n"},
		{"a line end typed in a text of LFs, LF", AssetKind::Text, "ab\ncd\n", U"ab\nc\nd\n", 5, span(2, 2, 0), "\n",
		 "ab\nc\nd\n"},
		{"a line end typed in a text of CR LFs, CR LF", AssetKind::Text, "ab\r\ncd", U"ab\nc\nd", 5, span(2, 2, 0), "\r\n",
		 "ab\r\nc\r\nd"},
		{"a paste of several lines", AssetKind::Script, "ab\r\ncd\r\n", U"ab\nX\nY\ncd\n", 7, span(2, 1, 0), "X\r\nY\r\n",
		 "ab\r\nX\r\nY\r\ncd\r\n"},
		{"a character typed into a run, at the caret", AssetKind::Script, "aa\r\n", U"aaa\n", 1, span(1, 1, 0), "a",
		 "aaa\r\n"},
		{"the same run, the caret at its end", AssetKind::Script, "aa\r\n", U"aaa\n", 3, span(1, 3, 0), "a", "aaa\r\n"},
		{"a CR alone kept where the control cannot show it", AssetKind::Text, std::string("a\rb\r\nc"), U"aXb\nc", 2,
		 span(1, 3, 0), "X", std::string("a\rXb\r\nc")},
		{"a NUL kept", AssetKind::Text, std::string("a\0b", 3), U"aYb", 2, span(1, 3, 0), "Y", std::string("a\0Yb", 4)},
		{"a code-page character stored as its byte", AssetKind::Script, "ab\r\n", accented, 3, span(1, 2, 0), "\xE9\x80",
		 "a\xE9\x80" "b\r\n"},
	};
	size_t planned = 0, undone = 0;
	for (const DiffCase &c : cases) {
		std::unique_ptr<DocumentBase> base = document_of(c.kind, c.text, c.kind == AssetKind::Script ? "t.wac" : "t.txt");
		TextDocument *document = text_of(*base);
		TEST_EXPECT(document != nullptr);
		ShownTextEdit edit;
		std::string error;
		// One place changed: one span.
		if (!ShownText(*document).edit(c.control, c.caret, edit, error) || edit.spans.size() != 1 ||
		    !same_span(edit.spans[0].span, c.span) || edit.spans[0].text != c.inserted) {
			const auto hex_of = [](const std::string &text) {
				std::string out;
				for (const char byte : text) {
					char hex[4];
					std::snprintf(hex, sizeof(hex), "%02X", static_cast<unsigned char>(byte));
					out += hex;
				}
				return out;
			};
			std::string points;
			for (const char32_t cp : c.control) points += std::to_string(uint32_t(cp)) + " ";
			const ShownTextSpan none;
			const ShownTextSpan &first = edit.spans.empty() ? none : edit.spans[0];
			std::fprintf(stderr, "%s: planned %zu span(s), the first %zu:%zu+%zu, bytes %s, wanted %s, control %s(%s)\n", c.name,
			             edit.spans.size(), first.span.line, first.span.column, first.span.length, hex_of(first.text).c_str(),
			             hex_of(c.inserted).c_str(), points.c_str(), error.c_str());
			return 1;
		}
		++planned;
		// Byte-exact against the document, and undone byte for byte.
		Diagnostic refused;
		TEST_EXPECT(document->apply(edits_of(edit), refused));
		if (document->text() != c.after) {
			std::fprintf(stderr, "%s: the document holds other bytes\n", c.name);
			return 1;
		}
		TEST_EXPECT(ShownText(*document).text() == c.control);
		document->undo();
		TEST_EXPECT(document->text() == c.text);
		++undone;
	}
	// The control holding the document's text: no edit.
	std::unique_ptr<DocumentBase> same = document_of(AssetKind::Script, "ab\r\ncd\r\n", "t.wac");
	ShownTextEdit none;
	std::string error;
	TEST_EXPECT(ShownText(*text_of(*same)).edit(U"ab\ncd\n", 0, none, error) && none.empty());
	// A character the code page has no byte for refuses the edit whole.
	const std::u32string unstorable{U'a', char32_t(0x2260), U'b', U'\n', U'c', U'd', U'\n'};
	TEST_EXPECT(!ShownText(*text_of(*same)).edit(unstorable, 2, none, error) && none.empty() &&
	            error.find("U+2260") != std::string::npos);
	std::printf("span diff: %zu edits planned, applied and undone byte for byte (%zu)\n", planned, undone);
	TEST_EXPECT(planned == 13 && undone == 13);
	return 0;
}

// A change at several places that keeps the line count (an indent of lines) is a span for each line
// it changes, so the line ends and the hidden bytes of the lines between and around are never
// rewritten: over ten lines whose fifth ends with an LF alone and holds a NUL, an indent of lines 3 to
// 9 is seven insertions (the ones further on first), applied one undo step that takes the bytes back
// whole; lines 3 and 9 alone two spans and nothing between; a change that moves the line count, and
// one a character of which the code page cannot hold, as one change as before.
int test_several_places() {
	std::string original = "alpha\r\nbeta\r\ngamma\r\ndelta\r\n";
	original += std::string("ep\0silon", 8);
	original += "\nzeta\r\neta\r\ntheta\r\niota\r\nkappa\r\n";
	std::unique_ptr<DocumentBase> base = document_of(AssetKind::Text, original, "t.txt");
	TextDocument *document = text_of(*base);
	TEST_EXPECT(document && document->text() == original && document->line_count() == 11);
	const ShownText shown(*document);
	TEST_EXPECT(shown.text() == U"alpha\nbeta\ngamma\ndelta\nepsilon\nzeta\neta\ntheta\niota\nkappa\n");
	// A tab at the start of each of `lines` of the control's text, and of the document's bytes.
	const auto indented = [&shown, &original](const std::vector<size_t> &lines, std::u32string &control, std::string &bytes) {
		control.clear();
		bytes.clear();
		size_t line = 1;
		bool at_start = true;
		const auto wanted = [&](size_t number) { return std::find(lines.begin(), lines.end(), number) != lines.end(); };
		for (const char32_t cp : shown.text()) {
			if (at_start && wanted(line)) control.push_back(U'\t');
			at_start = false;
			control.push_back(cp);
			if (cp == U'\n') {
				++line;
				at_start = true;
			}
		}
		line = 1;
		at_start = true;
		for (const char byte : original) {
			if (at_start && wanted(line)) bytes.push_back('\t');
			at_start = false;
			bytes.push_back(byte);
			if (byte == '\n') {
				++line;
				at_start = true;
			}
		}
	};
	std::u32string control;
	std::string expected;
	indented({3, 4, 5, 6, 7, 8, 9}, control, expected);
	ShownTextEdit planned;
	std::string error;
	TEST_EXPECT(shown.edit(control, 0, planned, error) && planned.spans.size() == 7);
	for (size_t i = 0; i < planned.spans.size(); ++i)
		TEST_EXPECT(planned.spans[i].span.line == 9 - i && planned.spans[i].span.column == 1 && planned.spans[i].span.length == 0 &&
		            planned.spans[i].text == "\t");
	// The whole change reaches from its first place to its last.
	TEST_EXPECT(planned.shown.from == shown.text().find(U"gamma") && planned.shown.removed > 0 && planned.shown.inserted > planned.shown.removed);
	Diagnostic refused;
	TEST_EXPECT(document->apply(edits_of(planned), refused) && document->text() == expected);
	// Line 5's end an LF alone and its NUL as they were; line 4's end and line 9's CR LF too.
	TEST_EXPECT(document->line(5) == std::string_view("\tep\0silon", 9) &&
	            document->text().find("silon\n\tzeta") != std::string::npos &&
	            document->text().find("delta\r\n\tep") != std::string::npos && document->text().find("iota\r\nkappa") != std::string::npos);
	document->undo();
	TEST_EXPECT(document->text() == original && !document->can_undo());
	// Lines 3 and 9 alone: two spans, and nothing is written between them.
	indented({3, 9}, control, expected);
	TEST_EXPECT(shown.edit(control, 0, planned, error) && planned.spans.size() == 2 && planned.spans[0].span.line == 9 &&
	            planned.spans[1].span.line == 3);
	TEST_EXPECT(document->apply(edits_of(planned), refused) && document->text() == expected);
	document->undo();
	TEST_EXPECT(document->text() == original);
	// A change that moves the line count is one change (lines 3 and 4 joined: the line end between them
	// goes with the span), as is any change of one place.
	const std::u32string joined = shown.text().substr(0, shown.text().find(U"gamma") + 5) + shown.text().substr(shown.text().find(U"delta"));
	TEST_EXPECT(shown.edit(joined, 0, planned, error) && planned.spans.size() == 1 && planned.spans[0].span.line == 3 &&
	            planned.spans[0].span.column == 6 && planned.spans[0].span.length == 2 && planned.spans[0].text.empty());
	// A character the code page has no byte for in one of several places refuses the whole edit.
	indented({3, 4}, control, expected);
	control.insert(control.find(U"delta") + 5, 1, char32_t(0x2260));
	TEST_EXPECT(!shown.edit(control, 0, planned, error) && planned.empty() && error.find("U+2260") != std::string::npos);
	std::printf("several places: seven lines indented as seven spans, line 5's LF alone and NUL untouched\n");
	return 0;
}

// The shown text's places: lines one for one, the bytes it does not show mapped past.
int test_shown_text() {
	std::unique_ptr<DocumentBase> base = document_of(AssetKind::Text, std::string("a\rb\r\nc\0d\ne", 10), "t.txt");
	const TextDocument &document = *text_of(*base);
	const ShownText shown(document);
	TEST_EXPECT(shown.text() == U"ab\ncd\ne" && shown.line_end() == "\r\n");
	// Each shown character's byte (past what is hidden before it), and back.
	const size_t bytes[] = {0, 2, 3, 5, 7, 8, 9, 10};
	for (size_t i = 0; i < 8; ++i) TEST_EXPECT(shown.document_offset(i) == bytes[i]);
	TEST_EXPECT(shown.shown_at(1) == 1 && shown.shown_at(2) == 1 && shown.shown_at(4) == 3 && shown.shown_at(6) == 4);
	TEST_EXPECT(document.line_count() == 3);
	size_t line = 0, column = 0;
	ShownText::place_of(shown.text(), 4, line, column);
	TEST_EXPECT(line == 1 && column == 1 && ShownText::offset_of(shown.text(), 1, 1) == 4);
	ShownText::place_of(shown.text(), 7, line, column);
	TEST_EXPECT(line == 2 && column == 1 && ShownText::offset_of(shown.text(), 2, 9) == 7 &&
	            ShownText::offset_of(shown.text(), 9, 0) == 7);
	return 0;
}

// --- the keystroke burst ----------------------------------------------------------------------------

int test_burst() {
	editor_test::Gathered out;
	TextBurst burst;
	TEST_EXPECT(!burst.open() && !burst.quiet(10.0));
	burst.end(out);
	TEST_EXPECT(out.requests.empty()); // nothing went out: no EndEdit
	const uint64_t token = burst.token();
	TEST_EXPECT(token != 0 && burst.token() == token);
	burst.sent("a.wac", 5, 1.0);
	// Typing on, a Backspace and a Delete at the place go on; a change elsewhere or in another file does not.
	TEST_EXPECT(burst.open() && burst.continues("a.wac", 5, 0) && burst.continues("a.wac", 4, 1) &&
	            burst.continues("a.wac", 5, 1) && !burst.continues("a.wac", 9, 0) && !burst.continues("b.wac", 5, 0));
	TEST_EXPECT(!burst.quiet(1.5) && burst.quiet(1.0 + TextBurst::kQuietSeconds));
	burst.end(out);
	TEST_EXPECT(out.requests.size() == 1 && out.requests[0].kind == EditorRequestKind::EndEdit &&
	            out.requests[0].path == "a.wac" && !burst.open());
	// The next burst a token of its own; one dropped (its step ended already) raises nothing.
	const uint64_t next = burst.token();
	TEST_EXPECT(next != token);
	burst.sent("a.wac", 1, 2.0);
	burst.drop();
	burst.end(out);
	TEST_EXPECT(out.requests.size() == 1 && burst.token() != next);
	return 0;
}

// --- the gutter marks and the highlights -------------------------------------------------------------

// A finding of `row` about `asset` at a line and a column.
Diagnostic finding_at(const FindingCodeRow &row, DiagnosticSeverity severity, const std::string &message,
		const std::string &asset, size_t line, size_t column) {
	Diagnostic d = make_finding(row, severity, message, asset);
	d.line = line;
	d.column = column;
	return d;
}

int test_marks() {
	std::unique_ptr<DocumentBase> base = document_of(AssetKind::Script, "a\r\nb\r\nc\r\n", "t.wac");
	SessionView view;
	view.documents.open = {std::shared_ptr<const DocumentBase>(std::move(base))};
	const DocumentBase &document = *view.documents.open.front();
	const FindingCodeRow &compile = finding_code(ScriptFinding::Compile);
	const FindingCodeRow *missing = finding_row("reference.missing");
	// A row about the file as a whole (Files shows it, no line of it).
	const FindingCodeRow *file_wide = nullptr;
	for (const FindingCodeRow &row : core_finding_codes())
		if (row.place == FindingPlace::File && !file_wide) file_wide = &row;
	TEST_EXPECT(missing && file_wide);
	view.findings.diagnostics = {
		finding_at(compile, DiagnosticSeverity::Warning, "first", "t.wac", 2, 3),
		finding_at(compile, DiagnosticSeverity::Error, "second", "t.wac", 2, 1),
		finding_at(*missing, DiagnosticSeverity::Warning, "missing", "t.wac", 3, 5),
		finding_at(compile, DiagnosticSeverity::Error, "another file's", "u.wac", 1, 1),
		finding_at(compile, DiagnosticSeverity::Error, "no line", "t.wac", 0, 0),
		finding_at(*file_wide, DiagnosticSeverity::Error, "the file's", "t.wac", 1, 1),
		finding_at(compile, DiagnosticSeverity::Error, "past the text", "t.wac", 9, 1),
	};
	view.revisions.touch(ViewConcern::Findings);
	ScriptViewport viewport("t.wac");
	PreviewClock clock;
	viewport.attach();
	TEST_EXPECT(viewport.follow(ViewportInput{view, clock, &document, ChangeClass::Loaded}, clock) == ViewportAction::Rebuild &&
	            viewport.take_action() == ViewportAction::Rebuild);
	const std::vector<ScriptMark> &marks = viewport.marks();
	const size_t made = marks.size();
	TEST_EXPECT(marks.size() == 2 && marks[0].line == 2 && marks[0].severity == DiagnosticSeverity::Error &&
	            marks[0].findings.size() == 2 && marks[0].tip() == "first\nsecond" && marks[0].findings[0].column == 3 &&
	            marks[0].findings[0].code == "script.compile");
	TEST_EXPECT(marks[1].line == 3 && marks[1].severity == DiagnosticSeverity::Warning &&
	            marks[1].findings[0].code == "reference.missing" && marks[1].findings[0].column == 5);
	// The findings moved: the marks made again, an Update; nothing moved: nothing made.
	const uint64_t serial = viewport.marks_serial();
	TEST_EXPECT(viewport.follow(ViewportInput{view, clock, &document, ChangeClass::None}, clock) == ViewportAction::Keep &&
	            viewport.marks_serial() == serial);
	view.findings.diagnostics.resize(1);
	view.revisions.touch(ViewConcern::Findings);
	TEST_EXPECT(viewport.follow(ViewportInput{view, clock, &document, ChangeClass::None}, clock) == ViewportAction::Update &&
	            viewport.take_action() == ViewportAction::Update && viewport.marks_serial() == serial + 1 &&
	            viewport.marks().size() == 1 && viewport.marks()[0].severity == DiagnosticSeverity::Warning);
	// Its envelope's items are the marks.
	const JsonValue items = viewport.items_json(ViewportInput{view, clock, &document, ChangeClass::None});
	TEST_EXPECT(items.array.size() == 1 && items.array[0].get_number("line", 0) == 2 &&
	            items.array[0].get_string("severity", "") == "warning" && items.array[0].get("findings")->array.size() == 1);
	std::printf("marks: %zu of 7 findings, on lines 2 and 3\n", made);
	return 0;
}

// The fixture script's words as the compiler read them, in the control's places.
int test_highlights() {
	const std::string text = repo_file("wac/text_document.wac");
	std::unique_ptr<DocumentBase> base = document_of(AssetKind::Script, text, "scripts/text_document.wac");
	std::vector<TextHighlight> words;
	script_highlights(*text_of(*base), words);
	size_t keywords = 0, commands = 0, operands = 0;
	for (const TextHighlight &word : words) {
		keywords += word.kind == TextHighlightKind::Keyword;
		commands += word.kind == TextHighlightKind::Command;
		operands += word.kind == TextHighlightKind::Operand;
	}
	std::printf("highlights: %zu keywords, %zu commands, %zu operands\n", keywords, commands, operands);
	// If, then, endif; true, fxrain, sound2tgt, ammoarea, ammo2tgt, ssnname; FX_Buildup, SS_EXPLO_BASE,
	// AMMO_AT_CONTRACT, ammo_satchel, TT_MISSION_START (a named value, bluekills, and the numbers none).
	TEST_EXPECT(keywords == 3 && commands == 6 && operands == 5);
	const auto at = [&](size_t line, size_t column) -> const TextHighlight * {
		for (const TextHighlight &word : words)
			if (word.span.line == line && word.span.column == column) return &word;
		return nullptr;
	};
	TEST_EXPECT(at(2, 1) && at(2, 1)->kind == TextHighlightKind::Keyword && at(2, 1)->span.length == 2);
	TEST_EXPECT(at(3, 2) && at(3, 2)->kind == TextHighlightKind::Command && at(3, 2)->span.length == 6);
	TEST_EXPECT(at(3, 9) && at(3, 9)->kind == TextHighlightKind::Operand && at(3, 9)->span.length == 10);
	TEST_EXPECT(at(8, 1) && at(8, 1)->kind == TextHighlightKind::Keyword && at(8, 1)->span.length == 5);
	// The viewport places them in the control's lines and columns (from 0); a text type without a
	// reader's port of its words (a configuration) has none.
	SessionView view;
	view.documents.open = {std::shared_ptr<const DocumentBase>(std::move(base))};
	ScriptViewport viewport("scripts/text_document.wac");
	PreviewClock clock;
	viewport.follow(ViewportInput{view, clock, view.documents.open.front().get(), ChangeClass::Loaded}, clock);
	TEST_EXPECT(viewport.highlights().size() == words.size());
	TEST_EXPECT(viewport.highlights()[0].line == 1 && viewport.highlights()[0].column == 0 && viewport.highlights()[0].length == 2);
	std::unique_ptr<DocumentBase> config = document_of(AssetKind::Config, "[Game]\r\nname = x\r\n", "game.cfg");
	SessionView other;
	other.documents.open = {std::shared_ptr<const DocumentBase>(std::move(config))};
	ScriptViewport plain("game.cfg");
	plain.follow(ViewportInput{other, clock, other.documents.open.front().get(), ChangeClass::Loaded}, clock);
	TEST_EXPECT(plain.status() == ViewportStatus::Ready && plain.highlights().empty());
	return 0;
}

// A reveal after a hidden byte on its line: a script whose first line holds a CR alone (the end of its
// comment for the compiler, hidden from the control) ahead of its If, so the keyword stands one column
// earlier in the control than in the document, and a Go to's span selects it alone, in the control's
// places, as its highlight does.
int test_reveal_after_hidden_byte() {
	std::unique_ptr<DocumentBase> base = document_of(
			AssetKind::Script, "; note\rIf true(bluekills) then\r\n\tfxrain FX_Buildup\r\nendif\r\n", "t.wac");
	SessionView view;
	view.documents.open = {std::shared_ptr<const DocumentBase>(std::move(base))};
	const DocumentBase &document = *view.documents.open.front();
	ScriptViewport viewport("t.wac");
	PreviewClock clock;
	viewport.follow(ViewportInput{view, clock, &document, ChangeClass::Loaded}, clock);
	TEST_EXPECT(viewport.shown_text().text().compare(0, 14, U"; noteIf true(") == 0);
	size_t keywords = 0;
	for (const ScriptHighlight &word : viewport.highlights())
		if (word.line == 0 && word.length == 2 && word.kind == TextHighlightKind::Keyword) {
			++keywords;
			TEST_EXPECT(word.column == 6);
		}
	TEST_EXPECT(keywords == 1);
	ViewEvent event;
	event.kind = ViewEventKind::RevealText;
	event.path = "t.wac";
	event.locator = TextDocument::locator(1, 8);
	event.seq = 7;
	viewport.receive(event);
	viewport.follow(ViewportInput{view, clock, &document, ChangeClass::None}, clock);
	const ScriptReveal &reveal = viewport.reveal();
	TEST_EXPECT(reveal.seq == 7 && same_span(reveal.span, span(1, 8, 2)) && reveal.line == 0 && reveal.column == 6 &&
	            reveal.end_line == 0 && reveal.end_column == 8);
	std::printf("reveal: the keyword after a hidden CR at columns 6 to 8 of the control\n");
	return 0;
}

// --- through a session -------------------------------------------------------------------------------

struct ScriptRig {
	editor_test::TempProjectDir dir{"opennova_editor_script_viewport"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::FakeDevices devices;
	const std::string script = "scripts/text_document.wac";
	std::string root() const { return dir.file("project"); }
	const SessionView &view() const { return session.view(); }
	const ScriptViewport *viewport(const std::string &path) {
		return static_cast<const ScriptViewport *>(session.viewports().find(path, ViewportKind::Script));
	}
	editor_test::FakeDevice *device(const std::string &path) { return devices.held(path, ViewportKind::Script); }
	void pump() {
		session.run_operations();
		devices.sync(session);
	}
};

// A project of the fixture script and what its operands name (as S13 D9's test writes it), so its
// references resolve; and the minted credits file its text form cannot carry.
bool make_project(ScriptRig &rig) {
	editor_test::handle_to_end(rig.session, request::new_project(rig.root(), "Scripts"));
	editor_test::create_missing_files(rig.session);
	opennova::rtxt::File table;
	table.sections.push_back({"mission", 1});
	table.entries.push_back({"MISSION_START", "The mission begins.", {}, 0});
	std::vector<uint8_t> strings;
	std::string error;
	if (!opennova::rtxt::write(table, strings, error)) return false;
	const std::string root = rig.root();
	const bool written =
	        editor_test::write_text(root + "/" + rig.script, repo_file("wac/text_document.wac")) &&
	        editor_test::write_text(root + "/particles/effects.ptl", repo_file("particle/synth_minimal_effect.ptl")) &&
	        editor_test::write_text(root + "/defs/ammo.def",
	                                "ammo AT_CONTRACT\nmax_age 1.5\nend\nammo ammo_satchel\nmax_age 2\nend\n") &&
	        editor_test::write_bytes(root + "/strings/text_document.bin", strings) &&
	        editor_test::write_text(root + "/menus/nlist.kda", repo_file("cbin/synth_nlist.kda"));
	editor_test::handle_to_end(rig.session, request::rescan());
	return written;
}

// The marks of a viewport's document of `code`.
size_t marks_of(const ScriptViewport &viewport, const char *code) {
	size_t count = 0;
	for (const ScriptMark &mark : viewport.marks())
		for (const ScriptMarkFinding &finding : mark.findings) count += finding.code == code;
	return count;
}

int test_session() {
	ScriptRig rig;
	TEST_EXPECT(make_project(rig));
	editor_test::handle_to_end(rig.session, request::open_document(rig.script));
	// Its Main viewport, made as it opened; the headless cache gives the active document's a device,
	// which makes its picture: the control's text.
	const ScriptViewport *viewport = rig.viewport(rig.script);
	TEST_EXPECT(viewport != nullptr);
	rig.pump();
	editor_test::FakeDevice *device = rig.device(rig.script);
	TEST_EXPECT(device && device->since(0) == std::vector<ViewportAction>{ViewportAction::Rebuild});
	const TextDocument *document = text_of(*rig.session.document_base_for(rig.script));
	TEST_EXPECT(document && viewport->status() == ViewportStatus::Ready && viewport->editable() &&
	            viewport->shown_text().text() == ShownText(*document).text() && viewport->highlights().size() == 14 &&
	            viewport->marks().empty());
	// A keystroke burst from the control: " )" typed after FX_Buildup, then a space, one token; then a
	// keystroke on line 1, a burst of its own (the first ended: its EndEdit before it).
	const std::string original = document->text();
	std::u32string control = viewport->shown_text().text();
	size_t at = ShownText::offset_of(control, 2, 18);
	control.insert(at, U" )");
	editor_test::Gathered out;
	TextBurst burst;
	std::string error;
	TEST_EXPECT(viewport->edit(viewport_context(rig.view(), *viewport), control, at + 2, 1.0, burst, out, error));
	TEST_EXPECT(out.requests.size() == 1 && out.requests[0].kind == EditorRequestKind::EditRecord &&
	            out.requests[0].edits.size() == 1 && out.requests[0].edits[0].gesture == burst.token());
	const auto *first = dynamic_cast<const TextSpanEdit *>(out.requests[0].edits[0].payload.get());
	TEST_EXPECT(first && same_span(first->span, span(3, 19, 0)) && first->text == " )");
	TEST_EXPECT(editor_test::serve(rig.session, out.requests) && document->line(3) == "\tfxrain FX_Buildup )");
	rig.pump();
	TEST_EXPECT(device->last() == ViewportAction::Update && viewport->shown_text().text() == control);
	control.insert(at + 2, U" ");
	out.requests.clear();
	TEST_EXPECT(viewport->edit(viewport_context(rig.view(), *viewport), control, at + 3, 1.2, burst, out, error));
	TEST_EXPECT(out.requests.size() == 1 && out.requests[0].edits[0].gesture == burst.token() &&
	            editor_test::serve(rig.session, out.requests));
	const uint64_t first_token = burst.token();
	const std::string first_line(document->line(1));
	control.insert(0, U";");
	out.requests.clear();
	TEST_EXPECT(viewport->edit(viewport_context(rig.view(), *viewport), control, 1, 1.4, burst, out, error));
	TEST_EXPECT(out.requests.size() == 2 && out.requests[0].kind == EditorRequestKind::EndEdit &&
	            out.requests[1].kind == EditorRequestKind::EditRecord && out.requests[1].edits[0].gesture != first_token);
	TEST_EXPECT(editor_test::serve(rig.session, out.requests));
	out.requests.clear();
	burst.end(out);
	TEST_EXPECT(out.requests.size() == 1 && out.requests[0].kind == EditorRequestKind::EndEdit &&
	            editor_test::serve(rig.session, out.requests));
	// The compile report the burst left is a mark once it ended and the validation ran: line 3's.
	rig.pump();
	TEST_EXPECT(marks_of(*viewport, "script.compile") == 1 && viewport->marks().size() == 1 &&
	            viewport->marks()[0].line == 3 && viewport->marks()[0].severity == DiagnosticSeverity::Warning &&
	            viewport->marks()[0].findings[0].column == 20);
	// Undo: the second burst one step, the first's two keystrokes one step; each an Update.
	const size_t taken = device->taken.size();
	editor_test::handle_to_end(rig.session, request::undo(rig.script));
	rig.pump();
	TEST_EXPECT(document->line(1) == first_line);
	TEST_EXPECT(document->line(3) == "\tfxrain FX_Buildup ) " && device->since(taken) == std::vector<ViewportAction>{ViewportAction::Update});
	editor_test::handle_to_end(rig.session, request::undo(rig.script));
	rig.pump();
	TEST_EXPECT(document->text() == original && viewport->shown_text().text() == ShownText(*document).text());
	TEST_EXPECT(viewport->marks().empty());
	// An indent of lines 3 to 5 from the control (a change at three places): one batch of three spans
	// under one token, its burst ended with it (the EndEdit follows it, an open burst none), served one
	// undo step that takes the bytes back whole.
	{
		const std::string before3(document->line(3)), before4(document->line(4)), before5(document->line(5));
		std::u32string indented = viewport->shown_text().text();
		for (const size_t line : {size_t(4), size_t(3), size_t(2)}) indented.insert(ShownText::offset_of(indented, line, 0), 1, U'\t');
		editor_test::Gathered indent;
		TextBurst indent_burst;
		TEST_EXPECT(viewport->edit(viewport_context(rig.view(), *viewport), indented,
		                           ShownText::offset_of(indented, 4, 1), 5.0, indent_burst, indent, error));
		TEST_EXPECT(indent.requests.size() == 2 && indent.requests[0].kind == EditorRequestKind::EditRecord &&
		            indent.requests[0].edits.size() == 3 && indent.requests[1].kind == EditorRequestKind::EndEdit &&
		            !indent_burst.open());
		const uint64_t indent_token = indent.requests[0].edits[0].gesture;
		TEST_EXPECT(indent_token != 0 && indent.requests[0].edits[1].gesture == indent_token &&
		            indent.requests[0].edits[2].gesture == indent_token);
		TEST_EXPECT(editor_test::serve(rig.session, indent.requests));
		TEST_EXPECT(document->line(3) == "\t" + before3 && document->line(4) == "\t" + before4 && document->line(5) == "\t" + before5);
		rig.pump();
		TEST_EXPECT(viewport->shown_text().text() == indented);
		editor_test::handle_to_end(rig.session, request::undo(rig.script));
		rig.pump();
		TEST_EXPECT(document->text() == original && viewport->shown_text().text() == ShownText(*document).text());
		// A burst open when the indent comes is ended before it, and the indent's token is its own: two
		// undo steps, the indent and then the typing.
		TextBurst open_burst;
		editor_test::Gathered typed;
		std::u32string typing = viewport->shown_text().text();
		typing.insert(0, U";");
		TEST_EXPECT(viewport->edit(viewport_context(rig.view(), *viewport), typing, 1, 6.0, open_burst, typed, error) &&
		            open_burst.open() && typed.requests.size() == 1 && editor_test::serve(rig.session, typed.requests));
		const uint64_t typing_token = typed.requests[0].edits[0].gesture;
		std::u32string both = typing;
		for (const size_t line : {size_t(4), size_t(3)}) both.insert(ShownText::offset_of(both, line, 0), 1, U'\t');
		typed.requests.clear();
		TEST_EXPECT(viewport->edit(viewport_context(rig.view(), *viewport), both, ShownText::offset_of(both, 3, 1), 6.2,
		                           open_burst, typed, error));
		TEST_EXPECT(typed.requests.size() == 3 && typed.requests[0].kind == EditorRequestKind::EndEdit &&
		            typed.requests[1].kind == EditorRequestKind::EditRecord && typed.requests[1].edits.size() == 2 &&
		            typed.requests[1].edits[0].gesture != typing_token && typed.requests[2].kind == EditorRequestKind::EndEdit &&
		            !open_burst.open() && editor_test::serve(rig.session, typed.requests));
		editor_test::handle_to_end(rig.session, request::undo(rig.script));
		rig.pump();
		TEST_EXPECT(document->line(1).substr(0, 1) == ";" && document->line(3) == before3 && document->line(4) == before4);
		editor_test::handle_to_end(rig.session, request::undo(rig.script));
		rig.pump();
		TEST_EXPECT(document->text() == original);
	}
	// A reload: the control's text made again, an Update (S13 V8 classes a document read again Unknown),
	// the device's take_text_ replacing what differs; the text made once.
	const size_t before_reload = device->taken.size();
	const uint64_t made_before = viewport->texts_made();
	editor_test::handle_to_end(rig.session, request::reload_document(rig.script));
	rig.pump();
	{
		const std::vector<ViewportAction> reloaded = device->since(before_reload);
		TEST_EXPECT(reloaded == std::vector<ViewportAction>{ViewportAction::Update});
	}
	viewport = rig.viewport(rig.script);
	TEST_EXPECT(viewport != nullptr && viewport->texts_made() == made_before + 1);
	// A Go to's span: the reference at 5:16 selected whole (AT_CONTRACT), in the control's places; a
	// keyword's at 2:1; the caret alone inside the comment.
	editor_test::handle_to_end(rig.session, request::open_document(rig.script, "5:16"));
	rig.pump();
	const ScriptReveal &reveal = viewport->reveal();
	TEST_EXPECT(reveal.seq != 0 && same_span(reveal.span, span(5, 16, 11)) && reveal.line == 4 && reveal.column == 15 &&
	            reveal.end_line == 4 && reveal.end_column == 26 && device->last() == ViewportAction::Update);
	const uint64_t seq = reveal.seq;
	editor_test::handle_to_end(rig.session, request::open_document(rig.script, "2:1"));
	rig.pump();
	TEST_EXPECT(viewport->reveal().seq > seq && same_span(viewport->reveal().span, span(2, 1, 2)) &&
	            viewport->reveal().end_column == 2);
	editor_test::handle_to_end(rig.session, request::open_document(rig.script, "1:3"));
	rig.pump();
	TEST_EXPECT(same_span(viewport->reveal().span, span(1, 3, 0)) && viewport->reveal().column == 2 &&
	            viewport->reveal().end_column == 2);
	// The envelope: its kind, as it stands (not as saved), its body and its marks; the kind's empty one.
	const JsonValue envelope = viewport_to_json(rig.view(), *viewport, JsonPage());
	TEST_EXPECT(envelope.get_string("kind", "") == "script" && envelope.get_string("status", "") == "ready" &&
	            !envelope.get_bool("as_saved", true) && envelope.get("body")->get_number("line_count", 0) == 9 &&
	            envelope.get("body")->get_bool("editable", false) && envelope.get("body")->get("reveal")->is_object() &&
	            envelope.get("items")->array.empty());
	const JsonValue empty = editor_test::empty_viewport_json(rig.view(), ViewportKind::Script);
	TEST_EXPECT(empty.get_string("reason", "") == "no_text" && empty.get_string("status", "") == "empty");
	// A SetViewport of its device's size (no canvas sizes it headless); a kind's member it has not
	// refused; a kind no row has refused naming the three.
	TEST_EXPECT(rig.session.handle(request::set_viewport(rig.script, R"({"kind": "script", "device": {"width": 640, "height": 400}})")) &&
	            rig.session.outcome().done() && viewport->state().width == 640 && viewport->state().height == 400);
	rig.session.handle(request::set_viewport(rig.script, R"({"kind": "script", "options": {}})"));
	TEST_EXPECT(!rig.session.outcome().done());
	JsonValue unknown;
	std::string parse_error, set_error;
	TEST_EXPECT(opennova::io::json_parse(R"({"kind": "scrpt"})", unknown, parse_error) &&
	            !rig.session.viewports().set(rig.view(), rig.script, unknown, set_error) &&
	            set_error.find("menu, model, script") != std::string::npos);
	// What the wire says of a viewport with no canvas (editor_viewport's ops): a hit refused as a kind
	// with no canvas, never answered as a hit on nothing; a render of a row refused; its items the marks
	// and its notes none; a drag (by a pixel's distance or to a point) and a command refused as
	// viewport.refused; a camera refused as it is no member of its state; the clock alone, set with no
	// document named, accepted and the script's own state untouched.
	{
		std::string query_error;
		const auto ask = [&](const std::string &args) {
			query_error.clear();
			return rig.session.query("viewport", parse(args), query_error);
		};
		TEST_EXPECT(ask(R"({"op": "hit", "x": 10, "y": 10})").is_null() && query_error.find("no canvas") != std::string::npos);
		ask(R"({"op": "render", "row": 1})");
		TEST_EXPECT(query_error.find("renders no row") != std::string::npos);
		const JsonValue items = ask(R"({"op": "items"})");
		TEST_EXPECT(query_error.empty() && items.get("items") && items.get("items")->array.size() == viewport->marks().size() &&
		            items.get_string("kind", "") == "script");
		const JsonValue notes = ask(R"({"op": "notes"})");
		TEST_EXPECT(query_error.empty() && notes.get("notes") && notes.get("notes")->array.empty());
		const auto refused_no_canvas = [&](const std::string &request_json) {
			const JsonValue answer = rig.session.handle_json(parse(request_json));
			const JsonValue *outcome = answer.get("outcome");
			const JsonValue *findings = outcome ? outcome->get("findings") : nullptr;
			const bool ok = answer.get_bool("ok", false) && outcome && !outcome->get_bool("done", true) && findings &&
			                findings->array.size() == 1 && findings->array[0].get_string("code", "") == "viewport.refused" &&
			                findings->array[0].get_string("message", "").find("no canvas") != std::string::npos;
			if (!ok) std::fprintf(stderr, "not refused as a kind with no canvas: %s\n", opennova::io::json_write(answer).c_str());
			return ok;
		};
		TEST_EXPECT(refused_no_canvas(R"({"kind": "edit_in_viewport", "drag": {"id": 1, "handle": "move", "by": [8, 0]}})"));
		TEST_EXPECT(refused_no_canvas(R"({"kind": "edit_in_viewport", "drag": {"id": 1, "handle": "move", "to": [8, 8]}})"));
		TEST_EXPECT(refused_no_canvas(R"({"kind": "edit_in_viewport", "command": {"name": "align_left", "ids": [1]}})"));
		rig.session.handle(request::set_viewport(rig.script, R"({"kind": "script", "camera": {"yaw": 1}})"));
		TEST_EXPECT(!rig.session.outcome().done());
		const double lines = ask(R"({"op": "state"})").get("body")->get_number("line_count", 0);
		const JsonValue clocked = rig.session.handle_json(parse(R"({"kind": "set_viewport", "viewport": {"clock": {"time_ms": 120}}})"));
		const JsonValue after = ask(R"({"op": "state"})");
		TEST_EXPECT(clocked.get_bool("ok", false) && clocked.get("outcome") && clocked.get("outcome")->get_bool("done", false) &&
		            after.get("clock")->get_number("time_ms", 0) == 120.0 && after.get("body")->get_number("line_count", -1) == lines &&
		            after.get_string("status", "") == "ready");
	}
	// The busy gate: an operation holding the documents makes it read only (an Update), and the planner
	// refuses an edit; the operation done, it takes edits again.
	TEST_EXPECT(rig.session.start_operation(std::make_unique<editor_test::HoldingOperation>()) != 0);
	rig.devices.sync(rig.session);
	TEST_EXPECT(!viewport->editable() && !viewport->read_only().empty() && device->last() == ViewportAction::Update);
	out.requests.clear();
	TEST_EXPECT(!viewport->edit(viewport_context(rig.view(), *viewport), U"x" + viewport->shown_text().text(), 1, 3.0,
	                            burst, out, error) &&
	            out.requests.empty());
	rig.pump();
	TEST_EXPECT(viewport->editable());
	// A credits file its text form cannot carry: held read only, its device so; the planner refuses.
	editor_test::handle_to_end(rig.session, request::open_document("menus/nlist.kda"));
	rig.pump();
	const ScriptViewport *credits = rig.viewport("menus/nlist.kda");
	TEST_EXPECT(credits && rig.device("menus/nlist.kda") && credits->status() == ViewportStatus::Ready && !credits->editable() &&
	            !credits->read_only().empty());
	TEST_EXPECT(!credits->edit(viewport_context(rig.view(), *credits), U"x" + credits->shown_text().text(), 1, 4.0, burst,
	                           out, error) &&
	            out.requests.empty());
	// Closed: its viewport gone, its device dropped at the next sync.
	const size_t actions = device->taken.size();
	editor_test::handle_to_end(rig.session, request::close_document(rig.script));
	rig.pump();
	TEST_EXPECT(!rig.viewport(rig.script) && !rig.device(rig.script));
	std::printf("session: %zu actions taken by the script's device\n", actions);
	return 0;
}

// --- the retail leg ----------------------------------------------------------------------------------

// The retail leg (OPENNOVA_JO_DIR): every script the install ships, as the editor reads it. Its
// highlights each one token of its line (inside the line, no blank and no line end in it), in order
// and apart; its text as the control shows it a line for each of the document's, each line's start
// the document's byte for byte; the install's counts pinned.
int test_retail() {
	const std::string install = retail::install();
	if (install.empty()) {
		retail::skip_leg("OPENNOVA_JO_DIR (the install's scripts)");
		return 0;
	}
	ProjectDocument project;
	project.target_game = "jo";
	opennova::Vfs mount;
	TEST_EXPECT(mount_retail(mount, install, project));
	size_t scripts = 0, keywords = 0, commands = 0, operands = 0, lines = 0;
	for (const opennova::VfsFileLocation &location : mount.list_files()) {
		const std::string &name = location.logical_name;
		std::vector<uint8_t> bytes;
		if (classify_asset(name, nullptr) != AssetKind::Script || !mount.read_file_raw(name, bytes)) continue;
		const DocumentType *type = document_type_for(AssetKind::Script);
		std::unique_ptr<DocumentBase> base = type->make();
		Diagnostic error;
		TEST_EXPECT(base->load_bytes(bytes, name, AssetKind::Script, "jo", error));
		const TextDocument *text = text_of(*base);
		if (!text) continue;
		++scripts;
		std::vector<TextHighlight> words;
		type->highlights(*text, words);
		const TextHighlight *last = nullptr;
		for (const TextHighlight &word : words) {
			keywords += word.kind == TextHighlightKind::Keyword;
			commands += word.kind == TextHighlightKind::Command;
			operands += word.kind == TextHighlightKind::Operand;
			const std::string_view line = text->line(word.span.line);
			const bool token = word.span.line >= 1 && word.span.line <= text->line_count() && word.span.column >= 1 &&
			                   word.span.length > 0 && word.span.column - 1 + word.span.length <= line.size() &&
			                   line.substr(word.span.column - 1, word.span.length).find_first_of(" \t\r\n") == std::string_view::npos;
			if (!token)
				std::fprintf(stderr, "retail: %s: the run at %zu:%zu (%zu) is no token of its line\n", name.c_str(), word.span.line,
				             word.span.column, word.span.length);
			TEST_EXPECT(token);
			TEST_EXPECT(!last || last->span.line < word.span.line ||
			            (last->span.line == word.span.line && last->span.column + last->span.length <= word.span.column));
			last = &word;
		}
		// The control's lines the document's, each start where the document's line starts.
		const ShownText shown(*text);
		std::vector<size_t> starts{0};
		for (size_t i = 0; i < shown.text().size(); ++i)
			if (shown.text()[i] == U'\n') starts.push_back(i + 1);
		TEST_EXPECT(starts.size() == text->line_count());
		for (size_t line = 1; line <= starts.size() && line <= text->line_count(); ++line) {
			size_t offset = 0;
			TEST_EXPECT(text->offset_of(line, 1, offset) && shown.shown_at(offset) == starts[line - 1] &&
			            shown.document_offset(starts[line - 1]) == offset);
		}
		lines += text->line_count();
	}
	std::printf("retail: %zu scripts, %zu lines; %zu keywords, %zu commands, %zu operands highlighted\n", scripts, lines,
	            keywords, commands, operands);
	// The install's counts, pinned (Joint Operations: Combined Arms): its operands the references
	// editor_text_document's retail leg counts.
	TEST_EXPECT(scripts == 23 && lines == 793 && keywords == 287 && commands == 209 && operands == 36);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failures = 0;
	failures += test_kind_table();
	failures += test_span_diff();
	failures += test_several_places();
	failures += test_shown_text();
	failures += test_burst();
	failures += test_marks();
	failures += test_highlights();
	failures += test_reveal_after_hidden_byte();
	failures += test_session();
	failures += test_retail();
	if (failures == 0) std::printf("editor_script_viewport: all passed\n");
	return failures == 0 ? 0 : 1;
}
