/* MNS lossless document model tests (opennova::mns::Document, ADR 0014).

   The reference fixture OPENNOVA_JO_ASSETS/fixtures/mns/menu_style.mns is the
   real shipped JO stylesheet, byte-exact from the revx02 menu set (no retail
   fixture lives in the tree; its 38-line comment header is NovaLogic's own
   format specification and the acid test for losslessness: CRLF line endings,
   no BOM, no final newline, tab-aligned defines, mixed-case values).

   Properties pinned here:
   - parse -> serialize is byte-identical for untouched documents (real file
     and synthetic edge cases), and render_nodes is that serialization;
   - the lines are classified the way the game's reader walks them, so each
     define's game_value is what evaluate() (the reader itself) reads;
   - evaluate() keeps what the reader read before it stops, and says where;
   - edits are minimal-delta (an edited value changes only its own line) and
     verified: an edit the game would read otherwise is refused, the document
     left as it was (reread);
   - diagnostics name what the game does with each odd line.

   Runs from the repo root (ctest WORKING_DIRECTORY), like mnu_compat. */

#include <base/io/strutil.h>
#include <formats/mns/mns.h>
#include <formats/mns/mns_document.h>

#include "common/file_io.h"
#include "common/test_expect.h"

#include <cstring>
#include <cstdio>
#include <string>

#include "common/retail_paths.h"
#include <vector>

namespace {

// The shipped menu_style.mns from the reference fixture set; "" without
// OPENNOVA_JO_ASSETS, when the retail legs below are skipped (SKIP-LEG).
std::string g_real_fixture;
const char *kRealFixture = "";

using opennova::mns::Document;
using opennova::mns::NodeKind;

std::string to_string(const std::vector<uint8_t> &bytes) {
	return std::string(bytes.begin(), bytes.end());
}

// Split into lines WITHOUT their EOLs (both sides of a comparison go through
// the same splitter, so EOL bytes are covered by the whole-string checks).
std::vector<std::string> split_lines(const std::string &s) {
	std::vector<std::string> lines;
	std::string current;
	for (size_t i = 0; i < s.size(); ++i) {
		if (s[i] == '\r') {
			lines.push_back(current);
			current.clear();
			if (i + 1 < s.size() && s[i + 1] == '\n') ++i;
		} else if (s[i] == '\n') {
			lines.push_back(current);
			current.clear();
		} else {
			current += s[i];
		}
	}
	if (!current.empty()) lines.push_back(current);
	return lines;
}

bool has_diagnostic(const Document &doc, const std::string &code) {
	for (const auto &d : doc.diagnostics()) {
		if (diagnostic_code_token(d.code) == code) return true;
	}
	return false;
}

bool has_error(const Document &doc) {
	for (const auto &d : doc.diagnostics())
		if (d.severity == opennova::mns::Severity::Error) return true;
	return false;
}

std::vector<const opennova::mns::Node *> pointers(const Document &doc) {
	std::vector<const opennova::mns::Node *> out;
	for (const auto &node : doc.nodes()) out.push_back(&node);
	return out;
}

// The document's own reading of every define it keeps (the last of each name)
// agrees with the reader's: the line model and evaluate() say the same.
bool model_matches_reader(const Document &doc) {
	const opennova::mns::EvaluationResult evaluated = doc.evaluate();
	opennova::mns::StyleSheet model;
	for (const auto &node : doc.nodes()) {
		if (node.kind != NodeKind::Define) continue;
		model.variables[opennova::strutil::to_upper(node.define_lines.front().name)] = opennova::mns::game_value(node);
	}
	if (evaluated.sheet.variables != model.variables) {
		for (const auto &entry : model.variables)
			std::fprintf(stderr, "  model %s = '%s', reader '%s'\n", entry.first.c_str(), entry.second.c_str(),
			             evaluated.sheet.get(entry.first).c_str());
		return false;
	}
	return true;
}

} // namespace

static int test_real_file_byte_roundtrip() {
	std::string src;
	TEST_EXPECT(test_io::read_file_text(kRealFixture, src));
	TEST_EXPECT(src.size() == 3761);

	Document doc = Document::parse(src);
	TEST_EXPECT(to_string(doc.serialize()) == src);
	TEST_EXPECT(opennova::mns::render_nodes(pointers(doc), "\r\n") == src);
	TEST_EXPECT(!doc.has_bom());
	TEST_EXPECT(doc.default_eol() == "\r\n");
	TEST_EXPECT(doc.diagnostics().empty());
	// The file ends without a final newline; losslessness must keep that.
	TEST_EXPECT(src.back() != '\n' && src.back() != '\r');

	std::printf("test_real_file_byte_roundtrip passed\n");
	return 0;
}

static int test_real_file_flatten() {
	std::string src;
	TEST_EXPECT(test_io::read_file_text(kRealFixture, src));
	const Document doc = Document::parse(src);
	const opennova::mns::EvaluationResult evaluated = doc.evaluate();
	TEST_EXPECT(evaluated.success && evaluated.stopped_line == 0);
	const opennova::mns::StyleSheet &sheet = evaluated.sheet;

	TEST_EXPECT(sheet.variables.size() == 12);
	TEST_EXPECT(sheet.get("DEF_FONTNAME") == "Gunpl22b.fnt");
	TEST_EXPECT(sheet.get("ITEM_SELECTED_BG") == "7f1E6DF3"); // value case kept
	TEST_EXPECT(sheet.get("DEF_IMAGE_DEFAULT_BG") == "JO_EPIL.TGA");
	TEST_EXPECT(sheet.get("SEMIOPAQUE_BLACK") == "2f000000");
	TEST_EXPECT(model_matches_reader(doc));

	// Delegation guard: the flat parse is the same view.
	opennova::mns::StyleSheet legacy;
	std::string error;
	TEST_EXPECT(opennova::mns::parse(src.data(), src.size(), legacy, error));
	TEST_EXPECT(legacy.variables == sheet.variables);

	std::printf("test_real_file_flatten passed\n");
	return 0;
}

