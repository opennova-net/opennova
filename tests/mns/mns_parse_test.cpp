// Tests for MNS stylesheet parser.

#include "mns/mns.h"

#include <cassert>
#include <cstring>
#include <iostream>

void test_empty() {
	mns::StyleSheet sheet;
	std::string error;

	bool ok = mns::parse("", 0, sheet, error);
	assert(ok);
	assert(sheet.variables.empty());

	std::cout << "test_empty passed\n";
}

void test_simple_variables() {
	mns::StyleSheet sheet;
	std::string error;

	const char *data = "FOO bar\nBAZ qux\n";
	bool ok = mns::parse(data, strlen(data), sheet, error);
	assert(ok);
	assert(sheet.variables.size() == 2);
	assert(sheet.get("foo") == "bar");
	assert(sheet.get("FOO") == "bar");
	assert(sheet.get("baz") == "qux");
	assert(sheet.get("BAZ") == "qux");

	std::cout << "test_simple_variables passed\n";
}

void test_tab_separator() {
	mns::StyleSheet sheet;
	std::string error;

	const char *data = "DEF_FONTNAME\tGunpl22b.fnt\n";
	bool ok = mns::parse(data, strlen(data), sheet, error);
	assert(ok);
	assert(sheet.get("DEF_FONTNAME") == "Gunpl22b.fnt");

	std::cout << "test_tab_separator passed\n";
}

void test_utf8_bom() {
	mns::StyleSheet sheet;
	std::string error;

	// UTF-8 BOM + "FOO bar\n". Split the literal so MSVC does not fold the
	// trailing 'F' into the \xBF hex escape ("\xBFF" is out of range).
	const char data[] = "\xEF\xBB\xBF" "FOO bar\n";
	bool ok = mns::parse(data, sizeof(data) - 1, sheet, error);
	assert(ok);
	assert(sheet.get("FOO") == "bar");

	std::cout << "test_utf8_bom passed\n";
}

void test_comment() {
	mns::StyleSheet sheet;
	std::string error;

	const char *data = "// This is a comment\nFOO bar\n// Another comment\nBAZ qux\n";
	bool ok = mns::parse(data, strlen(data), sheet, error);
	assert(ok);
	assert(sheet.variables.size() == 2);
	assert(sheet.get("FOO") == "bar");
	assert(sheet.get("BAZ") == "qux");

	std::cout << "test_comment passed\n";
}

void test_if_0() {
	mns::StyleSheet sheet;
	std::string error;

	const char *data = "BEFORE yes\n#if 0\nSKIPPED no\n#endif\nAFTER yes\n";
	bool ok = mns::parse(data, strlen(data), sheet, error);
	assert(ok);
	assert(sheet.variables.size() == 2);
	assert(sheet.get("BEFORE") == "yes");
	assert(sheet.get("AFTER") == "yes");
	assert(!sheet.has("SKIPPED"));

	std::cout << "test_if_0 passed\n";
}

void test_if_1() {
	mns::StyleSheet sheet;
	std::string error;

	const char *data = "BEFORE yes\n#if 1\nINCLUDED yes\n#endif\nAFTER yes\n";
	bool ok = mns::parse(data, strlen(data), sheet, error);
	assert(ok);
	assert(sheet.variables.size() == 3);
	assert(sheet.get("BEFORE") == "yes");
	assert(sheet.get("INCLUDED") == "yes");
	assert(sheet.get("AFTER") == "yes");

	std::cout << "test_if_1 passed\n";
}

void test_if_0_else() {
	mns::StyleSheet sheet;
	std::string error;

	const char *data = "#if 0\nSKIPPED no\n#else\nINCLUDED yes\n#endif\n";
	bool ok = mns::parse(data, strlen(data), sheet, error);
	assert(ok);
	assert(sheet.variables.size() == 1);
	assert(sheet.get("INCLUDED") == "yes");
	assert(!sheet.has("SKIPPED"));

	std::cout << "test_if_0_else passed\n";
}

void test_if_1_else() {
	mns::StyleSheet sheet;
	std::string error;

	const char *data = "#if 1\nINCLUDED yes\n#else\nSKIPPED no\n#endif\n";
	bool ok = mns::parse(data, strlen(data), sheet, error);
	assert(ok);
	assert(sheet.variables.size() == 1);
	assert(sheet.get("INCLUDED") == "yes");
	assert(!sheet.has("SKIPPED"));

	std::cout << "test_if_1_else passed\n";
}

