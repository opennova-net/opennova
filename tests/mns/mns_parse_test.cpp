// Tests for the MNS stylesheet flat parse view (opennova::mns::parse -> StyleSheet).
// Expectations are TEST_EXPECT (real assertions under the Release ctest
// config, where assert() would compile away).

#include <formats/mns/mns.h>

#include "common/test_expect.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int test_empty() {
	opennova::mns::StyleSheet sheet;
	std::string error;

	TEST_EXPECT(opennova::mns::parse("", 0, sheet, error));
	TEST_EXPECT(sheet.variables.empty());

	std::printf("test_empty passed\n");
	return 0;
}

static int test_simple_variables() {
	opennova::mns::StyleSheet sheet;
	std::string error;

	const char *data = "FOO bar\r\nBAZ qux\r\n";
	TEST_EXPECT(opennova::mns::parse(data, strlen(data), sheet, error));
	TEST_EXPECT(sheet.variables.size() == 2);
	TEST_EXPECT(sheet.get("foo") == "bar");
	TEST_EXPECT(sheet.get("FOO") == "bar");
	TEST_EXPECT(sheet.get("baz") == "qux");
	TEST_EXPECT(sheet.get("BAZ") == "qux");

	std::printf("test_simple_variables passed\n");
	return 0;
}

static int test_tab_separator() {
	opennova::mns::StyleSheet sheet;
	std::string error;

	const char *data = "DEF_FONTNAME\tGunpl22b.fnt\r\n";
	TEST_EXPECT(opennova::mns::parse(data, strlen(data), sheet, error));
	TEST_EXPECT(sheet.get("DEF_FONTNAME") == "Gunpl22b.fnt");

	std::printf("test_tab_separator passed\n");
	return 0;
}

static int test_utf8_bom() {
	opennova::mns::StyleSheet sheet;
	std::string error;

	// UTF-8 BOM + "FOO bar\r\n". Split the literal so MSVC does not fold the
	// trailing 'F' into the \xBF hex escape ("\xBFF" is out of range).
	const char data[] = "\xEF\xBB\xBF" "FOO bar\r\n";
	TEST_EXPECT(opennova::mns::parse(data, sizeof(data) - 1, sheet, error));
	TEST_EXPECT(sheet.get("FOO") == "bar");

	std::printf("test_utf8_bom passed\n");
	return 0;
}

static int test_comment() {
	opennova::mns::StyleSheet sheet;
	std::string error;

	const char *data = "// This is a comment\r\nFOO bar\r\n// Another comment\r\nBAZ qux\r\n";
	TEST_EXPECT(opennova::mns::parse(data, strlen(data), sheet, error));
	TEST_EXPECT(sheet.variables.size() == 2);
	TEST_EXPECT(sheet.get("FOO") == "bar");
	TEST_EXPECT(sheet.get("BAZ") == "qux");

	std::printf("test_comment passed\n");
	return 0;
}

static int test_if_0() {
	opennova::mns::StyleSheet sheet;
	std::string error;

	const char *data = "BEFORE yes\r\n#if 0\r\nSKIPPED no\r\n#endif\r\nAFTER yes\r\n";
	TEST_EXPECT(opennova::mns::parse(data, strlen(data), sheet, error));
	TEST_EXPECT(sheet.variables.size() == 2);
	TEST_EXPECT(sheet.get("BEFORE") == "yes");
	TEST_EXPECT(sheet.get("AFTER") == "yes");
	TEST_EXPECT(!sheet.has("SKIPPED"));

	std::printf("test_if_0 passed\n");
	return 0;
}

static int test_if_1() {
	opennova::mns::StyleSheet sheet;
	std::string error;

	const char *data = "BEFORE yes\r\n#if 1\r\nINCLUDED yes\r\n#endif\r\nAFTER yes\r\n";
	TEST_EXPECT(opennova::mns::parse(data, strlen(data), sheet, error));
	TEST_EXPECT(sheet.variables.size() == 3);
	TEST_EXPECT(sheet.get("BEFORE") == "yes");
	TEST_EXPECT(sheet.get("INCLUDED") == "yes");
	TEST_EXPECT(sheet.get("AFTER") == "yes");

	std::printf("test_if_1 passed\n");
	return 0;
}