static int test_real_file_entries() {
	std::string src;
	TEST_EXPECT(test_io::read_file_text(kRealFixture, src));
	Document doc = Document::parse(src);
	const auto entries = doc.entries();

	TEST_EXPECT(entries.size() == 12);
	// Authored case and 1-based physical lines (38 header comments + blank).
	TEST_EXPECT(entries[0].name == "DEF_FONTNAME");
	TEST_EXPECT(entries[0].line == 40);
	TEST_EXPECT(entries[11].name == "DEF_IMAGE_DEFAULT_BG");
	TEST_EXPECT(entries[11].line == 55);
	// The header block is separated from the first define by a blank line,
	// so it is NOT the first entry's preceding comment.
	TEST_EXPECT(entries[0].preceding_comments.empty());
	// Five blank-separated groups: 3 fonts / 4 text colors / 2 trim / 2 black / 1 image.
	TEST_EXPECT(entries[0].group == 0 && entries[2].group == 0);
	TEST_EXPECT(entries[3].group == 1 && entries[6].group == 1);
	TEST_EXPECT(entries[7].group == 2 && entries[8].group == 2);
	TEST_EXPECT(entries[9].group == 3 && entries[10].group == 3);
	TEST_EXPECT(entries[11].group == 4);
	for (const auto &e : entries) {
		TEST_EXPECT(!e.multiline);
		TEST_EXPECT(e.inline_comment.empty());
	}
	TEST_EXPECT(doc.find_entry("trim_color") == 7); // case-insensitive lookup

	std::printf("test_real_file_entries passed\n");
	return 0;
}

static int test_synthetic_byte_roundtrips() {
	const char *cases[] = {
		"",
		"FOO bar",                                     // no final newline
		"FOO bar\n",
		"FOO bar\r\nBAZ qux\r\n",                      // CRLF
		"A 1\r\nB 2\nC 3\r",                           // mixed EOLs, lone CR
		"FOO bar // inline comment\n",
		"FOO bar   \n",                                // trailing value whitespace
		"  FOO bar\n",                                 // indented define
		"FOO\n",                                       // bare name (the reader stops)
		"FOO bar \\\nbaz\n",                           // continuation
		"FOO bar \\ // after the backslash\n  baz\n",  // comment after continuation
		"FOO \\\n  value\n",                           // name-then-backslash
		"FOO\\\r\nvalue\r\n",                          // the NAME\ form
		"FOO \t\r\n\r\n// c\r\nvalue\r\nBAR 1\r\n",    // the value on a later line
		"FOO a\\\\b\n",                                // "\\" escape
		"FOO x\\\\\\\n  y\n",                          // escape then continuation
		"FOO a\\ b\n",                                 // lone backslash (joins)
		"FOO a\\\r\n\r\n// c\r\nb\r\n",                // continuation across blank and comment
		"\\ FOO bar\r\n",                              // a backslash the reader steps over
		"#if 0\nA 1\n// inactive comment\n\n#if 1\nB 2\n#endif\n#endif\nC 3\n",
		"#if 0\r\nX y\r\n// #endif\r\n#endif\r\n",      // a seek reaches the comment's '#'
		"#if 1\nKEPT yes\n#else\nDROPPED no\n#endif\n",
		"#unknown directive\nFOO bar\n",
		"\xEF\xBB\xBF" "FOO bar\n",                    // BOM
		"   \nFOO bar\n\t\n",                          // whitespace-only lines
	};
	for (const char *src : cases) {
		const std::string text(src);
		Document doc = Document::parse(text);
		if (to_string(doc.serialize()) != text) {
			std::fprintf(stderr, "round-trip failed for: %s\n", src);
			return 1;
		}
		// The shared renderer is the serialization (the BOM aside).
		const std::string body = doc.has_bom() ? text.substr(3) : text;
		TEST_EXPECT(opennova::mns::render_nodes(pointers(doc), doc.default_eol()) == body);
		TEST_EXPECT(opennova::mns::reread(pointers(doc), doc.default_eol()) == opennova::mns::Reread::Same);
	}
	// The editor's line ends: every one CRLF, the missing final one still missing.
	{
		std::vector<opennova::mns::Node> nodes = Document::parse(std::string("A 1\nB 2\rC 3")).nodes();
		std::vector<const opennova::mns::Node *> ends;
		bool changed = false;
		for (opennova::mns::Node &node : nodes) {
			changed = opennova::mns::end_lines_with(node, "\r\n") || changed;
			ends.push_back(&node);
		}
		TEST_EXPECT(changed && opennova::mns::render_nodes(ends, "\r\n") == "A 1\r\nB 2\r\nC 3");
		TEST_EXPECT(!opennova::mns::end_lines_with(nodes.front(), "\r\n"));
	}

	std::printf("test_synthetic_byte_roundtrips passed\n");
	return 0;
}

static int test_inline_comment_ends_value() {
	Document doc = Document::parse(std::string("FOO bar // c\r\n"));
	const auto entries = doc.entries();
	TEST_EXPECT(entries.size() == 1);
	TEST_EXPECT(entries[0].value == "bar");
	TEST_EXPECT(entries[0].raw_value == "bar");
	TEST_EXPECT(entries[0].inline_comment == "// c");
	TEST_EXPECT(doc.flatten().get("FOO") == "bar");

	std::printf("test_inline_comment_ends_value passed\n");
	return 0;
}

static int test_escapes_flatten() {
	// The editor-facing Entry unescapes "\\", but the reader copies the authored pair
	// intact after scanning over it.
	Document doc = Document::parse(std::string("FOO a\\\\b\r\n"));
	TEST_EXPECT(doc.flatten().get("FOO") == "a\\\\b");
	TEST_EXPECT(doc.entries()[0].value == "a\\b");
	TEST_EXPECT(doc.entries()[0].raw_value == "a\\\\b");
	TEST_EXPECT(doc.diagnostics().empty());

	// A lone backslash ends the segment and the text after the blanks joins it
	// [orig: NapiConfigMap_ParseKeyValueBuffer @ 0x639d27]: "a\ b" reads "ab". Diagnosed, not an error.
	Document lone = Document::parse(std::string("FOO a\\ b\r\n"));
	TEST_EXPECT(lone.flatten().get("FOO") == "ab");
	TEST_EXPECT(opennova::mns::game_value(lone.nodes()[0]) == "ab");
	TEST_EXPECT(has_diagnostic(lone, "lone-backslash") && !has_error(lone));
	TEST_EXPECT(lone.evaluate().success);
	// The blanks before the backslash stay.
	TEST_EXPECT(Document::parse(std::string("FOO a \\ b\r\n")).flatten().get("FOO") == "a b");

	std::printf("test_escapes_flatten passed\n");
	return 0;
}