void test_nested_if() {
	mns::StyleSheet sheet;
	std::string error;

	const char *data = "#if 0\nSKIPPED1 no\n#if 1\nSKIPPED2 no\n#endif\n#endif\nAFTER yes\n";
	bool ok = mns::parse(data, strlen(data), sheet, error);
	assert(ok);
	assert(sheet.variables.size() == 1);
	assert(sheet.get("AFTER") == "yes");
	assert(!sheet.has("SKIPPED1"));
	assert(!sheet.has("SKIPPED2"));

	std::cout << "test_nested_if passed\n";
}

void test_line_continuation() {
	mns::StyleSheet sheet;
	std::string error;

	const char *data = "FOO bar \\\nbaz\n";
	bool ok = mns::parse(data, strlen(data), sheet, error);
	assert(ok);
	assert(sheet.get("FOO") == "bar baz");

	std::cout << "test_line_continuation passed\n";
}

void test_substitute() {
	mns::StyleSheet sheet;
	sheet.variables["FOO"] = "hello";
	sheet.variables["BAR"] = "world";

	std::string result = sheet.substitute("Say %FOO% to the %BAR%!");
	assert(result == "Say hello to the world!");

	// Unknown variable left as-is.
	result = sheet.substitute("Unknown: %UNKNOWN%");
	assert(result == "Unknown: %UNKNOWN%");

	// Case insensitive.
	result = sheet.substitute("%foo% %FOO%");
	assert(result == "hello hello");

	// Lone percent.
	result = sheet.substitute("50% off");
	assert(result == "50% off");

	std::cout << "test_substitute passed\n";
}

void test_has() {
	mns::StyleSheet sheet;
	sheet.variables["FOO"] = "bar";

	assert(sheet.has("FOO"));
	assert(sheet.has("foo"));
	assert(sheet.has("Foo"));
	assert(!sheet.has("BAR"));

	std::cout << "test_has passed\n";
}

void test_write() {
	mns::StyleSheet sheet;
	sheet.variables["FOO"] = "bar";
	sheet.variables["BAZ"] = "qux";

	std::vector<uint8_t> out;
	std::string error;
	bool ok = mns::write(sheet, out, error);
	assert(ok);

	// Parse it back.
	mns::StyleSheet sheet2;
	ok = mns::parse(reinterpret_cast<const char *>(out.data()), out.size(), sheet2, error);
	assert(ok);
	assert(sheet2.get("FOO") == "bar");
	assert(sheet2.get("BAZ") == "qux");

	std::cout << "test_write passed\n";
}

void test_menu_style() {
	// Test parsing a snippet similar to the real menu_style.mnu file.
	mns::StyleSheet sheet;
	std::string error;

	const char *data =
		"// Style sheet for menus\n"
		"DEF_FONTNAME\t\t\t\tGunpl22b.fnt\n"
		"DEF_FONTNAME_LG\t\t\t\tGunpl27b.fnt\n"
		"DEF_TEXT_FG\t\t\t\t\tFFFFFFFF\n"
		"DEF_TEXT_MOUSEOVER_FG\t\tFFFF0000\n"
		"DEF_TEXT_SELECTED_FG\t\tFFFF0000\n"
		"DEF_TEXT_DISABLED_FG\t\tFF545252\n";

	bool ok = mns::parse(data, strlen(data), sheet, error);
	assert(ok);
	assert(sheet.get("DEF_FONTNAME") == "Gunpl22b.fnt");
	assert(sheet.get("DEF_FONTNAME_LG") == "Gunpl27b.fnt");
	assert(sheet.get("DEF_TEXT_FG") == "FFFFFFFF");
	assert(sheet.get("DEF_TEXT_MOUSEOVER_FG") == "FFFF0000");
	assert(sheet.get("DEF_TEXT_SELECTED_FG") == "FFFF0000");
	assert(sheet.get("DEF_TEXT_DISABLED_FG") == "FF545252");

	// Test substitution.
	std::string text = "Color: %DEF_TEXT_MOUSEOVER_FG%";
	std::string result = sheet.substitute(text);
	assert(result == "Color: FFFF0000");

	std::cout << "test_menu_style passed\n";
}

int main() {
	test_empty();
	test_simple_variables();
	test_tab_separator();
	test_utf8_bom();
	test_comment();
	test_if_0();
	test_if_1();
	test_if_0_else();
	test_if_1_else();
	test_nested_if();
	test_line_continuation();
	test_substitute();
	test_has();
	test_write();
	test_menu_style();

	std::cout << "\nAll tests passed!\n";
	return 0;
}