static int test_if_0_else() {
	opennova::mns::StyleSheet sheet;
	std::string error;

	const char *data = "#if 0\r\nSKIPPED no\r\n#else\r\nINCLUDED yes\r\n#endif\r\n";
	TEST_EXPECT(opennova::mns::parse(data, strlen(data), sheet, error));
	TEST_EXPECT(sheet.variables.size() == 1);
	TEST_EXPECT(sheet.get("INCLUDED") == "yes");
	TEST_EXPECT(!sheet.has("SKIPPED"));

	std::printf("test_if_0_else passed\n");
	return 0;
}

static int test_if_1_else() {
	opennova::mns::StyleSheet sheet;
	std::string error;

	const char *data = "#if 1\r\nINCLUDED yes\r\n#else\r\nSKIPPED no\r\n#endif\r\n";
	TEST_EXPECT(opennova::mns::parse(data, strlen(data), sheet, error));
	TEST_EXPECT(sheet.variables.size() == 1);
	TEST_EXPECT(sheet.get("INCLUDED") == "yes");
	TEST_EXPECT(!sheet.has("SKIPPED"));

	std::printf("test_if_1_else passed\n");
	return 0;
}

static int test_nested_if() {
	opennova::mns::StyleSheet sheet;
	std::string error;

	const char *data = "#if 0\r\nSKIPPED1 no\r\n#if 1\r\nSKIPPED2 no\r\n#endif\r\n#endif\r\nAFTER yes\r\n";
	TEST_EXPECT(opennova::mns::parse(data, strlen(data), sheet, error));
	TEST_EXPECT(sheet.variables.size() == 1);
	TEST_EXPECT(sheet.get("AFTER") == "yes");
	TEST_EXPECT(!sheet.has("SKIPPED1"));
	TEST_EXPECT(!sheet.has("SKIPPED2"));

	std::printf("test_nested_if passed\n");
	return 0;
}

static int test_line_continuation() {
	opennova::mns::StyleSheet sheet;
	std::string error;

	const char *data = "FOO bar \\\r\nbaz\r\n";
	TEST_EXPECT(opennova::mns::parse(data, strlen(data), sheet, error));
	TEST_EXPECT(sheet.get("FOO") == "bar baz");

	std::printf("test_line_continuation passed\n");
	return 0;
}

static int test_missing_value_delimiter_fails() {
	opennova::mns::StyleSheet sheet;
	std::string error;
	const char *data = "A 1\r\nFOO\r\nB 2\r\n";
	TEST_EXPECT(!opennova::mns::parse(data, strlen(data), sheet, error));
	TEST_EXPECT(error.find("line 2") == 0 && error.find("stops reading") != std::string::npos);
	// The game keeps what it read before the name that ends on its line end.
	TEST_EXPECT(sheet.get("A") == "1" && !sheet.has("B"));

	std::printf("test_missing_value_delimiter_fails passed\n");
	return 0;
}

// The game stops responding on a value line ended by a lone LF (or CR) that text
// follows [orig: NapiConfigMap_ParseKeyValueBuffer @ 0x639c3b]: the port stops there too.
static int test_lf_line_ends_stop() {
	opennova::mns::StyleSheet sheet;
	std::string error;
	const char *data = "FOO bar\nBAZ qux\n"; // lf
	TEST_EXPECT(!opennova::mns::parse(data, strlen(data), sheet, error));
	TEST_EXPECT(error.find("line 1") == 0 && error.find("LF") != std::string::npos);
	TEST_EXPECT(sheet.get("FOO") == "bar" && !sheet.has("BAZ"));

	std::printf("test_lf_line_ends_stop passed\n");
	return 0;
}