static int test_continuation_forms() {
	// Whitespace before the continuation backslash is part of the value.
	TEST_EXPECT(Document::parse(std::string("FOO bar \\\r\nbaz\r\n")).flatten().get("FOO") == "bar baz");
	// Name followed by a backslash: value begins on the next line.
	TEST_EXPECT(Document::parse(std::string("FOO \\\r\n  value\r\n")).flatten().get("FOO") == "value");
	// A comment line is stepped over: the value goes on after it.
	{
		Document doc = Document::parse(std::string("FOO bar \\\r\n// note\r\nbaz\r\nQUX 1\r\n"));
		TEST_EXPECT(doc.flatten().get("FOO") == "bar baz");
		TEST_EXPECT(doc.nodes().size() == 2 && doc.nodes()[0].define_lines.size() == 3);
		TEST_EXPECT(opennova::mns::crosses_structure(doc.nodes()[0]));
		TEST_EXPECT(model_matches_reader(doc));
	}
	// Continuation at EOF: the value is what came before the backslash, blanks and all.
	{
		Document doc = Document::parse(std::string("FOO bar \\"));
		TEST_EXPECT(doc.flatten().get("FOO") == "bar ");
		TEST_EXPECT(has_diagnostic(doc, "continuation-at-eof"));
		TEST_EXPECT(to_string(doc.serialize()) == "FOO bar \\");
	}

	std::printf("test_continuation_forms passed\n");
	return 0;
}

// The value forms the reader takes, each read by the document as by the reader
// (grill 2026-09-23 set E2).
static int test_value_forms() {
	// The NAME\ form (the shipped header documents it): the value on the next line.
	{
		Document doc = Document::parse(std::string("A\\\r\nval\r\nB 2\r\n"));
		TEST_EXPECT(doc.flatten().get("A") == "val" && doc.flatten().get("B") == "2");
		TEST_EXPECT(doc.nodes()[0].define_lines.size() == 2 && !has_diagnostic(doc, "value-on-next-line"));
		TEST_EXPECT(model_matches_reader(doc));
	}
	// A name with only blanks after it takes the next line as its value, even a line
	// that reads like a define of its own.
	{
		Document doc = Document::parse(std::string("A 1\r\nB \r\nD 3\r\n"));
		TEST_EXPECT(doc.flatten().get("B") == "D 3" && !doc.flatten().has("D"));
		TEST_EXPECT(has_diagnostic(doc, "value-on-next-line"));
		TEST_EXPECT(doc.nodes().size() == 2);
		TEST_EXPECT(model_matches_reader(doc));
	}
	// ... and at the end of the file the reader drops it.
	{
		Document doc = Document::parse(std::string("A 1\r\nB "));
		TEST_EXPECT(!doc.flatten().has("B") && has_diagnostic(doc, "no-value"));
		TEST_EXPECT(doc.evaluate().success);
	}
	// A value the file's end stops keeps its trailing blanks.
	{
		Document doc = Document::parse(std::string("A 1   "));
		TEST_EXPECT(doc.flatten().get("A") == "1   ");
		TEST_EXPECT(model_matches_reader(doc));
	}
	// A continuation crosses blank and comment lines.
	{
		Document doc = Document::parse(std::string("A x\\\r\n\r\n// note\r\ny\r\nB 2\r\n"));
		TEST_EXPECT(doc.flatten().get("A") == "xy");
		TEST_EXPECT(model_matches_reader(doc));
	}
	// A backslash at a line start is stepped over.
	{
		Document doc = Document::parse(std::string("\\ A 1\r\n"));
		TEST_EXPECT(doc.flatten().get("A") == "1" && doc.nodes()[0].kind == NodeKind::Define);
	}
	// A continued duplicate appends to the last new name instead: an error.
	{
		Document doc = Document::parse(std::string("A 1\r\nB 2\r\nA x\\\r\ny\r\n"));
		TEST_EXPECT(doc.flatten().get("B") == "2y" && doc.flatten().get("A") == "x");
		TEST_EXPECT(has_diagnostic(doc, "continued-duplicate") && has_error(doc));
	}

	std::printf("test_value_forms passed\n");
	return 0;
}

