/* MNS lossless document model tests (mns::Document, ADR 0014).

   The committed fixture fixtures/mns/menu_style.mns is the real shipped JO
   stylesheet, sourced byte-exact from the revx02 menu set (the same provenance
   as the fixtures/mnu menus; its 38-line comment header is NovaLogic's own
   format specification and the acid test for losslessness: CRLF line endings,
   no BOM, no final newline, tab-aligned defines, mixed-case values).

   Properties pinned here:
   - parse -> serialize is byte-identical for untouched documents (real file
     and synthetic edge cases);
   - flatten() reproduces the legacy mns::parse view exactly;
   - edits are minimal-delta (an edited value changes only its own line) and
     the collapse/append/escaping policies match ADR 0014;
   - diagnostics report the spec's error conditions without failing the parse.

   Runs from the repo root (ctest WORKING_DIRECTORY), like mnu_compat. */

#include "mns/mns.h"
#include "mns/mns_document.h"

#include "common/test_expect.h"

#include <cstring>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

const char *kRealFixture = "fixtures/mns/menu_style.mns";

bool read_file(const char *path, std::string &out) {
	std::ifstream f(path, std::ios::binary);
	if (!f) return false;
	std::ostringstream ss;
	ss << f.rdbuf();
	out = ss.str();
	return true;
}

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

int count_errors(const mns::Document &doc) {
	int n = 0;
	for (const auto &d : doc.diagnostics()) {
		if (d.severity == mns::Severity::Error) ++n;
	}
	return n;
}

bool has_diagnostic(const mns::Document &doc, const std::string &code) {
	for (const auto &d : doc.diagnostics()) {
		if (d.code == code) return true;
	}
	return false;
}

} // namespace

static int test_real_file_byte_roundtrip() {
	std::string src;
	TEST_EXPECT(read_file(kRealFixture, src));
	TEST_EXPECT(src.size() == 3761);

	mns::Document doc = mns::Document::parse(src);
	TEST_EXPECT(to_string(doc.serialize()) == src);
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
	TEST_EXPECT(read_file(kRealFixture, src));
	mns::StyleSheet sheet = mns::Document::parse(src).flatten();

	TEST_EXPECT(sheet.variables.size() == 12);
	TEST_EXPECT(sheet.get("DEF_FONTNAME") == "Gunpl22b.fnt");
	TEST_EXPECT(sheet.get("ITEM_SELECTED_BG") == "7f1E6DF3"); // value case kept
	TEST_EXPECT(sheet.get("DEF_IMAGE_DEFAULT_BG") == "JO_EPIL.TGA");
	TEST_EXPECT(sheet.get("SEMIOPAQUE_BLACK") == "2f000000");

	// Delegation guard: the legacy flat parse is the same view.
	mns::StyleSheet legacy;
	std::string error;
	TEST_EXPECT(mns::parse(src.data(), src.size(), legacy, error));
	TEST_EXPECT(legacy.variables == sheet.variables);

	std::printf("test_real_file_flatten passed\n");
	return 0;
}

static int test_real_file_entries() {
	std::string src;
	TEST_EXPECT(read_file(kRealFixture, src));
	mns::Document doc = mns::Document::parse(src);
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
		"FOO\n",                                       // bare name (empty value)
		"FOO bar \\\nbaz\n",                           // continuation
		"FOO bar \\ // after the backslash\n  baz\n",  // comment after continuation
		"FOO \\\n  value\n",                           // name-then-backslash
		"FOO a\\\\b\n",                                // "\\" escape
		"FOO x\\\\\\\n  y\n",                          // escape then continuation
		"FOO a\\ b\n",                                 // lone backslash (diagnostic)
		"#if 0\nA 1\n// inactive comment\n\n#if 1\nB 2\n#endif\n#endif\nC 3\n",
		"#if 1\nKEPT yes\n#else\nDROPPED no\n#endif\n",
		"#unknown directive\nFOO bar\n",
		"\xEF\xBB\xBF" "FOO bar\n",                    // BOM
		"   \nFOO bar\n\t\n",                          // whitespace-only lines
	};
	for (const char *src : cases) {
		const std::string text(src);
		mns::Document doc = mns::Document::parse(text);
		if (to_string(doc.serialize()) != text) {
			std::fprintf(stderr, "round-trip failed for: %s\n", src);
			return 1;
		}
	}

	std::printf("test_synthetic_byte_roundtrips passed\n");
	return 0;
}