// The reader, case by case, against the outcomes the grill's emulator of the retail
// disassembly gives (2026-09-23, set E2; docs/mnu/menu-re.md "The stylesheet reader"):
// what it stops on, what it reads, and the list order and spelling it leaves.
static int test_retail_reader_cases() {
	using opennova::mns::KeyValue;
	using opennova::mns::KeyValueList;
	using opennova::mns::ReadStatus;
	struct Case {
		const char *label;
		std::string text;
		std::vector<KeyValue> before; // the list the reader appends to, in creation order
		ReadStatus status;
		std::vector<KeyValue> after;
	};
	const Case cases[] = {
		{"crlf basic", std::string("A 1\r\nB 2\r\n", 10), {}, ReadStatus::Read, {{"A", "1"}, {"B", "2"}}},
		{"LF-only basic", std::string("A 1\nB 2\n", 8), {}, ReadStatus::Hangs, {{"A", "1"}}},
		{"LF-only blank-separated", std::string("A 1\n\nB 2\n", 9), {}, ReadStatus::Read, {{"A", "1"}, {"B", "2"}}},
		{"LF-only indented next", std::string("A 1\n  B 2\n", 10), {}, ReadStatus::Hangs, {{"A", "1"}}},
		{"CR-only", std::string("A 1\rB 2\r", 8), {}, ReadStatus::Hangs, {{"A", "1"}}},
		{"dup key last wins", std::string("A 1\r\na 2\r\n", 10), {}, ReadStatus::Read, {{"A", "2"}}},
		{"invalid name % mid", std::string("A 1\r\nB%C 2\r\nD 3\r\n", 17), {}, ReadStatus::Failed, {{"A", "1"}}},
		{"name then CRLF", std::string("A 1\r\nB\r\nD 3\r\n", 13), {}, ReadStatus::Failed, {{"A", "1"}}},
		{"name at EOF", std::string("A 1\r\nB", 6), {}, ReadStatus::Failed, {{"A", "1"}}},
		{"name space at EOF", std::string("A 1\r\nB ", 7), {}, ReadStatus::Read, {{"A", "1"}}},
		{"name space then CRLF", std::string("A 1\r\nB \r\nD 3\r\n", 14), {}, ReadStatus::Read, {{"A", "1"}, {"B", "D 3"}}},
		{"NAME backslash newline value", std::string("A\\\r\nval\r\nB 2\r\n", 14), {}, ReadStatus::Read, {{"A", "val"}, {"B", "2"}}},
		{"lone backslash a\\ b", std::string("FOO a\\ b\r\nX 1\r\n", 15), {}, ReadStatus::Read, {{"FOO", "ab"}, {"X", "1"}}},
		{"a \\ b", std::string("FOO a \\ b\r\n", 11), {}, ReadStatus::Read, {{"FOO", "a b"}}},
		{"double backslash", std::string("FOO a\\\\b\r\n", 10), {}, ReadStatus::Read, {{"FOO", "a\\\\b"}}},
		{"value starting with #", std::string("A #FF\r\n", 7), {}, ReadStatus::Hangs, {}},
		{"value starting //", std::string("A //c\r\nv\r\n", 10), {}, ReadStatus::Read, {{"A", "v"}}},
		{"unknown #define", std::string("#define X\r\nA 1\r\n", 16), {}, ReadStatus::Hangs, {}},
		{"# comment", std::string("A 1\r\n# hi\r\n", 11), {}, ReadStatus::Hangs, {{"A", "1"}}},
		{"lone # at EOF", std::string("A 1\r\n#", 6), {}, ReadStatus::Hangs, {{"A", "1"}}},
		{"dup else", std::string("#if 1\r\nA 1\r\n#else\r\nB 2\r\n#else\r\nC 3\r\n#endif\r\n", 44), {}, ReadStatus::Read, {{"A", "1"}, {"C", "3"}}},
		{"stray else depth0", std::string("A 1\r\n#else\r\nB 2\r\nC 3\r\n", 22), {}, ReadStatus::Read, {{"A", "1"}}},
		{"stray else then endif", std::string("A 1\r\n#else\r\nB 2\r\n#endif\r\nC 3\r\n", 30), {}, ReadStatus::Read, {{"A", "1"}, {"C", "3"}}},
		{"stray endif", std::string("A 1\r\n#endif\r\nB 2\r\n", 18), {}, ReadStatus::Read, {{"A", "1"}, {"B", "2"}}},
		{"2 stray endif", std::string("A 1\r\n#endif\r\n#endif\r\nB 2\r\n", 26), {}, ReadStatus::Read, {{"A", "1"}}},
		{"unterminated if", std::string("#if 1\r\nA 1\r\n", 12), {}, ReadStatus::Read, {{"A", "1"}}},
		{"if 0 basic", std::string("#if 0\r\nA 1\r\n#else\r\nB 2\r\n#endif\r\nC 3\r\n", 37), {}, ReadStatus::Read, {{"B", "2"}, {"C", "3"}}},
		{"if0 comment endif at token pos", std::string("#if 0\r\n// #endif\r\nA 1\r\n#endif\r\nB 2\r\n", 36), {}, ReadStatus::Read, {{"B", "2"}}},
		{"if0 comment endif after text", std::string("#if 0\r\nX y\r\n// #endif\r\nA 1\r\n#endif\r\nB 2\r\n", 41), {}, ReadStatus::Read, {{"A", "1"}, {"B", "2"}}},
		{"if0 value with #", std::string("#if 0\r\nX a#b\r\n#endif\r\nB 2\r\n", 27), {}, ReadStatus::Hangs, {}},
		{"ifdef", std::string("#ifdef X\r\nA 1\r\n#endif\r\n", 23), {}, ReadStatus::Read, {{"f", "X"}, {"A", "1"}}},
		{"endif trailing text", std::string("#if 1\r\nA 1\r\n#endif FOO\r\nB 2\r\n", 29), {}, ReadStatus::Failed, {{"A", "1"}}},
		{"continuation after dup", std::string("A 1\r\nB 2\r\nA x\\\r\ny\r\n", 19), {}, ReadStatus::Read, {{"A", "x"}, {"B", "2y"}}},
		{"cont on first-of-append", std::string("A x\\\r\ny\r\n", 9), {{"Z", "z"}, {"A", "old"}}, ReadStatus::Read, {{"Z", "z"}, {"A", "xy"}}},
		{"value trailing ws at EOF", std::string("A 1   ", 6), {}, ReadStatus::Read, {{"A", "1   "}}},
		{"value w/ single slash", std::string("A a/b\r\n", 7), {}, ReadStatus::Read, {{"A", "a/b"}}},
		{"value inline //", std::string("A a // c\r\nB 2\r\n", 15), {}, ReadStatus::Read, {{"A", "a"}, {"B", "2"}}},
		{"value < > %", std::string("A <b>%c%\r\n", 10), {}, ReadStatus::Read, {{"A", "<b>%c%"}}},
		{"if 0 then key w/o ws", std::string("#if 0\r\nA 1\r\n#endif\r\nB\r\n", 23), {}, ReadStatus::Failed, {}},
		{"empty line only", std::string("\r\n\r\n", 4), {}, ReadStatus::Read, {}},
		{"key w/ tab sep", std::string("A\t1\r\n", 5), {}, ReadStatus::Read, {{"A", "1"}}},
		{"nested if0 in if1", std::string("#if 1\r\n#if 0\r\nA 1\r\n#endif\r\nB 2\r\n#endif\r\n", 40), {}, ReadStatus::Read, {{"B", "2"}}},
		{"if X (non 0/1)", std::string("#if X\r\nA 1\r\n#endif\r\n", 20), {}, ReadStatus::Read, {{"A", "1"}}},
		{"stray else then else", std::string("A 1\r\n#else\r\nB 2\r\n#else\r\nC 3\r\n", 29), {}, ReadStatus::Read, {{"A", "1"}, {"C", "3"}}},
		{"value leading backslash", std::string("A \\x\r\n", 6), {}, ReadStatus::Read, {{"A", "x"}}},
		{"key starts with slash", std::string("/A 1\r\n", 6), {}, ReadStatus::Failed, {}},
		{"key contains backslash", std::string("A\\B 1\r\n", 7), {}, ReadStatus::Read, {{"A", "B 1"}}},
		{"lone bs then CRLF (cont)", std::string("A x \\\r\n  y\r\nB 2\r\n", 17), {}, ReadStatus::Read, {{"A", "x y"}, {"B", "2"}}},
		{"continuation crosses blank and comment", std::string("A x\\\r\n\r\n// note\r\ny\r\nB 2\r\n", 25), {}, ReadStatus::Read, {{"A", "xy"}, {"B", "2"}}},
		{"bom", std::string("\xef" "\xbb" "\xbf" "A 1\r\n", 8), {}, ReadStatus::Read, {{"A", "1"}}},
		{"nul stops", std::string("A 1\r\nB\x00" "2\r\n", 10), {}, ReadStatus::Failed, {{"A", "1"}}},
		{"if at eof", std::string("A 1\r\n#if", 8), {}, ReadStatus::Read, {{"A", "1"}}},
	};
	int failures = 0;
	for (const Case &c : cases) {
		KeyValueList list;
		list.nodes = c.before;
		const opennova::mns::ReadResult read = opennova::mns::parse_key_value_buffer(c.text.data(), c.text.size(), list);
		bool same = read.status == c.status && list.nodes.size() == c.after.size();
		for (size_t i = 0; same && i < c.after.size(); ++i)
			same = list.nodes[i].name == c.after[i].name && list.nodes[i].value == c.after[i].value;
		if (!same) {
			std::fprintf(stderr, "reader case '%s' differs from the emulator\n", c.label);
			++failures;
		}
	}
	TEST_EXPECT(failures == 0);
	// Where it stops: the name's end for a failure, the stuck byte for a hang.
	{
		KeyValueList list;
		const std::string text("A 1\r\nB%C 2\r\n", 12);
		const opennova::mns::ReadResult read = opennova::mns::parse_key_value_buffer(text.data(), text.size(), list);
		TEST_EXPECT(read.status == ReadStatus::Failed && read.offset == 6);
		TEST_EXPECT(opennova::mns::line_at_offset(text.data(), text.size(), read.offset) == 2);
	}
	{
		KeyValueList list;
		const std::string text("A 1\r\n# hi\r\n", 11);
		const opennova::mns::ReadResult read = opennova::mns::parse_key_value_buffer(text.data(), text.size(), list);
		TEST_EXPECT(read.status == ReadStatus::Hangs && read.offset == 5);
	}

	std::printf("test_retail_reader_cases passed\n");
	return 0;
}