static int test_conditionals_document() {
	// Inactive lines become InactiveText nodes; the reader skips them.
	{
		const std::string src = "#if 0\r\nX 1\r\n#endif\r\nY 2\r\n";
		Document doc = Document::parse(src);
		TEST_EXPECT(doc.nodes().size() == 4);
		TEST_EXPECT(doc.nodes()[0].kind == NodeKind::Directive);
		TEST_EXPECT(doc.nodes()[1].kind == NodeKind::InactiveText);
		TEST_EXPECT(doc.nodes()[2].kind == NodeKind::Directive);
		TEST_EXPECT(doc.nodes()[3].kind == NodeKind::Define);
		TEST_EXPECT(!doc.flatten().has("X"));
		TEST_EXPECT(doc.flatten().get("Y") == "2");
		TEST_EXPECT(to_string(doc.serialize()) == src);
	}
	// Non-0/1 argument: truthy + diagnostic.
	{
		Document doc = Document::parse(std::string("#if 2\r\nZ 3\r\n#endif\r\n"));
		TEST_EXPECT(doc.flatten().get("Z") == "3");
		TEST_EXPECT(has_diagnostic(doc, "noncanonical-if-arg"));
	}
	// Stray #endif / #else and an unterminated #if are tolerated with the reader's
	// semantics, and diagnosed as warnings.
	TEST_EXPECT(has_diagnostic(Document::parse(std::string("#endif\r\n")), "unbalanced-endif"));
	{
		// A stray #else switches the lines after it off until the next #else or #endif.
		Document doc = Document::parse(std::string("A 1\r\n#else\r\nB 2\r\n#endif\r\nC 3\r\n"));
		TEST_EXPECT(has_diagnostic(doc, "unbalanced-else") && !has_error(doc));
		TEST_EXPECT(doc.nodes()[2].kind == NodeKind::InactiveText);
		TEST_EXPECT(!doc.flatten().has("B") && doc.flatten().get("C") == "3");
		TEST_EXPECT(doc.evaluate().success);
	}
	{
		// A second #else switches the lines back on.
		Document doc = Document::parse(std::string("#if 1\r\nA 1\r\n#else\r\nB 2\r\n#else\r\nC 3\r\n#endif\r\n"));
		TEST_EXPECT(has_diagnostic(doc, "duplicate-else"));
		TEST_EXPECT(doc.flatten().has("A") && !doc.flatten().has("B") && doc.flatten().has("C"));
	}
	{
		Document doc = Document::parse(std::string("#if 0\r\nX 1\r\n"));
		TEST_EXPECT(has_diagnostic(doc, "unterminated-if") && !has_error(doc));
		TEST_EXPECT(!doc.flatten().has("X"));
	}
	// A directive encountered while a continuation is pending remains a
	// directive; the value resumes on the next ordinary active line.
	{
		const std::string src = "#if 1\r\nFOO bar \\\r\n#endif\r\nbaz\r\nQUX 7\r\n";
		Document doc = Document::parse(src);
		TEST_EXPECT(doc.flatten().get("FOO") == "bar baz");
		TEST_EXPECT(doc.flatten().get("QUX") == "7");
		TEST_EXPECT(!has_diagnostic(doc, "unterminated-if"));
		TEST_EXPECT(to_string(doc.serialize()) == src);
		std::string error;
		TEST_EXPECT(doc.set_value("FOO", "new", &error));
		TEST_EXPECT(doc.flatten().get("FOO") == "new");
		TEST_EXPECT(doc.flatten().get("QUX") == "7");
		TEST_EXPECT(to_string(doc.serialize()).find("#endif\r\n") != std::string::npos);
		const std::string edited = to_string(doc.serialize());
		TEST_EXPECT(!doc.remove_define("FOO", &error));
		TEST_EXPECT(to_string(doc.serialize()) == edited);
		TEST_EXPECT(!doc.move_define("FOO", 0, &error));
		TEST_EXPECT(to_string(doc.serialize()) == edited);
	}
	// While inactive, the reader seeks the next '#' byte rather than requiring it to
	// be line-leading.
	{
		const std::string src = "#if 0\r\nignored #else\r\nLIVE yes\r\n#endif\r\n";
		Document doc = Document::parse(src);
		TEST_EXPECT(doc.flatten().get("LIVE") == "yes");
		TEST_EXPECT(!has_diagnostic(doc, "unbalanced-else"));
		TEST_EXPECT(to_string(doc.serialize()) == src);
	}
	// A "//" line right after a directive is a comment the reader skips; after an
	// ordinary switched-off line the seek finds the '#' inside it.
	{
		Document at_token = Document::parse(std::string("#if 0\r\n// #endif\r\nA 1\r\n#endif\r\nB 2\r\n"));
		TEST_EXPECT(at_token.nodes()[1].kind == NodeKind::Comment);
		TEST_EXPECT(!at_token.flatten().has("A") && at_token.flatten().has("B"));
		Document seeking = Document::parse(std::string("#if 0\r\nX y\r\n// #endif\r\nA 1\r\n#endif\r\nB 2\r\n"));
		TEST_EXPECT(seeking.nodes()[2].kind == NodeKind::Directive);
		TEST_EXPECT(seeking.flatten().has("A") && seeking.flatten().has("B"));
		TEST_EXPECT(model_matches_reader(at_token) && model_matches_reader(seeking));
	}
	// A continuation can cross a false conditional: inactive ordinary source
	// does not join the value, and evaluation resumes after #endif.
	{
		const std::string src = "FOO a \\\r\n#if 0\r\nignored\r\n#endif\r\nb\r\n";
		Document doc = Document::parse(src);
		TEST_EXPECT(doc.flatten().get("FOO") == "a b");
		TEST_EXPECT(to_string(doc.serialize()) == src);
	}

	std::printf("test_conditionals_document passed\n");
	return 0;
}

static int test_duplicate_name_diagnostics() {
	{
		Document doc = Document::parse(std::string("FOO a\r\nfoo b\r\n"));
		TEST_EXPECT(has_diagnostic(doc, "duplicate-name"));
		TEST_EXPECT(doc.flatten().get("FOO") == "b"); // last wins
		// The reader keeps the first spelling.
		opennova::mns::KeyValueList list;
		const std::string bytes = to_string(doc.serialize());
		opennova::mns::parse_key_value_buffer(bytes.data(), bytes.size(), list);
		TEST_EXPECT(list.nodes.size() == 1 && list.nodes[0].name == "FOO" && list.nodes[0].value == "b");
	}
	// The same name in mutually exclusive #if branches is not a duplicate.
	{
		Document doc = Document::parse(std::string("#if 0\r\nFOO a\r\n#else\r\nFOO b\r\n#endif\r\n"));
		TEST_EXPECT(!has_diagnostic(doc, "duplicate-name"));
		TEST_EXPECT(doc.flatten().get("FOO") == "b");
	}

	std::printf("test_duplicate_name_diagnostics passed\n");
	return 0;
}