static int test_inline_comment_ends_value() {
	mns::Document doc = mns::Document::parse(std::string("FOO bar // c\n"));
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
	// The editor-facing Entry unescapes "\\", but the retail evaluator copies
	// the authored pair intact after scanning over it.
	mns::Document doc = mns::Document::parse(std::string("FOO a\\\\b\n"));
	TEST_EXPECT(doc.flatten().get("FOO") == "a\\\\b");
	TEST_EXPECT(doc.entries()[0].value == "a\\b");
	TEST_EXPECT(doc.entries()[0].raw_value == "a\\\\b");
	TEST_EXPECT(doc.diagnostics().empty());

	// A bare backslash stays literal (legacy behavior) but is diagnosed.
	mns::Document lone = mns::Document::parse(std::string("FOO a\\ b\n"));
	TEST_EXPECT(lone.flatten().get("FOO") == "a\\ b");
	TEST_EXPECT(has_diagnostic(lone, "lone-backslash"));

	std::printf("test_escapes_flatten passed\n");
	return 0;
}

static int test_continuation_forms() {
	// Whitespace before the continuation backslash is part of the value.
	TEST_EXPECT(mns::Document::parse(std::string("FOO bar \\\nbaz\n")).flatten().get("FOO") == "bar baz");
	// Name followed by a backslash: value begins on the next line.
	TEST_EXPECT(mns::Document::parse(std::string("FOO \\\n  value\n")).flatten().get("FOO") == "value");
	// A continuation line that is only a comment ends the value.
	{
		mns::Document doc = mns::Document::parse(std::string("FOO bar \\\n// note\n"));
		TEST_EXPECT(doc.flatten().get("FOO") == "bar");
		const auto entries = doc.entries();
		TEST_EXPECT(entries.size() == 1);
		TEST_EXPECT(entries[0].multiline);
	}
	// Continuation at EOF: value ends, warning emitted.
	{
		mns::Document doc = mns::Document::parse(std::string("FOO bar \\"));
		TEST_EXPECT(doc.flatten().get("FOO") == "bar");
		TEST_EXPECT(has_diagnostic(doc, "continuation-at-eof"));
		TEST_EXPECT(to_string(doc.serialize()) == "FOO bar \\");
	}

	std::printf("test_continuation_forms passed\n");
	return 0;
}