static int test_substitute() {
	opennova::mns::StyleSheet sheet;
	sheet.variables["FOO"] = "hello";
	sheet.variables["BAR"] = "world";

	TEST_EXPECT(sheet.substitute("Say %FOO% to the %BAR%!") == "Say hello to the world!");

	// Unknown variable left as-is.
	TEST_EXPECT(sheet.substitute("Unknown: %UNKNOWN%") == "Unknown: %UNKNOWN%");

	// Case insensitive.
	TEST_EXPECT(sheet.substitute("%foo% %FOO%") == "hello hello");

	// Lone percent.
	TEST_EXPECT(sheet.substitute("50% off") == "50% off");

	// A name the expansion stops inside is no reference; the scan goes on after the '%'.
	sheet.variables["A B"] = "no";
	sheet.variables["A/B"] = "no";
	TEST_EXPECT(sheet.substitute("%A B%") == "%A B%");
	TEST_EXPECT(sheet.substitute("%A/B%%FOO%") == "%A/B%hello");
	// "%%" is two literal '%'s, the second free to open a reference.
	TEST_EXPECT(sheet.substitute("100%%") == "100%%");
	TEST_EXPECT(sheet.substitute("%%FOO%") == "%hello");
	// A line end does not stop a name; a value is not expanded again.
	sheet.variables["X\nY"] = "joined";
	sheet.variables["NEST"] = "%FOO%";
	TEST_EXPECT(sheet.substitute("%X\nY%") == "joined");
	TEST_EXPECT(sheet.substitute("%NEST%") == "%FOO%");

	std::printf("test_substitute passed\n");
	return 0;
}