static int test_invalid_name_diagnostic() {
	// The reader stops at a name that ends on one of % < > # / [orig: @ 0x639f32]; what
	// it read before stays.
	{
		const Document doc = Document::parse(std::string("OK 1\r\nA%B v\r\nC 3\r\n"));
		TEST_EXPECT(has_diagnostic(doc, "invalid-name-char"));
		const opennova::mns::EvaluationResult evaluated = doc.evaluate();
		TEST_EXPECT(!evaluated.success && !evaluated.hangs && evaluated.stopped_line == 2);
		TEST_EXPECT(evaluated.sheet.get("OK") == "1" && !evaluated.sheet.has("C"));
	}

	TEST_EXPECT(Document::is_valid_name("DEF_TEXT_FG"));
	TEST_EXPECT(!Document::is_valid_name(""));
	TEST_EXPECT(!Document::is_valid_name("A B"));
	TEST_EXPECT(!Document::is_valid_name("A%B"));
	TEST_EXPECT(!Document::is_valid_name("A/B"));
	TEST_EXPECT(!Document::is_valid_name("A\\B"));
	TEST_EXPECT(!Document::is_valid_name("A<B"));
	TEST_EXPECT(!Document::is_valid_name("A>B"));
	TEST_EXPECT(!Document::is_valid_name("A#B"));
	TEST_EXPECT(Document::is_valid_name("A\"B")); // the spec's six exclude the quote

	std::printf("test_invalid_name_diagnostic passed\n");
	return 0;
}

// What the game stops responding on (not ported; refused as errors) and the
// directive forms the document cannot show the way the reader takes them.
static int test_hang_and_form_errors() {
	struct Case { const char *src; const char *code; bool hangs; };
	const Case cases[] = {
		{"A 1\nB 2\n", "line-ending", true},
		{"A 1\r\n# a comment\r\n", "unknown-directive", true},
		{"A #FF0000\r\n", "value-starts-with-hash", true},
		{"#if 0\r\nX a#b\r\n#endif\r\n", "unknown-directive", true},
		{"#ifdef X\r\nA 1\r\n#endif\r\n", "directive-form", false},
		{"#if 1\r\nA 1\r\n#endif FOO\r\nB 2\r\n", "directive-tail", false},
		{"#if\r\nA 1\r\n", "if-without-argument", false},
	};
	for (const Case &c : cases) {
		const Document doc = Document::parse(std::string(c.src));
		if (!has_diagnostic(doc, c.code) || !has_error(doc)) {
			std::fprintf(stderr, "no %s error for: %s\n", c.code, c.src);
			return 1;
		}
		if (c.hangs) TEST_EXPECT(doc.evaluate().hangs);
	}
	// A value that starts with a directive's name is no hang: the reader runs the directive
	// and the next text it reads is the value (an error all the same: the lines are not
	// read as shown).
	{
		struct Directive { const char *src; const char *name; const char *value; };
		const Directive directives[] = {
			{"k #endif\r\nw\r\n", "K", "w"},
			{"k #if 1\r\nw\r\n#endif\r\n", "K", "w"},
			{"A x\\ #endif\r\nw\r\n", "A", "xw"},
		};
		for (const Directive &d : directives) {
			const Document doc = Document::parse(std::string(d.src));
			TEST_EXPECT(has_diagnostic(doc, "value-is-directive") && !has_diagnostic(doc, "value-starts-with-hash"));
			const opennova::mns::EvaluationResult evaluated = doc.evaluate();
			if (!evaluated.success || evaluated.hangs || evaluated.sheet.get(d.name) != d.value) {
				std::fprintf(stderr, "the reader reads %s as '%s' in: %s\n", d.name, evaluated.sheet.get(d.name).c_str(), d.src);
				return 1;
			}
		}
	}
	// The one diagnostic a stop the document does not explain gets.
	{
		const Document doc = Document::parse(std::string("A 1\r\n#if 1foo\r\nB 2\r\n"));
		const opennova::mns::EvaluationResult evaluated = doc.evaluate();
		TEST_EXPECT(!evaluated.success && evaluated.stopped_line == 2);
		TEST_EXPECT(evaluated.sheet.get("A") == "1" && !evaluated.sheet.has("B"));
	}

	std::printf("test_hang_and_form_errors passed\n");
	return 0;
}

static int test_edit_stability_set_value() {
	std::string src;
	TEST_EXPECT(test_io::read_file_text(kRealFixture, src));
	Document doc = Document::parse(src);

	std::string error;
	TEST_EXPECT(doc.set_value("DEF_TEXT_FG", "11223344", &error));
	const std::string out = to_string(doc.serialize());

	const auto before = split_lines(src);
	const auto after = split_lines(out);
	TEST_EXPECT(before.size() == after.size());
	int diffs = 0;
	size_t diff_index = 0;
	for (size_t i = 0; i < before.size(); ++i) {
		if (before[i] != after[i]) {
			++diffs;
			diff_index = i;
		}
	}
	TEST_EXPECT(diffs == 1);
	// Only the value text changed; the name and alignment tabs survive.
	std::string expected = before[diff_index];
	const size_t pos = expected.find("FFFFFFFF");
	TEST_EXPECT(pos != std::string::npos);
	expected.replace(pos, 8, "11223344");
	TEST_EXPECT(after[diff_index] == expected);
	TEST_EXPECT(doc.flatten().get("DEF_TEXT_FG") == "11223344");

	std::printf("test_edit_stability_set_value passed\n");
	return 0;
}

static int test_edit_preserves_inline_comment() {
	Document doc = Document::parse(std::string("FOO bar\t// keep me\r\n"));
	std::string error;
	TEST_EXPECT(doc.set_value("FOO", "qux", &error));
	TEST_EXPECT(to_string(doc.serialize()) == "FOO qux\t// keep me\r\n");

	std::printf("test_edit_preserves_inline_comment passed\n");
	return 0;
}

static int test_edit_multiline_collapse() {
	// Collapsing keeps the first line's layout and coalesces every spanned
	// inline comment (content preserved, position approximated; ADR 0014).
	Document doc = Document::parse(std::string("FOO bar \\ // c1\r\n  baz // c2\r\n"));
	std::string error;
	TEST_EXPECT(doc.set_value("FOO", "new", &error));
	TEST_EXPECT(to_string(doc.serialize()) == "FOO new\t// c1 c2\r\n");
	TEST_EXPECT(doc.flatten().get("FOO") == "new");
	TEST_EXPECT(!doc.entries()[0].multiline);
	// The NAME\ form collapses without its backslash.
	Document named = Document::parse(std::string("FOO\\\r\n  old\r\n"));
	TEST_EXPECT(named.set_value("FOO", "new", &error));
	TEST_EXPECT(to_string(named.serialize()) == "FOO\tnew\r\n");

	std::printf("test_edit_multiline_collapse passed\n");
	return 0;
}