static int test_conditionals_document() {
	// Inactive lines become InactiveText nodes; flatten excludes them.
	{
		const std::string src = "#if 0\nX 1\n#endif\nY 2\n";
		mns::Document doc = mns::Document::parse(src);
		TEST_EXPECT(doc.nodes().size() == 4);
		TEST_EXPECT(doc.nodes()[0].kind == mns::NodeKind::Directive);
		TEST_EXPECT(doc.nodes()[1].kind == mns::NodeKind::InactiveText);
		TEST_EXPECT(doc.nodes()[2].kind == mns::NodeKind::Directive);
		TEST_EXPECT(doc.nodes()[3].kind == mns::NodeKind::Define);
		TEST_EXPECT(!doc.flatten().has("X"));
		TEST_EXPECT(doc.flatten().get("Y") == "2");
		TEST_EXPECT(to_string(doc.serialize()) == src);
	}
	// Non-0/1 argument: truthy (legacy) + diagnostic.
	{
		mns::Document doc = mns::Document::parse(std::string("#if 2\nZ 3\n#endif\n"));
		TEST_EXPECT(doc.flatten().get("Z") == "3");
		TEST_EXPECT(has_diagnostic(doc, "noncanonical-if-arg"));
	}
	// Stray #endif / #else and an unterminated #if are diagnosed.
	TEST_EXPECT(has_diagnostic(mns::Document::parse(std::string("#endif\n")), "unbalanced-endif"));
	TEST_EXPECT(has_diagnostic(mns::Document::parse(std::string("#else\nW 4\n#endif\n")), "unbalanced-else"));
	{
		mns::Document doc = mns::Document::parse(std::string("#if 0\nX 1\n"));
		TEST_EXPECT(has_diagnostic(doc, "unterminated-if"));
		TEST_EXPECT(!doc.flatten().has("X"));
	}
	// A directive encountered while a continuation is pending remains a
	// directive; the value resumes on the next ordinary active line.
	{
		const std::string src = "#if 1\nFOO bar \\\n#endif\nbaz\nQUX 7\n";
		mns::Document doc = mns::Document::parse(src);
		TEST_EXPECT(doc.flatten().get("FOO") == "bar baz");
		TEST_EXPECT(doc.flatten().get("QUX") == "7");
		TEST_EXPECT(!has_diagnostic(doc, "unterminated-if"));
		TEST_EXPECT(to_string(doc.serialize()) == src);
		std::string error;
		TEST_EXPECT(doc.set_value("FOO", "new", &error));
		TEST_EXPECT(doc.flatten().get("FOO") == "new");
		TEST_EXPECT(doc.flatten().get("QUX") == "7");
		TEST_EXPECT(to_string(doc.serialize()).find("#endif\n") != std::string::npos);
		const std::string edited = to_string(doc.serialize());
		TEST_EXPECT(!doc.remove_define("FOO", &error));
		TEST_EXPECT(to_string(doc.serialize()) == edited);
		TEST_EXPECT(!doc.move_define("FOO", 0, &error));
		TEST_EXPECT(to_string(doc.serialize()) == edited);
	}
	// While inactive, the retail scanner seeks the next '#' byte rather than
	// requiring it to be line-leading.
	{
		const std::string src = "#if 0\nignored #else\nLIVE yes\n#endif\n";
		mns::Document doc = mns::Document::parse(src);
		TEST_EXPECT(doc.flatten().get("LIVE") == "yes");
		TEST_EXPECT(!has_diagnostic(doc, "unbalanced-else"));
		TEST_EXPECT(to_string(doc.serialize()) == src);
	}
	// A continuation can cross a false conditional: inactive ordinary source
	// does not join the value, and evaluation resumes after #endif.
	{
		const std::string src = "FOO a \\\n#if 0\nignored\n#endif\nb\n";
		mns::Document doc = mns::Document::parse(src);
		TEST_EXPECT(doc.flatten().get("FOO") == "a b");
		TEST_EXPECT(to_string(doc.serialize()) == src);
	}

	std::printf("test_conditionals_document passed\n");
	return 0;
}

static int test_duplicate_name_diagnostics() {
	{
		mns::Document doc = mns::Document::parse(std::string("FOO a\nfoo b\n"));
		TEST_EXPECT(has_diagnostic(doc, "duplicate-name"));
		TEST_EXPECT(doc.flatten().get("FOO") == "b"); // last wins
	}
	// The same name in mutually exclusive #if branches is not a duplicate.
	{
		mns::Document doc = mns::Document::parse(std::string("#if 0\nFOO a\n#else\nFOO b\n#endif\n"));
		TEST_EXPECT(!has_diagnostic(doc, "duplicate-name"));
		TEST_EXPECT(doc.flatten().get("FOO") == "b");
	}

	std::printf("test_duplicate_name_diagnostics passed\n");
	return 0;
}

static int test_invalid_name_diagnostic() {
	mns::Document doc = mns::Document::parse(std::string("A%B v\n"));
	TEST_EXPECT(has_diagnostic(doc, "invalid-name-char"));
	TEST_EXPECT(doc.flatten().get("A%B") == "v"); // still parsed, lenient

	TEST_EXPECT(mns::Document::is_valid_name("DEF_TEXT_FG"));
	TEST_EXPECT(!mns::Document::is_valid_name(""));
	TEST_EXPECT(!mns::Document::is_valid_name("A B"));
	TEST_EXPECT(!mns::Document::is_valid_name("A%B"));
	TEST_EXPECT(!mns::Document::is_valid_name("A/B"));
	TEST_EXPECT(!mns::Document::is_valid_name("A\\B"));
	TEST_EXPECT(!mns::Document::is_valid_name("A<B"));
	TEST_EXPECT(!mns::Document::is_valid_name("A>B"));
	TEST_EXPECT(!mns::Document::is_valid_name("A#B"));
	TEST_EXPECT(mns::Document::is_valid_name("A\"B")); // the spec's six exclude the quote

	std::printf("test_invalid_name_diagnostic passed\n");
	return 0;
}