// The name rule the menus' expansion scans by [orig: NapiXML_ExpandVariablesInText @
// 0x63a000, the stops @ 0x63a2aa..0x63a2be, the empty name @ 0x63a2d1].
static int test_variable_references() {
	using opennova::mns::holds_variable_reference;
	using opennova::mns::is_variable_reference;
	using opennova::mns::variable_name;
	using opennova::mns::variable_reference_at;
	TEST_EXPECT(variable_reference_at("x%DEF_TEXT_FG%y", 1) == 13);
	TEST_EXPECT(variable_reference_at("x%DEF_TEXT_FG%y", 0) == 0);
	for (const char *stop : {"%A B%", "%A<B%", "%A>B%", "%A/B%", "%A\\B%", "%%", "%A", "%"})
		TEST_EXPECT(variable_reference_at(stop, 0) == 0);
	TEST_EXPECT(variable_reference_at(std::string("%A\0B%", 5), 0) == 0);
	TEST_EXPECT(variable_reference_at("%A\tB%", 0) == 5);

	TEST_EXPECT(is_variable_reference("%def_text_fg%"));
	TEST_EXPECT(!is_variable_reference("%A%B%") && !is_variable_reference(" %A%") && !is_variable_reference("%%"));
	TEST_EXPECT(!is_variable_reference("%A B%") && !is_variable_reference(""));
	TEST_EXPECT(variable_name("%def_text_fg%") == "def_text_fg" && variable_name("%A B%") == "%A B%");

	TEST_EXPECT(holds_variable_reference("Version %V%") && holds_variable_reference("1%%X%"));
	TEST_EXPECT(!holds_variable_reference("50% of 100%") && !holds_variable_reference("%A B%") &&
	            !holds_variable_reference("%%"));

	std::printf("test_variable_references passed\n");
	return 0;
}