static int test_add_rename_remove_move() {
	std::string error;

	// Append to a document without a final newline: the last line gains the
	// document EOL, then the new define lands on its own line.
	{
		Document doc = Document::parse(std::string("FOO bar"));
		TEST_EXPECT(doc.add_define("NEW", "value", -1, "", &error));
		TEST_EXPECT(to_string(doc.serialize()) == "FOO bar\r\nNEW\tvalue\r\n");
	}
	// Same shape on the real fixture (CRLF document EOL).
	if (!g_real_fixture.empty()) {
		std::string src;
		TEST_EXPECT(test_io::read_file_text(kRealFixture, src));
		Document doc = Document::parse(src);
		TEST_EXPECT(doc.add_define("MY_COLOR", "FF102030", -1, "added by test", &error));
		TEST_EXPECT(to_string(doc.serialize()) ==
				src + "\r\n" + "MY_COLOR\tFF102030\t// added by test\r\n");
		TEST_EXPECT(doc.flatten().get("MY_COLOR") == "FF102030");
	}
	// Insert before a node (after an existing entry); a new line is always CRLF.
	{
		Document doc = Document::parse(std::string("A 1\r\nB 2\r\n"));
		const int a_node = doc.entries()[0].node_index;
		TEST_EXPECT(doc.add_define("MID", "x", a_node + 1, "", &error));
		TEST_EXPECT(to_string(doc.serialize()) == "A 1\r\nMID\tx\r\nB 2\r\n");
	}
	// Duplicate / invalid rejections leave the document untouched.
	{
		Document doc = Document::parse(std::string("FOO bar\r\n"));
		TEST_EXPECT(!doc.add_define("foo", "x", -1, "", &error));
		TEST_EXPECT(!doc.add_define("BAD NAME", "x", -1, "", &error));
		TEST_EXPECT(!doc.add_define("OK", "a // b", -1, "", &error));
		TEST_EXPECT(!doc.add_define("OK", "", -1, "", &error));
		TEST_EXPECT(!doc.add_define("OK", "#FF0000", -1, "", &error));
		TEST_EXPECT(to_string(doc.serialize()) == "FOO bar\r\n");
	}
	// Rename keeps the value bytes and alignment; collisions reject;
	// case-only renames are allowed.
	{
		Document doc = Document::parse(std::string("FOO\t\tbar\r\nBAZ qux\r\n"));
		TEST_EXPECT(doc.rename_define("FOO", "QUX", &error));
		TEST_EXPECT(to_string(doc.serialize()) == "QUX\t\tbar\r\nBAZ qux\r\n");
		TEST_EXPECT(!doc.rename_define("QUX", "baz", &error));
		TEST_EXPECT(doc.rename_define("QUX", "Qux", &error));
		TEST_EXPECT(to_string(doc.serialize()) == "Qux\t\tbar\r\nBAZ qux\r\n");
		TEST_EXPECT(doc.flatten().has("QUX"));
	}
	// Remove drops the whole node (continuations included); comments above stay.
	{
		Document doc = Document::parse(std::string("// c\r\nFOO a \\\r\nb\r\nBAZ q\r\n"));
		TEST_EXPECT(doc.remove_define("FOO", &error));
		TEST_EXPECT(to_string(doc.serialize()) == "// c\r\nBAZ q\r\n");
		TEST_EXPECT(!doc.remove_define("FOO", &error));
	}
	// Move reorders whole nodes.
	{
		Document doc = Document::parse(std::string("A 1\r\nB 2\r\nC 3\r\n"));
		TEST_EXPECT(doc.move_define("C", 0, &error));
		TEST_EXPECT(to_string(doc.serialize()) == "C 3\r\nA 1\r\nB 2\r\n");
		// Moving the no-final-newline last node terminates its line.
		Document doc2 = Document::parse(std::string("A 1\r\nB 2"));
		TEST_EXPECT(doc2.move_define("B", 0, &error));
		TEST_EXPECT(to_string(doc2.serialize()) == "B 2\r\nA 1\r\n");
	}

	std::printf("test_add_rename_remove_move passed\n");
	return 0;
}

// The three edits the game would read differently are refused, the document left as
// it was.
static int test_edits_the_game_reads_otherwise() {
	std::string error;
	// 1. A define added or moved into a switched-off #if block.
	{
		const std::string src = "#if 0\r\nOLD 1\r\n#endif\r\nA 1\r\n";
		Document doc = Document::parse(src);
		TEST_EXPECT(!doc.add_define("NEW", "2", 1, "", &error));
		TEST_EXPECT(error.find("switched-off") != std::string::npos);
		TEST_EXPECT(!doc.move_define("A", 1, &error));
		TEST_EXPECT(to_string(doc.serialize()) == src);
	}
	// 2. A define added after a line whose value continues at the end of the file.
	{
		const std::string src = "FOO bar \\";
		Document doc = Document::parse(src);
		TEST_EXPECT(!doc.add_define("NEW", "value", -1, "", &error));
		TEST_EXPECT(error.find("continues") != std::string::npos);
		TEST_EXPECT(to_string(doc.serialize()) == src);
	}
	// 3. A name defined twice: the edits the game would read otherwise are refused; the
	// value and the comment edit the definition the game reads.
	{
		const std::string src = "FOO a\r\nBAR 1\r\nFOO b\r\n";
		Document doc = Document::parse(src);
		TEST_EXPECT(!doc.rename_define("FOO", "BAZ", &error));
		TEST_EXPECT(error.find("defined 2 times (lines 1, 3)") != std::string::npos);
		TEST_EXPECT(!doc.move_define("FOO", 0, &error));
		TEST_EXPECT(to_string(doc.serialize()) == src);
		TEST_EXPECT(doc.set_value("FOO", "c", &error));
		TEST_EXPECT(to_string(doc.serialize()) == "FOO a\r\nBAR 1\r\nFOO c\r\n");
		TEST_EXPECT(doc.set_inline_comment("FOO", "the one read", &error));
		TEST_EXPECT(to_string(doc.serialize()) == "FOO a\r\nBAR 1\r\nFOO c\t// the one read\r\n");
		TEST_EXPECT(doc.remove_define("FOO", &error));
		TEST_EXPECT(to_string(doc.serialize()) == "BAR 1\r\n");
	}
	// reread names what would change.
	{
		const Document doc = Document::parse(std::string("#if 0\r\nX 1\r\n#endif\r\n"));
		std::vector<opennova::mns::Node> nodes = doc.nodes();
		nodes.insert(nodes.begin() + 1, opennova::mns::make_define("NEW", "2", "", "\r\n"));
		std::vector<const opennova::mns::Node *> ptrs;
		for (const auto &node : nodes) ptrs.push_back(&node);
		size_t first = 0;
		TEST_EXPECT(opennova::mns::reread(ptrs, "\r\n", &first) == opennova::mns::Reread::Inactive && first == 1);
		const Document open_end = Document::parse(std::string("A x \\"));
		std::vector<opennova::mns::Node> after = open_end.nodes();
		after.push_back(opennova::mns::make_comment("note", "\r\n"));
		std::vector<const opennova::mns::Node *> after_ptrs;
		for (const auto &node : after) after_ptrs.push_back(&node);
		TEST_EXPECT(opennova::mns::reread(after_ptrs, "\r\n") == opennova::mns::Reread::Continued);
	}

	std::printf("test_edits_the_game_reads_otherwise passed\n");
	return 0;
}