static int test_edit_stability_set_value() {
	std::string src;
	TEST_EXPECT(read_file(kRealFixture, src));
	mns::Document doc = mns::Document::parse(src);

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
	mns::Document doc = mns::Document::parse(std::string("FOO bar\t// keep me\n"));
	std::string error;
	TEST_EXPECT(doc.set_value("FOO", "qux", &error));
	TEST_EXPECT(to_string(doc.serialize()) == "FOO qux\t// keep me\n");

	std::printf("test_edit_preserves_inline_comment passed\n");
	return 0;
}

static int test_edit_multiline_collapse() {
	// Collapsing keeps the first line's layout and coalesces every spanned
	// inline comment (content preserved, position approximated; ADR 0014).
	mns::Document doc = mns::Document::parse(std::string("FOO bar \\ // c1\n  baz // c2\n"));
	std::string error;
	TEST_EXPECT(doc.set_value("FOO", "new", &error));
	TEST_EXPECT(to_string(doc.serialize()) == "FOO new\t// c1 c2\n");
	TEST_EXPECT(doc.flatten().get("FOO") == "new");
	TEST_EXPECT(!doc.entries()[0].multiline);

	std::printf("test_edit_multiline_collapse passed\n");
	return 0;
}

static int test_add_rename_remove_move() {
	std::string error;

	// Append to a document without a final newline: the last line gains the
	// document EOL, then the new define lands on its own line.
	{
		mns::Document doc = mns::Document::parse(std::string("FOO bar"));
		TEST_EXPECT(doc.add_define("NEW", "value", -1, "", &error));
		TEST_EXPECT(to_string(doc.serialize()) == "FOO bar\r\nNEW\tvalue\r\n");
	}
	// Same shape on the real fixture (CRLF document EOL).
	{
		std::string src;
		TEST_EXPECT(read_file(kRealFixture, src));
		mns::Document doc = mns::Document::parse(src);
		TEST_EXPECT(doc.add_define("MY_COLOR", "FF102030", -1, "added by test", &error));
		TEST_EXPECT(to_string(doc.serialize()) ==
				src + "\r\n" + "MY_COLOR\tFF102030\t// added by test\r\n");
		TEST_EXPECT(doc.flatten().get("MY_COLOR") == "FF102030");
	}
	// Insert before a node (after an existing entry).
	{
		mns::Document doc = mns::Document::parse(std::string("A 1\nB 2\n"));
		const int a_node = doc.entries()[0].node_index;
		TEST_EXPECT(doc.add_define("MID", "x", a_node + 1, "", &error));
		TEST_EXPECT(to_string(doc.serialize()) == "A 1\nMID\tx\nB 2\n");
	}
	// Duplicate / invalid rejections leave the document untouched.
	{
		mns::Document doc = mns::Document::parse(std::string("FOO bar\n"));
		TEST_EXPECT(!doc.add_define("foo", "x", -1, "", &error));
		TEST_EXPECT(!doc.add_define("BAD NAME", "x", -1, "", &error));
		TEST_EXPECT(!doc.add_define("OK", "a // b", -1, "", &error));
		TEST_EXPECT(to_string(doc.serialize()) == "FOO bar\n");
	}
	// Rename keeps the value bytes and alignment; collisions reject;
	// case-only renames are allowed.
	{
		mns::Document doc = mns::Document::parse(std::string("FOO\t\tbar\nBAZ qux\n"));
		TEST_EXPECT(doc.rename_define("FOO", "QUX", &error));
		TEST_EXPECT(to_string(doc.serialize()) == "QUX\t\tbar\nBAZ qux\n");
		TEST_EXPECT(!doc.rename_define("QUX", "baz", &error));
		TEST_EXPECT(doc.rename_define("QUX", "Qux", &error));
		TEST_EXPECT(to_string(doc.serialize()) == "Qux\t\tbar\nBAZ qux\n");
		TEST_EXPECT(doc.flatten().has("QUX"));
	}
	// Remove drops the whole node (continuations included); comments above stay.
	{
		mns::Document doc = mns::Document::parse(std::string("// c\nFOO a \\\nb\nBAZ q\n"));
		TEST_EXPECT(doc.remove_define("FOO", &error));
		TEST_EXPECT(to_string(doc.serialize()) == "// c\nBAZ q\n");
		TEST_EXPECT(!doc.remove_define("FOO", &error));
	}
	// Remove under duplicates removes every active define with the name.
	{
		mns::Document doc = mns::Document::parse(std::string("FOO a\nFOO b\nBAR c\n"));
		TEST_EXPECT(doc.remove_define("FOO", &error));
		TEST_EXPECT(to_string(doc.serialize()) == "BAR c\n");
	}
	// Move reorders whole nodes.
	{
		mns::Document doc = mns::Document::parse(std::string("A 1\nB 2\nC 3\n"));
		TEST_EXPECT(doc.move_define("C", 0, &error));
		TEST_EXPECT(to_string(doc.serialize()) == "C 3\nA 1\nB 2\n");
		// Moving the no-final-newline last node terminates its line.
		mns::Document doc2 = mns::Document::parse(std::string("A 1\nB 2"));
		TEST_EXPECT(doc2.move_define("B", 0, &error));
		TEST_EXPECT(to_string(doc2.serialize()) == "B 2\nA 1\n");
	}

	std::printf("test_add_rename_remove_move passed\n");
	return 0;
}

