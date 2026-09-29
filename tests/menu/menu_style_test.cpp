// The menu shell's stylesheets (engine/runtime/menu/menu_style.h): menu_style.mns into an
// empty list, then brand.mns onto it [orig: Menu_InitShellResources @ 0x552500], each
// through the retail reader; later definitions win and keep the first spelling; a
// missing or empty file changes nothing; a sheet the reader stops in keeps what it read.

#include <runtime/menu/menu_style.h>

#include "common/test_expect.h"

#include <cstdio>
#include <map>
#include <string>

namespace {

using opennova::menu::load_shell_style;
using opennova::menu::ShellStyle;

// A reader over named byte strings: the shell's file lookup.
opennova::menu::ShellStyleReader reader(const std::map<std::string, std::string> &files) {
	return [files](const std::string &name, std::string &bytes) {
		const auto found = files.find(name);
		if (found == files.end()) return false;
		bytes = found->second;
		return true;
	};
}

} // namespace

static int test_order_and_override() {
	TEST_EXPECT(std::string(opennova::menu::kShellStylesheets[0].name) == "menu_style.mns");
	TEST_EXPECT(!opennova::menu::kShellStylesheets[0].append);
	TEST_EXPECT(std::string(opennova::menu::kShellStylesheets[1].name) == "brand.mns");
	TEST_EXPECT(opennova::menu::kShellStylesheets[1].append);
	TEST_EXPECT(opennova::menu::is_shell_stylesheet("MENU_STYLE.MNS") && !opennova::menu::is_shell_stylesheet("other.mns"));

	const ShellStyle style = load_shell_style(reader({
	        {"menu_style.mns", "DEF_TEXT_FG FFFFFFFF\r\nTRIM_COLOR FF808080\r\n"},
	        {"brand.mns", "trim_color FF102030\r\nBRAND_ONLY 1\r\n"},
	}));
	TEST_EXPECT(style.sheets.size() == 2 && style.sheets[0].present && style.sheets[1].present);
	const opennova::mns::StyleSheet sheet = style.list.sheet();
	TEST_EXPECT(sheet.get("DEF_TEXT_FG") == "FFFFFFFF");
	TEST_EXPECT(sheet.get("TRIM_COLOR") == "FF102030"); // brand.mns wins
	TEST_EXPECT(sheet.get("BRAND_ONLY") == "1");
	// One node per name, the first spelling kept.
	TEST_EXPECT(style.list.nodes.size() == 3 && style.list.nodes[1].name == "TRIM_COLOR");

	std::printf("test_order_and_override passed\n");
	return 0;
}

static int test_missing_and_empty_sheets() {
	// No brand.mns (retail ships none): menu_style.mns alone.
	{
		const ShellStyle style = load_shell_style(reader({{"menu_style.mns", "A 1\r\n"}}));
		TEST_EXPECT(style.sheets[0].present && !style.sheets[1].present && style.any_present());
		TEST_EXPECT(style.list.sheet().get("A") == "1");
	}
	// Neither: an empty list, so every %VAR% stays literal.
	{
		const ShellStyle style = load_shell_style(reader({}));
		TEST_EXPECT(!style.any_present() && style.list.nodes.empty());
	}
	// An empty menu_style.mns changes nothing, and brand.mns still appends.
	{
		const ShellStyle style = load_shell_style(reader({{"menu_style.mns", ""}, {"brand.mns", "B 2\r\n"}}));
		TEST_EXPECT(!style.sheets[0].present && style.sheets[1].present);
		TEST_EXPECT(style.list.nodes.size() == 1 && style.list.sheet().get("B") == "2");
	}

	std::printf("test_missing_and_empty_sheets passed\n");
	return 0;
}

static int test_a_sheet_the_reader_stops_in() {
	// The reader stops at a name that ends on its line end; what it read stays, and
	// brand.mns is read after it all the same.
	{
		const ShellStyle style = load_shell_style(reader({
		        {"menu_style.mns", "A 1\r\nBROKEN\r\nC 3\r\n"},
		        {"brand.mns", "D 4\r\n"},
		}));
		TEST_EXPECT(style.sheets[0].read.status == opennova::mns::ReadStatus::Failed);
		TEST_EXPECT(style.sheets[0].stopped_line == 2);
		const opennova::mns::StyleSheet sheet = style.list.sheet();
		TEST_EXPECT(sheet.get("A") == "1" && !sheet.has("C") && sheet.get("D") == "4");
	}
	// A sheet retail would stop responding on (LF line ends) is read up to that place.
	{
		const ShellStyle style = load_shell_style(reader({{"menu_style.mns", "A 1\nB 2\n"}}));
		TEST_EXPECT(style.sheets[0].read.status == opennova::mns::ReadStatus::Hangs);
		TEST_EXPECT(style.list.sheet().get("A") == "1" && !style.list.sheet().has("B"));
	}
	// brand.mns continuing a name menu_style.mns already has: the continued text goes
	// to the list's head, the node made last.
	{
		const ShellStyle style = load_shell_style(reader({
		        {"menu_style.mns", "Z z\r\nA old\r\n"},
		        {"brand.mns", "Z x\\\r\ny\r\n"},
		}));
		TEST_EXPECT(style.list.sheet().get("Z") == "x" && style.list.sheet().get("A") == "oldy");
	}

	std::printf("test_a_sheet_the_reader_stops_in passed\n");
	return 0;
}

int main() {
	int failures = 0;
	failures += test_order_and_override();
	failures += test_missing_and_empty_sheets();
	failures += test_a_sheet_the_reader_stops_in();
	if (failures == 0) std::printf("\nAll tests passed!\n");
	return failures == 0 ? 0 : 1;
}