static int test_set_inline_comment() {
	Document doc = Document::parse(std::string("FOO bar\r\n"));
	std::string error;
	TEST_EXPECT(doc.set_inline_comment("FOO", "the trim color", &error));
	TEST_EXPECT(to_string(doc.serialize()) == "FOO bar\t// the trim color\r\n");
	TEST_EXPECT(doc.entries()[0].inline_comment == "// the trim color");
	// Clearing removes the comment and its separator whitespace.
	TEST_EXPECT(doc.set_inline_comment("FOO", "", &error));
	TEST_EXPECT(to_string(doc.serialize()) == "FOO bar\r\n");
	// A "//"-prefixed comment is kept verbatim.
	TEST_EXPECT(doc.set_inline_comment("FOO", "//raw", &error));
	TEST_EXPECT(to_string(doc.serialize()) == "FOO bar\t//raw\r\n");

	std::printf("test_set_inline_comment passed\n");
	return 0;
}

static int test_value_validation() {
	Document doc = Document::parse(std::string("FOO bar\r\n"));
	std::string error;
	TEST_EXPECT(!doc.set_value("FOO", "a//b", &error));
	TEST_EXPECT(!doc.set_value("FOO", "a\nb", &error));
	TEST_EXPECT(!doc.set_value("FOO", "", &error));
	TEST_EXPECT(!doc.set_value("FOO", "#FF0000", &error));
	TEST_EXPECT(!doc.set_value("MISSING", "x", &error));
	TEST_EXPECT(to_string(doc.serialize()) == "FOO bar\r\n");

	// Backslashes round-trip through the "\\" escape.
	TEST_EXPECT(doc.set_value("FOO", "a\\b", &error));
	TEST_EXPECT(to_string(doc.serialize()) == "FOO a\\\\b\r\n");
	TEST_EXPECT(doc.flatten().get("FOO") == "a\\\\b");

	// Leading/trailing whitespace is unrepresentable and gets trimmed.
	TEST_EXPECT(doc.set_value("FOO", "  x  ", &error));
	TEST_EXPECT(doc.flatten().get("FOO") == "x");

	TEST_EXPECT(Document::is_valid_value("FF8000"));
	TEST_EXPECT(Document::is_valid_value("a\\b"));
	TEST_EXPECT(!Document::is_valid_value("a//b"));
	TEST_EXPECT(!Document::is_valid_value("a\nb"));
	TEST_EXPECT(!Document::is_valid_value(""));
	TEST_EXPECT(!Document::is_valid_value("#FF8000"));
	// The authored form the editor stores: pairs only, no lone backslash.
	TEST_EXPECT(opennova::mns::is_valid_game_value("c:\\\\games"));
	TEST_EXPECT(!opennova::mns::is_valid_game_value("a\\b"));
	TEST_EXPECT(!opennova::mns::is_valid_game_value(""));
	TEST_EXPECT(!opennova::mns::is_valid_game_value("#FF"));

	std::printf("test_value_validation passed\n");
	return 0;
}

static int test_source_text_get_set() {
	// BOM stickiness across source-text round-trips.
	{
		const std::string with_bom = std::string("\xEF\xBB\xBF") + "FOO bar\r\n";
		Document doc = Document::parse(with_bom);
		TEST_EXPECT(doc.has_bom());
		TEST_EXPECT(doc.source_text() == "FOO bar\r\n"); // BOM excluded from text
		doc.set_source_text("FOO baz\r\n");
		TEST_EXPECT(doc.has_bom()); // sticky
		TEST_EXPECT(to_string(doc.serialize()) == with_bom.substr(0, 3) + "FOO baz\r\n");
		TEST_EXPECT(doc.flatten().get("FOO") == "baz"); // the reader skips the BOM
	}
	// get -> set -> serialize is byte-faithful on the real file.
	if (!g_real_fixture.empty()) {
		std::string src;
		TEST_EXPECT(test_io::read_file_text(kRealFixture, src));
		Document doc = Document::parse(src);
		doc.set_source_text(doc.source_text());
		TEST_EXPECT(to_string(doc.serialize()) == src);
	}

	std::printf("test_source_text_get_set passed\n");
	return 0;
}

static int test_entries_groups_and_comments() {
	Document doc = Document::parse(
			std::string("// Fonts\r\nA 1\r\nB 2\r\n\r\n// Colors\r\nC 3\r\n"));
	const auto entries = doc.entries();
	TEST_EXPECT(entries.size() == 3);
	TEST_EXPECT(entries[0].group == 0);
	TEST_EXPECT(entries[0].preceding_comments.size() == 1);
	TEST_EXPECT(entries[0].preceding_comments[0] == "// Fonts");
	TEST_EXPECT(entries[1].group == 0);
	TEST_EXPECT(entries[1].preceding_comments.empty());
	TEST_EXPECT(entries[2].group == 1);
	TEST_EXPECT(entries[2].preceding_comments.size() == 1);
	TEST_EXPECT(entries[2].preceding_comments[0] == "// Colors");

	std::printf("test_entries_groups_and_comments passed\n");
	return 0;
}

