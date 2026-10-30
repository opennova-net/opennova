// The process's named string tables (formats/rtxt/text_tables.h): registration by name without
// case, the override table searched first, the miss marker, the display form, the menu tokens'
// chain and the override install from a file's bytes.
#include "rtxt_test_util.h"

#include <formats/rtxt/text_tables.h>

#include <memory>
#include <string>
#include <vector>

using namespace opennova::rtxt;
using rtxt_test::expect;

namespace {

std::shared_ptr<File> one_entry(const std::string &section, const std::string &key,
		const std::string &text) {
	auto f = std::make_shared<File>();
	f->sections = {{section, 1}};
	f->entries = {{key, text, {0, 0}, 0}};
	f->build_lookup();
	return f;
}

bool registration() {
	clear_tables();
	register_table("gametext", one_entry("Item Names", "STR_ITM0001", "M4 Carbine"));
	bool ok = expect(registered_table("GAMETEXT") != nullptr, "table names match without case");
	ok &= expect(table_lookup("gametext", "Item Names", "str_itm0001") == "M4 Carbine",
			"keys match without case");
	register_table("gametext", nullptr);
	ok &= expect(registered_table("gametext") == nullptr, "registering null unregisters");
	return ok;
}

bool miss_marker() {
	clear_tables();
	register_table("gameerr", one_entry("Errors", "ERR_NET", "Network failure"));
	bool ok = expect(table_lookup("gameerr", "Errors", "NOPE") == "??Errors:NOPE??",
			"a miss is the visible marker");
	ok &= expect(table_lookup("gameerr", "Wrong Section", "ERR_NET") ==
					"??Wrong Section:ERR_NET??",
			"the section scopes the lookup");
	ok &= expect(table_lookup("unregistered", "s", "k") == "??s:k??",
			"an unregistered table is a miss too");
	return ok;
}

bool override_first() {
	clear_tables();
	register_table("gametext", one_entry("menu", "TITLE", "Base game title"));
	set_text_override(one_entry("menu", "TITLE", "Expansion title"));
	bool ok = expect(table_lookup("gametext", "menu", "TITLE") == "Expansion title",
			"the override table answers first");
	ok &= expect(table_text_or("gametext", "menu", "TITLE", "fb") == "Expansion title",
			"the fallback form reads the override first");
	ok &= expect(table_text_or("gametext", "menu", "OTHER", "fb") == "fb",
			"a key neither has answers the fallback");
	ok &= expect(table_text_or("absent", "menu", "TITLE", "fb") == "fb",
			"no table answers the fallback, the override unsearched");
	std::string out;
	ok &= expect(!table_text("absent", "menu", "TITLE", out), "no table, no text");
	set_text_override(one_entry("menu", "OTHER", "Override"));
	ok &= expect(table_lookup("gametext", "menu", "TITLE") == "Base game title",
			"keys the override lacks read the named table");
	set_text_override(nullptr);
	ok &= expect(table_lookup("gametext", "menu", "TITLE") == "Base game title",
			"no override, the named table answers");
	return ok;
}

bool display_form() {
	clear_tables();
	register_table("menutxt", one_entry("menu", "BTN_OK", "{hot}OK"));
	bool ok = expect(table_lookup("menutxt", "menu", "BTN_OK") == "{hot}OK",
			"the lookup returns the raw text");
	ok &= expect(table_lookup_display("menutxt", "menu", "BTN_OK") == "OK",
			"the display form strips the marker");
	ok &= expect(table_lookup_display("menutxt", "menu", "MISSING") == "??menu:MISSING??",
			"the miss marker is never stripped");
	return ok;
}

bool menu_tokens() {
	clear_tables();
	register_table(kTableGameUi, one_entry("Menu", "TOKEN", "gameui text"));
	bool ok = expect(menu_text("TOKEN", "fb") == "gameui text",
			"the menu's own table answers what the menu resource lacks");
	register_table(kTableMenuTxt, one_entry("Menu", "TOKEN", "menutxt text"));
	ok &= expect(menu_text("TOKEN", "fb") == "menutxt text", "the menu resource answers first");
	ok &= expect(menu_text("ABSENT", "fb") == "fb", "neither: the fallback");
	return ok;
}

bool override_from_bytes() {
	clear_tables();
	std::vector<uint8_t> bytes;
	std::string error;
	bool ok = expect(write(*one_entry("Menu", "TITLE", "Loose title"), bytes, error),
			"the override table writes");
	install_text_override_bytes(bytes.data(), bytes.size());
	register_table(kTableMenuTxt, one_entry("Menu", "TITLE", "Base title"));
	ok &= expect(table_lookup(kTableMenuTxt, "Menu", "TITLE") == "Loose title",
			"a table that parses is installed");
	install_text_override_bytes(nullptr, 0);
	ok &= expect(text_override() == nullptr, "no bytes leave none");
	const uint8_t junk[3] = {1, 2, 3};
	install_text_override_bytes(junk, sizeof junk);
	ok &= expect(text_override() == nullptr, "a file that does not parse leaves none");
	return ok;
}

} // namespace

int main() {
	bool ok = registration();
	ok &= miss_marker();
	ok &= override_first();
	ok &= display_form();
	ok &= menu_tokens();
	ok &= override_from_bytes();
	clear_tables();
	set_text_override(nullptr);
	if (!ok) return 1;
	std::printf("OK: rtxt text tables\n");
	return 0;
}