static int test_has() {
	opennova::mns::StyleSheet sheet;
	sheet.variables["FOO"] = "bar";

	TEST_EXPECT(sheet.has("FOO"));
	TEST_EXPECT(sheet.has("foo"));
	TEST_EXPECT(sheet.has("Foo"));
	TEST_EXPECT(!sheet.has("BAR"));

	std::printf("test_has passed\n");
	return 0;
}

static int test_write() {
	opennova::mns::StyleSheet sheet;
	sheet.variables["FOO"] = "bar";
	sheet.variables["BAZ"] = "qux";

	std::vector<uint8_t> out;
	std::string error;
	TEST_EXPECT(opennova::mns::write(sheet, out, error));

	// Parse it back.
	opennova::mns::StyleSheet sheet2;
	TEST_EXPECT(opennova::mns::parse(reinterpret_cast<const char *>(out.data()), out.size(), sheet2, error));
	TEST_EXPECT(sheet2.get("FOO") == "bar");
	TEST_EXPECT(sheet2.get("BAZ") == "qux");

	std::printf("test_write passed\n");
	return 0;
}

static int test_menu_style() {
	// Test parsing a snippet similar to the real menu_style.mns file.
	opennova::mns::StyleSheet sheet;
	std::string error;

	const char *data =
		"// Style sheet for menus\r\n"
		"DEF_FONTNAME\t\t\t\tGunpl22b.fnt\r\n"
		"DEF_FONTNAME_LG\t\t\t\tGunpl27b.fnt\r\n"
		"DEF_TEXT_FG\t\t\t\t\tFFFFFFFF\r\n"
		"DEF_TEXT_MOUSEOVER_FG\t\tFFFF0000\r\n"
		"DEF_TEXT_SELECTED_FG\t\tFFFF0000\r\n"
		"DEF_TEXT_DISABLED_FG\t\tFF545252\r\n";

	TEST_EXPECT(opennova::mns::parse(data, strlen(data), sheet, error));
	TEST_EXPECT(sheet.get("DEF_FONTNAME") == "Gunpl22b.fnt");
	TEST_EXPECT(sheet.get("DEF_FONTNAME_LG") == "Gunpl27b.fnt");
	TEST_EXPECT(sheet.get("DEF_TEXT_FG") == "FFFFFFFF");
	TEST_EXPECT(sheet.get("DEF_TEXT_MOUSEOVER_FG") == "FFFF0000");
	TEST_EXPECT(sheet.get("DEF_TEXT_SELECTED_FG") == "FFFF0000");
	TEST_EXPECT(sheet.get("DEF_TEXT_DISABLED_FG") == "FF545252");

	// Test substitution.
	TEST_EXPECT(sheet.substitute("Color: %DEF_TEXT_MOUSEOVER_FG%") == "Color: FFFF0000");

	std::printf("test_menu_style passed\n");
	return 0;
}

int main() {
	int failures = 0;
	failures += test_empty();
	failures += test_simple_variables();
	failures += test_tab_separator();
	failures += test_utf8_bom();
	failures += test_comment();
	failures += test_if_0();
	failures += test_if_1();
	failures += test_if_0_else();
	failures += test_if_1_else();
	failures += test_nested_if();
	failures += test_line_continuation();
	failures += test_missing_value_delimiter_fails();
	failures += test_lf_line_ends_stop();
	failures += test_retail_reader_cases();
	failures += test_substitute();
	failures += test_variable_references();
	failures += test_has();
	failures += test_write();
	failures += test_menu_style();

	if (failures == 0) {
		std::printf("\nAll tests passed!\n");
	}
	return failures == 0 ? 0 : 1;
}