// What a diagnostic says is a code (DiagnosticCode), each with its own stable token, lower case
// and hyphenated, which the Godot binding and the editor read (the editor's stylesheet type keys
// a finding's row by the code).
static int test_diagnostic_code_tokens() {
	using opennova::mns::DiagnosticCode;
	std::vector<std::string> tokens;
	for (size_t i = 0; i < opennova::mns::kDiagnosticCodeCount; ++i) {
		const std::string token = diagnostic_code_token(static_cast<DiagnosticCode>(i));
		bool formed = !token.empty() && token.front() != '-' && token.back() != '-';
		for (const char c : token) formed = formed && ((c >= 'a' && c <= 'z') || c == '-');
		TEST_EXPECT(formed);
		for (const std::string &before : tokens) TEST_EXPECT(before != token);
		tokens.push_back(token);
	}
	TEST_EXPECT(tokens.size() == 23);
	TEST_EXPECT(std::string(diagnostic_code_token(DiagnosticCode::LineEnding)) == "line-ending" &&
	            std::string(diagnostic_code_token(DiagnosticCode::DuplicateName)) == "duplicate-name" &&
	            std::string(diagnostic_code_token(DiagnosticCode::ContinuationAtEof)) == "continuation-at-eof" &&
	            std::string(diagnostic_code_token(DiagnosticCode::Hangs)) == "hangs" &&
	            std::string(diagnostic_code_token(DiagnosticCode::Stops)) == "stops");
	std::printf("test_diagnostic_code_tokens passed\n");
	return 0;
}

static bool result_has_diagnostic(const opennova::mns::EvaluationResult &result,
		const std::string &code) {
	for (const auto &d : result.diagnostics) {
		if (diagnostic_code_token(d.code) == code) return true;
	}
	return false;
}

static int test_retail_evaluation_result() {
	// The reader checks the first character of the #if argument and consumes it; the
	// rest of a directive's line is read on: "#if 1foo" reads "foo" as a name that
	// ends on its line end, where the reader stops (grill 2026-09-23, E2).
	{
		const std::string src =
				"#if 0foo\r\nZERO hidden\r\n#else\r\nZERO_ELSE kept\r\n#endif\r\n"
				"#if 1foo\r\nONE kept\r\n#endif\r\n";
		const Document doc = Document::parse(src);
		const opennova::mns::EvaluationResult result = doc.evaluate();
		TEST_EXPECT(!result.success && result.stopped_line == 6);
		TEST_EXPECT(result.sheet.variables.size() == 1 && result.sheet.get("ZERO_ELSE") == "kept");
		TEST_EXPECT(result_has_diagnostic(result, "directive-tail"));
	}
	{
		const std::string src =
				"#if 2\r\nTWO kept\r\n#endif\r\n"
				"Case first\r\nCASE last\r\n"
				"PATH c:\\\\games\\\\jo\r\n";
		const Document doc = Document::parse(src);
		const opennova::mns::EvaluationResult result = doc.evaluate();
		TEST_EXPECT(result.success);
		TEST_EXPECT(result.sheet.get("TWO") == "kept");
		TEST_EXPECT(result.sheet.get("case") == "last");
		TEST_EXPECT(result.sheet.get("PATH") == "c:\\\\games\\\\jo");
		TEST_EXPECT(result_has_diagnostic(result, "noncanonical-if-arg"));
		TEST_EXPECT(result_has_diagnostic(result, "duplicate-name"));
	}
	// A stray #else is no failure: the reader switches the lines after it off and reads
	// the sheet to its end.
	{
		const Document stray = Document::parse(std::string("#else\r\nOK value\r\n"));
		const opennova::mns::EvaluationResult result = stray.evaluate();
		TEST_EXPECT(result.success && !result.sheet.has("OK"));
		TEST_EXPECT(result_has_diagnostic(result, "unbalanced-else"));
		opennova::mns::StyleSheet flat;
		std::string error;
		const char *text = "#else\r\nOK value\r\n";
		TEST_EXPECT(opennova::mns::parse(text, std::strlen(text), flat, error) && error.empty());
	}

	std::printf("test_retail_evaluation_result passed\n");
	return 0;
}

int main(int argc, char **argv) {
    retail::configure_mixed(argc, argv);
	int failures = 0;
	g_real_fixture = retail::reference_fixture("mns/menu_style.mns");
	kRealFixture = g_real_fixture.c_str();
	const bool retail_leg = !g_real_fixture.empty();
	if (retail_leg) {
		failures += test_real_file_byte_roundtrip();
		failures += test_real_file_flatten();
		failures += test_real_file_entries();
	}
	failures += test_synthetic_byte_roundtrips();
	failures += test_inline_comment_ends_value();
	failures += test_escapes_flatten();
	failures += test_continuation_forms();
	failures += test_value_forms();
	failures += test_conditionals_document();
	failures += test_duplicate_name_diagnostics();
	failures += test_invalid_name_diagnostic();
	failures += test_hang_and_form_errors();
	if (retail_leg) failures += test_edit_stability_set_value();
	failures += test_edit_preserves_inline_comment();
	failures += test_edit_multiline_collapse();
	failures += test_add_rename_remove_move();
	failures += test_edits_the_game_reads_otherwise();
	failures += test_set_inline_comment();
	failures += test_value_validation();
	failures += test_source_text_get_set();
	failures += test_entries_groups_and_comments();
	failures += test_diagnostic_code_tokens();
	failures += test_retail_evaluation_result();

	if (failures == 0) {
		std::printf("\nAll tests passed!\n");
	}
	if (failures == 0 && !retail_leg)
		return retail::skip_leg("OPENNOVA_JO_ASSETS/fixtures/mns/menu_style.mns (the shipped style sheet)");
	return failures == 0 ? 0 : 1;
}