static int test_set_inline_comment() {
	mns::Document doc = mns::Document::parse(std::string("FOO bar\n"));
	std::string error;
	TEST_EXPECT(doc.set_inline_comment("FOO", "the trim color", &error));
	TEST_EXPECT(to_string(doc.serialize()) == "FOO bar\t// the trim color\n");
	TEST_EXPECT(doc.entries()[0].inline_comment == "// the trim color");
	// Clearing removes the comment and its separator whitespace.
	TEST_EXPECT(doc.set_inline_comment("FOO", "", &error));
	TEST_EXPECT(to_string(doc.serialize()) == "FOO bar\n");
	// A "//"-prefixed comment is kept verbatim.
	TEST_EXPECT(doc.set_inline_comment("FOO", "//raw", &error));
	TEST_EXPECT(to_string(doc.serialize()) == "FOO bar\t//raw\n");

	std::printf("test_set_inline_comment passed\n");
	return 0;
}

static int test_value_validation() {
	mns::Document doc = mns::Document::parse(std::string("FOO bar\n"));
	std::string error;
	TEST_EXPECT(!doc.set_value("FOO", "a//b", &error));
	TEST_EXPECT(!doc.set_value("FOO", "a\nb", &error));
	TEST_EXPECT(!doc.set_value("MISSING", "x", &error));
	TEST_EXPECT(to_string(doc.serialize()) == "FOO bar\n");

	// Backslashes round-trip through the "\\" escape.
	TEST_EXPECT(doc.set_value("FOO", "a\\b", &error));
	TEST_EXPECT(to_string(doc.serialize()) == "FOO a\\\\b\n");
	TEST_EXPECT(doc.flatten().get("FOO") == "a\\\\b");

	// Leading/trailing whitespace is unrepresentable and gets trimmed.
	TEST_EXPECT(doc.set_value("FOO", "  x  ", &error));
	TEST_EXPECT(doc.flatten().get("FOO") == "x");

	TEST_EXPECT(mns::Document::is_valid_value("FF8000"));
	TEST_EXPECT(mns::Document::is_valid_value("a\\b"));
	TEST_EXPECT(!mns::Document::is_valid_value("a//b"));
	TEST_EXPECT(!mns::Document::is_valid_value("a\nb"));

	std::printf("test_value_validation passed\n");
	return 0;
}

