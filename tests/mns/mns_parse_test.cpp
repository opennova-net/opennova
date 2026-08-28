// Tests for the MNS stylesheet flat parse view (opennova::mns::parse -> StyleSheet).
// Expectations are TEST_EXPECT (real assertions under the Release ctest
// config, where assert() would compile away).

#include <formats/mns/mns.h>

#include "common/test_expect.h"

#include <cstdio>
#include <cstring>

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

	const char *data = "FOO bar\nBAZ qux\n";
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

	const char *data = "DEF_FONTNAME\tGunpl22b.fnt\n";
	TEST_EXPECT(opennova::mns::parse(data, strlen(data), sheet, error));
	TEST_EXPECT(sheet.get("DEF_FONTNAME") == "Gunpl22b.fnt");

	std::printf("test_tab_separator passed\n");
	return 0;
}

static int test_utf8_bom() {
	opennova::mns::StyleSheet sheet;
	std::string error;

	// UTF-8 BOM + "FOO bar\n". Split the literal so MSVC does not fold the
	// trailing 'F' into the \xBF hex escape ("\xBFF" is out of range).
	const char data[] = "\xEF\xBB\xBF" "FOO bar\n";
	TEST_EXPECT(opennova::mns::parse(data, sizeof(data) - 1, sheet, error));
	TEST_EXPECT(sheet.get("FOO") == "bar");

	std::printf("test_utf8_bom passed\n");
	return 0;
}

static int test_comment() {
	opennova::mns::StyleSheet sheet;
	std::string error;

	const char *data = "// This is a comment\nFOO bar\n// Another comment\nBAZ qux\n";
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

	const char *data = "BEFORE yes\n#if 0\nSKIPPED no\n#endif\nAFTER yes\n";
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

	const char *data = "BEFORE yes\n#if 1\nINCLUDED yes\n#endif\nAFTER yes\n";
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

	const char *data = "#if 0\nSKIPPED no\n#else\nINCLUDED yes\n#endif\n";
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

	const char *data = "#if 1\nINCLUDED yes\n#else\nSKIPPED no\n#endif\n";
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

	const char *data = "#if 0\nSKIPPED1 no\n#if 1\nSKIPPED2 no\n#endif\n#endif\nAFTER yes\n";
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

	const char *data = "FOO bar \\\nbaz\n";
	TEST_EXPECT(opennova::mns::parse(data, strlen(data), sheet, error));
	TEST_EXPECT(sheet.get("FOO") == "bar baz");

	std::printf("test_line_continuation passed\n");
	return 0;
}

static int test_missing_value_delimiter_fails() {
	opennova::mns::StyleSheet sheet;
	std::string error;
	const char *data = "FOO\n";
	TEST_EXPECT(!opennova::mns::parse(data, strlen(data), sheet, error));
	TEST_EXPECT(error.find("delimiter") != std::string::npos);

	std::printf("test_missing_value_delimiter_fails passed\n");
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

	std::printf("test_substitute passed\n");
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
		"// Style sheet for menus\n"
		"DEF_FONTNAME\t\t\t\tGunpl22b.fnt\n"
		"DEF_FONTNAME_LG\t\t\t\tGunpl27b.fnt\n"
		"DEF_TEXT_FG\t\t\t\t\tFFFFFFFF\n"
		"DEF_TEXT_MOUSEOVER_FG\t\tFFFF0000\n"
		"DEF_TEXT_SELECTED_FG\t\tFFFF0000\n"
		"DEF_TEXT_DISABLED_FG\t\tFF545252\n";

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
	failures += test_substitute();
	failures += test_has();
	failures += test_write();
	failures += test_menu_style();

	if (failures == 0) {
		std::printf("\nAll tests passed!\n");
	}
	return failures == 0 ? 0 : 1;
}