static int test_source_text_get_set() {
	// BOM stickiness across source-text round-trips.
	{
		const std::string with_bom = std::string("\xEF\xBB\xBF") + "FOO bar\n";
		mns::Document doc = mns::Document::parse(with_bom);
		TEST_EXPECT(doc.has_bom());
		TEST_EXPECT(doc.source_text() == "FOO bar\n"); // BOM excluded from text
		doc.set_source_text("FOO baz\n");
		TEST_EXPECT(doc.has_bom()); // sticky
		TEST_EXPECT(to_string(doc.serialize()) == with_bom.substr(0, 3) + "FOO baz\n");
	}
	// get -> set -> serialize is byte-faithful on the real file.
	{
		std::string src;
		TEST_EXPECT(read_file(kRealFixture, src));
		mns::Document doc = mns::Document::parse(src);
		doc.set_source_text(doc.source_text());
		TEST_EXPECT(to_string(doc.serialize()) == src);
	}

	std::printf("test_source_text_get_set passed\n");
	return 0;
}

static int test_entries_groups_and_comments() {
	mns::Document doc = mns::Document::parse(
			std::string("// Fonts\nA 1\nB 2\n\n// Colors\nC 3\n"));
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

static bool result_has_diagnostic(const mns::EvaluationResult &result,
		const std::string &code) {
	for (const auto &d : result.diagnostics) {
		if (d.code == code) return true;
	}
	return false;
}

static int test_retail_evaluation_result() {
	// The retail loader checks the first character of the #if argument. It is
	// deliberately lenient after that character and reports the odd spelling.
	const std::string src =
			"#if 0foo\nZERO hidden\n#else\nZERO_ELSE kept\n#endif\n"
			"#if 1foo\nONE kept\n#endif\n"
			"#if 2\nTWO kept\n#endif\n"
			"Case first\nCASE last\n"
			"PATH c:\\\\games\\\\jo\n";
	const mns::Document doc = mns::Document::parse(src);
	const mns::EvaluationResult result = doc.evaluate();
	TEST_EXPECT(result.success);
	TEST_EXPECT(!result.sheet.has("ZERO"));
	TEST_EXPECT(result.sheet.get("ZERO_ELSE") == "kept");
	TEST_EXPECT(result.sheet.get("ONE") == "kept");
	TEST_EXPECT(result.sheet.get("TWO") == "kept");
	TEST_EXPECT(result.sheet.get("case") == "last");
	TEST_EXPECT(result.sheet.get("PATH") == "c:\\\\games\\\\jo");
	TEST_EXPECT(result_has_diagnostic(result, "noncanonical-if-arg"));
	TEST_EXPECT(result_has_diagnostic(result, "duplicate-name"));

	// A syntax error keeps a useful partial sheet, but success is false and the
	// legacy flat API now reports the failure instead of silently accepting it.
	const mns::Document invalid =
			mns::Document::parse(std::string("#else\nOK value\n"));
	const mns::EvaluationResult failed = invalid.evaluate();
	TEST_EXPECT(!failed.success);
	TEST_EXPECT(failed.sheet.get("OK") == "value");
	TEST_EXPECT(result_has_diagnostic(failed, "unbalanced-else"));
	mns::StyleSheet flat;
	std::string error;
	const char *invalid_text = "#else\nOK value\n";
	TEST_EXPECT(!mns::parse(invalid_text, std::strlen(invalid_text), flat, error));
	TEST_EXPECT(!error.empty());

	std::printf("test_retail_evaluation_result passed\n");
	return 0;
}

int main() {
	int failures = 0;
	failures += test_real_file_byte_roundtrip();
	failures += test_real_file_flatten();
	failures += test_real_file_entries();
	failures += test_synthetic_byte_roundtrips();
	failures += test_inline_comment_ends_value();
	failures += test_escapes_flatten();
	failures += test_continuation_forms();
	failures += test_conditionals_document();
	failures += test_duplicate_name_diagnostics();
	failures += test_invalid_name_diagnostic();
	failures += test_edit_stability_set_value();
	failures += test_edit_preserves_inline_comment();
	failures += test_edit_multiline_collapse();
	failures += test_add_rename_remove_move();
	failures += test_set_inline_comment();
	failures += test_value_validation();
	failures += test_source_text_get_set();
	failures += test_entries_groups_and_comments();
	failures += test_retail_evaluation_result();

	if (failures == 0) {
		std::printf("\nAll tests passed!\n");
	}
	return failures == 0 ? 0 : 1;
}
